// SPDX-License-Identifier: GPL-2.0+
/*
 * Marvell PTP driver for 88E1510, 88E1512, 88E1514 and 88E1518 PHYs
 *
 * Ideas taken from 88E6xxx DSA and DP83640 drivers. This file
 * implements the packet timestamping support only (PTP).  TAI
 * support is separate.
 */
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/marvell_ptp.h>
#include <linux/netdevice.h>
#include <linux/ptp_classify.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/uaccess.h>

#define TX_TIMEOUT_MS	40
#define RX_TIMEOUT_MS	40

#define PTP_PORT_CONFIG_0			0
#define PTP_PORT_CONFIG_0_DISTSPECCHECK		BIT(11)
#define PTP_PORT_CONFIG_0_DISTSOVERWRITE	BIT(1)
#define PTP_PORT_CONFIG_0_DISPTP		BIT(0)
#define PTP_PORT_CONFIG_1			1
#define PTP_PORT_CONFIG_1_IPJUMP(x)		(((x) & 0x3f) << 8)
#define PTP_PORT_CONFIG_1_ETJUMP(x)		((x) & 0x1f)
#define PTP_PORT_CONFIG_2			2
#define PTP_PORT_CONFIG_2_DEPINTEN		BIT(1)
#define PTP_PORT_CONFIG_2_ARRINTEN		BIT(0)
#define PTP_ARR_STATUS0				8
#define PTP_ARR_STATUS1				12
#define PTP_DEP_STATUS				16

struct marvell_ptp_cb {
	unsigned long timeout;
	u16 seq;
};
#define MARVELL_PTP_CB(skb)	((struct marvell_ptp_cb *)(skb)->cb)

/* RX queue support */

/* Deliver a skb with its timestamp back to the networking core */
static void marvell_rxq_rx(struct sk_buff *skb, u64 ns)
{
	struct skb_shared_hwtstamps *shhwtstamps = skb_hwtstamps(skb);

	memset(shhwtstamps, 0, sizeof(*shhwtstamps));
	shhwtstamps->hwtstamp = ns_to_ktime(ns);
	netif_rx(skb);
}

/* Get a rx timestamp entry. Try the free list, and if that fails,
 * steal the oldest off the pending list.
 */
static struct marvell_rxts *marvell_rxq_get_rxts(struct marvell_rxq *rxq)
{
	if (!list_empty(&rxq->rx_free))
		return list_first_entry(&rxq->rx_free, struct marvell_rxts,
					node);

	return list_last_entry(&rxq->rx_pend, struct marvell_rxts, node);
}

static void marvell_rxq_init(struct marvell_rxq *rxq)
{
	int i;

	mutex_init(&rxq->rx_mutex);
	INIT_LIST_HEAD(&rxq->rx_free);
	INIT_LIST_HEAD(&rxq->rx_pend);
	skb_queue_head_init(&rxq->rx_queue);

	for (i = 0; i < ARRAY_SIZE(rxq->rx_ts); i++)
		list_add_tail(&rxq->rx_ts[i].node, &rxq->rx_free);
}

static void marvell_rxq_purge(struct marvell_rxq *rxq)
{
	skb_queue_purge(&rxq->rx_queue);
}

static void marvell_rxq_rx_ts(struct marvell_rxq *rxq, u16 seq, u64 ns)
{
	struct marvell_rxts *rxts;
	struct sk_buff *skb;
	bool found = false;

	mutex_lock(&rxq->rx_mutex);

	/* Search the rx queue for a matching skb */
	skb_queue_walk(&rxq->rx_queue, skb) {
		if (MARVELL_PTP_CB(skb)->seq == seq) {
			__skb_unlink(skb, &rxq->rx_queue);
			found = true;
			break;
		}
	}

	if (!found) {
		rxts = marvell_rxq_get_rxts(rxq);
		rxts->ns = ns;
		rxts->seq = seq;
		list_move(&rxts->node, &rxq->rx_pend);
	}

	mutex_unlock(&rxq->rx_mutex);

	if (found)
		marvell_rxq_rx(skb, ns);
}

