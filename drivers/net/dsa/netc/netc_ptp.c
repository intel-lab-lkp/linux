// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/*
 * NXP NETC switch driver
 * Copyright 2025-2026 NXP
 */

#include <linux/ptp_classify.h>
#include <linux/ptp_clock_kernel.h>

#include "netc_switch.h"

#define NETC_TS_REQ_ID_NUM		(NETC_TAG_TS_REQ_ID + 1)
#define NETC_PTP_TX_TSTAMP_TIMEOUT	(5 * HZ)

static int netc_get_phc_index(struct netc_switch *priv)
{
	if (!priv->tmr_dev)
		return -ENODEV;

	return ptp_clock_index_by_dev(&priv->tmr_dev->dev);
}

int netc_get_ts_info(struct dsa_switch *ds, int port,
		     struct kernel_ethtool_ts_info *info)
{
	struct netc_switch *priv = ds->priv;

	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				SOF_TIMESTAMPING_RX_SOFTWARE |
				SOF_TIMESTAMPING_SOFTWARE;

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
			ntmp_ipft_delete_entry(&priv->ntmp, entry_id);
			np->ptp_ipft_eid[i] = NTMP_NULL_ENTRY_ID;
		}
	}
}

static int netc_build_ptp_ipft_keye(struct ipft_keye_data *keye, int port,
				    enum netc_ptp_type type)
{
	u16 src_port, frm_attr_flags;

	keye->precedence = cpu_to_le16(0xf000);
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
		keye->frm_attr_flags_mask = keye->frm_attr_flags;
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

int netc_port_hwtstamp_set(struct dsa_switch *ds, int port,
			   struct kernel_hwtstamp_config *config,
			   struct netlink_ext_ack *extack)
{
	struct netc_port *np = NETC_PORT(ds, port);
	struct netc_switch *priv = ds->priv;
	int rx_filter, err;

	switch (config->tx_type) {
	case HWTSTAMP_TX_ON:
	case HWTSTAMP_TX_OFF:
		np->ptp_tx_type = config->tx_type;
		break;
	case HWTSTAMP_TX_ONESTEP_SYNC:
		if (!priv->tmr_dev)
			return -ERANGE;

		np->ptp_tx_type = config->tx_type;
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

	config->rx_filter = rx_filter;

	return 0;
}

int netc_port_hwtstamp_get(struct dsa_switch *ds, int port,
			   struct kernel_hwtstamp_config *config)
{
	struct netc_port *np = NETC_PORT(ds, port);

	config->tx_type = np->ptp_tx_type;
	config->rx_filter = np->ptp_rx_filter;

	return 0;
}

static int netc_port_txtstamp_twostep(struct netc_port *np,
				      struct sk_buff *clone)
{
	DECLARE_BITMAP(ts_req_id_bitmap, NETC_TS_REQ_ID_NUM);
	struct netc_switch *priv = np->switch_priv;
	struct sk_buff_head free_list;
	struct sk_buff *skb, *skb_tmp;
	unsigned long ts_req_id;
	int err = 0;

	bitmap_zero(ts_req_id_bitmap, NETC_TS_REQ_ID_NUM);
	__skb_queue_head_init(&free_list);
	spin_lock_bh(&np->ptp_lock);

	skb_queue_walk_safe(&np->skb_txtstamp_queue, skb, skb_tmp) {
		if (time_before(NETC_SKB_CB(skb)->ptp_tx_time +
				NETC_PTP_TX_TSTAMP_TIMEOUT, jiffies)) {
			dev_dbg_ratelimited(priv->dev,
					    "Port %d ts_req_id %u which seems lost\n",
					    np->dp->index, NETC_SKB_CB(skb)->ts_req_id);

			__skb_unlink(skb, &np->skb_txtstamp_queue);
			__skb_queue_tail(&free_list, skb);
		} else {
			__set_bit(NETC_SKB_CB(skb)->ts_req_id, ts_req_id_bitmap);
		}
	}

