// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/*
 * NXP NETC switch driver
 * Copyright 2025-2026 NXP
 */

#include <linux/kref.h>
#include <linux/ptp_classify.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/slab.h>

#include "netc_switch.h"

#define NETC_NUM_TS_REQ_ID		16
#define NETC_TXTSTAMP_TIMEOUT		(5 * HZ)
#define NETC_MAX_STEP_OFFSET		0x1ff

static void netc_port_set_onestep_control(struct netc_port *np,
					  bool csum_update, int offset)
{
	u32 val;

	val = PM_SINGLE_STEP_EN | FIELD_PREP(PM_SINGLE_STEP_OFFSET, offset);
	if (csum_update)
		val |= PM_SINGLE_STEP_CH;
	netc_mac_port_wr(np, NETC_PM_SINGLE_STEP(0), val);
}

static void netc_onestep_destroy_work(struct work_struct *work)
{
	struct netc_onestep *onestep = container_of(work, struct netc_onestep,
						    destroy_work);

	/* refcnt reaching zero does not by itself mean onestep->work has
	 * stopped: the last in-flight skb destructor calls schedule_work(&work)
	 * *before* the netc_onestep_put() that drops the final reference, so at
	 * the moment refcnt hits zero onestep->work may still be pending or
	 * running on another CPU. destroy_work and work are distinct work_structs
	 * and can run concurrently, so cancel_work_sync() is required to drain
	 * onestep->work before mutex_destroy()/kfree() below, otherwise a
	 * still-running work would touch freed memory. No new schedule_work(&work)
	 * can occur after this point because no references remain, so this
	 * cancel is final.
	 */
	cancel_work_sync(&onestep->work);
	mutex_destroy(&onestep->work_lock);
	kfree(onestep);
}

static void netc_onestep_release(struct kref *ref)
{
	struct netc_onestep *onestep = container_of(ref, struct netc_onestep,
						    refcnt);

	/* This may be called from the skb destructor in softirq context
	 * (napi_consume_skb()), where cancel_work_sync() must not be used.
	 * Defer the final teardown to process context.
	 */
	schedule_work(&onestep->destroy_work);
}

static void netc_onestep_get(struct netc_onestep *onestep)
{
	kref_get(&onestep->refcnt);
}

void netc_onestep_put(struct netc_onestep *onestep)
{
	kref_put(&onestep->refcnt, netc_onestep_release);
}

static void netc_onestep_skb_destructor(struct sk_buff *skb)
{
	struct netc_onestep *onestep = skb_shinfo(skb)->destructor_arg;

	/* skb has been transmitted by hardware. Schedule work to send the next
	 * queued one-step Sync packet, then release this skb's reference on the
	 * context. If the port has already been torn down and this is the last
	 * reference, the context is freed via netc_onestep_release().
	 */
	schedule_work(&onestep->work);
	netc_onestep_put(onestep);
}

