// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2023 Intel Corporation */

#include <net/libeth/netdev.h>

#include "idpf.h"

/**
 * idpf_get_rxnfc - command to get RX flow classification rules
 * @netdev: network interface device structure
 * @cmd: ethtool rxnfc command
 * @rule_locs: pointer to store rule locations
 *
 * Returns Success if the command is supported.
 */
static int idpf_get_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd,
			  u32 __always_unused *rule_locs)
{
	struct idpf_vport *vport;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = vport->num_rxq;
		idpf_vport_ctrl_unlock(netdev);

		return 0;
	default:
		break;
	}

	idpf_vport_ctrl_unlock(netdev);

	return -EOPNOTSUPP;
}

/**
 * idpf_get_rxfh_key_size - get the RSS hash key size
 * @netdev: network interface device structure
 *
 * Returns the key size on success, error value on failure.
 */
static u32 idpf_get_rxfh_key_size(struct net_device *netdev)
{
	struct idpf_netdev_priv *np = netdev_priv(netdev);
	struct idpf_vport_user_config_data *user_config;

	if (!idpf_is_cap_ena_all(np->adapter, IDPF_RSS_CAPS, IDPF_CAP_RSS))
		return -EOPNOTSUPP;

	user_config = &np->adapter->vport_config[np->vport_idx]->user_config;

	return user_config->rss_data.rss_key_size;
}

/**
 * idpf_get_rxfh_indir_size - get the rx flow hash indirection table size
 * @netdev: network interface device structure
 *
 * Returns the table size on success, error value on failure.
 */
static u32 idpf_get_rxfh_indir_size(struct net_device *netdev)
{
	struct idpf_netdev_priv *np = netdev_priv(netdev);
	struct idpf_vport_user_config_data *user_config;

	if (!idpf_is_cap_ena_all(np->adapter, IDPF_RSS_CAPS, IDPF_CAP_RSS))
		return -EOPNOTSUPP;

	user_config = &np->adapter->vport_config[np->vport_idx]->user_config;

	return user_config->rss_data.rss_lut_size;
}

/**
 * idpf_get_rxfh - get the rx flow hash indirection table
 * @netdev: network interface device structure
 * @rxfh: pointer to param struct (indir, key, hfunc)
 *
 * Reads the indirection table directly from the hardware. Always returns 0.
 */
static int idpf_get_rxfh(struct net_device *netdev,
			 struct ethtool_rxfh_param *rxfh)
{
	struct idpf_netdev_priv *np = netdev_priv(netdev);
	struct idpf_rss_data *rss_data;
	struct idpf_adapter *adapter;
	int err = 0;
	u16 i;

	idpf_vport_ctrl_lock(netdev);

	adapter = np->adapter;

	if (!idpf_is_cap_ena_all(adapter, IDPF_RSS_CAPS, IDPF_CAP_RSS)) {
		err = -EOPNOTSUPP;
		goto unlock_mutex;
	}

	rss_data = &adapter->vport_config[np->vport_idx]->user_config.rss_data;
	if (np->state != __IDPF_VPORT_UP)
		goto unlock_mutex;

	rxfh->hfunc = ETH_RSS_HASH_TOP;

	if (rxfh->key)
		memcpy(rxfh->key, rss_data->rss_key, rss_data->rss_key_size);

	if (rxfh->indir) {
		for (i = 0; i < rss_data->rss_lut_size; i++)
			rxfh->indir[i] = rss_data->rss_lut[i];
	}

unlock_mutex:
	idpf_vport_ctrl_unlock(netdev);

	return err;
}

/**
 * idpf_set_rxfh - set the rx flow hash indirection table
 * @netdev: network interface device structure
 * @rxfh: pointer to param struct (indir, key, hfunc)
 * @extack: extended ACK from the Netlink message
 *
 * Returns -EINVAL if the table specifies an invalid queue id, otherwise
 * returns 0 after programming the table.
 */
static int idpf_set_rxfh(struct net_device *netdev,
			 struct ethtool_rxfh_param *rxfh,
			 struct netlink_ext_ack *extack)
{
	struct idpf_netdev_priv *np = netdev_priv(netdev);
	struct idpf_rss_data *rss_data;
	struct idpf_adapter *adapter;
	struct idpf_vport *vport;
	int err = 0;
	u16 lut;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	adapter = vport->adapter;