	ts_req_id = find_first_zero_bit(ts_req_id_bitmap, NETC_TS_REQ_ID_NUM);
	if (ts_req_id == NETC_TS_REQ_ID_NUM) {
		err = -EBUSY;
		goto unlock_ptp;
	}

	NETC_SKB_CB(clone)->ts_req_id = ts_req_id;
	NETC_SKB_CB(clone)->ptp_tx_time = jiffies;
	skb_shinfo(clone)->tx_flags |= SKBTX_IN_PROGRESS;
	__skb_queue_tail(&np->skb_txtstamp_queue, clone);

unlock_ptp:
	spin_unlock_bh(&np->ptp_lock);

	/* Free timed-out SKBs outside the spinlock to avoid calling
	 * kfree_skb() with a destructor (sock_efree) under a spinlock.
	 */
	__skb_queue_purge(&free_list);

	return err;
}

void netc_twostep_tstamp_handler(struct dsa_switch *ds, int port,
				 u8 ts_req_id, u64 ts)
{
	struct sk_buff *skb, *skb_tmp, *skb_match = NULL;
	struct netc_port *np = NETC_PORT(ds, port);
	struct skb_shared_hwtstamps hwtstamps;
	struct netc_switch *priv = ds->priv;

	spin_lock_bh(&np->ptp_lock);
	skb_queue_walk_safe(&np->skb_txtstamp_queue, skb, skb_tmp) {
		if (NETC_SKB_CB(skb)->ts_req_id != ts_req_id)
			continue;

		__skb_unlink(skb, &np->skb_txtstamp_queue);
		skb_match = skb;
		break;
	}
	spin_unlock_bh(&np->ptp_lock);

	if (!skb_match) {
		dev_dbg_ratelimited(priv->dev,
				    "Port %d received an expired Tx timestamp response (ts_req_id %u)",
				    port, ts_req_id);
		return;
	}

	hwtstamps.hwtstamp = ns_to_ktime(ts);
	skb_complete_tx_timestamp(skb_match, &hwtstamps);
}

bool netc_port_rxtstamp(struct dsa_switch *ds, int port,
			struct sk_buff *skb, unsigned int type)
{
	struct skb_shared_hwtstamps *hwtstamps = skb_hwtstamps(skb);
	u64 ts = NETC_SKB_CB(skb)->tstamp;

	hwtstamps->hwtstamp = ns_to_ktime(ts);

