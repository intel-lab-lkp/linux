// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2023, 2024 Pengutronix,
//               Marc Kleine-Budde <kernel@pengutronix.de>
//

#include <net/netdev_queues.h>

#include "rockchip_canfd.h"

static bool rkcanfd_tx_tail_is_eff(const struct rkcanfd_priv *priv)
{
	const struct canfd_frame *cfd;
	const struct sk_buff *skb;
	unsigned int tx_tail;

	lockdep_assert_held(&priv->tx_lock);

	if (!rkcanfd_get_tx_pending(priv))
		return false;

	tx_tail = rkcanfd_get_tx_tail(priv);
	skb = priv->can.echo_skb[tx_tail];
	if (!skb) {
		netdev_err(priv->ndev,
			   "%s: echo_skb[%u]=NULL tx_head=0x%08x tx_tail=0x%08x\n",
			   __func__, tx_tail,
			   priv->tx_head, priv->tx_tail);

		return false;
	}

	cfd = (struct canfd_frame *)skb->data;

	return cfd->can_id & CAN_EFF_FLAG;
}

unsigned int rkcanfd_get_effective_tx_free(const struct rkcanfd_priv *priv)
{
	lockdep_assert_held(&priv->tx_lock);

	/* The original echo can precede completion of a retry. */
	if (!priv->tx_retry_enabled || priv->tx_retry_pending)
		return 0;

	if (priv->devtype_data.quirks & RKCANFD_QUIRK_RK3568_ERRATUM_6 &&
	    rkcanfd_tx_tail_is_eff(priv))
		return 0;

	return rkcanfd_get_tx_free(priv);
}

static void rkcanfd_tx_retry_work(struct work_struct *work)
{
	struct rkcanfd_priv *priv = container_of(to_delayed_work(work),
						 struct rkcanfd_priv, tx_retry_work);
	const u32 tx_req = RKCANFD_REG_CMD_TX0_REQ | RKCANFD_REG_CMD_TX1_REQ;
	unsigned long flags;
	u32 reg_cmd;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (!priv->tx_retry_enabled || !priv->tx_retry_pending)
		goto out_unlock;

	reg_cmd = rkcanfd_read(priv, RKCANFD_REG_CMD);

	if (reg_cmd & tx_req) {
		schedule_delayed_work(&priv->tx_retry_work, 1);
		goto out_unlock;
	}

	priv->tx_retry_pending = false;

	if (rkcanfd_get_effective_tx_free(priv) >= RKCANFD_TX_START_THRESHOLD)
		netif_wake_queue(priv->ndev);

out_unlock:
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

void rkcanfd_tx_retry_init(struct rkcanfd_priv *priv)
{
	INIT_DELAYED_WORK(&priv->tx_retry_work, rkcanfd_tx_retry_work);
}

void rkcanfd_tx_retry_start(struct rkcanfd_priv *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_retry_enabled = netif_running(priv->ndev);
	priv->tx_retry_pending = false;
	spin_unlock_irqrestore(&priv->tx_lock, flags);
}

void rkcanfd_tx_retry_stop(struct rkcanfd_priv *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->tx_lock, flags);
	priv->tx_retry_enabled = false;
	priv->tx_retry_pending = false;
	netif_stop_queue(priv->ndev);
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	cancel_delayed_work(&priv->tx_retry_work);
}

void rkcanfd_tx_retry_stop_sync(struct rkcanfd_priv *priv)
{
	rkcanfd_tx_retry_stop(priv);
	cancel_delayed_work_sync(&priv->tx_retry_work);
}

static void rkcanfd_start_xmit_write_cmd(const struct rkcanfd_priv *priv,
					 const u32 reg_cmd)
{
	lockdep_assert_held(&priv->tx_lock);

	if (priv->devtype_data.quirks & RKCANFD_QUIRK_RK3568_ERRATUM_12)
		rkcanfd_write(priv, RKCANFD_REG_MODE, priv->reg_mode_default |
			      RKCANFD_REG_MODE_SPACE_RX_MODE);

	rkcanfd_write(priv, RKCANFD_REG_CMD, reg_cmd);

	if (priv->devtype_data.quirks & RKCANFD_QUIRK_RK3568_ERRATUM_12)
		rkcanfd_write(priv, RKCANFD_REG_MODE, priv->reg_mode_default);
}

void rkcanfd_xmit_retry(struct rkcanfd_priv *priv)
{
	const unsigned int tx_tail = rkcanfd_get_tx_tail(priv);
	const u32 reg_cmd = RKCANFD_REG_CMD_TX_REQ(tx_tail);

	lockdep_assert_held(&priv->tx_lock);

	if (!priv->tx_retry_enabled)
		return;

	priv->tx_retry_pending = true;
	netif_stop_queue(priv->ndev);

	rkcanfd_start_xmit_write_cmd(priv, reg_cmd);
	schedule_delayed_work(&priv->tx_retry_work, 1);
}