static bool marvell_rxq_rxtstamp(struct marvell_rxq *rxq, struct sk_buff *skb,
				 u16 seq)
{
	struct marvell_rxts *rxts;
	bool found = false;
	u64 ns;

	mutex_lock(&rxq->rx_mutex);

	/* Search the pending receive timestamps for a matching seqid */
	list_for_each_entry(rxts, &rxq->rx_pend, node) {
		if (rxts->seq == seq) {
			found = true;
			ns = rxts->ns;
			/* Move this timestamp entry to the free list */
			list_move_tail(&rxts->node, &rxq->rx_free);
			break;
		}
	}

	if (!found) {
		/* Store the seqid and queue the skb. Do this under the lock
		 * to ensure we don't miss any timestamps appended to the
		 * rx_pend list.
		 */
		MARVELL_PTP_CB(skb)->seq = seq;
		MARVELL_PTP_CB(skb)->timeout = jiffies +
			msecs_to_jiffies(RX_TIMEOUT_MS);
		__skb_queue_tail(&rxq->rx_queue, skb);
	}

	mutex_unlock(&rxq->rx_mutex);

	if (found)
		/* We found the corresponding timestamp. If we can add the
		 * timestamp, do we need to go through the netif_rx_ni()
		 * path, or would it be more efficient to add the timestamp
		 * and return "false" from marvell_ptp_rxtstamp() instead?
		 */
		marvell_rxq_rx(skb, ns);

	return found;
}

static void marvell_rxq_expire(struct marvell_rxq *rxq,
			       struct sk_buff_head *list)
{
	struct sk_buff *skb;

	mutex_lock(&rxq->rx_mutex);
	while ((skb = skb_dequeue(&rxq->rx_queue)) != NULL) {
		if (!time_is_before_jiffies(MARVELL_PTP_CB(skb)->timeout)) {
			__skb_queue_head(&rxq->rx_queue, skb);
			break;
		}
		__skb_queue_tail(list, skb);
	}
	mutex_unlock(&rxq->rx_mutex);
}

/* Extract the sequence ID */
static u16 ptp_seqid(const struct ptp_header *ptp_hdr)
{
	const __be16 *seqp = &ptp_hdr->sequence_id;

	return be16_to_cpup(seqp);
}

static u8 ptp_msgid(const struct ptp_header *ptp_hdr)
{
	return ptp_hdr->tsmt & 15;
}

static void marvell_ptp_schedule(struct marvell_ptp *ptp)
{
	marvell_tai_schedule(ptp->tai, 0);
}

/* Check for a rx timestamp entry, try to find the corresponding skb and
 * deliver it, otherwise add the rx timestamp to the queue of pending
 * timestamps.
 */
static int marvell_ptp_rx_ts(struct marvell_ptp *ptp, int q)
{
	struct marvell_ts ts;
	u16 reg;
	int err;
	u64 ns;

	if (q)
		reg = PTP_ARR_STATUS1;
	else
		reg = PTP_ARR_STATUS0;

	err = ptp->ops->ptp_port_read_ts(ptp->dev, &ts, reg);
	if (err <= 0)
		return 0;

	if ((ts.stat & MV_STATUS_INTSTATUS_MASK) !=
	    MV_STATUS_INTSTATUS_NORMAL)
		dev_warn(ptp->dev,
			 "rx timestamp overrun (q=%u stat=0x%x seq=%u)\n",
			 q, ts.stat, ts.seq);

	ns = marvell_tai_cyc2time(ptp->tai, ts.time);

	marvell_rxq_rx_ts(&ptp->rxq[q], ts.seq, ns);

	return 1;
}

/* Check whether the packet is suitable for timestamping, and if so,
 * try to find a pending timestamp for it. If no timestamp is found,
 * queue the packet with a timeout.
 */
