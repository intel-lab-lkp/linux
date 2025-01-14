// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 Socket CANfd driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/can/dev.h>
#include <linux/can/rx-offload.h>
#include <linux/ethtool.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>

#define DRVNAME "nct6694-can"

/*
 * USB command module type for NCT6694 CANfd controller.
 * This defines the module type used for communication with the NCT6694
 * CANfd controller over the USB interface.
 */
#define NCT6694_CAN_MOD			0x05

/* Command 00h - CAN Setting and Initialization */
#define NCT6694_CAN_SETTING		0x00
#define NCT6694_CAN_SETTING_SEL(idx)	(idx ? 0x01 : 0x00)
#define NCT6694_CAN_SETTING_CTRL1_MON	BIT(0)
#define NCT6694_CAN_SETTING_CTRL1_NISO	BIT(1)
#define NCT6694_CAN_SETTING_CTRL1_LBCK	BIT(2)

/* Command 01h - CAN Information */
#define NCT6694_CAN_INFORMATION		0x01
#define NCT6694_CAN_INFORMATION_SEL	0x00

/* Command 02h - CAN Event */
#define NCT6694_CAN_EVENT		0x02
#define NCT6694_CAN_EVENT_SEL(idx, mask)	\
	((idx ? 0x80 : 0x00) | ((mask) & 0xFF))
#define NCT6694_CAN_EVENT_ERR		BIT(0)
#define NCT6694_CAN_EVENT_STATUS	BIT(1)
#define NCT6694_CAN_EVENT_TX_EVT	BIT(2)
#define NCT6694_CAN_EVENT_RX_EVT	BIT(3)
#define NCT6694_CAN_EVENT_REC		BIT(4)
#define NCT6694_CAN_EVENT_TEC		BIT(5)
#define NCT6694_CAN_EVENT_MASK		GENMASK(3, 0)
#define NCT6694_CAN_EVT_TX_FIFO_EMPTY	BIT(7)	/* Read-clear */
#define NCT6694_CAN_EVT_RX_DATA_LOST	BIT(5)	/* Read-clear */
#define NCT6694_CAN_EVT_RX_HALF_FULL	BIT(6)	/* Read-clear */
#define NCT6694_CAN_EVT_RX_DATA_IN	BIT(7)	/* Read-clear*/

/* Command 10h - CAN Deliver */
#define NCT6694_CAN_DELIVER		0x10
#define NCT6694_CAN_DELIVER_SEL(buf_cnt)	\
	((buf_cnt) & 0xFF)

/* Command 11h - CAN Receive */
#define NCT6694_CAN_RECEIVE		0x11
#define NCT6694_CAN_RECEIVE_SEL(idx, buf_cnt)	\
	((idx ? 0x80 : 0x00) | ((buf_cnt) & 0xFF))

#define NCT6694_CAN_FRAME_TAG_CAN0	0xC0
#define NCT6694_CAN_FRAME_TAG_CAN1	0xC1
#define NCT6694_CAN_FRAME_FLAG_EFF	BIT(0)
#define NCT6694_CAN_FRAME_FLAG_RTR	BIT(1)
#define NCT6694_CAN_FRAME_FLAG_FD	BIT(2)
#define NCT6694_CAN_FRAME_FLAG_BRS	BIT(3)
#define NCT6694_CAN_FRAME_FLAG_ERR	BIT(4)

#define NCT6694_NAPI_WEIGHT		32

enum nct6694_event_err {
	NCT6694_CAN_EVT_ERR_NO_ERROR = 0,
	NCT6694_CAN_EVT_ERR_CRC_ERROR,
	NCT6694_CAN_EVT_ERR_STUFF_ERROR,
	NCT6694_CAN_EVT_ERR_ACK_ERROR,
	NCT6694_CAN_EVT_ERR_FORM_ERROR,
	NCT6694_CAN_EVT_ERR_BIT_ERROR,
	NCT6694_CAN_EVT_ERR_TIMEOUT_ERROR,
	NCT6694_CAN_EVT_ERR_UNKNOWN_ERROR,
};