netdev_tx_t rkcanfd_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rkcanfd_priv *priv = netdev_priv(ndev);
	u32 reg_frameinfo, reg_id, reg_cmd;
	unsigned int tx_head, frame_len;
	const struct canfd_frame *cfd;
	unsigned long flags;
	int err;
	u8 i;

	if (can_dev_dropped_skb(ndev, skb))
		return NETDEV_TX_OK;

	spin_lock_irqsave(&priv->tx_lock, flags);
	if (unlikely(!priv->tx_enabled)) {
		netif_stop_queue(ndev);
		goto out_busy;
	}

	if (!netif_subqueue_maybe_stop(priv->ndev, 0,
				       rkcanfd_get_effective_tx_free(priv),
				       RKCANFD_TX_STOP_THRESHOLD,
				       RKCANFD_TX_START_THRESHOLD)) {
		if (net_ratelimit())
			netdev_info(priv->ndev,
				    "Stopping tx-queue (tx_head=0x%08x, tx_tail=0x%08x, tx_pending=%d)\n",
				    priv->tx_head, priv->tx_tail,
				    rkcanfd_get_tx_pending(priv));

		goto out_busy;
	}

	cfd = (struct canfd_frame *)skb->data;

	if (cfd->can_id & CAN_EFF_FLAG) {
		reg_frameinfo = RKCANFD_REG_FD_FRAMEINFO_FRAME_FORMAT;
		reg_id = FIELD_PREP(RKCANFD_REG_FD_ID_EFF, cfd->can_id);
	} else {
		reg_frameinfo = 0;
		reg_id = FIELD_PREP(RKCANFD_REG_FD_ID_SFF, cfd->can_id);
	}

	if (cfd->can_id & CAN_RTR_FLAG)
		reg_frameinfo |= RKCANFD_REG_FD_FRAMEINFO_RTR;

	if (can_is_canfd_skb(skb)) {
		reg_frameinfo |= RKCANFD_REG_FD_FRAMEINFO_FDF;

		if (cfd->flags & CANFD_BRS)
			reg_frameinfo |= RKCANFD_REG_FD_FRAMEINFO_BRS;

		reg_frameinfo |= FIELD_PREP(RKCANFD_REG_FD_FRAMEINFO_DATA_LENGTH,
					    can_fd_len2dlc(cfd->len));
	} else {
		reg_frameinfo |= FIELD_PREP(RKCANFD_REG_FD_FRAMEINFO_DATA_LENGTH,
					    cfd->len);
	}

	tx_head = rkcanfd_get_tx_head(priv);
	reg_cmd = RKCANFD_REG_CMD_TX_REQ(tx_head);

	/* Keep the original skb alive until IRQs are restored so echo
	 * preparation cannot invoke its socket destructor under tx_lock.
	 */
	frame_len = can_skb_get_frame_len(skb);
	err = can_put_echo_skb(skb_get(skb), ndev, tx_head, frame_len);
	if (unlikely(err || !priv->can.echo_skb[tx_head]))
		goto out_drop;

	rkcanfd_write(priv, RKCANFD_REG_FD_TXFRAMEINFO, reg_frameinfo);
	rkcanfd_write(priv, RKCANFD_REG_FD_TXID, reg_id);
	for (i = 0; i < cfd->len; i += 4)
		rkcanfd_write(priv, RKCANFD_REG_FD_TXDATA0 + i,
			      *(u32 *)(cfd->data + i));

	netdev_sent_queue(priv->ndev, frame_len);

	WRITE_ONCE(priv->tx_head, priv->tx_head + 1);

	rkcanfd_start_xmit_write_cmd(priv, reg_cmd);

	netif_subqueue_maybe_stop(priv->ndev, 0,
				  rkcanfd_get_effective_tx_free(priv),
				  RKCANFD_TX_STOP_THRESHOLD,
				  RKCANFD_TX_START_THRESHOLD);

	spin_unlock_irqrestore(&priv->tx_lock, flags);
	dev_consume_skb_any(skb);

	return NETDEV_TX_OK;

out_drop:
	ndev->stats.tx_dropped++;
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;

out_busy:
	spin_unlock_irqrestore(&priv->tx_lock, flags);

	return NETDEV_TX_BUSY;
}

void rkcanfd_handle_tx_done_one(struct rkcanfd_priv *priv, const u32 ts,
				unsigned int *frame_len_p)
{
	struct net_device_stats *stats = &priv->ndev->stats;
	unsigned int tx_tail;
	struct sk_buff *skb;

	lockdep_assert_held(&priv->tx_lock);

	tx_tail = rkcanfd_get_tx_tail(priv);
	skb = priv->can.echo_skb[tx_tail];

	/* Manual handling of CAN Bus Error counters. See
	 * rkcanfd_get_corrected_berr_counter() for detailed
	 * explanation.
	 */
	if (priv->bec.txerr)
		priv->bec.txerr--;

	if (skb)
		rkcanfd_skb_set_timestamp(priv, skb, ts);
	stats->tx_bytes +=
		can_rx_offload_get_echo_skb_queue_timestamp(&priv->offload,
							    tx_tail, ts,
							    frame_len_p);
	stats->tx_packets++;
}