	if (!idpf_is_cap_ena_all(adapter, IDPF_RSS_CAPS, IDPF_CAP_RSS)) {
		err = -EOPNOTSUPP;
		goto unlock_mutex;
	}

	rss_data = &adapter->vport_config[vport->idx]->user_config.rss_data;
	if (np->state != __IDPF_VPORT_UP)
		goto unlock_mutex;

	if (rxfh->hfunc != ETH_RSS_HASH_NO_CHANGE &&
	    rxfh->hfunc != ETH_RSS_HASH_TOP) {
		err = -EOPNOTSUPP;
		goto unlock_mutex;
	}

	if (rxfh->key)
		memcpy(rss_data->rss_key, rxfh->key, rss_data->rss_key_size);

	if (rxfh->indir) {
		for (lut = 0; lut < rss_data->rss_lut_size; lut++)
			rss_data->rss_lut[lut] = rxfh->indir[lut];
	}

	err = idpf_config_rss(vport);

unlock_mutex:
	idpf_vport_ctrl_unlock(netdev);

	return err;
}

/**
 * idpf_get_channels: get the number of channels supported by the device
 * @netdev: network interface device structure
 * @ch: channel information structure
 *
 * Report maximum of TX and RX. Report one extra channel to match our MailBox
 * Queue.
 */
static void idpf_get_channels(struct net_device *netdev,
			      struct ethtool_channels *ch)
{
	struct idpf_netdev_priv *np = netdev_priv(netdev);
	struct idpf_vport_config *vport_config;
	u16 num_txq, num_rxq;
	u16 combined;

	vport_config = np->adapter->vport_config[np->vport_idx];

	num_txq = vport_config->user_config.num_req_tx_qs;
	num_rxq = vport_config->user_config.num_req_rx_qs;

	combined = min(num_txq, num_rxq);

	/* Report maximum channels */
	ch->max_combined = min_t(u16, vport_config->max_q.max_txq,
				 vport_config->max_q.max_rxq);
	ch->max_rx = vport_config->max_q.max_rxq;
	ch->max_tx = vport_config->max_q.max_txq;

	ch->max_other = IDPF_MAX_MBXQ;
	ch->other_count = IDPF_MAX_MBXQ;

	ch->combined_count = combined;
	ch->rx_count = num_rxq - combined;
	ch->tx_count = num_txq - combined;
}

/**
 * idpf_set_channels: set the new channel count
 * @netdev: network interface device structure
 * @ch: channel information structure
 *
 * Negotiate a new number of channels with CP. Returns 0 on success, negative
 * on failure.
 */
static int idpf_set_channels(struct net_device *netdev,
			     struct ethtool_channels *ch)
{
	struct idpf_vport_config *vport_config;
	unsigned int num_req_tx_q;
	unsigned int num_req_rx_q;
	struct idpf_vport *vport;
	u16 num_txq, num_rxq;
	struct device *dev;
	int err = 0;
	u16 idx;

	if (ch->rx_count && ch->tx_count) {
		netdev_err(netdev, "Dedicated RX or TX channels cannot be used simultaneously\n");
		return -EINVAL;
	}

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	idx = vport->idx;
	vport_config = vport->adapter->vport_config[idx];

	num_txq = vport_config->user_config.num_req_tx_qs;
	num_rxq = vport_config->user_config.num_req_rx_qs;

	num_req_tx_q = ch->combined_count + ch->tx_count;
	num_req_rx_q = ch->combined_count + ch->rx_count;

	dev = &vport->adapter->pdev->dev;
	/* It's possible to specify number of queues that exceeds max.
	 * Stack checks max combined_count and max [tx|rx]_count but not the
	 * max combined_count + [tx|rx]_count. These checks should catch that.
	 */
	if (num_req_tx_q > vport_config->max_q.max_txq) {
		dev_info(dev, "Maximum TX queues is %d\n",
			 vport_config->max_q.max_txq);
		err = -EINVAL;
		goto unlock_mutex;
	}
	if (num_req_rx_q > vport_config->max_q.max_rxq) {
		dev_info(dev, "Maximum RX queues is %d\n",
			 vport_config->max_q.max_rxq);
		err = -EINVAL;
		goto unlock_mutex;
	}

	if (num_req_tx_q == num_txq && num_req_rx_q == num_rxq)
		goto unlock_mutex;

	vport_config->user_config.num_req_tx_qs = num_req_tx_q;
	vport_config->user_config.num_req_rx_qs = num_req_rx_q;

