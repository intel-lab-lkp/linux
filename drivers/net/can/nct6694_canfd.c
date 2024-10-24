// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 Socket CANfd driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/can/dev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/mfd/core.h>
#include <linux/platform_device.h>
#include <linux/mfd/nct6694.h>

#define DRVNAME "nct6694-can"

#define RX_QUATA        64

/* Host interface */
#define REQUEST_CAN_MOD			0x05

/* Message Channel*/
/* Command 00h */
#define REQUEST_CAN_CMD0_LEN		0x18
#define REQUEST_CAN_CMD0_OFFSET(idx)	(idx ? 0x0100 : 0x0000)
#define CAN_NBR_IDX			0x00
#define CAN_DBR_IDX			0x04
#define CAN_CTRL1_IDX			0x0C
#define CAN_CTRL1_MON			BIT(0)
#define CAN_CTRL1_NISO			BIT(1)
#define CAN_CTRL1_LBCK			BIT(2)

/* Command 01h */
#define REQUEST_CAN_CMD1_LEN		0x08
#define REQUEST_CAN_CMD1_OFFSET		0x0001
#define CAN_CLK_IDX			0x04
#define CAN_CLK_LEN			0x04

/* Command 02h */
#define REQUEST_CAN_CMD2_LEN            0x10
#define REQUEST_CAN_CMD2_OFFSET(idx, mask)			\
	({ typeof(mask) mask_ = (mask);				\
	   idx ? ((0x80 | (mask_ & 0xFF)) << 8 | 0x02) :	\
		 ((0x00 | (mask_ & 0xFF)) << 8 | 0x02); })

#define CAN_ERR_IDX(idx)		(idx ? 0x08 : 0x00)	/* Read-clear */
#define CAN_STATUS_IDX(idx)		(idx ? 0x09 : 0x01)
#define CAN_TX_EVT_IDX(idx)		(idx ? 0x0A : 0x02)
#define CAN_RX_EVT_IDX(idx)		(idx ? 0x0B : 0x03)
#define CAN_REC_IDX(idx)		(idx ? 0x0C : 0x04)
#define CAN_TEC_IDX(idx)		(idx ? 0x0D : 0x05)
#define CAN_EVENT_ERR			BIT(0)
#define CAN_EVENT_STATUS		BIT(1)
#define CAN_EVENT_TX_EVT		BIT(2)
#define CAN_EVENT_RX_EVT		BIT(3)
#define CAN_EVENT_REC			BIT(4)
#define CAN_EVENT_TEC			BIT(5)
#define CAN_EVENT_ERR_NO_ERROR		0x00	/* Read-clear */
#define CAN_EVENT_ERR_CRC_ERROR		0x01	/* Read-clear */
#define CAN_EVENT_ERR_STUFF_ERROR	0x02	/* Read-clear */
#define CAN_EVENT_ERR_ACK_ERROR		0x03	/* Read-clear */
#define CAN_EVENT_ERR_FORM_ERROR	0x04	/* Read-clear */
#define CAN_EVENT_ERR_BIT_ERROR		0x05	/* Read-clear */
#define CAN_EVENT_ERR_TIMEOUT_ERROR	0x06	/* Read-clear */
#define CAN_EVENT_ERR_UNKNOWN_ERROR	0x07	/* Read-clear */
#define CAN_EVENT_STATUS_ERROR_ACTIVE	0x00
#define CAN_EVENT_STATUS_ERROR_PASSIVE	0x01
#define CAN_EVENT_STATUS_BUS_OFF	0x02
#define CAN_EVENT_STATUS_WARNING	0x03
#define CAN_EVENT_TX_EVT_TX_FIFO_EMPTY	BIT(7)	/* Read-clear */
#define CAN_EVENT_RX_EVT_DATA_LOST	BIT(5)	/* Read-clear */
#define CAN_EVENT_RX_EVT_HALF_FULL	BIT(6)	/* Read-clear */
#define CAN_EVENT_RX_EVT_DATA_IN	BIT(7)