static void netc_port_program_onestep(struct netc_port *np,
				      struct netc_onestep *onestep,
				      struct sk_buff *skb,
				      u64 tstamp)
{
	u16 correction_offset = NETC_SKB_CB(skb)->correction_offset;
	u16 tstamp_offset = NETC_SKB_CB(skb)->timestamp_offset;
	u8 *hdr = skb_mac_header(skb);
	bool csum_update = false;
	__be32 new_sec_l, new_ns;
	__be16 new_sec_h;
	u64 sec;
	u32 ns;

	NETC_SKB_CB(skb)->tstamp = tstamp;
	NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_ONESTEP;

	/* Update originTimestamp field of Sync packet
	 * - 48 bits seconds field
	 * - 32 bits nanoseconds field
	 */
	sec = div_u64_rem(tstamp, NSEC_PER_SEC, &ns);
	new_sec_h = htons((sec >> 32) & 0xffff);
	new_sec_l = htonl(sec & 0xffffffff);
	new_ns = htonl(ns);

	if (NETC_SKB_CB(skb)->is_udp) {
		__be32 old_sec_l, old_ns;
		struct udphdr *uh;
		__be16 old_sec_h;

		if (skb->ip_summed == CHECKSUM_PARTIAL) {
			csum_update = true;
			goto update_timestamp;
		}

		if (unlikely(!skb_transport_header_was_set(skb)))
			uh = (struct udphdr *)(hdr + tstamp_offset -
					       sizeof(struct ptp_header) -
					       sizeof(struct udphdr));
		else
			uh = udp_hdr(skb);

		/* For IPv4, a UDP checksum of zero on the wire means "no
		 * checksum". For IPv6, its UDP checksum is mandatory and
		 * never zero.
		 */
		if (!uh->check)
			goto update_timestamp;

		old_sec_h = __get_unaligned_t(__be16, hdr + tstamp_offset);
		old_sec_l = __get_unaligned_t(__be32, hdr + tstamp_offset + 2);
		old_ns = __get_unaligned_t(__be32, hdr + tstamp_offset + 6);
		inet_proto_csum_replace2(&uh->check, skb, old_sec_h,
					 new_sec_h, false);
		inet_proto_csum_replace4(&uh->check, skb, old_sec_l,
					 new_sec_l, false);
		inet_proto_csum_replace4(&uh->check, skb, old_ns,
					 new_ns, false);
		csum_update = true;
	}

update_timestamp:
	__put_unaligned_t(__be16, new_sec_h, hdr + tstamp_offset);
	__put_unaligned_t(__be32, new_sec_l, hdr + tstamp_offset + 2);
	__put_unaligned_t(__be32, new_ns, hdr + tstamp_offset + 6);

	netc_port_set_onestep_control(np, csum_update, correction_offset);

	/* Orphan the skb to release the socket send buffer quota immediately.
	 * This is safe because sock_wfree() does not access skb->data or any
	 * frame content. After skb_orphan(), we install our own destructor so
	 * that when the conduit driver frees the skb after TX completion, we
	 * get notified to send the next queued Sync packet.
	 */
	skb_orphan(skb);
	netc_onestep_get(onestep); /* in-flight reference */
	skb_shinfo(skb)->destructor_arg = onestep;
	skb->destructor = netc_onestep_skb_destructor;
}

static u64 netc_get_phc_time(struct netc_switch *priv)
{
	if (unlikely(!priv->tmr_dev))
		return 0;

	return netc_timer_get_current_time(priv->tmr_dev);
}

void netc_port_onestep_work(struct work_struct *work)
{
	struct netc_onestep *onestep = container_of(work, struct netc_onestep,
						    work);
	struct netc_tagger_data *tagger_data;
	struct netc_switch *priv;
	struct netc_port *np;
	struct sk_buff *skb;
	u64 tstamp;

	/* Serialize the whole hardware access against port disable. work_lock
	 * is a mutex (this runs in process context and netc_get_phc_time() may
	 * sleep). If the port has been disabled, bail out immediately; np and
	 * priv are only dereferenced after the @active check passes, so they
	 * are always valid here.
	 */
	mutex_lock(&onestep->work_lock);
	if (unlikely(!onestep->active)) {
		netc_port_purge_onestep_queue(onestep, true);
		goto unlock_work;
	}

	/* Send only one queued Sync per run. The shared SINGLE_STEP register
	 * must match the frame currently being transmitted, so the next frame
	 * is programmed only after this one completes TX, when its skb
	 * destructor reschedules this work. Dequeue under onestep->queue_lock,
	 * and if the queue has drained, release the in-flight slot so a later
	 * frame from the xmit path kicks the work again.
	 */
	spin_lock_bh(&onestep->queue_lock);
	skb = __skb_dequeue(&onestep->queue);
	if (!skb) {
		onestep->in_flight = false;
		spin_unlock_bh(&onestep->queue_lock);
		goto unlock_work;
	}
	spin_unlock_bh(&onestep->queue_lock);

	np = onestep->np;
	priv = np->switch_priv;
	tstamp = netc_get_phc_time(priv);
	if (unlikely(!tstamp)) {
		/* The PTP timer is not available, so there is no correct
		 * timestamp to program. Drop this frame and re-kick to process
		 * the remaining queued frames.
		 *
		 * netc_port_program_onestep() has not run for this skb yet, so
		 * netc_onestep_skb_destructor() is not installed on it. Freeing
		 * it therefore does not reschedule the work, so the work must be
		 * rescheduled explicitly to keep draining the queue.
		 */
		dev_dbg_ratelimited(priv->dev,
				    "Port %d PTP timer unavailable, drop Sync\n",
				    np->dp->index);
		kfree_skb(skb);
		schedule_work(&onestep->work);
		goto unlock_work;
	}

	/* Reuse the offsets cached at enqueue time; only the timestamp is
	 * read fresh so it reflects the actual TX moment.
	 */
	netc_port_program_onestep(np, onestep, skb, tstamp);

	/* Tag and hand the frame directly to the conduit via the tagger,
	 * bypassing dsa_user_xmit() so the TX stats are not counted twice.
	 * And there is no need to check if tagger_data is NULL, because
	 * dsa_tree_teardown_ports() executes before
	 * dsa_switch_teardown_tag_protocol(), so tagger_data cannot be
	 * NULL when onestep->active is set.
	 */
	tagger_data = priv->ds->tagger_data;
	tagger_data->onestep_sync_xmit(skb, np->dp->user);

unlock_work:
	mutex_unlock(&onestep->work_lock);
}