enum nct6694_event_status {
	NCT6694_CAN_EVT_STS_ERROR_ACTIVE = 0,
	NCT6694_CAN_EVT_STS_ERROR_PASSIVE,
	NCT6694_CAN_EVT_STS_BUS_OFF,
	NCT6694_CAN_EVT_STS_WARNING,
};

struct __packed nct6694_can_setting {
	__le32 nbr;
	__le32 dbr;
	u8 active;
	u8 reserved[3];
	__le16 ctrl1;
	__le16 ctrl2;
	__le32 nbtp;
	__le32 dbtp;
};

struct __packed nct6694_can_information {
	u8 tx_fifo_cnt;
	u8 rx_fifo_cnt;
	u8 reserved[2];
	__le32 can_clk;
};

struct __packed nct6694_can_event {
	u8 err;
	u8 status;
	u8 tx_evt;
	u8 rx_evt;
	u8 rec;
	u8 tec;
	u8 reserved[2];
};

struct __packed nct6694_can_frame {
	u8 tag;
	u8 flag;
	u8 reserved;
	u8 length;
	__le32 id;
	u8 data[64];
};

union __packed nct6694_can_tx {
	struct nct6694_can_frame frame;
	struct nct6694_can_setting setting;
};

union __packed nct6694_can_rx {
	struct nct6694_can_event event[2];
	struct nct6694_can_frame frame;
	struct nct6694_can_information info;
};

struct nct6694_can_priv {
	struct can_priv can;	/* must be the first member */
	struct can_rx_offload offload;
	struct net_device *ndev;
	struct nct6694 *nct6694;
	struct mutex lock;
	struct sk_buff *tx_skb;
	struct workqueue_struct *wq;
	struct work_struct tx_work;
	union nct6694_can_tx *tx;
	union nct6694_can_rx *rx;
	unsigned char can_idx;
};

static inline struct nct6694_can_priv *rx_offload_to_priv(struct can_rx_offload *offload)
{
	return container_of(offload, struct nct6694_can_priv, offload);
}

static const struct can_bittiming_const nct6694_can_bittiming_nominal_const = {
	.name = DRVNAME,
	.tseg1_min = 2,
	.tseg1_max = 256,
	.tseg2_min = 2,
	.tseg2_max = 128,
	.sjw_max = 128,
	.brp_min = 1,
	.brp_max = 511,
	.brp_inc = 1,
};

static const struct can_bittiming_const nct6694_can_bittiming_data_const = {
	.name = DRVNAME,
	.tseg1_min = 1,
	.tseg1_max = 32,
	.tseg2_min = 1,
	.tseg2_max = 16,
	.sjw_max = 16,
	.brp_min = 1,
	.brp_max = 31,
	.brp_inc = 1,
};

static void nct6694_can_rx_offload(struct can_rx_offload *offload,
				   struct sk_buff *skb)
{
	struct nct6694_can_priv *priv = rx_offload_to_priv(offload);
	int ret;

	ret = can_rx_offload_queue_tail(offload, skb);
	if (ret)
		priv->ndev->stats.rx_fifo_errors++;
}

static void nct6694_can_handle_lost_msg(struct net_device *ndev)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;

	netdev_err(ndev, "RX FIFO overflow, message(s) lost.\n");

	stats->rx_errors++;
	stats->rx_over_errors++;

	skb = alloc_can_err_skb(ndev, &cf);
	if (!skb)
		return;

	cf->can_id |= CAN_ERR_CRTL;
	cf->data[1] = CAN_ERR_CRTL_RX_OVERFLOW;

	nct6694_can_rx_offload(&priv->offload, skb);
}