	err = idpf_initiate_soft_reset(vport, IDPF_SR_Q_CHANGE);
	if (err) {
		/* roll back queue change */
		vport_config->user_config.num_req_tx_qs = num_txq;
		vport_config->user_config.num_req_rx_qs = num_rxq;
	}

unlock_mutex:
	idpf_vport_ctrl_unlock(netdev);

	return err;
}

/**
 * idpf_get_ringparam - Get ring parameters
 * @netdev: network interface device structure
 * @ring: ethtool ringparam structure
 * @kring: unused
 * @ext_ack: unused
 *
 * Returns current ring parameters. TX and RX rings are reported separately,
 * but the number of rings is not reported.
 */
static void idpf_get_ringparam(struct net_device *netdev,
			       struct ethtool_ringparam *ring,
			       struct kernel_ethtool_ringparam *kring,
			       struct netlink_ext_ack *ext_ack)
{
	struct idpf_vport *vport;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	ring->rx_max_pending = IDPF_MAX_RXQ_DESC;
	ring->tx_max_pending = IDPF_MAX_TXQ_DESC;
	ring->rx_pending = vport->rxq_desc_count;
	ring->tx_pending = vport->txq_desc_count;

	kring->tcp_data_split = idpf_vport_get_hsplit(vport);

	idpf_vport_ctrl_unlock(netdev);
}

/**
 * idpf_set_ringparam - Set ring parameters
 * @netdev: network interface device structure
 * @ring: ethtool ringparam structure
 * @kring: unused
 * @ext_ack: unused
 *
 * Sets ring parameters. TX and RX rings are controlled separately, but the
 * number of rings is not specified, so all rings get the same settings.
 */
static int idpf_set_ringparam(struct net_device *netdev,
			      struct ethtool_ringparam *ring,
			      struct kernel_ethtool_ringparam *kring,
			      struct netlink_ext_ack *ext_ack)
{
	struct idpf_vport_user_config_data *config_data;
	u32 new_rx_count, new_tx_count;
	struct idpf_vport *vport;
	int i, err = 0;
	u16 idx;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	idx = vport->idx;

	if (ring->tx_pending < IDPF_MIN_TXQ_DESC) {
		netdev_err(netdev, "Descriptors requested (Tx: %u) is less than min supported (%u)\n",
			   ring->tx_pending,
			   IDPF_MIN_TXQ_DESC);
		err = -EINVAL;
		goto unlock_mutex;
	}

	if (ring->rx_pending < IDPF_MIN_RXQ_DESC) {
		netdev_err(netdev, "Descriptors requested (Rx: %u) is less than min supported (%u)\n",
			   ring->rx_pending,
			   IDPF_MIN_RXQ_DESC);
		err = -EINVAL;
		goto unlock_mutex;
	}

	new_rx_count = ALIGN(ring->rx_pending, IDPF_REQ_RXQ_DESC_MULTIPLE);
	if (new_rx_count != ring->rx_pending)
		netdev_info(netdev, "Requested Rx descriptor count rounded up to %u\n",
			    new_rx_count);

	new_tx_count = ALIGN(ring->tx_pending, IDPF_REQ_DESC_MULTIPLE);
	if (new_tx_count != ring->tx_pending)
		netdev_info(netdev, "Requested Tx descriptor count rounded up to %u\n",
			    new_tx_count);

	if (new_tx_count == vport->txq_desc_count &&
	    new_rx_count == vport->rxq_desc_count &&
	    kring->tcp_data_split == idpf_vport_get_hsplit(vport))
		goto unlock_mutex;

	if (!idpf_vport_set_hsplit(vport, kring->tcp_data_split)) {
		NL_SET_ERR_MSG_MOD(ext_ack,
				   "setting TCP data split is not supported");
		err = -EOPNOTSUPP;

		goto unlock_mutex;
	}

	config_data = &vport->adapter->vport_config[idx]->user_config;
	config_data->num_req_txq_desc = new_tx_count;
	config_data->num_req_rxq_desc = new_rx_count;

	/* Since we adjusted the RX completion queue count, the RX buffer queue
	 * descriptor count needs to be adjusted as well
	 */
	for (i = 0; i < vport->num_bufqs_per_qgrp; i++)
		vport->bufq_desc_count[i] =
			IDPF_RX_BUFQ_DESC_COUNT(new_rx_count,
						vport->num_bufqs_per_qgrp);