static int netc_port_onestep_alloc(struct netc_port *np)
{
	struct netc_onestep *onestep;

	onestep = kzalloc_obj(*onestep);
	if (!onestep)
		return -ENOMEM;

	kref_init(&onestep->refcnt); /* port (owner) reference */
	np->onestep = onestep;
	onestep->np = np;
	mutex_init(&onestep->work_lock);
	spin_lock_init(&onestep->queue_lock);
	__skb_queue_head_init(&onestep->queue);
	INIT_WORK(&onestep->work, netc_port_onestep_work);
	INIT_WORK(&onestep->destroy_work, netc_onestep_destroy_work);

	return 0;
}

int netc_port_ptp_init(struct netc_port *np)
{
	/* Initialize to invalid entry IDs */
	for (int i = 0; i < NETC_PTP_MAX; i++)
		np->ptp_ipft_eid[i] = NTMP_NULL_ENTRY_ID;

	spin_lock_init(&np->tstamp_lock);
	__skb_queue_head_init(&np->skb_txtstamp_queue);

	return netc_port_onestep_alloc(np);
}

static int netc_get_phc_index(struct netc_switch *priv)
{
	if (!priv->tmr_dev)
		return -1;

	return ptp_clock_index_by_dev(&priv->tmr_dev->dev);
}

int netc_get_ts_info(struct dsa_switch *ds, int port,
		     struct kernel_ethtool_ts_info *info)
{
	struct netc_switch *priv = ds->priv;

	info->phc_index = netc_get_phc_index(priv);
	if (info->phc_index < 0)
		return 0;

	info->so_timestamping |= SOF_TIMESTAMPING_TX_HARDWARE |
				 SOF_TIMESTAMPING_RX_HARDWARE |
				 SOF_TIMESTAMPING_RAW_HARDWARE;

	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON) |
			 BIT(HWTSTAMP_TX_ONESTEP_SYNC);

	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			   BIT(HWTSTAMP_FILTER_PTP_V2_EVENT) |
			   BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
			   BIT(HWTSTAMP_FILTER_PTP_V2_L4_EVENT);

	return 0;
}

static void netc_port_del_ptp_filter(struct netc_port *np)
{
	struct netc_switch *priv = np->switch_priv;
	u32 entry_id;
	int i;

	for (i = 0; i < NETC_PTP_MAX; i++) {
		entry_id = np->ptp_ipft_eid[i];
		if (entry_id != NTMP_NULL_ENTRY_ID) {
			/* There is no need to check the return value of
			 * ntmp_ipft_delete_entry(); in general, deleted
			 * existing entries will be successful.
			 */
			ntmp_ipft_delete_entry(&priv->ntmp, entry_id);
			np->ptp_ipft_eid[i] = NTMP_NULL_ENTRY_ID;
		}
	}
}

static int netc_build_ptp_ipft_keye(struct ipft_keye_data *keye, int port,
				    enum netc_ptp_type type)
{
	u16 src_port, frm_attr_flags;

	keye->precedence = cpu_to_le16(NETC_IPFT_PTP_PRECEDENCE);
	src_port = FIELD_PREP(IPFT_SRC_PORT, port);
	src_port |= IPFT_SRC_PORT_MASK;
	keye->src_port = cpu_to_le16(src_port);