static void nct6694_can_rx(struct net_device *ndev, u8 rx_evt)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	struct nct6694_can_frame *frame = &priv->rx->frame;
	struct nct6694_cmd_header cmd_hd = {
		.mod = NCT6694_CAN_MOD,
		.cmd = NCT6694_CAN_RECEIVE,
		.sel = NCT6694_CAN_RECEIVE_SEL(priv->can_idx, 1),
		.len = cpu_to_le16(sizeof(*frame))
	};
	struct canfd_frame *cfd;
	struct can_frame *cf;
	struct sk_buff *skb;
	int ret;

	ret = nct6694_read_msg(priv->nct6694, &cmd_hd, frame);
	if (ret)
		return;

	if (frame->flag & NCT6694_CAN_FRAME_FLAG_FD) {
		skb = alloc_canfd_skb(priv->ndev, &cfd);
		if (!skb)
			return;

		cfd->can_id = le32_to_cpu(frame->id);
		cfd->len = frame->length;
		if (frame->flag & NCT6694_CAN_FRAME_FLAG_EFF)
			cfd->can_id |= CAN_EFF_FLAG;
		if (frame->flag & NCT6694_CAN_FRAME_FLAG_BRS)
			cfd->flags |= CANFD_BRS;
		if (frame->flag & NCT6694_CAN_FRAME_FLAG_ERR)
			cfd->flags |= CANFD_ESI;

		memcpy(cfd->data, frame->data, cfd->len);
	} else {
		skb = alloc_can_skb(priv->ndev, &cf);
		if (!skb)
			return;

		cf->can_id = le32_to_cpu(frame->id);
		cf->len = frame->length;
		if (frame->flag & NCT6694_CAN_FRAME_FLAG_EFF)
			cf->can_id |= CAN_EFF_FLAG;
		if (frame->flag & NCT6694_CAN_FRAME_FLAG_RTR)
			cf->can_id |= CAN_RTR_FLAG;

		memcpy(cf->data, frame->data, cf->len);
	}

	nct6694_can_rx_offload(&priv->offload, skb);
}

static void nct6694_can_clean(struct net_device *ndev)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);

	if (priv->tx_skb || netif_queue_stopped(ndev))
		ndev->stats.tx_errors++;
	dev_kfree_skb(priv->tx_skb);
	priv->tx_skb = NULL;
}

static int nct6694_can_get_berr_counter(const struct net_device *ndev,
					struct can_berr_counter *bec)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	struct nct6694_can_event *evt = priv->rx->event;
	struct nct6694_cmd_header cmd_hd;
	u8 mask = NCT6694_CAN_EVENT_REC | NCT6694_CAN_EVENT_TEC;
	int ret;

	guard(mutex)(&priv->lock);

	cmd_hd = (struct nct6694_cmd_header) {
		.mod = NCT6694_CAN_MOD,
		.cmd = NCT6694_CAN_EVENT,
		.sel = NCT6694_CAN_EVENT_SEL(priv->can_idx, mask),
		.len = cpu_to_le16(sizeof(priv->rx->event))
	};

	ret = nct6694_read_msg(priv->nct6694, &cmd_hd, evt);
	if (ret < 0)
		return ret;

	bec->rxerr = evt[priv->can_idx].rec;
	bec->txerr = evt[priv->can_idx].tec;

	return 0;
}

static void nct6694_can_handle_state_change(struct net_device *ndev,
					    u8 status)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	enum can_state new_state = priv->can.state;
	enum can_state rx_state, tx_state;
	struct can_berr_counter bec;
	struct can_frame *cf;
	struct sk_buff *skb;

	nct6694_can_get_berr_counter(ndev, &bec);
	can_state_get_by_berr_counter(ndev, &bec, &tx_state, &rx_state);

	switch (status) {
	case NCT6694_CAN_EVT_STS_ERROR_ACTIVE:
		new_state = CAN_STATE_ERROR_ACTIVE;
		break;
	case NCT6694_CAN_EVT_STS_ERROR_PASSIVE:
		new_state = CAN_STATE_ERROR_PASSIVE;
		break;
	case NCT6694_CAN_EVT_STS_BUS_OFF:
		new_state = CAN_STATE_BUS_OFF;
		break;
	case NCT6694_CAN_EVT_STS_WARNING:
		new_state = CAN_STATE_ERROR_WARNING;
		break;
	default:
		netdev_err(ndev, "Receive unknown CAN status event.\n");
		return;
	}

	/* state hasn't changed */
	if (new_state == priv->can.state)
		return;

	skb = alloc_can_err_skb(ndev, &cf);

	tx_state = bec.txerr >= bec.rxerr ? new_state : 0;
	rx_state = bec.txerr <= bec.rxerr ? new_state : 0;
	can_change_state(ndev, cf, tx_state, rx_state);

	if (new_state == CAN_STATE_BUS_OFF) {
		can_bus_off(ndev);
	} else if (skb) {
		cf->can_id |= CAN_ERR_CNT;
		cf->data[6] = bec.txerr;
		cf->data[7] = bec.rxerr;
	}

	nct6694_can_rx_offload(&priv->offload, skb);
}

