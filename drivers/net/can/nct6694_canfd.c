// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 Socket CANfd driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/can/dev.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>

#define DRVNAME "nct6694-can"

/* Host interface */
#define NCT6694_CAN_MOD			0x05

/* Message Channel*/
/* Command 00h */
#define NCT6694_CAN_CMD0_LEN		0x18
#define NCT6694_CAN_CMD0_OFFSET(idx)	(idx ? 0x0100 : 0x0000)
#define NCT6694_CAN_CTRL1_MON		BIT(0)
#define NCT6694_CAN_CTRL1_NISO		BIT(1)
#define NCT6694_CAN_CTRL1_LBCK		BIT(2)

/* Command 01h */
#define NCT6694_CAN_CMD1_LEN		0x08
#define NCT6694_CAN_CMD1_OFFSET		0x0001

/* Command 02h */
#define NCT6694_CAN_CMD2_LEN            0x10
#define NCT6694_CAN_CMD2_OFFSET(idx, mask)			\
	({ typeof(mask) mask_ = (mask);				\
	   idx ? ((0x80 | (mask_ & 0xFF)) << 8 | 0x02) :	\
		 ((0x00 | (mask_ & 0xFF)) << 8 | 0x02); })

#define NCT6694_CAN_EVENT_ERR		BIT(0)
#define NCT6694_CAN_EVENT_STATUS	BIT(1)
#define NCT6694_CAN_EVENT_TX_EVT	BIT(2)
#define NCT6694_CAN_EVENT_RX_EVT	BIT(3)
#define NCT6694_CAN_EVENT_REC		BIT(4)
#define NCT6694_CAN_EVENT_TEC		BIT(5)
#define NCT6694_CAN_EVT_TX_FIFO_EMPTY	BIT(7)	/* Read-clear */
#define NCT6694_CAN_EVT_RX_DATA_LOST	BIT(5)	/* Read-clear */
#define NCT6694_CAN_EVT_RX_HALF_FULL	BIT(6)	/* Read-clear */
#define NCT6694_CAN_EVT_RX_DATA_IN	BIT(7)

/* Command 10h */
#define NCT6694_CAN_CMD10_LEN		0x90
#define NCT6694_CAN_CMD10_OFFSET(buf_cnt)	\
	(((buf_cnt) & 0xFF) << 8 | 0x10)
#define NCT6694_CAN_TAG_CAN0		0xC0
#define NCT6694_CAN_TAG_CAN1		0xC1
#define NCT6694_CAN_FLAG_EFF		BIT(0)
#define NCT6694_CAN_FLAG_RTR		BIT(1)
#define NCT6694_CAN_FLAG_FD		BIT(2)
#define NCT6694_CAN_FLAG_BRS		BIT(3)
#define NCT6694_CAN_FLAG_ERR		BIT(4)

/* Command 11h */
#define NCT6694_CAN_CMD11_LEN		0x90
#define NCT6694_CAN_CMD11_OFFSET(idx, buf_cnt)			\
	({ typeof(buf_cnt) buf_cnt_ = (buf_cnt);		\
	   idx ? ((0x80 | (buf_cnt_ & 0xFF)) << 8 | 0x11) :	\
		 ((0x00 | (buf_cnt_ & 0xFF)) << 8 | 0x11); })

#define NCT6694_CAN_RX_QUOTA		64

enum nct6694_event_err {
	NCT6694_CAN_EVT_NO_ERROR,
	NCT6694_CAN_EVT_CRC_ERROR,
	NCT6694_CAN_EVT_STUFF_ERROR,
	NCT6694_CAN_EVT_ACK_ERROR,
	NCT6694_CAN_EVT_FORM_ERROR,
	NCT6694_CAN_EVT_BIT_ERROR,
	NCT6694_CAN_EVT_TIMEOUT_ERROR,
	NCT6694_CAN_EVT_UNKNOWN_ERROR,
};

enum nct6694_event_status {
	NCT6694_CAN_EVT_ERROR_ACTIVE,
	NCT6694_CAN_EVT_ERROR_PASSIVE,
	NCT6694_CAN_EVT_BUS_OFF,
	NCT6694_CAN_EVT_WARNING,
};