	err = idpf_initiate_soft_reset(vport, IDPF_SR_Q_DESC_CHANGE);

unlock_mutex:
	idpf_vport_ctrl_unlock(netdev);

	return err;
}

static const char * const idpf_gstrings_port_stats[] = {
	"rx_bytes",
	"rx_unicast",
	"rx_multicast",
	"rx_broadcast",
	"rx_discards",
	"rx_errors",
	"rx_unknown_protocol",
	"tx_bytes",
	"tx_unicast",
	"tx_multicast",
	"tx_broadcast",
	"tx_discards",
	"tx_errors",
	"rx_invalid_frame_length",
	"rx_overflow_drop",
};
static_assert(ARRAY_SIZE(idpf_gstrings_port_stats) ==
	      sizeof_field(struct virtchnl2_vport_stats, counters) /
	      sizeof(__le64));

#define IDPF_PORT_STATS_LEN ARRAY_SIZE(idpf_gstrings_port_stats)

/**
 * idpf_add_stat_strings - Copy port stat strings into ethtool buffer
 * @p: ethtool buffer
 */
static void idpf_add_stat_strings(u8 **p)
{
	for (u32 i = 0; i < IDPF_PORT_STATS_LEN; i++)
		ethtool_puts(p, idpf_gstrings_port_stats[i]);
}

/**
 * idpf_get_strings - Get string set
 * @netdev: network interface device structure
 * @sset: id of string set
 * @data: buffer for string data
 *
 * Builds string tables for various string sets
 */
static void idpf_get_strings(struct net_device *netdev, u32 sset, u8 *data)
{
	switch (sset) {
	case ETH_SS_STATS:
		idpf_add_stat_strings(&data);
		libeth_ethtool_get_strings(netdev, sset, data);
		break;
	default:
		break;
	}
}

/**
 * idpf_get_sset_count - Get length of string set
 * @netdev: network interface device structure
 * @sset: id of string set
 *
 * Reports size of various string tables.
 */
static int idpf_get_sset_count(struct net_device *netdev, int sset)
{
	u32 count;

	if (sset != ETH_SS_STATS)
		return -EINVAL;

	count = IDPF_PORT_STATS_LEN;
	count += libeth_ethtool_get_sset_count(netdev, sset);

	return count;
}

/**
 * idpf_add_port_stats - Copy port stats into ethtool buffer
 * @vport: virtual port struct
 * @data: ethtool buffer to copy into
 */
static void idpf_add_port_stats(struct idpf_vport *vport, u64 **data)
{
	u64 *stats = *data;

	memcpy(stats, &vport->vport_stats.counters,
	       sizeof(vport->vport_stats.counters));

	for (u32 i = 0; i < IDPF_PORT_STATS_LEN; i++)
		le64_to_cpus(&stats[i]);

	*data += IDPF_PORT_STATS_LEN;
}

/**
 * idpf_get_ethtool_stats - report device statistics
 * @netdev: network interface device structure
 * @stats: ethtool statistics structure
 * @data: pointer to data buffer
 *
 * All statistics are added to the data buffer as an array of u64.
 */
static void idpf_get_ethtool_stats(struct net_device *netdev,
				   struct ethtool_stats __always_unused *stats,
				   u64 *data)
{
	struct idpf_netdev_priv *np = netdev_priv(netdev);
	struct idpf_vport *vport;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	if (np->state != __IDPF_VPORT_UP) {
		idpf_vport_ctrl_unlock(netdev);

		return;
	}

	rcu_read_lock();

	idpf_add_port_stats(vport, &data);
	libeth_ethtool_get_stats(netdev, stats, data);

	rcu_read_unlock();

	idpf_vport_ctrl_unlock(netdev);
}

/**
 * idpf_find_rxq_vec - find rxq vector from q index
 * @vport: virtual port associated to queue
 * @q_num: q index used to find queue
 *
 * returns pointer to rx vector
 */
static struct idpf_q_vector *idpf_find_rxq_vec(const struct idpf_vport *vport,
					       int q_num)
{
	int q_grp, q_idx;

	if (!idpf_is_queue_model_split(vport->rxq_model))
		return vport->rxq_grps->singleq.rxqs[q_num]->q_vector;

	q_grp = q_num / IDPF_DFLT_SPLITQ_RXQ_PER_GROUP;
	q_idx = q_num % IDPF_DFLT_SPLITQ_RXQ_PER_GROUP;

	return vport->rxq_grps[q_grp].splitq.rxq_sets[q_idx]->rxq.q_vector;
}