static void nct6694_handle_bus_err(struct net_device *ndev, u8 bus_err)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	struct can_frame *cf;
	struct sk_buff *skb;

	if (bus_err == NCT6694_CAN_EVT_ERR_NO_ERROR)
		return;

	priv->can.can_stats.bus_error++;

	skb = alloc_can_err_skb(ndev, &cf);
	if (skb)
		cf->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;

	switch (bus_err) {
	case NCT6694_CAN_EVT_ERR_CRC_ERROR:
		netdev_dbg(ndev, "CRC error\n");
		ndev->stats.rx_errors++;
		if (skb)
			cf->data[3] |= CAN_ERR_PROT_LOC_CRC_SEQ;
		break;

	case NCT6694_CAN_EVT_ERR_STUFF_ERROR:
		netdev_dbg(ndev, "Stuff error\n");
		ndev->stats.rx_errors++;
		if (skb)
			cf->data[2] |= CAN_ERR_PROT_STUFF;
		break;

	case NCT6694_CAN_EVT_ERR_ACK_ERROR:
		netdev_dbg(ndev, "Ack error\n");
		ndev->stats.tx_errors++;
		if (skb) {
			cf->can_id |= CAN_ERR_ACK;
			cf->data[2] |= CAN_ERR_PROT_TX;
		}
		break;

	case NCT6694_CAN_EVT_ERR_FORM_ERROR:
		netdev_dbg(ndev, "Form error\n");
		ndev->stats.rx_errors++;
		if (skb)
			cf->data[2] |= CAN_ERR_PROT_FORM;
		break;

	case NCT6694_CAN_EVT_ERR_BIT_ERROR:
		netdev_dbg(ndev, "Bit error\n");
		ndev->stats.tx_errors++;
		if (skb)
			cf->data[2] |= CAN_ERR_PROT_TX | CAN_ERR_PROT_BIT;
		break;

	default:
		break;
	}

	nct6694_can_rx_offload(&priv->offload, skb);
}

static void nct6694_can_tx_irq(struct net_device *ndev)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;

	guard(mutex)(&priv->lock);
	stats->tx_bytes += can_get_echo_skb(ndev, 0, NULL);
	stats->tx_packets++;
	netif_wake_queue(ndev);
}

static irqreturn_t nct6694_can_irq(int irq, void *data)
{
	struct net_device *ndev = data;
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	struct nct6694_can_event *evt = priv->rx->event;
	struct nct6694_cmd_header cmd_hd;
	u8 tx_evt, rx_evt, bus_err, can_status;
	u8 mask_sts = NCT6694_CAN_EVENT_MASK;
	irqreturn_t handled = IRQ_NONE;
	int can_idx = priv->can_idx;
	int ret;

	scoped_guard(mutex, &priv->lock) {
		cmd_hd = (struct nct6694_cmd_header) {
			.mod = NCT6694_CAN_MOD,
			.cmd = NCT6694_CAN_EVENT,
			.sel = NCT6694_CAN_EVENT_SEL(priv->can_idx, mask_sts),
			.len = cpu_to_le16(sizeof(priv->rx->event))
		};

		ret = nct6694_read_msg(priv->nct6694, &cmd_hd, evt);
		if (ret < 0)
			return handled;

		tx_evt = evt[can_idx].tx_evt;
		rx_evt = evt[can_idx].rx_evt;
		bus_err = evt[can_idx].err;
		can_status = evt[can_idx].status;
	}

	if (rx_evt & NCT6694_CAN_EVT_RX_DATA_IN) {
		nct6694_can_rx(ndev, rx_evt);
		handled = IRQ_HANDLED;
	}

	if (rx_evt & NCT6694_CAN_EVT_RX_DATA_LOST) {
		nct6694_can_handle_lost_msg(ndev);
		handled = IRQ_HANDLED;
	}

	if (can_status) {
		nct6694_can_handle_state_change(ndev, can_status);
		handled = IRQ_HANDLED;
	}

	if (priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING) {
		nct6694_handle_bus_err(ndev, bus_err);
		handled = IRQ_HANDLED;
	}

	if (handled)
		can_rx_offload_threaded_irq_finish(&priv->offload);

	if (tx_evt & NCT6694_CAN_EVT_TX_FIFO_EMPTY)
		nct6694_can_tx_irq(ndev);

	return handled;
}