struct __packed nct6694_can_cmd0 {
	u32 nbr;
	u32 dbr;
	u32 active:8;
	u32 reserved:24;
	u16 ctrl1;
	u16 ctrl2;
	u32 nbtp;
	u32 dbtp;
};

struct __packed nct6694_can_cmd1 {
	u8 tx_fifo_cnt;
	u8 rx_fifo_cnt;
	u16 reserved;
	__le32 can_clk;
};

struct __packed nct6694_can_cmd2 {
	u8 err1;
	u8 status1;
	u8 tx_evt1;
	u8 rx_evt1;
	u8 rec1;
	u8 tec1;
	u8 reserved1[2];
	u8 err2;
	u8 status2;
	u8 tx_evt2;
	u8 rx_evt2;
	u8 rec2;
	u8 tec2;
	u8 reserved2[2];
};

struct __packed nct6694_can_cmd10_11 {
	u8 tag;
	u8 flag;
	u8 reserved;
	u8 dlc;
	__le32 id;
	u8 data[64];
	u8 msg_buf[72];
};

struct nct6694_canfd_priv {
	struct can_priv can;	/* must be the first member */
	struct net_device *ndev;
	struct nct6694 *nct6694;
	struct mutex lock;
	struct sk_buff *tx_skb;
	struct workqueue_struct *wq;
	struct work_struct tx_work;
	unsigned char *tx_buf;
	unsigned char *rx_buf;
	unsigned char can_idx;
	bool tx_busy;
};

static const struct can_bittiming_const nct6694_canfd_bittiming_nominal_const = {
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

static const struct can_bittiming_const nct6694_canfd_bittiming_data_const = {
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

static void nct6694_canfd_clean(struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);

	if (priv->tx_skb || priv->tx_busy)
		ndev->stats.tx_errors++;
	dev_kfree_skb(priv->tx_skb);
	if (priv->tx_busy)
		can_free_echo_skb(priv->ndev, 0, NULL);
	priv->tx_skb = NULL;
	priv->tx_busy = false;
}

static int nct6694_canfd_get_berr_counter(const struct net_device *ndev,
					  struct can_berr_counter *bec)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct nct6694_can_cmd2 *buf = (struct nct6694_can_cmd2 *)priv->rx_buf;
	u8 mask = NCT6694_CAN_EVENT_REC | NCT6694_CAN_EVENT_TEC;
	int ret;

	guard(mutex)(&priv->lock);

	ret = nct6694_read_msg(priv->nct6694, NCT6694_CAN_MOD,
			       NCT6694_CAN_CMD2_OFFSET(priv->can_idx, mask),
			       NCT6694_CAN_CMD2_LEN,
			       buf);
	if (ret < 0)
		return ret;

	bec->rxerr = priv->can_idx ? buf->rec2 : buf->rec1;
	bec->txerr = priv->can_idx ? buf->tec2 : buf->tec1;

	return 0;
}

static int nuv_canfd_handle_lost_msg(struct net_device *ndev)
{
	struct net_device_stats *stats = &ndev->stats;
	struct sk_buff *skb;
	struct can_frame *frame;

	netdev_err(ndev, "RX FIFO overflow, message(s) lost.\n");

	stats->rx_errors++;
	stats->rx_over_errors++;

	skb = alloc_can_err_skb(ndev, &frame);
	if (unlikely(!skb))
		return 0;

	frame->can_id |= CAN_ERR_CRTL;
	frame->data[1] = CAN_ERR_CRTL_RX_OVERFLOW;

	netif_receive_skb(skb);

	return 1;
}