/* Command 10h */
#define REQUEST_CAN_CMD10_LEN		0x90
#define REQUEST_CAN_CMD10_OFFSET(buf_cnt)	\
	(((buf_cnt) & 0xFF) << 8 | 0x10)
#define CAN_TAG_IDX			0x00
#define CAN_FLAG_IDX			0x01
#define CAN_DLC_IDX			0x03
#define CAN_ID_IDX			0x04
#define CAN_DATA_IDX			0x08
#define CAN_TAG_CAN0			0xC0
#define CAN_TAG_CAN1			0xC1
#define CAN_FLAG_EFF			BIT(0)
#define CAN_FLAG_RTR			BIT(1)
#define CAN_FLAG_FD			BIT(2)
#define CAN_FLAG_BRS			BIT(3)
#define CAN_FLAG_ERR			BIT(4)

/* Command 11h */
#define REQUEST_CAN_CMD11_LEN		0x90
#define REQUEST_CAN_CMD11_OFFSET(idx, buf_cnt)			\
	({ typeof(buf_cnt) buf_cnt_ = (buf_cnt);		\
	   idx ? ((0x80 | (buf_cnt_ & 0xFF)) << 8 | 0x11) :	\
		 ((0x00 | (buf_cnt_ & 0xFF)) << 8 | 0x11); })

struct nct6694_canfd_priv {
	struct can_priv can;	/* must be the first member */

	struct nct6694 *nct6694;
	struct net_device *ndev;
	struct work_struct rx_work;
	struct work_struct tx_work;
	unsigned char data_buf[REQUEST_CAN_CMD10_LEN];
	unsigned char can_idx;
};

static inline void set_buf16(void *buf, u16 u16_val)
{
	u8 *p = (u8 *)buf;

	p[0] = u16_val & 0xFF;
	p[1] = (u16_val >> 8) & 0xFF;
}

static inline void set_buf32(void *buf, u32 u32_val)
{
	u8 *p = (u8 *)buf;

	p[0] = u32_val & 0xFF;
	p[1] = (u32_val >> 8) & 0xFF;
	p[2] = (u32_val >> 16) & 0xFF;
	p[3] = (u32_val >> 24) & 0xFF;
}

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

static void nct6694_canfd_set_bittiming(struct net_device *ndev,
					unsigned char *buf)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	const struct can_bittiming *n_bt = &priv->can.bittiming;
	const struct can_bittiming *d_bt = &priv->can.data_bittiming;

	set_buf32(&buf[CAN_NBR_IDX], n_bt->bitrate);
	set_buf32(&buf[CAN_DBR_IDX], d_bt->bitrate);

	pr_info("%s: can(%d): NBR = %d, DBR = %d\n", __func__, priv->can_idx,
		n_bt->bitrate, d_bt->bitrate);
}

static int nct6694_canfd_start(struct net_device *ndev)
{
	int ret;
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	unsigned char buf[REQUEST_CAN_CMD0_LEN] = {0};
	u16 temp = 0;

	nct6694_canfd_set_bittiming(ndev, buf);

	if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		temp |= CAN_CTRL1_MON;

	if ((priv->can.ctrlmode & CAN_CTRLMODE_FD) &&
	    priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO)
		temp |= CAN_CTRL1_NISO;

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		temp |= CAN_CTRL1_LBCK;

	set_buf16(&buf[CAN_CTRL1_IDX], temp);

	ret = nct6694_write_msg(priv->nct6694, REQUEST_CAN_MOD,
				REQUEST_CAN_CMD0_OFFSET(priv->can_idx),
				REQUEST_CAN_CMD0_LEN, buf);
	if (ret < 0) {
		pr_err("%s: Failed to set data bittiming\n", __func__);
		return ret;
	}

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

	return 0;
}

static void nct6694_canfd_stop(struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);

	priv->can.state = CAN_STATE_STOPPED;
}