static void nct6694_can_tx(struct net_device *ndev)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	struct nct6694_can_frame *frame = &priv->tx->frame;
	struct nct6694_cmd_header cmd_hd = {
		.mod = NCT6694_CAN_MOD,
		.cmd = NCT6694_CAN_DELIVER,
		.sel = NCT6694_CAN_DELIVER_SEL(1),
		.len = cpu_to_le16(sizeof(*frame))
	};
	struct net_device_stats *stats = &ndev->stats;
	struct sk_buff *skb = priv->tx_skb;
	struct canfd_frame *cfd;
	struct can_frame *cf;
	u32 txid;
	int err;

	memset(frame, 0, sizeof(*frame));

	if (priv->can_idx == 0)
		frame->tag = NCT6694_CAN_FRAME_TAG_CAN0;
	else
		frame->tag = NCT6694_CAN_FRAME_TAG_CAN1;

	if (can_is_canfd_skb(skb)) {
		cfd = (struct canfd_frame *)priv->tx_skb->data;

		if (cfd->flags & CANFD_BRS)
			frame->flag |= NCT6694_CAN_FRAME_FLAG_BRS;

		if (cfd->can_id & CAN_EFF_FLAG) {
			txid = cfd->can_id & CAN_EFF_MASK;
			frame->flag |= NCT6694_CAN_FRAME_FLAG_EFF;
		} else {
			txid = cfd->can_id & CAN_SFF_MASK;
		}
		frame->flag |= NCT6694_CAN_FRAME_FLAG_FD;
		frame->id = cpu_to_le32(txid);
		frame->length = cfd->len;

		memcpy(frame->data, cfd->data, cfd->len);
	} else {
		cf = (struct can_frame *)priv->tx_skb->data;

		if (cf->can_id & CAN_RTR_FLAG)
			frame->flag |= NCT6694_CAN_FRAME_FLAG_RTR;

		if (cf->can_id & CAN_EFF_FLAG) {
			txid = cf->can_id & CAN_EFF_MASK;
			frame->flag |= NCT6694_CAN_FRAME_FLAG_EFF;
		} else {
			txid = cf->can_id & CAN_SFF_MASK;
		}
		frame->id = cpu_to_le32(txid);
		frame->length = cf->len;

		memcpy(frame->data, cf->data, cf->len);
	}

	err = nct6694_write_msg(priv->nct6694, &cmd_hd, frame);
	if (err) {
		netdev_err(ndev, "%s: Tx FIFO full!\n", __func__);
		can_free_echo_skb(ndev, 0, NULL);
		stats->tx_dropped++;
		stats->tx_errors++;
		netif_wake_queue(ndev);
	}
}

static void nct6694_can_tx_work(struct work_struct *work)
{
	struct nct6694_can_priv *priv = container_of(work,
						     struct nct6694_can_priv,
						     tx_work);
	struct net_device *ndev = priv->ndev;

	guard(mutex)(&priv->lock);

	if (priv->tx_skb) {
		if (priv->can.state == CAN_STATE_BUS_OFF) {
			nct6694_can_clean(ndev);
		} else {
			nct6694_can_tx(ndev);
			can_put_echo_skb(priv->tx_skb, ndev, 0, 0);
			priv->tx_skb = NULL;
		}
	}
}