bool marvell_ptp_rxtstamp(struct marvell_ptp *ptp, struct sk_buff *skb,
			  int type)
{
	const struct ptp_header *ptp_hdr;
	u16 msgidvec, seq;
	u8 msgid;
	int q;

	if (ptp->rx_filter == HWTSTAMP_FILTER_NONE)
		return false;

	ptp_hdr = ptp_parse_header(skb, type);
	if (!ptp_hdr)
		return false;

	msgid = ptp_msgid(ptp_hdr);
	seq = ptp_seqid(ptp_hdr);

	/* Only check for timestamps for PTP packets whose message ID value
	 * is one that we are capturing timestamps for. This is part of the
	 * global configuration and is therefore fixed.
	 */
	msgidvec = BIT(msgid);
	if (msgidvec & ~MV_PTP_MSD_ID_TS_EN) {
		dev_dbg(ptp->dev, "not timestamping rx msgid %u seq %u\n",
			msgid, seq);
		return false;
	}

	/* Determine the queue which the timestamp for this message ID will
	 * appear. This is part of the global configuration and is therefore
	 * fixed.
	 */
	q = !!(msgidvec & MV_PTP_TS_ARR_PTR);

	if (!marvell_rxq_rxtstamp(&ptp->rxq[q], skb, seq))
		marvell_ptp_schedule(ptp);

	return true;
}
EXPORT_SYMBOL_GPL(marvell_ptp_rxtstamp);

/* Move any expired skbs on to our own list, and then hand the contents of
 * our list to netif_rx() - this avoids calling netif_rx() with our
 * mutex held.
 */
static void marvell_ptp_rx_expire(struct marvell_ptp *ptp)
{
	struct sk_buff_head list;
	struct sk_buff *skb;
	int i;

	__skb_queue_head_init(&list);

	for (i = 0; i < ARRAY_SIZE(ptp->rxq); i++)
		marvell_rxq_expire(&ptp->rxq[i], &list);

	while ((skb = __skb_dequeue(&list)) != NULL)
		netif_rx(skb);
}

/* Complete the transmit timestamping; this is called to read the transmit
 * timestamp from the PHY, and report back the transmitted timestamp.
 */
static int marvell_ptp_txtstamp_complete(struct marvell_ptp *ptp)
{
	struct skb_shared_hwtstamps shhwtstamps;
	struct sk_buff *skb = ptp->tx_skb;
	struct marvell_ts ts;
	int err;
	u64 ns;

	err = ptp->ops->ptp_port_read_ts(ptp->dev, &ts, PTP_DEP_STATUS);
	if (err < 0)
		goto fail;

	if (err == 0) {
		if (time_is_before_jiffies(MARVELL_PTP_CB(skb)->timeout)) {
			dev_warn(ptp->dev, "tx timestamp timeout\n");
			goto free;
		}
		return 0;
	}

	/* Check the status */
	if ((ts.stat & MV_STATUS_INTSTATUS_MASK) !=
	    MV_STATUS_INTSTATUS_NORMAL) {
		dev_warn(ptp->dev, "tx timestamp overrun (stat=0x%x seq=%u)\n",
			 ts.stat, ts.seq);
		goto free;
	}

	/* Reject if the sequence number doesn't match */
	if (ts.seq != MARVELL_PTP_CB(skb)->seq) {
		dev_warn(ptp->dev, "tx timestamp unexpected sequence id\n");
		goto free;
	}

	ptp->tx_skb = NULL;

	/* Set the timestamp */
	ns = marvell_tai_cyc2time(ptp->tai, ts.time);
	memset(&shhwtstamps, 0, sizeof(shhwtstamps));
	shhwtstamps.hwtstamp = ns_to_ktime(ns);
	skb_complete_tx_timestamp(skb, &shhwtstamps);
	return 1;

fail:
	dev_err_ratelimited(ptp->dev, "failed reading PTP: %pe\n",
			    ERR_PTR(err));
free:
	dev_kfree_skb_any(skb);
	ptp->tx_skb = NULL;
	return -1;
}

/* Check whether the skb will be timestamped on transmit; we only support
 * a single outstanding skb. Add it if the slot is available.
 */
static bool marvell_ptp_do_txtstamp(struct marvell_ptp *ptp,
				    struct sk_buff *skb, int type)
{
	const struct ptp_header *ptp_hdr;
	u8 msgid;