static void nuv_canfd_read_fifo(struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct nct6694_can_cmd10_11 *buf = (struct nct6694_can_cmd10_11 *)priv->rx_buf;
	struct net_device_stats *stats = &ndev->stats;
	struct canfd_frame *cf;
	struct sk_buff *skb;
	int can_idx = priv->can_idx;
	u32 id;
	int ret;
	u8 fd_format = 0;
	int i;

	guard(mutex)(&priv->lock);

	ret = nct6694_read_msg(priv->nct6694, NCT6694_CAN_MOD,
			       NCT6694_CAN_CMD11_OFFSET(can_idx, 1),
			       NCT6694_CAN_CMD11_LEN, buf);
	if (ret < 0)
		return;

	/* Check type of frame and create skb */
	fd_format = buf->flag & NCT6694_CAN_FLAG_FD;
	if (fd_format)
		skb = alloc_canfd_skb(ndev, &cf);
	else
		skb = alloc_can_skb(ndev, (struct can_frame **)&cf);

	if (!skb) {
		stats->rx_dropped++;
		return;
	}

	cf->len = buf->dlc;

	/* Get ID and set flag by its type(Standard ID format or Ext ID format) */
	id = le32_to_cpu(buf->id);
	if (buf->flag & NCT6694_CAN_FLAG_EFF) {
		/*
		 * In case the Extended ID frame is received, the standard
		 * and extended part of the ID are swapped in the register,
		 * so swap them back to obtain the correct ID.
		 */
		id |= CAN_EFF_FLAG;
	}

	cf->can_id = id;

	/* Set ESI flag */
	if (buf->flag & NCT6694_CAN_FLAG_ERR) {
		cf->flags |= CANFD_ESI;
		netdev_dbg(ndev, "ESI Error\n");
	}

	/* Set RTR and BRS */
	if (!fd_format && (buf->flag & NCT6694_CAN_FLAG_RTR)) {
		cf->can_id |= CAN_RTR_FLAG;
	} else {
		if (buf->flag & NCT6694_CAN_FLAG_BRS)
			cf->flags |= CANFD_BRS;

		for (i = 0; i < cf->len; i++)
			cf->data[i] = buf->data[i];
	}

	/* Remove the packet from FIFO */
	stats->rx_packets++;
	stats->rx_bytes += cf->len;
	netif_receive_skb(skb);
}

static int nct6694_canfd_do_rx_poll(struct net_device *ndev, int quota)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct nct6694_can_cmd2 *buf = (struct nct6694_can_cmd2 *)priv->rx_buf;
	int can_idx = priv->can_idx;
	u32 pkts = 0;
	u8 mask_rx = NCT6694_CAN_EVENT_RX_EVT;
	u8 rx_evt;

	for (;;) {
		scoped_guard(mutex, &priv->lock) {
			nct6694_read_msg(priv->nct6694, NCT6694_CAN_MOD,
					 NCT6694_CAN_CMD2_OFFSET(can_idx, mask_rx),
					 NCT6694_CAN_CMD2_LEN, buf);

			rx_evt = can_idx ? buf->rx_evt2 : buf->rx_evt1;
		}

		if (rx_evt & NCT6694_CAN_EVT_RX_DATA_LOST)
			nuv_canfd_handle_lost_msg(ndev);

		/* No data */
		if ((rx_evt & NCT6694_CAN_EVT_RX_DATA_IN) == 0)
			break;

		if (quota <= 0)
			break;

		nuv_canfd_read_fifo(ndev);
		quota--;
		pkts++;
	}

	return pkts;
}

static int nct6694_canfd_handle_lec_err(struct net_device *ndev, u8 bus_err)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;

	if (bus_err == NCT6694_CAN_EVT_NO_ERROR)
		return 0;

	priv->can.can_stats.bus_error++;
	stats->rx_errors++;

	/* Propagate the error condition to the CAN stack. */
	skb = alloc_can_err_skb(ndev, &cf);

	if (unlikely(!skb))
		return 0;

	/* Read the error counter register and check for new errors. */
	cf->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;

	switch (bus_err) {
	case NCT6694_CAN_EVT_CRC_ERROR:
		cf->data[3] = CAN_ERR_PROT_LOC_CRC_SEQ;
		break;

	case NCT6694_CAN_EVT_STUFF_ERROR:
		cf->data[2] |= CAN_ERR_PROT_STUFF;
		break;

	case NCT6694_CAN_EVT_ACK_ERROR:
		cf->data[3] = CAN_ERR_PROT_LOC_ACK;
		break;

	case NCT6694_CAN_EVT_FORM_ERROR:
		cf->data[2] |= CAN_ERR_PROT_FORM;
		break;

	case NCT6694_CAN_EVT_BIT_ERROR:
		cf->data[2] |= CAN_ERR_PROT_BIT |
			       CAN_ERR_PROT_BIT0 |
			       CAN_ERR_PROT_BIT1;
		break;

	case NCT6694_CAN_EVT_TIMEOUT_ERROR:
		cf->data[2] |= CAN_ERR_PROT_UNSPEC;
		break;

	case NCT6694_CAN_EVT_UNKNOWN_ERROR:
		cf->data[2] |= CAN_ERR_PROT_UNSPEC;
		/*
		 * It means 'unspecified'(the value is '0').
		 * But it is not sure if it's ok to send an error package
		 * without specific error bit.
		 */
		break;

	default:
		break;
	}

	/* Reset the error counter, ack the IRQ and re-enable the counter. */
	stats->rx_packets++;
	stats->rx_bytes += cf->can_dlc;
	netif_receive_skb(skb);

	return 1;
}