static netdev_tx_t nct6694_can_start_xmit(struct sk_buff *skb,
					  struct net_device *ndev)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);

	if (can_dev_dropped_skb(ndev, skb))
		return NETDEV_TX_OK;

	if (priv->tx_skb) {
		netdev_err(ndev, "hard_xmit called while tx busy\n");
		return NETDEV_TX_BUSY;
	}

	netif_stop_queue(ndev);
	priv->tx_skb = skb;
	queue_work(priv->wq, &priv->tx_work);

	return NETDEV_TX_OK;
}

static int nct6694_can_start(struct net_device *ndev)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	struct nct6694_can_setting *setting = &priv->tx->setting;
	struct nct6694_cmd_header cmd_hd = {
		.mod = NCT6694_CAN_MOD,
		.cmd = NCT6694_CAN_SETTING,
		.sel = NCT6694_CAN_SETTING_SEL(priv->can_idx),
		.len = cpu_to_le16(sizeof(*setting))
	};
	const struct can_bittiming *n_bt = &priv->can.bittiming;
	const struct can_bittiming *d_bt = &priv->can.data_bittiming;
	int ret;

	guard(mutex)(&priv->lock);

	memset(setting, 0, sizeof(*setting));
	setting->nbr = cpu_to_le32(n_bt->bitrate);
	setting->dbr = cpu_to_le32(d_bt->bitrate);

	if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		setting->ctrl1 |= cpu_to_le16(NCT6694_CAN_SETTING_CTRL1_MON);

	if ((priv->can.ctrlmode & CAN_CTRLMODE_FD) &&
	    priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO)
		setting->ctrl1 |= cpu_to_le16(NCT6694_CAN_SETTING_CTRL1_NISO);

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		setting->ctrl1 |= cpu_to_le16(NCT6694_CAN_SETTING_CTRL1_LBCK);

	ret = nct6694_write_msg(priv->nct6694, &cmd_hd, setting);
	if (ret)
		return ret;

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	return ret;
}

static int nct6694_can_stop(struct net_device *ndev)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	free_irq(ndev->irq, ndev);
	destroy_workqueue(priv->wq);
	priv->wq = NULL;
	nct6694_can_clean(ndev);
	priv->can.state = CAN_STATE_STOPPED;
	can_rx_offload_disable(&priv->offload);
	close_candev(ndev);

	return 0;
}