/**
 * idpf_find_txq_vec - find txq vector from q index
 * @vport: virtual port associated to queue
 * @q_num: q index used to find queue
 *
 * returns pointer to tx vector
 */
static struct idpf_q_vector *idpf_find_txq_vec(const struct idpf_vport *vport,
					       int q_num)
{
	int q_grp;

	if (!idpf_is_queue_model_split(vport->txq_model))
		return vport->txqs[q_num]->q_vector;

	q_grp = q_num / IDPF_DFLT_SPLITQ_TXQ_PER_GROUP;

	return vport->txq_grps[q_grp].complq->q_vector;
}

/**
 * __idpf_get_q_coalesce - get ITR values for specific queue
 * @ec: ethtool structure to fill with driver's coalesce settings
 * @q_vector: queue vector corresponding to this queue
 * @type: queue type
 */
static void __idpf_get_q_coalesce(struct ethtool_coalesce *ec,
				  const struct idpf_q_vector *q_vector,
				  enum virtchnl2_queue_type type)
{
	if (type == VIRTCHNL2_QUEUE_TYPE_RX) {
		ec->use_adaptive_rx_coalesce =
				IDPF_ITR_IS_DYNAMIC(q_vector->rx_intr_mode);
		ec->rx_coalesce_usecs = q_vector->rx_itr_value;
	} else {
		ec->use_adaptive_tx_coalesce =
				IDPF_ITR_IS_DYNAMIC(q_vector->tx_intr_mode);
		ec->tx_coalesce_usecs = q_vector->tx_itr_value;
	}
}

/**
 * idpf_get_q_coalesce - get ITR values for specific queue
 * @netdev: pointer to the netdev associated with this query
 * @ec: coalesce settings to program the device with
 * @q_num: update ITR/INTRL (coalesce) settings for this queue number/index
 *
 * Return 0 on success, and negative on failure
 */
static int idpf_get_q_coalesce(struct net_device *netdev,
			       struct ethtool_coalesce *ec,
			       u32 q_num)
{
	const struct idpf_netdev_priv *np = netdev_priv(netdev);
	const struct idpf_vport *vport;
	int err = 0;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	if (np->state != __IDPF_VPORT_UP)
		goto unlock_mutex;

	if (q_num >= vport->num_rxq && q_num >= vport->num_txq) {
		err = -EINVAL;
		goto unlock_mutex;
	}

	if (q_num < vport->num_rxq)
		__idpf_get_q_coalesce(ec, idpf_find_rxq_vec(vport, q_num),
				      VIRTCHNL2_QUEUE_TYPE_RX);

	if (q_num < vport->num_txq)
		__idpf_get_q_coalesce(ec, idpf_find_txq_vec(vport, q_num),
				      VIRTCHNL2_QUEUE_TYPE_TX);

unlock_mutex:
	idpf_vport_ctrl_unlock(netdev);

	return err;
}

/**
 * idpf_get_coalesce - get ITR values as requested by user
 * @netdev: pointer to the netdev associated with this query
 * @ec: coalesce settings to be filled
 * @kec: unused
 * @extack: unused
 *
 * Return 0 on success, and negative on failure
 */
static int idpf_get_coalesce(struct net_device *netdev,
			     struct ethtool_coalesce *ec,
			     struct kernel_ethtool_coalesce *kec,
			     struct netlink_ext_ack *extack)
{
	/* Return coalesce based on queue number zero */
	return idpf_get_q_coalesce(netdev, ec, 0);
}

/**
 * idpf_get_per_q_coalesce - get ITR values as requested by user
 * @netdev: pointer to the netdev associated with this query
 * @q_num: queue for which the itr values has to retrieved
 * @ec: coalesce settings to be filled
 *
 * Return 0 on success, and negative on failure
 */

static int idpf_get_per_q_coalesce(struct net_device *netdev, u32 q_num,
				   struct ethtool_coalesce *ec)
{
	return idpf_get_q_coalesce(netdev, ec, q_num);
}

/**
 * __idpf_set_q_coalesce - set ITR values for specific queue
 * @ec: ethtool structure from user to update ITR settings
 * @qv: queue vector for which itr values has to be set
 * @is_rxq: is queue type rx
 *
 * Returns 0 on success, negative otherwise.
 */