static int nct6694_canfd_set_mode(struct net_device *ndev, enum can_mode mode)
{
	switch (mode) {
	case CAN_MODE_START:
		nct6694_canfd_start(ndev);
		netif_wake_queue(ndev);
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int nct6694_canfd_get_berr_counter(const struct net_device *ndev,
					  struct can_berr_counter *bec)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	unsigned char mask = CAN_EVENT_REC | CAN_EVENT_TEC;
	unsigned char buf[REQUEST_CAN_CMD2_LEN] = {0};
	int ret;

	ret = nct6694_read_msg(priv->nct6694, REQUEST_CAN_MOD,
			       REQUEST_CAN_CMD2_OFFSET(priv->can_idx, mask),
			       REQUEST_CAN_CMD2_LEN, 0x00,
			       REQUEST_CAN_CMD2_LEN, (unsigned char *)&buf);
	if (ret < 0)
		return -EINVAL;

	bec->rxerr = buf[CAN_REC_IDX(priv->can_idx)];
	bec->txerr = buf[CAN_TEC_IDX(priv->can_idx)];

	return 0;
}

static int nct6694_canfd_open(struct net_device *ndev)
{
	int ret;

	ret = open_candev(ndev);
	if (ret)
		return ret;

	ret = nct6694_canfd_start(ndev);
	if (ret) {
		close_candev(ndev);
		return ret;
	}

	netif_start_queue(ndev);

	return 0;
}

static int nct6694_canfd_close(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	nct6694_canfd_stop(ndev);
	close_candev(ndev);

	return 0;
}

static netdev_tx_t nct6694_canfd_start_xmit(struct sk_buff *skb,
					    struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct nct6694 *nct6694 = priv->nct6694;
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	struct net_device_stats *stats = &ndev->stats;
	int can_idx = priv->can_idx;
	u32 txid = 0;
	int i;
	unsigned int echo_byte;
	u8 data_buf[REQUEST_CAN_CMD10_LEN] = {0};

	if (can_dropped_invalid_skb(ndev, skb))
		return NETDEV_TX_OK;

	/*
	 * No check for NCT66794 because the TX bit is read-clear
	 * and may be read-cleared by other function
	 * Just check the result of tx command.
	 */
	/* Check if the TX buffer is full */
	netif_stop_queue(ndev);

	if (can_idx == 0)
		data_buf[CAN_TAG_IDX] = CAN_TAG_CAN0;
	else
		data_buf[CAN_TAG_IDX] = CAN_TAG_CAN1;

	if (cf->can_id & CAN_EFF_FLAG) {
		txid = cf->can_id & CAN_EFF_MASK;
		/*
		 * In case the Extended ID frame is transmitted, the
		 * standard and extended part of the ID are swapped
		 * in the register, so swap them back to send the
		 * correct ID.
		 */
		data_buf[CAN_FLAG_IDX] |= CAN_FLAG_EFF;
	} else {
		txid = cf->can_id & CAN_SFF_MASK;
	}

	set_buf32(&data_buf[CAN_ID_IDX], txid);

	data_buf[CAN_DLC_IDX] = cf->len;

	if ((priv->can.ctrlmode & CAN_CTRLMODE_FD) && can_is_canfd_skb(skb)) {
		data_buf[CAN_FLAG_IDX] |= CAN_FLAG_FD;
		if (cf->flags & CANFD_BRS)
			data_buf[CAN_FLAG_IDX] |= CAN_FLAG_BRS;
	}

	if (cf->can_id & CAN_RTR_FLAG)
		data_buf[CAN_FLAG_IDX] |= CAN_FLAG_RTR;

	/* set data to buf */
	for (i = 0; i < cf->len; i++)
		data_buf[CAN_DATA_IDX + i] = *(u8 *)(cf->data + i);

	can_put_echo_skb(skb, ndev, 0, 0);

	memcpy(priv->data_buf, data_buf, REQUEST_CAN_CMD10_LEN);
	queue_work(nct6694->async_workqueue, &priv->tx_work);

	stats->tx_bytes += cf->len;
	stats->tx_packets++;
	echo_byte = can_get_echo_skb(ndev, 0, NULL);

	netif_wake_queue(ndev);

	return NETDEV_TX_OK;
}

static void nct6694_canfd_tx_work(struct work_struct *work)
{
	struct nct6694_canfd_priv *priv;

	priv = container_of(work, struct nct6694_canfd_priv, tx_work);

	nct6694_write_msg(priv->nct6694, REQUEST_CAN_MOD,
			  REQUEST_CAN_CMD10_OFFSET(1),
			  REQUEST_CAN_CMD10_LEN,
			  priv->data_buf);
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

	pr_info("%s: CAN_ERR_CRTL_RX_OVERFLOW\r\n", __func__);

	frame->can_id |= CAN_ERR_CRTL;
	frame->data[1] = CAN_ERR_CRTL_RX_OVERFLOW;

	netif_receive_skb(skb);

	return 1;
}

static void nuv_canfd_read_fifo(struct net_device *ndev)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct canfd_frame *cf;
	struct sk_buff *skb;
	int can_idx = priv->can_idx;
	u32 id;
	int ret;
	u8 data_buf[REQUEST_CAN_CMD11_LEN] = {0};
	u8 fd_format = 0;
	int i;

	ret = nct6694_read_msg(priv->nct6694, REQUEST_CAN_MOD,
			       REQUEST_CAN_CMD11_OFFSET(can_idx, 1),
			       REQUEST_CAN_CMD11_LEN, 0, REQUEST_CAN_CMD11_LEN,
			       data_buf);
	if (ret < 0)
		return;

	/* Check type of frame and create skb */
	fd_format = data_buf[CAN_FLAG_IDX] & CAN_FLAG_FD;
	if (fd_format)
		skb = alloc_canfd_skb(ndev, &cf);
	else
		skb = alloc_can_skb(ndev, (struct can_frame **)&cf);

	if (!skb) {
		stats->rx_dropped++;
		return;
	}

	cf->len = data_buf[CAN_DLC_IDX];

	/* Get ID and set flag by its type(Standard ID format or Ext ID format) */
	id = le32_to_cpu(*(u32 *)(&data_buf[CAN_ID_IDX]));
	if (data_buf[CAN_FLAG_IDX] & CAN_FLAG_EFF) {
		/*
		 * In case the Extended ID frame is received, the standard
		 * and extended part of the ID are swapped in the register,
		 * so swap them back to obtain the correct ID.
		 */
		id |= CAN_EFF_FLAG;
	}

	cf->can_id = id;

	/* Set ESI flag */
	if (data_buf[CAN_FLAG_IDX] & CAN_FLAG_ERR) {
		cf->flags |= CANFD_ESI;
		netdev_dbg(ndev, "ESI Error\n");
	}

	/* Set RTR and BRS */
	if (!fd_format && (data_buf[CAN_FLAG_IDX] & CAN_FLAG_RTR)) {
		cf->can_id |= CAN_RTR_FLAG;
	} else {
		if (data_buf[CAN_FLAG_IDX] & CAN_FLAG_BRS)
			cf->flags |= CANFD_BRS;

		for (i = 0; i < cf->len; i++)
			*(u8 *)(cf->data + i) = data_buf[CAN_DATA_IDX + i];
	}

	/* Remove the packet from FIFO */
	stats->rx_packets++;
	stats->rx_bytes += cf->len;
	netif_receive_skb(skb);
}