static int nct6694_canfd_handle_state_change(struct net_device *ndev,
					     enum can_state new_state)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;
	struct can_berr_counter bec;

	switch (new_state) {
	case CAN_STATE_ERROR_ACTIVE:
		/* error active state */
		priv->can.can_stats.error_warning++;
		priv->can.state = CAN_STATE_ERROR_ACTIVE;
		break;
	case CAN_STATE_ERROR_WARNING:
		/* error warning state */
		priv->can.can_stats.error_warning++;
		priv->can.state = CAN_STATE_ERROR_WARNING;
		break;
	case CAN_STATE_ERROR_PASSIVE:
		/* error passive state */
		priv->can.can_stats.error_passive++;
		priv->can.state = CAN_STATE_ERROR_PASSIVE;
		break;
	case CAN_STATE_BUS_OFF:
		/* bus-off state */
		priv->can.state = CAN_STATE_BUS_OFF;
		priv->can.can_stats.bus_off++;
		can_bus_off(ndev);
		break;
	default:
		break;
	}

	/* propagate the error condition to the CAN stack */
	skb = alloc_can_err_skb(ndev, &cf);
	if (unlikely(!skb))
		return 0;

	nct6694_canfd_get_berr_counter(ndev, &bec);

	switch (new_state) {
	case CAN_STATE_ERROR_WARNING:
		/* error warning state */
		cf->can_id |= CAN_ERR_CRTL;
		cf->data[1] = (bec.txerr > bec.rxerr) ? CAN_ERR_CRTL_TX_WARNING :
							CAN_ERR_CRTL_RX_WARNING;
		cf->data[6] = bec.txerr;
		cf->data[7] = bec.rxerr;
		break;
	case CAN_STATE_ERROR_PASSIVE:
		/* error passive state */
		cf->can_id |= CAN_ERR_CRTL;
		cf->data[1] |= CAN_ERR_CRTL_RX_PASSIVE;
		if (bec.txerr > 127)
			cf->data[1] |= CAN_ERR_CRTL_TX_PASSIVE;
		cf->data[6] = bec.txerr;
		cf->data[7] = bec.rxerr;
		break;
	case CAN_STATE_BUS_OFF:
		/* bus-off state */
		cf->can_id |= CAN_ERR_BUSOFF;
		break;
	default:
		break;
	}

	stats->rx_packets++;
	stats->rx_bytes += cf->can_dlc;
	netif_receive_skb(skb);

	return 1;
}

static int nct6694_canfd_handle_state_errors(struct net_device *ndev,
					     unsigned char can_status)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	int work_done = 0;

	if (can_status == NCT6694_CAN_EVT_ERROR_ACTIVE &&
	    priv->can.state != CAN_STATE_ERROR_ACTIVE) {
		netdev_dbg(ndev, "Error, entered active state\n");
		work_done += nct6694_canfd_handle_state_change(ndev,
							       CAN_STATE_ERROR_ACTIVE);
	}

	if (can_status == NCT6694_CAN_EVT_WARNING &&
	    priv->can.state != CAN_STATE_ERROR_WARNING) {
		netdev_dbg(ndev, "Error, entered warning state\n");
		work_done += nct6694_canfd_handle_state_change(ndev,
							       CAN_STATE_ERROR_WARNING);
	}

	if (can_status == NCT6694_CAN_EVT_ERROR_PASSIVE &&
	    priv->can.state != CAN_STATE_ERROR_PASSIVE) {
		netdev_dbg(ndev, "Error, entered passive state\n");
		work_done += nct6694_canfd_handle_state_change(ndev,
							       CAN_STATE_ERROR_PASSIVE);
	}

	if (can_status == NCT6694_CAN_EVT_BUS_OFF &&
	    priv->can.state != CAN_STATE_BUS_OFF) {
		netdev_dbg(ndev, "Error, entered bus-off state\n");
		work_done += nct6694_canfd_handle_state_change(ndev,
							       CAN_STATE_BUS_OFF);
	}

	return work_done;
}