	switch (type) {
	case NETC_PTP_L2:
		keye->ethertype = htons(ETH_P_1588);
		keye->ethertype_mask = htons(0xffff);
		break;
	case NETC_PTP_L4_IPV4_EVENT:
	case NETC_PTP_L4_IPV4_GENERAL:
	case NETC_PTP_L4_IPV6_EVENT:
	case NETC_PTP_L4_IPV6_GENERAL:
		frm_attr_flags = IPFT_FAF_IP_HDR | FIELD_PREP(IPFT_FAF_L4_CODE,
				 IPFT_FAF_UDP_HDR);
		if (type == NETC_PTP_L4_IPV6_EVENT ||
		    type == NETC_PTP_L4_IPV6_GENERAL)
			frm_attr_flags |= IPFT_FAF_IP_VER6;

		keye->frm_attr_flags = cpu_to_le16(frm_attr_flags);

		/* Set IP version bit in flags_mask to match IPv4 or IPv6
		 * packets
		 */
		frm_attr_flags |= IPFT_FAF_IP_VER6;
		keye->frm_attr_flags_mask = cpu_to_le16(frm_attr_flags);
		keye->ip_protocol = IPPROTO_UDP;
		keye->ip_protocol_mask = 0xff;

		if (type == NETC_PTP_L4_IPV4_EVENT ||
		    type == NETC_PTP_L4_IPV6_EVENT)
			keye->l4_dst_port = htons(PTP_EV_PORT);
		else
			keye->l4_dst_port = htons(PTP_GEN_PORT);

		keye->l4_dst_port_mask = htons(0xffff);
		break;
	default:
		return -ERANGE;
	}

	return 0;
}

static int netc_port_add_ipft_ptp_entry(struct netc_port *np,
					enum netc_ptp_type type)
{
	struct netc_switch *priv = np->switch_priv;
	struct ipft_entry_data *entry;
	struct ipft_keye_data *keye;
	u32 cfg;
	int err;

	entry = kzalloc_obj(*entry);
	if (!entry)
		return -ENOMEM;

	keye = &entry->keye;
	err = netc_build_ptp_ipft_keye(keye, np->dp->index, type);
	if (err)
		goto free_entry;

	cfg = FIELD_PREP(IPFT_FLTFA, IPFT_FLTFA_REDIRECT);
	cfg |= FIELD_PREP(IPFT_HR, NETC_HR_PTP_TRAP);
	cfg |= IPFT_TIMECAPE | IPFT_RRT;
	entry->cfge.cfg = cpu_to_le32(cfg);

	err = ntmp_ipft_add_entry(&priv->ntmp, entry);
	if (err)
		goto free_entry;

	np->ptp_ipft_eid[type] = entry->entry_id;

free_entry:
	kfree(entry);

	return err;
}

static int netc_port_add_l2_ptp_filter(struct netc_port *np)
{
	return netc_port_add_ipft_ptp_entry(np, NETC_PTP_L2);
}

static int netc_port_add_l4_ptp_filter(struct netc_port *np)
{
	int err;

	err = netc_port_add_ipft_ptp_entry(np, NETC_PTP_L4_IPV4_EVENT);
	if (err)
		return err;

	err = netc_port_add_ipft_ptp_entry(np, NETC_PTP_L4_IPV4_GENERAL);
	if (err)
		goto del_ptp_filter;

	err = netc_port_add_ipft_ptp_entry(np, NETC_PTP_L4_IPV6_EVENT);
	if (err)
		goto del_ptp_filter;

	err = netc_port_add_ipft_ptp_entry(np, NETC_PTP_L4_IPV6_GENERAL);
	if (err)
		goto del_ptp_filter;

	return 0;

del_ptp_filter:
	netc_port_del_ptp_filter(np);

	return err;
}

static int netc_port_add_l2_l4_ptp_filter(struct netc_port *np)
{
	int err;

	err = netc_port_add_l2_ptp_filter(np);
	if (err)
		return err;

	err = netc_port_add_l4_ptp_filter(np);
	if (err)
		goto del_ptp_filter;

	return 0;

del_ptp_filter:
	netc_port_del_ptp_filter(np);

	return err;
}

static int netc_port_set_ptp_filter(struct netc_port *np, int rx_filter)
{
	int err = 0;

	if (np->ptp_rx_filter == rx_filter)
		return 0;

	if (np->ptp_rx_filter != HWTSTAMP_FILTER_NONE ||
	    rx_filter == HWTSTAMP_FILTER_NONE) {
		netc_port_del_ptp_filter(np);
		np->ptp_rx_filter = HWTSTAMP_FILTER_NONE;
	}

	switch (rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
		err = netc_port_add_l2_ptp_filter(np);
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
		err = netc_port_add_l4_ptp_filter(np);
		break;
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
		err = netc_port_add_l2_l4_ptp_filter(np);
		break;
	default:
		err = -ERANGE;
	}

	if (err)
		return err;

	np->ptp_rx_filter = rx_filter;

	return 0;
}