	if (ptp->tx_type != HWTSTAMP_TX_ON)
		return false;

	if (!(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP))
		return false;

	ptp_hdr = ptp_parse_header(skb, type);
	if (!ptp_hdr)
		return false;

	msgid = ptp_msgid(ptp_hdr);
	if (BIT(msgid) & ~MV_PTP_MSD_ID_TS_EN) {
		dev_dbg(ptp->dev, "not timestamping tx msgid %u seq %u\n",
			msgid, ptp_seqid(ptp_hdr));
		return false;
	}

	MARVELL_PTP_CB(skb)->seq = ptp_seqid(ptp_hdr);
	MARVELL_PTP_CB(skb)->timeout = jiffies +
		msecs_to_jiffies(TX_TIMEOUT_MS);

	if (cmpxchg(&ptp->tx_skb, NULL, skb) != NULL)
		return false;

	/* DP83640 marks the skb for hw timestamping. Since the MAC driver
	 * may call skb_tx_timestamp() but may not support timestamping
	 * itself, it may not set this flag. So, we need to do this here.
	 */
	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	marvell_ptp_schedule(ptp);

	return true;
}

void marvell_ptp_txtstamp(struct marvell_ptp *ptp, struct sk_buff *skb,
			  int type)
{
	if (!marvell_ptp_do_txtstamp(ptp, skb, type))
		kfree_skb(skb);
}
EXPORT_SYMBOL_GPL(marvell_ptp_txtstamp);

int marvell_ptp_hwtstamp(struct marvell_ptp *ptp,
			 struct kernel_hwtstamp_config *kcfg,
			 struct netlink_ext_ack *ack)
{
	u16 cfg0 = PTP_PORT_CONFIG_0_DISPTP;
	u16 cfg2 = 0;
	int err;

	if (kcfg->flags)
		return -EINVAL;

	switch (kcfg->tx_type) {
	case HWTSTAMP_TX_OFF:
		break;

	case HWTSTAMP_TX_ON:
		cfg0 = 0;
		cfg2 |= PTP_PORT_CONFIG_2_DEPINTEN;
		break;

	default:
		return -ERANGE;
	}

	switch (kcfg->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;

	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		/* We accept 802.1AS, IEEE 1588v1 and IEEE 1588v2. We could
		 * filter on 802.1AS using the transportSpecific field, but
		 * that affects the transmit path too.
		 */
		kcfg->rx_filter = HWTSTAMP_FILTER_SOME;
		cfg0 = 0;
		cfg2 |= PTP_PORT_CONFIG_2_ARRINTEN;
		break;

	default:
		return -ERANGE;
	}

	err = ptp->ops->ptp_port_modify(ptp->dev, PTP_PORT_CONFIG_0,
					PTP_PORT_CONFIG_0_DISPTP, cfg0);
	if (err)
		return err;

	err = ptp->ops->ptp_port_write(ptp->dev, PTP_PORT_CONFIG_2, cfg2);
	if (err)
		return err;

	ptp->tx_type = kcfg->tx_type;
	ptp->rx_filter = kcfg->rx_filter;

	return 0;
}
EXPORT_SYMBOL_GPL(marvell_ptp_hwtstamp);

int marvell_ptp_ts_info(struct marvell_ptp *ptp,
			struct kernel_ethtool_ts_info *ts_info)
{
	ts_info->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
				   SOF_TIMESTAMPING_RX_HARDWARE |
				   SOF_TIMESTAMPING_RAW_HARDWARE;
	ts_info->phc_index = marvell_tai_ptp_clock_index(ptp->tai);
	ts_info->tx_types = BIT(HWTSTAMP_TX_OFF) |
			    BIT(HWTSTAMP_TX_ON);
	ts_info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			      BIT(HWTSTAMP_FILTER_SOME);

	return 0;
}
EXPORT_SYMBOL_GPL(marvell_ptp_ts_info);