	return false;
}

static void netc_port_prepare_onestep_sync(struct netc_port *np,
					   struct sk_buff *skb,
					   u32 ptp_class, bool *twostep)
{
	struct netc_switch *priv = np->switch_priv;
	u16 correction_offset, timestamp_offset;
	struct ptp_header *ptp_hdr;
	u8 msg_type, twostep_flag;
	bool is_udp = false;
	u32 pkt_type;
	u8 *pkt_hdr;

	ptp_hdr = ptp_parse_header(skb, ptp_class);
	if (!ptp_hdr) {
		dev_dbg_ratelimited(priv->dev,
				    "Port %d failed to parse Sync header\n",
				    np->dp->index);
		return;
	}

	pkt_hdr = skb_mac_header(skb);
	correction_offset = (u8 *)&ptp_hdr->correction - pkt_hdr;
	timestamp_offset = (u8 *)ptp_hdr + sizeof(*ptp_hdr) - pkt_hdr;

	/* Ensure that the entire originTimestamp field is present in the
	 * linear buffer of the skb.
	 */
	if (pkt_hdr + timestamp_offset + 10 > skb->data + skb_headlen(skb)) {
		dev_dbg_ratelimited(priv->dev,
				    "Port %d Sync header not in linear area\n",
				    np->dp->index);
		return;
	}

	msg_type = ptp_get_msgtype(ptp_hdr, ptp_class);
	twostep_flag = ptp_hdr->flag_field[0] & 0x2;

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
	 * is later used to pass the netc_port pointer to
	 * netc_onestep_skb_destructor() for TX completion notification.
	 * MSG_ZEROCOPY also uses destructor_arg (via skb_zcopy_init()) to
	 * track user-space page references. Overwriting it in that case would
	 * leak the ubuf_info reference and prevent user pages from being
	 * released. PTP applications do not use MSG_ZEROCOPY, but guard
	 * against it defensively.
	 */
	if (skb_zcopy(skb)) {
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
	NETC_SKB_CB(skb)->timestamp_offset = timestamp_offset;
	NETC_SKB_CB(skb)->is_udp = is_udp;
	NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_ONESTEP;
}

void netc_port_txtstamp(struct dsa_switch *ds, int port, struct sk_buff *skb)
{
	struct netc_port *np = NETC_PORT(ds, port);
	bool twostep = false;
	u32 ptp_class;

	NETC_SKB_CB(skb)->ptp_flag = 0;
	ptp_class = ptp_classify_raw(skb);
	if (ptp_class == PTP_CLASS_NONE)
		return;

	if (np->ptp_tx_type == HWTSTAMP_TX_ONESTEP_SYNC)
		netc_port_prepare_onestep_sync(np, skb, ptp_class, &twostep);

	if (np->ptp_tx_type == HWTSTAMP_TX_ON || twostep) {
		struct sk_buff *clone = skb_clone_sk(skb);

		if (unlikely(!clone))
			return;

		if (netc_port_txtstamp_twostep(np, clone)) {
			kfree_skb(clone);
			return;
		}

		NETC_SKB_CB(skb)->clone = clone;
		NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_TWOSTEP;
	}
}

static void netc_port_set_onestep_control(struct netc_port *np, bool udp,
					  int offset)
{
	u32 val;

	val = PM_SINGLE_STEP_EN | FIELD_PREP(PM_SINGLE_STEP_OFFSET, offset);
	if (udp)
		val |= PM_SINGLE_STEP_CH;
	netc_mac_port_wr(np, NETC_PM_SINGLE_STEP(0), val);
}

static void netc_onestep_skb_destructor(struct sk_buff *skb)
{
	struct netc_port *np = skb_shinfo(skb)->destructor_arg;

	/* skb has been transmitted by hardware, schedule work to send
	 * the next queued one-step Sync packet.
	 */
	schedule_work(&np->onestep_work);
}

static void netc_port_program_onestep(struct netc_port *np,
				      struct sk_buff *skb,
				      u64 tstamp)
{
	u16 correction_offset = NETC_SKB_CB(skb)->correction_offset;
	u16 timestamp_offset = NETC_SKB_CB(skb)->timestamp_offset;
	bool is_udp = NETC_SKB_CB(skb)->is_udp;
	u8 *pkt_hdr = skb_mac_header(skb);
	u64 sec;
	u32 ns;

	NETC_SKB_CB(skb)->tstamp = tstamp;
	NETC_SKB_CB(skb)->ptp_flag = NETC_PTP_FLAG_ONESTEP;

	/* Update originTimestamp field of Sync packet
	 * - 48 bits seconds field
	 * - 32 bits nanoseconds field
	 */
	sec = div_u64_rem(tstamp, NSEC_PER_SEC, &ns);
	put_unaligned_be16((sec >> 32) & 0xffff, pkt_hdr + timestamp_offset);
	put_unaligned_be32(sec & 0xffffffff, pkt_hdr + timestamp_offset + 2);
	put_unaligned_be32(ns, pkt_hdr + timestamp_offset + 6);

	netc_port_set_onestep_control(np, is_udp, correction_offset);

	/* Orphan the skb to release the socket send buffer quota immediately.
	 * This is safe because sock_wfree() only updates sk_wmem_alloc and
	 * does not touch skb->data. After skb_orphan(), we install our own
	 * destructor so that when the conduit driver frees the skb after TX
	 * completion, we get notified to send the next queued Sync packet.
	 */
	skb_orphan(skb);
	skb_shinfo(skb)->destructor_arg = np;
	skb->destructor = netc_onestep_skb_destructor;
}

void netc_port_onestep_work(struct work_struct *work)
{
	struct netc_port *np = container_of(work, struct netc_port,
					    onestep_work);
	struct netc_switch *priv = np->switch_priv;
	struct netc_tagger_data *tagger_data;
	struct sk_buff *skb;
	u64 tstamp;

	/* Dequeue the next pending skb while still holding the in-flight slot,
	 * so a newly arriving one-step Sync cannot jump ahead of it. Only
	 * release the slot when the queue is empty. This keeps ordering and
	 * closes the enqueue/wakeup race.
	 */
	spin_lock_bh(&np->ptp_lock);
	skb = __skb_dequeue(&np->skb_onestep_queue);
	if (!skb) {
		__clear_bit(NETC_FLAG_ONESTEP_IN_PROGRESS, &np->flags);
		spin_unlock_bh(&np->ptp_lock);
		return;
	}
	spin_unlock_bh(&np->ptp_lock);

	tstamp = netc_timer_get_current_time(priv->tmr_dev);
	if (!tstamp) {
		/* The PTP timer is not available, so there is no correct
		 * timestamp to program. Drop this frame and re-kick to
		 * process the remaining queued frames (or release the slot).
		 *
		 * netc_port_program_onestep() has not run for this skb yet, so
		 * netc_onestep_skb_destructor() is not installed on it. Freeing
		 * it therefore does not reschedule onestep_work, so the work
		 * must be rescheduled explicitly to keep draining the queue.
		 */
		dev_dbg_ratelimited(priv->dev,
				    "Port %d PTP timer unavailable, drop Sync\n",
				    np->dp->index);
		kfree_skb(skb);
		schedule_work(&np->onestep_work);
		return;
	}

	/* Reuse the offsets cached at enqueue time; only the timestamp is
	 * read fresh so it reflects the actual TX moment.
	 */
	netc_port_program_onestep(np, skb, tstamp);

	/* Tag and hand the frame directly to the conduit via the tagger,
	 * bypassing dsa_user_xmit() so the TX stats are not counted twice.
	 */
	tagger_data = priv->ds->tagger_data;
	tagger_data->onestep_sync_xmit(skb, np->dp->user);
}

struct sk_buff *netc_onestep_sync_handler(struct dsa_switch *ds, int port,
					  struct sk_buff *skb)
{
	struct netc_port *np = NETC_PORT(ds, port);
	struct netc_switch *priv = ds->priv;
	u64 tstamp;

	/* Serialize one-step Sync packets: only one can be in-flight at a
	 * time because the SINGLE_STEP register is shared and must match the
	 * packet currently being transmitted. Claim the in-flight slot under
	 * ptp_lock. If another one-step Sync is already in-flight, queue this
	 * skb and return NULL; ownership is transferred to the queue, so no
	 * extra reference is needed and netc_xmit() stops processing it.
	 */
	spin_lock_bh(&np->ptp_lock);
	if (test_bit(NETC_FLAG_ONESTEP_IN_PROGRESS, &np->flags)) {
		__skb_queue_tail(&np->skb_onestep_queue, skb);
		spin_unlock_bh(&np->ptp_lock);

		return NULL;
	}

	tstamp = netc_timer_get_current_time(priv->tmr_dev);
	if (!tstamp) {
		spin_unlock_bh(&np->ptp_lock);

		/* This is a valid one-step Sync frame, but the PTP timer is
		 * not available, so there is no correct timestamp to program.
		 * Drop the frame rather than transmit a Sync with a bogus
		 * correction field.
		 */
		dev_dbg_ratelimited(priv->dev,
				    "Port %d PTP timer unavailable, drop Sync\n",
				    np->dp->index);
		kfree_skb(skb);

		return NULL;
	}

	__set_bit(NETC_FLAG_ONESTEP_IN_PROGRESS, &np->flags);
	spin_unlock_bh(&np->ptp_lock);

	/* We own the in-flight slot. Program the register and install the
	 * destructor.
	 */
	netc_port_program_onestep(np, skb, tstamp);

	return skb;
}