void netc_port_purge_txtstamp_queue(struct netc_port *np)
{
	struct sk_buff_head free_list;

	__skb_queue_head_init(&free_list);

	spin_lock_bh(&np->tstamp_lock);
	skb_queue_splice_init(&np->skb_txtstamp_queue, &free_list);
	spin_unlock_bh(&np->tstamp_lock);

	__skb_queue_purge(&free_list);
}

void netc_port_purge_onestep_queue(struct netc_onestep *onestep,
				   bool clear_flight)
{
	struct sk_buff_head free_list;

	__skb_queue_head_init(&free_list);

	spin_lock_bh(&onestep->queue_lock);
	skb_queue_splice_init(&onestep->queue, &free_list);
	if (clear_flight)
		onestep->in_flight = false;
	spin_unlock_bh(&onestep->queue_lock);

	__skb_queue_purge(&free_list);
}

int netc_port_hwtstamp_set(struct dsa_switch *ds, int port,
			   struct kernel_hwtstamp_config *config,
			   struct netlink_ext_ack *extack)
{
	struct netc_port *np = NETC_PORT(ds, port);
	struct netc_switch *priv = ds->priv;
	int rx_filter, err;

	if ((config->tx_type != HWTSTAMP_TX_OFF ||
	     config->rx_filter != HWTSTAMP_FILTER_NONE) &&
	    !priv->tmr_dev)
		return -EOPNOTSUPP;

	switch (config->tx_type) {
	case HWTSTAMP_TX_ON:
	case HWTSTAMP_TX_OFF:
	case HWTSTAMP_TX_ONESTEP_SYNC:
		break;
	default:
		return -ERANGE;
	}

	switch (config->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		rx_filter = HWTSTAMP_FILTER_NONE;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
		rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
		break;
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		break;
	default:
		return -ERANGE;
	}

	err = netc_port_set_ptp_filter(np, rx_filter);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Failed to set PTP filter");
		return err;
	}

	WRITE_ONCE(np->ptp_tx_type, config->tx_type);
	if (config->tx_type == HWTSTAMP_TX_OFF)
		netc_port_purge_txtstamp_queue(np);

	if (config->tx_type != HWTSTAMP_TX_ONESTEP_SYNC)
		netc_port_purge_onestep_queue(np->onestep, false);

	config->rx_filter = rx_filter;

	return 0;
}

int netc_port_hwtstamp_get(struct dsa_switch *ds, int port,
			   struct kernel_hwtstamp_config *config)
{
	struct netc_port *np = NETC_PORT(ds, port);

	config->tx_type = READ_ONCE(np->ptp_tx_type);
	config->rx_filter = np->ptp_rx_filter;

	return 0;
}

static void netc_port_txtstamp_twostep(struct netc_port *np,
				       struct sk_buff *nskb)
{
	DECLARE_BITMAP(ts_req_id_bitmap, NETC_NUM_TS_REQ_ID);
	struct sk_buff *clone = skb_clone_sk(nskb);
	struct netc_switch *priv = np->switch_priv;
	struct sk_buff_head free_list;
	struct sk_buff *skb, *skb_tmp;
	unsigned long ts_req_id;
	int err = 0;

	if (unlikely(!clone))
		return;

	bitmap_zero(ts_req_id_bitmap, NETC_NUM_TS_REQ_ID);
	__skb_queue_head_init(&free_list);
	spin_lock_bh(&np->tstamp_lock);

	skb_queue_walk_safe(&np->skb_txtstamp_queue, skb, skb_tmp) {
		if (time_before(NETC_SKB_CB(skb)->ptp_tx_time +
				NETC_TXTSTAMP_TIMEOUT, jiffies)) {
			dev_dbg_ratelimited(priv->dev,
					    "Port %d ts_req_id %u which seems lost\n",
					    np->dp->index, NETC_SKB_CB(skb)->ts_req_id);

			__skb_unlink(skb, &np->skb_txtstamp_queue);
			__skb_queue_tail(&free_list, skb);
		} else {
			__set_bit(NETC_SKB_CB(skb)->ts_req_id, ts_req_id_bitmap);
		}
	}