static int nct6694_canfd_do_rx_poll(struct net_device *ndev, int quota)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	int can_idx = priv->can_idx;
	u32 pkts = 0;
	u8 data_buf_can_evt[REQUEST_CAN_CMD2_LEN] = {0};
	u8 mask_rx = CAN_EVENT_RX_EVT;
	u8 rx_evt;

	for (;;) {
		nct6694_read_msg(priv->nct6694, REQUEST_CAN_MOD,
				 REQUEST_CAN_CMD2_OFFSET(can_idx, mask_rx),
				 REQUEST_CAN_CMD2_LEN, 0,
				 REQUEST_CAN_CMD2_LEN, data_buf_can_evt);

		/* Handle lost messages when handling RX because it is read-cleared reg */
		rx_evt = data_buf_can_evt[CAN_RX_EVT_IDX(can_idx)];
		if (rx_evt & CAN_EVENT_RX_EVT_DATA_LOST)
			nuv_canfd_handle_lost_msg(ndev);

		/* No data */
		if ((rx_evt & CAN_EVENT_RX_EVT_DATA_IN) == 0)
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

	if (bus_err == CAN_EVENT_ERR_NO_ERROR)
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
	case CAN_EVENT_ERR_CRC_ERROR:
		cf->data[3] = CAN_ERR_PROT_LOC_CRC_SEQ;
		break;

	case CAN_EVENT_ERR_STUFF_ERROR:
		cf->data[2] |= CAN_ERR_PROT_STUFF;
		break;

	case CAN_EVENT_ERR_ACK_ERROR:
		cf->data[3] = CAN_ERR_PROT_LOC_ACK;
		break;

	case CAN_EVENT_ERR_FORM_ERROR:
		cf->data[2] |= CAN_ERR_PROT_FORM;
		break;

	case CAN_EVENT_ERR_BIT_ERROR:
		cf->data[2] |= CAN_ERR_PROT_BIT |
			       CAN_ERR_PROT_BIT0 |
			       CAN_ERR_PROT_BIT1;
		break;

	case CAN_EVENT_ERR_TIMEOUT_ERROR:
		cf->data[2] |= CAN_ERR_PROT_UNSPEC;
		break;

	case CAN_EVENT_ERR_UNKNOWN_ERROR:
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
					     u8 *can_evt_buf)
{
	struct nct6694_canfd_priv *priv = netdev_priv(ndev);
	int can_idx = priv->can_idx;
	int work_done = 0;
	u8 can_status;

	can_status = can_evt_buf[CAN_STATUS_IDX(can_idx)];

	if (can_status == CAN_EVENT_STATUS_ERROR_ACTIVE &&
	    priv->can.state != CAN_STATE_ERROR_ACTIVE) {
		netdev_dbg(ndev, "Error, entered active state\n");
		work_done += nct6694_canfd_handle_state_change(ndev,
							       CAN_STATE_ERROR_ACTIVE);
	}

	if (can_status == CAN_EVENT_STATUS_WARNING &&
	    priv->can.state != CAN_STATE_ERROR_WARNING) {
		netdev_dbg(ndev, "Error, entered warning state\n");
		work_done += nct6694_canfd_handle_state_change(ndev,
							       CAN_STATE_ERROR_WARNING);
	}

	if (can_status == CAN_EVENT_STATUS_ERROR_PASSIVE &&
	    priv->can.state != CAN_STATE_ERROR_PASSIVE) {
		netdev_dbg(ndev, "Error, entered passive state\n");
		work_done += nct6694_canfd_handle_state_change(ndev,
							       CAN_STATE_ERROR_PASSIVE);
	}

	if (can_status == CAN_EVENT_STATUS_BUS_OFF &&
	    priv->can.state != CAN_STATE_BUS_OFF) {
		netdev_dbg(ndev, "Error, entered bus-off state\n");
		work_done += nct6694_canfd_handle_state_change(ndev,
							       CAN_STATE_BUS_OFF);
	}

	return work_done;
}