static int __idpf_set_q_coalesce(const struct ethtool_coalesce *ec,
				 struct idpf_q_vector *qv, bool is_rxq)
{
	u32 use_adaptive_coalesce, coalesce_usecs;
	bool is_dim_ena = false;
	u16 itr_val;

	if (is_rxq) {
		is_dim_ena = IDPF_ITR_IS_DYNAMIC(qv->rx_intr_mode);
		use_adaptive_coalesce = ec->use_adaptive_rx_coalesce;
		coalesce_usecs = ec->rx_coalesce_usecs;
		itr_val = qv->rx_itr_value;
	} else {
		is_dim_ena = IDPF_ITR_IS_DYNAMIC(qv->tx_intr_mode);
		use_adaptive_coalesce = ec->use_adaptive_tx_coalesce;
		coalesce_usecs = ec->tx_coalesce_usecs;
		itr_val = qv->tx_itr_value;
	}
	if (coalesce_usecs != itr_val && use_adaptive_coalesce) {
		netdev_err(qv->vport->netdev, "Cannot set coalesce usecs if adaptive enabled\n");

		return -EINVAL;
	}

	if (is_dim_ena && use_adaptive_coalesce)
		return 0;

	if (coalesce_usecs > IDPF_ITR_MAX) {
		netdev_err(qv->vport->netdev,
			   "Invalid value, %d-usecs range is 0-%d\n",
			   coalesce_usecs, IDPF_ITR_MAX);

		return -EINVAL;
	}

	if (coalesce_usecs % 2) {
		coalesce_usecs--;
		netdev_info(qv->vport->netdev,
			    "HW only supports even ITR values, ITR rounded to %d\n",
			    coalesce_usecs);
	}

	if (is_rxq) {
		qv->rx_itr_value = coalesce_usecs;
		if (use_adaptive_coalesce) {
			qv->rx_intr_mode = IDPF_ITR_DYNAMIC;
		} else {
			qv->rx_intr_mode = !IDPF_ITR_DYNAMIC;
			idpf_vport_intr_write_itr(qv, qv->rx_itr_value,
						  false);
		}
	} else {
		qv->tx_itr_value = coalesce_usecs;
		if (use_adaptive_coalesce) {
			qv->tx_intr_mode = IDPF_ITR_DYNAMIC;
		} else {
			qv->tx_intr_mode = !IDPF_ITR_DYNAMIC;
			idpf_vport_intr_write_itr(qv, qv->tx_itr_value, true);
		}
	}

	/* Update of static/dynamic itr will be taken care when interrupt is
	 * fired
	 */
	return 0;
}

/**
 * idpf_set_q_coalesce - set ITR values for specific queue
 * @vport: vport associated to the queue that need updating
 * @ec: coalesce settings to program the device with
 * @q_num: update ITR/INTRL (coalesce) settings for this queue number/index
 * @is_rxq: is queue type rx
 *
 * Return 0 on success, and negative on failure
 */
static int idpf_set_q_coalesce(const struct idpf_vport *vport,
			       const struct ethtool_coalesce *ec,
			       int q_num, bool is_rxq)
{
	struct idpf_q_vector *qv;

	qv = is_rxq ? idpf_find_rxq_vec(vport, q_num) :
		      idpf_find_txq_vec(vport, q_num);

	if (qv && __idpf_set_q_coalesce(ec, qv, is_rxq))
		return -EINVAL;

	return 0;
}

/**
 * idpf_set_coalesce - set ITR values as requested by user
 * @netdev: pointer to the netdev associated with this query
 * @ec: coalesce settings to program the device with
 * @kec: unused
 * @extack: unused
 *
 * Return 0 on success, and negative on failure
 */
static int idpf_set_coalesce(struct net_device *netdev,
			     struct ethtool_coalesce *ec,
			     struct kernel_ethtool_coalesce *kec,
			     struct netlink_ext_ack *extack)
{
	struct idpf_netdev_priv *np = netdev_priv(netdev);
	struct idpf_vport *vport;
	int i, err = 0;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	if (np->state != __IDPF_VPORT_UP)
		goto unlock_mutex;

	for (i = 0; i < vport->num_txq; i++) {
		err = idpf_set_q_coalesce(vport, ec, i, false);
		if (err)
			goto unlock_mutex;
	}