	ts_req_id = find_first_zero_bit(ts_req_id_bitmap, NETC_NUM_TS_REQ_ID);
	if (ts_req_id == NETC_NUM_TS_REQ_ID) {
		dev_dbg_ratelimited(priv->dev,
				    "Port %d has no available ts_req_id\n",
				    np->dp->index);
		err = -EBUSY;
		goto unlock_tstamp;
	}

	NETC_SKB_CB(nskb)->ptp_flag = NETC_PTP_FLAG_TWOSTEP;
	NETC_SKB_CB(nskb)->ts_req_id = ts_req_id;
	NETC_SKB_CB(clone)->ts_req_id = ts_req_id;
	NETC_SKB_CB(clone)->ptp_tx_time = jiffies;
	skb_shinfo(clone)->tx_flags |= SKBTX_IN_PROGRESS;
	__skb_queue_tail(&np->skb_txtstamp_queue, clone);

unlock_tstamp:
	spin_unlock_bh(&np->tstamp_lock);

	if (err)
		kfree_skb(clone);

	/* Free timed-out SKBs outside the spinlock to avoid calling
	 * kfree_skb() with a destructor (sock_efree) under a spinlock.
	 */
	__skb_queue_purge(&free_list);
}

void netc_port_twostep_tstamp_handler(struct dsa_switch *ds, int port,
				      u8 ts_req_id, u64 ts)
{
	struct sk_buff *skb, *skb_tmp, *skb_match = NULL;
	struct netc_port *np = NETC_PORT(ds, port);
	struct skb_shared_hwtstamps hwtstamps;
	struct netc_switch *priv = ds->priv;

	spin_lock_bh(&np->tstamp_lock);
	skb_queue_walk_safe(&np->skb_txtstamp_queue, skb, skb_tmp) {
		if (NETC_SKB_CB(skb)->ts_req_id != ts_req_id)
			continue;

		__skb_unlink(skb, &np->skb_txtstamp_queue);
		skb_match = skb;
		break;
	}
	spin_unlock_bh(&np->tstamp_lock);

	if (!skb_match) {
		dev_dbg_ratelimited(priv->dev,
				    "Port %d ts_req_id %u which seems lost\n",
				    port, ts_req_id);
		return;
	}

	hwtstamps.hwtstamp = ns_to_ktime(ts);
	skb_complete_tx_timestamp(skb_match, &hwtstamps);
}

bool netc_port_rxtstamp(struct dsa_switch *ds, int port, struct sk_buff *skb,
			unsigned int type)
{
	struct skb_shared_hwtstamps *hwtstamps = skb_hwtstamps(skb);
	u64 ts = NETC_SKB_CB(skb)->tstamp;

	/* ts == 0 indicates the hardware did not capture the RX timestamp
	 * of the frame.
	 */
	if (!ts)
		return false;

	hwtstamps->hwtstamp = ns_to_ktime(ts);

	return false;
}