static void nct6694_canfd_rx_work(struct work_struct *work)
{
	struct nct6694_canfd_priv *priv;
	struct net_device *ndev;
	struct net_device_stats *stats;
	int ret, can_idx;
	int work_done = 0;
	int quota = RX_QUATA;
	u8 data_buf_can_evt[REQUEST_CAN_CMD2_LEN] = {0};
	u8 bus_err;
	u8 mask_sts = CAN_EVENT_ERR | CAN_EVENT_STATUS;

	priv = container_of(work, struct nct6694_canfd_priv, rx_work);
	ndev = priv->ndev;
	can_idx = priv->can_idx;
	stats = &ndev->stats;

	ret = nct6694_read_msg(priv->nct6694, REQUEST_CAN_MOD,
			       REQUEST_CAN_CMD2_OFFSET(can_idx, mask_sts),
			       REQUEST_CAN_CMD2_LEN, 0, REQUEST_CAN_CMD2_LEN,
			       data_buf_can_evt);
	if (ret < 0)
		return;

	/* Handle bus state changes */
	work_done += nct6694_canfd_handle_state_errors(ndev, data_buf_can_evt);

	/* Handle lec errors on the bus */
	if (priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING) {
		bus_err = data_buf_can_evt[CAN_ERR_IDX(can_idx)];
		work_done += nct6694_canfd_handle_lec_err(ndev, bus_err);
	}

	/*
	 * Check data lost and handle normal messages on RX.
	 * Don't check rx fifo-empty here because the data-lost bit in the same reg is read-cleared,
	 * we handle it when handling rx event
	 */

	work_done += nct6694_canfd_do_rx_poll(ndev, quota - work_done);
}