static int nct6694_can_set_mode(struct net_device *ndev, enum can_mode mode)
{
	switch (mode) {
	case CAN_MODE_START:
		nct6694_can_clean(ndev);
		nct6694_can_start(ndev);
		netif_wake_queue(ndev);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int nct6694_can_open(struct net_device *ndev)
{
	struct nct6694_can_priv *priv = netdev_priv(ndev);
	int ret;

	ret = open_candev(ndev);
	if (ret)
		return ret;

	can_rx_offload_enable(&priv->offload);

	ret = request_threaded_irq(ndev->irq, NULL,
				   nct6694_can_irq, IRQF_ONESHOT,
				   "nct6694_can", ndev);
	if (ret) {
		netdev_err(ndev, "Failed to request IRQ\n");
		goto close_candev;
	}

	priv->wq = alloc_ordered_workqueue("%s-nct6694_wq",
					   WQ_FREEZABLE | WQ_MEM_RECLAIM,
					   ndev->name);
	if (!priv->wq) {
		ret = -ENOMEM;
		goto free_irq;
	}

	priv->tx_skb = NULL;

	ret = nct6694_can_start(ndev);
	if (ret)
		goto destroy_wq;

	netif_start_queue(ndev);

	return 0;

destroy_wq:
	destroy_workqueue(priv->wq);
free_irq:
	free_irq(ndev->irq, ndev);
close_candev:
	can_rx_offload_disable(&priv->offload);
	close_candev(ndev);
	return ret;
}

static const struct net_device_ops nct6694_can_netdev_ops = {
	.ndo_open = nct6694_can_open,
	.ndo_stop = nct6694_can_stop,
	.ndo_start_xmit = nct6694_can_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

static const struct ethtool_ops nct6694_can_ethtool_ops = {
	.get_ts_info = ethtool_op_get_ts_info,
};

static int nct6694_can_get_clock(struct nct6694_can_priv *priv)
{
	struct nct6694_can_information *info = &priv->rx->info;
	struct nct6694_cmd_header cmd_hd = {
		.mod = NCT6694_CAN_MOD,
		.cmd = NCT6694_CAN_INFORMATION,
		.sel = NCT6694_CAN_INFORMATION_SEL,
		.len = cpu_to_le16(sizeof(*info))
	};
	int ret;

	ret = nct6694_read_msg(priv->nct6694, &cmd_hd, info);
	if (ret)
		return ret;

	return le32_to_cpu(info->can_clk);
}

static int nct6694_can_probe(struct platform_device *pdev)
{
	const struct mfd_cell *cell = mfd_get_cell(pdev);
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	struct nct6694_can_priv *priv;
	struct net_device *ndev;
	int ret, irq, can_clk;

	irq = irq_create_mapping(nct6694->domain,
				 NCT6694_IRQ_CAN1 + cell->id);
	if (!irq)
		return irq;

	ndev = alloc_candev(sizeof(struct nct6694_can_priv), 1);
	if (!ndev)
		return -ENOMEM;

	ndev->irq = irq;
	ndev->flags |= IFF_ECHO;
	ndev->netdev_ops = &nct6694_can_netdev_ops;
	ndev->ethtool_ops = &nct6694_can_ethtool_ops;

	priv = netdev_priv(ndev);
	priv->nct6694 = nct6694;
	priv->ndev = ndev;

	priv->tx = devm_kzalloc(&pdev->dev, sizeof(union nct6694_can_tx),
				GFP_KERNEL);
	if (!priv->tx) {
		ret = -ENOMEM;
		goto free_candev;
	}

	priv->rx = devm_kzalloc(&pdev->dev, sizeof(union nct6694_can_rx),
				GFP_KERNEL);
	if (!priv->rx) {
		ret = -ENOMEM;
		goto free_candev;
	}

	can_clk = nct6694_can_get_clock(priv);
	if (can_clk < 0) {
		ret = dev_err_probe(&pdev->dev, can_clk,
				    "Failed to get clock\n");
		goto free_candev;
	}

	devm_mutex_init(&pdev->dev, &priv->lock);
	INIT_WORK(&priv->tx_work, nct6694_can_tx_work);

	priv->can_idx = cell->id;
	priv->can.state = CAN_STATE_STOPPED;
	priv->can.clock.freq = can_clk;
	priv->can.bittiming_const = &nct6694_can_bittiming_nominal_const;
	priv->can.data_bittiming_const = &nct6694_can_bittiming_data_const;
	priv->can.do_set_mode = nct6694_can_set_mode;
	priv->can.do_get_berr_counter = nct6694_can_get_berr_counter;

	priv->can.ctrlmode = CAN_CTRLMODE_FD;

	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK		|
				       CAN_CTRLMODE_LISTENONLY		|
				       CAN_CTRLMODE_FD			|
				       CAN_CTRLMODE_FD_NON_ISO		|
				       CAN_CTRLMODE_BERR_REPORTING;

	ret = can_rx_offload_add_manual(ndev, &priv->offload,
					NCT6694_NAPI_WEIGHT);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "Failed to add rx_offload\n");
		goto free_candev;
	}

	platform_set_drvdata(pdev, priv);
	SET_NETDEV_DEV(priv->ndev, &pdev->dev);

	ret = register_candev(priv->ndev);
	if (ret)
		goto del_rx_offload;

	return 0;

del_rx_offload:
	can_rx_offload_del(&priv->offload);
free_candev:
	free_candev(ndev);
	return ret;
}

static void nct6694_can_remove(struct platform_device *pdev)
{
	struct nct6694_can_priv *priv = platform_get_drvdata(pdev);

	cancel_work_sync(&priv->tx_work);
	unregister_candev(priv->ndev);
	can_rx_offload_del(&priv->offload);
	free_candev(priv->ndev);
}

static struct platform_driver nct6694_can_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_can_probe,
	.remove		= nct6694_can_remove,
};

module_platform_driver(nct6694_can_driver);

MODULE_DESCRIPTION("USB-CAN FD driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:nct6694-can");