	for (i = 0; i < vport->num_rxq; i++) {
		err = idpf_set_q_coalesce(vport, ec, i, true);
		if (err)
			goto unlock_mutex;
	}

unlock_mutex:
	idpf_vport_ctrl_unlock(netdev);

	return err;
}

/**
 * idpf_set_per_q_coalesce - set ITR values as requested by user
 * @netdev: pointer to the netdev associated with this query
 * @q_num: queue for which the itr values has to be set
 * @ec: coalesce settings to program the device with
 *
 * Return 0 on success, and negative on failure
 */
static int idpf_set_per_q_coalesce(struct net_device *netdev, u32 q_num,
				   struct ethtool_coalesce *ec)
{
	struct idpf_vport *vport;
	int err;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	err = idpf_set_q_coalesce(vport, ec, q_num, false);
	if (err) {
		idpf_vport_ctrl_unlock(netdev);

		return err;
	}

	err = idpf_set_q_coalesce(vport, ec, q_num, true);

	idpf_vport_ctrl_unlock(netdev);

	return err;
}

/**
 * idpf_get_msglevel - Get debug message level
 * @netdev: network interface device structure
 *
 * Returns current debug message level.
 */
static u32 idpf_get_msglevel(struct net_device *netdev)
{
	struct idpf_adapter *adapter = idpf_netdev_to_adapter(netdev);

	return adapter->msg_enable;
}

/**
 * idpf_set_msglevel - Set debug message level
 * @netdev: network interface device structure
 * @data: message level
 *
 * Set current debug message level. Higher values cause the driver to
 * be noisier.
 */
static void idpf_set_msglevel(struct net_device *netdev, u32 data)
{
	struct idpf_adapter *adapter = idpf_netdev_to_adapter(netdev);

	adapter->msg_enable = data;
}

/**
 * idpf_get_link_ksettings - Get Link Speed and Duplex settings
 * @netdev: network interface device structure
 * @cmd: ethtool command
 *
 * Reports speed/duplex settings.
 **/
static int idpf_get_link_ksettings(struct net_device *netdev,
				   struct ethtool_link_ksettings *cmd)
{
	struct idpf_vport *vport;

	idpf_vport_ctrl_lock(netdev);
	vport = idpf_netdev_to_vport(netdev);

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	cmd->base.autoneg = AUTONEG_DISABLE;
	cmd->base.port = PORT_NONE;
	if (vport->link_up) {
		cmd->base.duplex = DUPLEX_FULL;
		cmd->base.speed = vport->link_speed_mbps;
	} else {
		cmd->base.duplex = DUPLEX_UNKNOWN;
		cmd->base.speed = SPEED_UNKNOWN;
	}

	idpf_vport_ctrl_unlock(netdev);

	return 0;
}

static const struct ethtool_ops idpf_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS |
				     ETHTOOL_COALESCE_USE_ADAPTIVE,
	.supported_ring_params	= ETHTOOL_RING_USE_TCP_DATA_SPLIT,
	.get_msglevel		= idpf_get_msglevel,
	.set_msglevel		= idpf_set_msglevel,
	.get_link		= ethtool_op_get_link,
	.get_coalesce		= idpf_get_coalesce,
	.set_coalesce		= idpf_set_coalesce,
	.get_per_queue_coalesce = idpf_get_per_q_coalesce,
	.set_per_queue_coalesce = idpf_set_per_q_coalesce,
	.get_ethtool_stats	= idpf_get_ethtool_stats,
	.get_strings		= idpf_get_strings,
	.get_sset_count		= idpf_get_sset_count,
	.get_channels		= idpf_get_channels,
	.get_rxnfc		= idpf_get_rxnfc,
	.get_rxfh_key_size	= idpf_get_rxfh_key_size,
	.get_rxfh_indir_size	= idpf_get_rxfh_indir_size,
	.get_rxfh		= idpf_get_rxfh,
	.set_rxfh		= idpf_set_rxfh,
	.set_channels		= idpf_set_channels,
	.get_ringparam		= idpf_get_ringparam,
	.set_ringparam		= idpf_set_ringparam,
	.get_link_ksettings	= idpf_get_link_ksettings,
};

/**
 * idpf_set_ethtool_ops - Initialize ethtool ops struct
 * @netdev: network interface device structure
 *
 * Sets ethtool ops struct in our netdev so that ethtool can call
 * our functions.
 */
void idpf_set_ethtool_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &idpf_ethtool_ops;
}