static void nct6694_can_handler(void *private_data)
{
	struct nct6694_canfd_priv *priv = private_data;
	struct nct6694 *nct6694 = priv->nct6694;

	queue_work(nct6694->async_workqueue, &priv->rx_work);
}

static const struct net_device_ops nct6694_canfd_netdev_ops = {
	.ndo_open = nct6694_canfd_open,
	.ndo_stop = nct6694_canfd_close,
	.ndo_start_xmit = nct6694_canfd_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

static int nct6694_canfd_probe(struct platform_device *pdev)
{
	const struct mfd_cell *cell = mfd_get_cell(pdev);
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	struct nct6694_canfd_priv *priv;
	struct net_device *ndev;
	unsigned int can_clk;
	int ret;

	ret = nct6694_read_msg(nct6694, REQUEST_CAN_MOD,
			       REQUEST_CAN_CMD1_OFFSET,
			       REQUEST_CAN_CMD1_LEN,
			       CAN_CLK_IDX, CAN_CLK_LEN,
			       (unsigned char *)&can_clk);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: Failed to get can clock frequency: %pe\n",
			__func__, ERR_PTR(ret));
		return ret;
	}

	pr_info("can_clk = %d\n", le32_to_cpu(can_clk));

	ndev = alloc_candev(sizeof(struct nct6694_canfd_priv), 1);
	if (!ndev)
		return -ENOMEM;

	ndev->flags |= IFF_ECHO;
	ndev->netdev_ops = &nct6694_canfd_netdev_ops;

	priv = netdev_priv(ndev);
	priv->nct6694 = nct6694;
	priv->ndev = ndev;

	ret = nct6694_register_handler(nct6694, CAN_IRQ_STATUS,
				       nct6694_can_handler, priv);
	if (ret) {
		dev_err(&pdev->dev, "%s:  Failed to register handler: %pe\n",
			__func__, ERR_PTR(ret));
		free_candev(ndev);
		return ret;
	}

	INIT_WORK(&priv->rx_work, nct6694_canfd_rx_work);
	INIT_WORK(&priv->tx_work, nct6694_canfd_tx_work);

	priv->can_idx = cell->id;
	priv->can.state = CAN_STATE_STOPPED;
	priv->can.clock.freq = le32_to_cpu(can_clk);
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
	if (ret) {
		dev_err(&pdev->dev, "register_candev failed: %d\n", ret);
		free_candev(ndev);
		return ret;
	}

	return 0;
}

static void nct6694_canfd_remove(struct platform_device *pdev)
{
	struct nct6694_canfd_priv *priv = platform_get_drvdata(pdev);

	cancel_work_sync(&priv->rx_work);
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

static int __init nct6694_init(void)
{
	int err;

	err = platform_driver_register(&nct6694_canfd_driver);
	if (!err) {
		if (err)
			platform_driver_unregister(&nct6694_canfd_driver);
	}

	return err;
}
subsys_initcall(nct6694_init);

static void __exit nct6694_exit(void)
{
	platform_driver_unregister(&nct6694_canfd_driver);
}
module_exit(nct6694_exit);

MODULE_DESCRIPTION("USB-CAN FD driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