static int nct6694_canfd_poll(struct net_device *ndev, int quota)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct nct6694_can_cmd2 *buf = (struct nct6694_can_cmd2 *)priv->rx_buf;
	int can_idx = priv->can_idx;
	int work_done = 0, ret;
	u8 evt_mask = NCT6694_CAN_EVENT_ERR | NCT6694_CAN_EVENT_STATUS;
	u8 bus_err, can_status;

	scoped_guard(mutex, &priv->lock) {
		ret = nct6694_read_msg(priv->nct6694, NCT6694_CAN_MOD,
				       NCT6694_CAN_CMD2_OFFSET(can_idx, evt_mask),
				       NCT6694_CAN_CMD2_LEN, buf);
		if (ret < 0)
			return IRQ_NONE;

		if (can_idx) {
			bus_err = buf->err2;
			can_status = buf->status2;
		} else {
			bus_err = buf->err1;
			can_status = buf->status1;
		}
	}

	/* Handle bus state changes */
	work_done += nct6694_canfd_handle_state_errors(ndev, can_status);

	/* Handle lec errors on the bus */
	if (priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING)
		work_done += nct6694_canfd_handle_lec_err(ndev, bus_err);

	/* Handle RX events */
	work_done += nct6694_canfd_do_rx_poll(ndev, quota - work_done);
	return work_done;
}

static void nct6694_canfd_tx_irq(struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;

	guard(mutex)(&priv->lock);
	stats->tx_bytes += can_get_echo_skb(ndev, 0, NULL);
	stats->tx_packets++;
	priv->tx_busy = false;
	netif_wake_queue(ndev);
}