static int marvell_ptp_port_config(struct marvell_ptp *ptp)
{
	int err;

	/* Disable transport specific check (if the PTP common header)
	 * Disable timestamp overwriting (so we can read a stable entry.)
	 * Disable PTP
	 */
	err = ptp->ops->ptp_port_write(ptp->dev, PTP_PORT_CONFIG_0,
				       PTP_PORT_CONFIG_0_DISTSPECCHECK |
				       PTP_PORT_CONFIG_0_DISTSOVERWRITE |
				       PTP_PORT_CONFIG_0_DISPTP);
	if (err < 0)
		return err;

	/* Set ether-type jump to 12 (to ether protocol)
	 * Set IP jump to 2 (to skip over ether protocol)
	 * Does this mean it won't pick up on VLAN packets?
	 */
	err = ptp->ops->ptp_port_write(ptp->dev, PTP_PORT_CONFIG_1,
				       PTP_PORT_CONFIG_1_ETJUMP(12) |
				       PTP_PORT_CONFIG_1_IPJUMP(2));
	if (err < 0)
		return err;

	/* Disable all interrupts */
	ptp->ops->ptp_port_write(ptp->dev, PTP_PORT_CONFIG_2, 0);

	return 0;
}

static void marvell_ptp_port_disable(struct marvell_ptp *ptp)
{
	/* Disable PTP */
	ptp->ops->ptp_port_write(ptp->dev, PTP_PORT_CONFIG_0,
				 PTP_PORT_CONFIG_0_DISPTP);

	/* Disable interrupts */
	ptp->ops->ptp_port_write(ptp->dev, PTP_PORT_CONFIG_2, 0);
}

long marvell_ptp_aux_work(struct marvell_ptp *ptp)
{
	if (ptp->tx_skb)
		marvell_ptp_txtstamp_complete(ptp);

	marvell_ptp_rx_ts(ptp, 0);
	marvell_ptp_rx_ts(ptp, 1);
	marvell_ptp_rx_expire(ptp);

	if (ptp->tx_skb)
		return 0;
	else if (!skb_queue_empty(&ptp->rxq[0].rx_queue) ||
		 !skb_queue_empty(&ptp->rxq[1].rx_queue))
		return 1;

	return -1;
}
EXPORT_SYMBOL_GPL(marvell_ptp_aux_work);

irqreturn_t marvell_ptp_irq(struct marvell_ptp *ptp)
{
	irqreturn_t ret = IRQ_NONE;

	if (marvell_ptp_rx_ts(ptp, 0))
		ret = IRQ_HANDLED;

	if (marvell_ptp_rx_ts(ptp, 1))
		ret = IRQ_HANDLED;

	if (ptp->tx_skb && marvell_ptp_txtstamp_complete(ptp))
		ret = IRQ_HANDLED;

	return ret;
}
EXPORT_SYMBOL_GPL(marvell_ptp_irq);

int marvell_ptp_probe(struct marvell_ptp *ptp, struct device *dev,
		      struct marvell_tai *tai,
		      const struct marvell_ptp_ops *ops)
{
	int i;

	ptp->ops = ops;
	ptp->dev = dev;
	ptp->tai = tai;

	for (i = 0; i < ARRAY_SIZE(ptp->rxq); i++)
		marvell_rxq_init(&ptp->rxq[i]);

	/* Configure this PTP port */
	return marvell_ptp_port_config(ptp);
}
EXPORT_SYMBOL_GPL(marvell_ptp_probe);

void marvell_ptp_remove(struct marvell_ptp *ptp)
{
	int i;

	/* Free or dequeue all pending skbs */
	if (ptp->tx_skb)
		kfree_skb(ptp->tx_skb);

	for (i = 0; i < ARRAY_SIZE(ptp->rxq); i++)
		marvell_rxq_purge(&ptp->rxq[i]);

	/* Ensure that the port is disabled */
	marvell_ptp_port_disable(ptp);
}
EXPORT_SYMBOL_GPL(marvell_ptp_remove);

MODULE_AUTHOR("Russell King");
MODULE_DESCRIPTION("Marvell PTP library");
MODULE_LICENSE("GPL v2");