static void netc_port_prepare_onestep_sync(struct netc_port *np,
					   struct sk_buff *skb,
					   u32 ptp_class, bool *twostep)
{
	struct netc_switch *priv = np->switch_priv;
	u16 correction_offset, tstamp_offset;
	struct ptp_header *ptp_hdr;
	u8 msg_type, twostep_flag;
	bool is_udp = false;
	u32 pkt_type;
	u8 *pkt_hdr;

	if (unlikely(skb_linearize(skb))) {
		NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_DROP;
		return;
	}

	ptp_hdr = ptp_parse_header(skb, ptp_class);
	if (unlikely(!ptp_hdr)) {
		NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_DROP;
		dev_dbg_ratelimited(priv->dev,
				    "Port %d failed to parse Sync header\n",
				    np->dp->index);
		return;
	}

	msg_type = ptp_get_msgtype(ptp_hdr, ptp_class);
	twostep_flag = ptp_hdr->flag_field[0] & 0x2;

	pkt_hdr = skb_mac_header(skb);
	correction_offset = (u8 *)&ptp_hdr->correction - pkt_hdr;
	tstamp_offset = (u8 *)ptp_hdr + sizeof(*ptp_hdr) - pkt_hdr;

	/* Ensure that the entire originTimestamp field is present in the
	 * linear buffer of the skb and the correction_offset must be within
	 * the hardware capability.
	 */
	if (unlikely(tstamp_offset + 10 > skb_headlen(skb) ||
		     correction_offset > NETC_MAX_STEP_OFFSET)) {
		NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_DROP;
		dev_dbg_ratelimited(priv->dev,
				    "Port %d PTP offset check error\n",
				    np->dp->index);
		return;
	}

	/* Only a Sync frame with the twoStepFlag cleared can use one-step
	 * timestamping. A frame that requests two-step (or is not a Sync)
	 * carries different on-wire fields, so this is a real classification;
	 * report it through *twostep so the caller falls back to the two-step
	 * path.
	 */
	if (msg_type != PTP_MSGTYPE_SYNC || twostep_flag != 0) {
		*twostep = true;
		return;
	}

	/* This is a genuine one-step Sync frame. skb_shinfo()->destructor_arg
	 * is later used to pass the np->onestep pointer to
	 * netc_onestep_skb_destructor() for TX completion notification.
	 * MSG_ZEROCOPY also uses destructor_arg (via skb_zcopy_init()) to
	 * track user-space page references. Overwriting it in that case would
	 * leak the ubuf_info reference and prevent user pages from being
	 * released. PTP applications do not use MSG_ZEROCOPY, but guard
	 * against it defensively.
	 */
	if (skb_zcopy(skb)) {
		NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_DROP;
		dev_dbg_ratelimited(priv->dev,
				    "Port %d one-step Sync not supported on zerocopy skb\n",
				    np->dp->index);
		return;
	}

	pkt_type = ptp_class & PTP_CLASS_PMASK;
	if (pkt_type == PTP_CLASS_IPV4 || pkt_type == PTP_CLASS_IPV6)
		is_udp = true;

	/* Cache the parsing results so the tagger xmit path and the deferred
	 * work do not need to re-parse the PTP header, and so that
	 * netc_port_program_onestep() can derive these parameters from the
	 * skb.
	 */
	NETC_SKB_CB(skb)->correction_offset = correction_offset;
	NETC_SKB_CB(skb)->timestamp_offset = tstamp_offset;
	NETC_SKB_CB(skb)->is_udp = is_udp;
	NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_ONESTEP;
}

void netc_port_txtstamp(struct dsa_switch *ds, int port, struct sk_buff *skb)
{
	struct netc_port *np = NETC_PORT(ds, port);
	int tx_type = READ_ONCE(np->ptp_tx_type);
	bool twostep = false;
	u32 ptp_class;

	NETC_SKB_CB(skb)->ptp_flag = 0;
	ptp_class = ptp_classify_raw(skb);
	if (ptp_class == PTP_CLASS_NONE)
		return;

	if (tx_type == HWTSTAMP_TX_ONESTEP_SYNC)
		netc_port_prepare_onestep_sync(np, skb, ptp_class, &twostep);

	if (tx_type == HWTSTAMP_TX_ON || twostep)
		netc_port_txtstamp_twostep(np, skb);
}

void netc_port_onestep_sync_enqueue(struct dsa_switch *ds, int port,
				    struct sk_buff *skb)
{
	struct netc_port *np = NETC_PORT(ds, port);
	struct netc_onestep *onestep = np->onestep;
	bool kick = false;

	/* This runs in the xmit path (softirq / BH-disabled), so it must not
	 * sleep: only queue the frame here and let netc_port_onestep_work()
	 * program the SINGLE_STEP register and transmit it from process
	 * context. The shared SINGLE_STEP register can describe only one frame
	 * at a time, so at most one one-step Sync may be in flight. Track that
	 * with @in_flight under onestep->queue_lock.
	 *
	 * Enqueue the frame and, only if no frame is currently in flight, claim
	 * the in-flight slot and kick the work. When a frame is already in
	 * flight, just queue: its skb destructor will kick the work to send the
	 * next one once it completes TX, so the frames are transmitted strictly
	 * one at a time in order.
	 *
	 * PTP Sync frames are periodic, low-rate control-plane frames and only
	 * reach this TX path when the local socket requested hardware TX
	 * timestamping on a one-step port, so the queue cannot be flooded and
	 * needs no depth cap.
	 */
	spin_lock_bh(&onestep->queue_lock);
	__skb_queue_tail(&onestep->queue, skb);
	if (!onestep->in_flight) {
		onestep->in_flight = true;
		kick = true;
	}
	spin_unlock_bh(&onestep->queue_lock);

	/* Ownership is transferred to the queue; netc_xmit() stops processing
	 * this skb. The work will program and transmit it.
	 */
	if (kick)
		schedule_work(&onestep->work);
}