static irqreturn_t nct6694_canfd_irq(int irq, void *data)
{
	struct net_device *ndev = data;
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct nct6694_can_cmd2 *buf = (struct nct6694_can_cmd2 *)priv->rx_buf;
	int can_idx = priv->can_idx;
	int ret;
	u8 mask_sts = NCT6694_CAN_EVENT_TX_EVT;
	u8 tx_evt;

	scoped_guard(mutex, &priv->lock) {
		ret = nct6694_read_msg(priv->nct6694, NCT6694_CAN_MOD,
				       NCT6694_CAN_CMD2_OFFSET(can_idx, mask_sts),
				       NCT6694_CAN_CMD2_LEN, buf);
		if (ret < 0)
			return IRQ_NONE;

		tx_evt = can_idx ? buf->tx_evt2 : buf->tx_evt1;
	}

	if (tx_evt) {
		nct6694_canfd_tx_irq(ndev);
	} else {
		ret = nct6694_canfd_poll(ndev, NCT6694_CAN_RX_QUOTA);
		if (!ret)
			return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static int nct6694_canfd_start(struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct nct6694_can_cmd0 *buf = (struct nct6694_can_cmd0 *)priv->tx_buf;
	const struct can_bittiming *n_bt = &priv->can.bittiming;
	const struct can_bittiming *d_bt = &priv->can.data_bittiming;
	int ret;

	guard(mutex)(&priv->lock);

	memset(priv->tx_buf, 0, NCT6694_CAN_CMD0_LEN);
	buf->nbr = n_bt->bitrate;
	buf->dbr = d_bt->bitrate;

	if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		buf->ctrl1 |= NCT6694_CAN_CTRL1_MON;

	if ((priv->can.ctrlmode & CAN_CTRLMODE_FD) &&
	    priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO)
		buf->ctrl1 |= NCT6694_CAN_CTRL1_NISO;

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		buf->ctrl1 |= NCT6694_CAN_CTRL1_LBCK;

	ret = nct6694_write_msg(priv->nct6694, NCT6694_CAN_MOD,
				NCT6694_CAN_CMD0_OFFSET(priv->can_idx),
				NCT6694_CAN_CMD0_LEN, buf);
	if (ret < 0)
		return ret;

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	return 0;
}

static int nct6694_canfd_stop(struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	free_irq(ndev->irq, ndev);
	destroy_workqueue(priv->wq);
	priv->wq = NULL;
	nct6694_canfd_clean(ndev);
	priv->can.state = CAN_STATE_STOPPED;
	close_candev(ndev);

	return 0;
}

static int nct6694_canfd_set_mode(struct net_device *ndev, enum can_mode mode)
{
	switch (mode) {
	case CAN_MODE_START:
		nct6694_canfd_clean(ndev);
		nct6694_canfd_start(ndev);
		netif_wake_queue(ndev);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int nct6694_canfd_open(struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	int ret;

	ret = open_candev(ndev);
	if (ret)
		return ret;

	ret = request_threaded_irq(ndev->irq, NULL,
				   nct6694_canfd_irq, IRQF_ONESHOT,
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
	priv->tx_busy = false;

	ret = nct6694_canfd_start(ndev);
	if (ret)
		goto destroy_wq;

	netif_start_queue(ndev);

	return 0;

destroy_wq:
	destroy_workqueue(priv->wq);
free_irq:
	free_irq(ndev->irq, ndev);
close_candev:
	close_candev(ndev);
	return ret;
}

static netdev_tx_t nct6694_canfd_start_xmit(struct sk_buff *skb,
					    struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);

	if (priv->tx_skb || priv->tx_busy) {
		netdev_err(ndev, "hard_xmit called while tx busy\n");
		return NETDEV_TX_BUSY;
	}

	if (can_dev_dropped_skb(ndev, skb))
		return NETDEV_TX_OK;

	netif_stop_queue(ndev);
	priv->tx_skb = skb;
	queue_work(priv->wq, &priv->tx_work);

	return NETDEV_TX_OK;
}

static void nct6694_canfd_tx(struct net_device *ndev, struct canfd_frame *cf)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct nct6694_can_cmd10_11 *buf = (struct nct6694_can_cmd10_11 *)priv->tx_buf;
	u32 txid = 0;
	int i;

	memset(buf, 0, NCT6694_CAN_CMD10_LEN);

	if (priv->can_idx == 0)
		buf->tag = NCT6694_CAN_TAG_CAN0;
	else
		buf->tag = NCT6694_CAN_TAG_CAN1;

	if (cf->can_id & CAN_EFF_FLAG) {
		txid = cf->can_id & CAN_EFF_MASK;
		/*
		 * In case the Extended ID frame is transmitted, the
		 * standard and extended part of the ID are swapped
		 * in the register, so swap them back to send the
		 * correct ID.
		 */
		buf->flag |= NCT6694_CAN_FLAG_EFF;
	} else {
		txid = cf->can_id & CAN_SFF_MASK;
	}

	buf->id = cpu_to_le32(txid);
	buf->dlc = cf->len;

	if ((priv->can.ctrlmode & CAN_CTRLMODE_FD) &&
	    can_is_canfd_skb(priv->tx_skb)) {
		buf->flag |= NCT6694_CAN_FLAG_FD;
		if (cf->flags & CANFD_BRS)
			buf->flag |= NCT6694_CAN_FLAG_BRS;
	}

	if (cf->can_id & CAN_RTR_FLAG)
		buf->flag |= NCT6694_CAN_FLAG_RTR;

	/* set data to buf */
	for (i = 0; i < cf->len; i++)
		buf->data[i] = cf->data[i];

	nct6694_write_msg(priv->nct6694, NCT6694_CAN_MOD,
			  NCT6694_CAN_CMD10_OFFSET(1),
			  NCT6694_CAN_CMD10_LEN,
			  buf);
}

static void nct6694_canfd_tx_work(struct work_struct *work)
{
	struct nct6694_canfd_priv *priv = container_of(work,
						       struct nct6694_canfd_priv,
						       tx_work);
	struct net_device *ndev = priv->ndev;
	struct canfd_frame *cf;

	guard(mutex)(&priv->lock);

	if (priv->tx_skb) {
		if (priv->can.state == CAN_STATE_BUS_OFF) {
			nct6694_canfd_clean(ndev);
		} else {
			cf = (struct canfd_frame *)priv->tx_skb->data;
			nct6694_canfd_tx(ndev, cf);
			priv->tx_busy = true;
			can_put_echo_skb(priv->tx_skb, ndev, 0, 0);
			priv->tx_skb = NULL;
		}
	}
}

static const struct net_device_ops nct6694_canfd_netdev_ops = {
	.ndo_open = nct6694_canfd_open,
	.ndo_stop = nct6694_canfd_stop,
	.ndo_start_xmit = nct6694_canfd_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

static int nct6694_canfd_get_clock(struct nct6694_canfd_priv *priv)
{
	struct nct6694_can_cmd1 *buf = (struct nct6694_can_cmd1 *)priv->rx_buf;
	int ret;

	ret = nct6694_read_msg(priv->nct6694, NCT6694_CAN_MOD,
			       NCT6694_CAN_CMD1_OFFSET,
			       NCT6694_CAN_CMD1_LEN,
			       buf);
	if (ret)
		return ret;

	return le32_to_cpu(buf->can_clk);
}

static int nct6694_canfd_probe(struct platform_device *pdev)
{
	const struct mfd_cell *cell = mfd_get_cell(pdev);
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	struct nct6694_canfd_priv *priv;
	struct net_device *ndev;
	int ret, irq, can_clk;

	irq = irq_create_mapping(nct6694->domain,
				 NCT6694_IRQ_CAN1 + cell->id);
	if (!irq)
		return -EINVAL;

	ndev = alloc_candev(sizeof(struct nct6694_canfd_priv), 1);
	if (!ndev)
		return -ENOMEM;

	ndev->irq = irq;
	ndev->flags |= IFF_ECHO;
	ndev->netdev_ops = &nct6694_canfd_netdev_ops;

	priv = netdev_priv(ndev);
	priv->nct6694 = nct6694;
	priv->ndev = ndev;

	priv->tx_buf = devm_kcalloc(&pdev->dev, NCT6694_MAX_PACKET_SZ,
				    sizeof(unsigned char), GFP_KERNEL);
	if (!priv->tx_buf) {
		ret = -ENOMEM;
		goto free_candev;
	}

	priv->rx_buf = devm_kcalloc(&pdev->dev, NCT6694_MAX_PACKET_SZ,
				    sizeof(unsigned char), GFP_KERNEL);
	if (!priv->rx_buf) {
		ret = -ENOMEM;
		goto free_candev;
	}

	can_clk = nct6694_canfd_get_clock(priv);
	if (can_clk < 0) {
		ret = -EIO;
		goto free_candev;
	}

	mutex_init(&priv->lock);
	INIT_WORK(&priv->tx_work, nct6694_canfd_tx_work);

	priv->can_idx = cell->id;
	priv->can.state = CAN_STATE_STOPPED;
	priv->can.clock.freq = can_clk;
	priv->can.bittiming_const = &nct6694_canfd_bittiming_nominal_const;
	priv->can.data_bittiming_const = &nct6694_canfd_bittiming_data_const;
	priv->can.do_set_mode = nct6694_canfd_set_mode;
	priv->can.do_get_berr_counter = nct6694_canfd_get_berr_counter;

	priv->can.ctrlmode = CAN_CTRLMODE_FD;

	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK		|
				       CAN_CTRLMODE_LISTENONLY		|
				       CAN_CTRLMODE_FD			|
				       CAN_CTRLMODE_FD_NON_ISO		|
				       CAN_CTRLMODE_BERR_REPORTING;

	platform_set_drvdata(pdev, priv);
	SET_NETDEV_DEV(priv->ndev, &pdev->dev);

	ret = register_candev(priv->ndev);
	if (ret)
		goto free_candev;

	return 0;

free_candev:
	free_candev(ndev);
	return dev_err_probe(&pdev->dev, ret, "Probe failed\n");
}

static void nct6694_canfd_remove(struct platform_device *pdev)
{
	struct nct6694_canfd_priv *priv = platform_get_drvdata(pdev);

	cancel_work_sync(&priv->tx_work);
	unregister_candev(priv->ndev);
	free_candev(priv->ndev);
}

static struct platform_driver nct6694_canfd_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_canfd_probe,
	.remove		= nct6694_canfd_remove,
};

module_platform_driver(nct6694_canfd_driver);

MODULE_DESCRIPTION("USB-CAN FD driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
