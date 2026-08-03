// SPDX-License-Identifier: GPL-2.0
/*
 * Arasan CANFD driver
 *
 * Copyright (C) 2026 Synaptics Incorporated
 *
 * Author: Jisheng Zhang <jszhang@kernel.org>
 *
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/phy/phy.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/u64_stats_sync.h>

#define INTERRUPT_MASK0			0x3fc
#define INTERRUPT_MASK1			0x400
#define OPERATIONAL			0x404
#define  OPERATIONAL_EN			BIT(0)
#define  LOOPBACK			BIT(7)
#define CONTROL				0x408
#define  SOFTRESET			BIT(0)
#define  DONTCHECKID			BIT(4)
#define  DISABLE_ECC			BIT(5)
#define  LVL_ISOFD			BIT(6)
#define  DMA_SOFTRESET			BIT(20)
#define STATUS				0x40c
#define  STATUS_REC_MASK		GENMASK(9, 0)
#define  STATUS_TEC_MASK		GENMASK(19, 10)
#define HITXFIFO			0x410
#define LOTXFIFO			0x414
#define LVL_CLASSIC_BAUD		0x424
#define LVL_FD_BAUD			0x428
#define LVL_ALLOWABLE_CLASSIC_JUMP	0x42c
#define LVL_ALLOWABLE_FD_JUMP		0x430
#define LVL_CLASSIC_END_OF_SYNC_SEG	0x434
#define LVL_FD_END_OF_SYNC_SEG		0x438
#define LVL_CLASSIC_END_OF_PROP_SEG	0x43c
#define LVL_FD_END_OF_PROP_SEG		0x440
#define LVL_CLASSIC_END_OF_PHASE_SEG1	0x444
#define LVL_FD_END_OF_PHASE_SEG1	0x448
#define LVL_TUR_INCREMENT		0x450
#define INTERRUPTS_FROM_HW		0x48c
#define  OVERLOAD			BIT(0)
#define  GOOD_RX_FRAME			BIT(1)
#define  TX_RETRANSMIT			BIT(2)
#define  ERROR_RX_FRAME			BIT(6)
#define  TX_FIFO_EMPTY			BIT(8)
#define  USED_BUFFERS_NOT_EMPTY		BIT(11)
#define  CL_NO_ACK			BIT(12)
#define  FD_NO_ACK			BIT(13)
#define  CLTX_GOOD			BIT(14)
#define  FDTX_GOOD			BIT(15)
#define  BUS_STATE1			BIT(16)
#define  BUS_STATE2			BIT(17)
#define  PANICS				BIT(26)
#define BASE_ADDRESS			0x490
#define NUMBER_OF_PAIRS			0x494
#define WATCHDOG_LIMIT			0x498
#define SELF_ADDRESS			0x49c
#define ALLOWED_RETRANSMITS		0x4a0
#define RX_PTR_BUFS			0x4a4
#define RX_PTR_BUFS_COUNT		0x4a8
#define USED_BUFS_PTR			0x4ac
#define  TYPE_MASK			GENMASK(1, 0)
#define  TYPE_TX			1
#define  TYPE_RX			2
#define  TYPE_TX_ABORT			3
#define USED_BUFS_PTR_COUNT		0x4b0
#define LVL_SSP_DELAY			0x4d0
#define ACTUAL_SSP_DELAY		0x4d4
#define R_BASE_ADDR			0xf00
#define R_ECC_ADDR			0xf04
#define W_BASE_ADDR			0xf08
#define W_ECC_ADDR			0xf0c

#define CAN_MAX_RX_QUEUE		8
#define CAN_MAX_TX_QUEUE		8
#define CAN_MAX_FILTER			255
#define ARASAN_CANFD_NAPI_WEIGHT	8
#define DEFAULT_SSP			17

#define TX_HEADER0_ID			GENMASK(22, 12)
#define TX_HEADER0_ESI			BIT(24)
#define TX_HEADER0_BRS			BIT(25)
#define TX_HEADER0_RTR			BIT(26)
#define TX_HEADER0_IDE			BIT(27)
#define TX_HEADER0_TYPE_MSK		GENMASK(31, 28)
#define TX_HEADER0_TYPE_CL		1
#define TX_HEADER0_TYPE_FD		2

#define RX_HEADER0_TYPE_MSK		GENMASK(1, 0)
#define RX_HEADER0_TYPE_CL		1
#define RX_HEADER0_TYPE_FD		2
#define RX_HEADER0_B_EXT		BIT(2)
#define RX_HEADER0_B_RTR		BIT(3)
#define RX_HEADER0_STATUS_MSK		GENMASK(13, 4)
#define RX_HEADER0_DLC_MSK		GENMASK(24, 14)
#define RX_HEADER0_ID_0_6		GENMASK(31, 25)
#define RX_HEADER1_ID_7_10		GENMASK(3, 0)
#define RX_HEADER1_EXTID		GENMASK(21, 4)
#define RX_HEADER1_EXTID		GENMASK(21, 4)
#define RX_HEADER2_CL_RTR		BIT(8)
#define RX_HEADER2_CL_IDE		BIT(9)
#define RX_HEADER2_FD_IDE		BIT(18)
#define RX_HEADER2_FD_ESI		BIT(19)
#define RX_HEADER2_FD_BRS		BIT(20)

struct can_rx_filter {
	u32 mask;
	u32 id;
};

struct can_rx_frame {
	struct {
		u32 header0;
		u32 header1;
		u32 header2;
		u32 resv_0;
		u32 timestamp_l;
		u32 timestamp_h;
	} header;
	u8 data[CANFD_MAX_DLEN];
};

struct can_tx_frame {
	struct header {
		u32 header0;
		u32 ext_id;
		u32 resv_0;
	} header;
	u8 data[CANFD_MAX_DLEN];
};

struct can_read_buffer {
	struct can_tx_frame tx_frames[CAN_MAX_TX_QUEUE];
	struct can_rx_filter rx_filters[CAN_MAX_FILTER];
};

struct can_write_buffer {
	struct can_rx_frame rx_frames[CAN_MAX_RX_QUEUE];
};

struct can_ecc_buffer {
	struct {
		u8 tx_frames[CAN_MAX_TX_QUEUE][sizeof(struct can_tx_frame) / 4];
		u8 rx_filters[CAN_MAX_FILTER][sizeof(struct can_rx_filter) / 4];
	} read;

	struct {
		u8 rx_frames[CAN_MAX_RX_QUEUE][sizeof(struct can_rx_frame) / 4];
	} write;
};

struct can_buffer {
	struct can_read_buffer r_buffer;
	struct can_write_buffer w_buffer;
	struct can_ecc_buffer ecc_buffer;
};

struct arasan_canfd_priv {
	struct can_priv can;
	/* Lock for synchronizing TX handling */
	spinlock_t tx_lock;
	u32 txq_avail;
	struct napi_struct napi;
	struct device *dev;
	void __iomem *base;
	struct can_buffer *buf;
	dma_addr_t buf_dma;
	u32 ssp;
	struct clk *sys_clk;
	struct clk *can_clk;
	struct reset_control *sys_rst;
	struct reset_control *can_rst;
	struct phy *transceiver;
};

static const struct can_bittiming_const arasan_canfd_bittiming_const = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 256,
	.tseg2_min = 1,
	.tseg2_max = 128,
	.sjw_max = 128,
	.brp_min = 1,
	.brp_max = 65536,
	.brp_inc = 1,
};

static const struct can_bittiming_const arasan_canfd_data_bittiming_const = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 256,
	.tseg2_min = 1,
	.tseg2_max = 128,
	.sjw_max = 128,
	.brp_min = 1,
	.brp_max = 65536,
	.brp_inc = 1,
};

static const u32 ecc_syndrom[7] = {
	0xC14840FF,
	0x2124FF90,
	0x6CFF0808,
	0xFF01A444,
	0x16F092A6,
	0x101F7161,
	0x8A820F1B,
};

static inline int parity(unsigned int x)
{
	/*
	 * public domain code snippet, lifted from
	 * http://www-graphics.stanford.edu/~seander/bithacks.html
	 */
	x ^= x >> 1;
	x ^= x >> 2;
	x = (x & 0x11111111U) * 0x11111111U;
	return (x >> 28) & 1;
}

static u8 ecc_calc(u32 data)
{
	int i;
	u8 result = 0;

	for (i = 0; i < 7; i++) {
		if (parity(data & ecc_syndrom[i]))
			result |= BIT(i);
	}

	return result;
}

static inline u32 arasan_canfd_read(const struct arasan_canfd_priv *priv, u32 off)
{
	return readl_relaxed(priv->base + off);
}

static inline void arasan_canfd_write(const struct arasan_canfd_priv *priv, u32 off,
				      u32 value)
{
	writel_relaxed(value, priv->base + off);
}

static int arasan_canfd_set_bittiming(struct net_device *ndev)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	struct can_bittiming *bt = &priv->can.bittiming;
	struct can_bittiming *dbt = &priv->can.fd.data_bittiming;
	u32 cl_2, cl_4, fd_2, fd_4, classic_cycle, fd_cycle;

	if (arasan_canfd_read(priv, OPERATIONAL) & OPERATIONAL_EN) {
		netdev_alert(ndev, "BUG! Cannot set bittiming when CAN operational is enabled\n");
		return -EPERM;
	}

	arasan_canfd_write(priv, LVL_TUR_INCREMENT, 0x8000);

	classic_cycle = priv->can.clock.freq / bt->bitrate;
	cl_2 = classic_cycle / 2;
	cl_4 = classic_cycle / 4;
	arasan_canfd_write(priv, LVL_CLASSIC_BAUD, classic_cycle);
	arasan_canfd_write(priv, LVL_ALLOWABLE_CLASSIC_JUMP, cl_2);
	arasan_canfd_write(priv, LVL_CLASSIC_END_OF_SYNC_SEG, cl_4);
	arasan_canfd_write(priv, LVL_CLASSIC_END_OF_PROP_SEG, cl_2 - 1);
	arasan_canfd_write(priv, LVL_CLASSIC_END_OF_PHASE_SEG1,
			   classic_cycle * bt->sample_point / 1000);

	fd_cycle = priv->can.clock.freq / dbt->bitrate;
	fd_2 = fd_cycle / 2;
	fd_4 = fd_cycle / 4;
	arasan_canfd_write(priv, LVL_FD_BAUD, fd_cycle);
	arasan_canfd_write(priv, LVL_ALLOWABLE_FD_JUMP, fd_2);
	arasan_canfd_write(priv, LVL_FD_END_OF_SYNC_SEG, fd_4);
	arasan_canfd_write(priv, LVL_FD_END_OF_PROP_SEG, fd_2 - 1);
	arasan_canfd_write(priv, LVL_FD_END_OF_PHASE_SEG1, fd_cycle * dbt->sample_point / 1000);

	if (dbt->bitrate > 1000000)
		arasan_canfd_write(priv, LVL_SSP_DELAY, priv->ssp);

	return 0;
}

static int arasan_canfd_chip_start(struct net_device *ndev)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	u32 ier, ctrl, op = OPERATIONAL_EN;
	int ret;

	ret = arasan_canfd_set_bittiming(ndev);
	if (ret < 0)
		return ret;

	ier = GENMASK(31, 0);
	ier &= ~(GOOD_RX_FRAME | TX_FIFO_EMPTY | CLTX_GOOD | FDTX_GOOD | PANICS);
	arasan_canfd_write(priv, INTERRUPT_MASK0, ier);

	/* loopback? */
	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		op |= LOOPBACK;

	ctrl = arasan_canfd_read(priv, CONTROL);
	ctrl |= DONTCHECKID;
	arasan_canfd_write(priv, CONTROL, ctrl);

	arasan_canfd_write(priv, OPERATIONAL, op);

	priv->txq_avail = GENMASK(CAN_MAX_TX_QUEUE - 1, 0);
	priv->can.state = CAN_STATE_ERROR_ACTIVE;
	return 0;
}

/**
 * arasan_canfd_do_set_mode - This sets the mode of the driver
 * @ndev:	Pointer to net_device structure
 * @mode:	Tells the mode of the driver
 *
 * This check the drivers state and calls the corresponding modes to set.
 *
 * Return: 0 on success and failure value on error
 */
static int arasan_canfd_do_set_mode(struct net_device *ndev, enum can_mode mode)
{
	int ret;

	switch (mode) {
	case CAN_MODE_START:
		ret = arasan_canfd_chip_start(ndev);
		if (ret < 0) {
			netdev_err(ndev, "arasan_canfd_chip_start failed!\n");
			return ret;
		}
		netif_wake_queue(ndev);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static void arasan_canfd_err_interrupt(struct net_device *ndev, u32 isr)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	u32 val = arasan_canfd_read(priv, STATUS);
	u32 txerr = FIELD_GET(STATUS_TEC_MASK, val);
	u32 rxerr = FIELD_GET(STATUS_REC_MASK, val);
	struct can_frame cf = { };

	if (isr & OVERLOAD) {
		cf.can_id |= CAN_ERR_PROT;
		cf.data[2] |= CAN_ERR_PROT_OVERLOAD;
	}

	if (isr & ERROR_RX_FRAME) {
		stats->rx_errors++;
		cf.data[2] |= CAN_ERR_PROT_FORM;
	}

	if (isr & TX_RETRANSMIT) {
		priv->can.can_stats.arbitration_lost++;
		cf.can_id |= CAN_ERR_LOSTARB;
		cf.data[0] |= CAN_ERR_LOSTARB_UNSPEC;
	}

	if (isr & (CL_NO_ACK | FD_NO_ACK)) {
		stats->tx_errors++;
		cf.can_id |= CAN_ERR_ACK;
		cf.data[3] |= CAN_ERR_PROT_LOC_ACK;
	}

	if (isr & BUS_STATE2) {
		priv->can.state = CAN_STATE_BUS_OFF;
		priv->can.can_stats.bus_off++;
		can_bus_off(ndev);
		cf.can_id |= CAN_ERR_BUSOFF;
	}

	if (isr & BUS_STATE1) {
		priv->can.state = CAN_STATE_ERROR_PASSIVE;
		priv->can.can_stats.error_passive++;
		cf.can_id |= CAN_ERR_CRTL | CAN_ERR_CNT;
		cf.data[1] = txerr > rxerr ? CAN_ERR_CRTL_TX_PASSIVE :
			CAN_ERR_CRTL_RX_PASSIVE;
		cf.data[6] = txerr;
		cf.data[7] = rxerr;
	}

	if (cf.can_id) {
		struct can_frame *skb_cf;
		struct sk_buff *skb = alloc_can_err_skb(ndev, &skb_cf);

		if (skb) {
			skb_cf->can_id |= cf.can_id;
			memcpy(skb_cf->data, cf.data, CAN_ERR_DLC);
			netif_rx(skb);
		}
	}
}

static void arasan_canfd_write_frame(struct net_device *ndev, struct sk_buff *skb, int idx)
{
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	struct can_tx_frame tx_frame;
	u32 *data = (u32 *)&tx_frame;
	u8 *ecc = priv->buf->ecc_buffer.read.tx_frames[idx];
	int i;

	tx_frame.header.header0 = can_fd_len2dlc(cf->len);

	if (cf->can_id & CAN_EFF_FLAG) {
		/* Extended CAN ID format */
		tx_frame.header.header0 |= TX_HEADER0_IDE;
		tx_frame.header.header0 |= FIELD_PREP(TX_HEADER0_ID,
				(cf->can_id & CAN_EFF_MASK) >> 18);
		tx_frame.header.ext_id = cf->can_id;
	} else {
		/* Standard CAN ID format */
		tx_frame.header.header0 |= FIELD_PREP(TX_HEADER0_ID, cf->can_id & CAN_SFF_MASK);
		tx_frame.header.ext_id = 0;
	}

	if (cf->can_id & CAN_RTR_FLAG)
		tx_frame.header.header0 |= TX_HEADER0_RTR;

	if (can_is_canfd_skb(skb)) {
		/* CAN FD frame format */
		tx_frame.header.header0 |= FIELD_PREP(TX_HEADER0_TYPE_MSK, TX_HEADER0_TYPE_FD);

		if (cf->flags & CANFD_BRS)
			tx_frame.header.header0 |= TX_HEADER0_BRS;

		if (cf->flags & CANFD_ESI)
			tx_frame.header.header0 |= TX_HEADER0_ESI;

	} else {
		/* CAN 2.0 frame format */
		tx_frame.header.header0 |= FIELD_PREP(TX_HEADER0_TYPE_MSK, TX_HEADER0_TYPE_CL);
	}

	memcpy(tx_frame.data, cf->data, cf->len);
	priv->buf->r_buffer.tx_frames[idx] = tx_frame;

	for (i = 0; i < sizeof(tx_frame) / sizeof(u32); i++)
		ecc[i] = ecc_calc(data[i]);

	arasan_canfd_write(priv, HITXFIFO,
			   priv->buf_dma + offsetof(struct can_buffer, r_buffer.tx_frames[idx]));

	can_put_echo_skb(skb, ndev, idx % CAN_MAX_TX_QUEUE, 0);
}

static int arasan_canfd_start_xmit_queue(struct sk_buff *skb, struct net_device *ndev)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	int idx;

	guard(spinlock)(&priv->tx_lock);

	idx = ffs(priv->txq_avail);
	if (unlikely(!idx))
		return -ENOSPC;

	--idx;
	priv->txq_avail &= ~(1 << idx);
	arasan_canfd_write_frame(ndev, skb, idx);

	/* Check if the TX queue is full */
	if (!priv->txq_avail)
		netif_stop_queue(ndev);

	return 0;
}

static netdev_tx_t arasan_canfd_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	int ret;

	if (can_dev_dropped_skb(ndev, skb))
		return NETDEV_TX_OK;

	ret = arasan_canfd_start_xmit_queue(skb, ndev);
	if (ret < 0) {
		netdev_err(ndev, "BUG!, TX full when queue awake!\n");
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	return NETDEV_TX_OK;
}

static int arasan_canfd_tx(struct net_device *ndev, unsigned long addr)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	int idx;

	guard(spinlock)(&priv->tx_lock);

	idx = (addr - priv->buf_dma - offsetof(struct can_buffer, r_buffer.tx_frames)) /
		sizeof(struct can_tx_frame);
	if (idx > CAN_MAX_TX_QUEUE)
		return 0;

	stats->tx_bytes += can_get_echo_skb(ndev, idx % CAN_MAX_TX_QUEUE, NULL);
	stats->tx_packets++;
	priv->txq_avail |= (1 << idx);

	netif_wake_queue(ndev);

	return 1;
}

static int arasan_canfd_rx(struct net_device *ndev, unsigned long addr)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct canfd_frame *cf;
	struct sk_buff *skb;
	u32 header0, header1, header2;
	struct can_rx_frame *rx_frame;
	int idx, type;

	idx = (addr - priv->buf_dma - offsetof(struct can_buffer, w_buffer.rx_frames)) /
		sizeof(struct can_rx_frame);
	if (idx > CAN_MAX_RX_QUEUE)
		return 0;

	rx_frame = &priv->buf->w_buffer.rx_frames[idx];
	header0 = rx_frame->header.header0;
	header1 = rx_frame->header.header1;
	header2 = rx_frame->header.header2;

	type = FIELD_GET(RX_HEADER0_TYPE_MSK, header0);
	if (type == RX_HEADER0_TYPE_FD)
		skb = alloc_canfd_skb(ndev, &cf);
	else
		skb = alloc_can_skb(ndev, (struct can_frame **)&cf);

	if (unlikely(!skb)) {
		stats->rx_dropped++;
		return 0;
	}

	if (type == RX_HEADER0_TYPE_FD) {
		cf->len = can_fd_dlc2len(FIELD_GET(RX_HEADER0_DLC_MSK, header0));

		if (header2 & RX_HEADER2_FD_BRS)
			cf->flags |= CANFD_BRS;

		if (header2 & RX_HEADER2_FD_ESI)
			cf->flags |= CANFD_ESI;
	} else {
		cf->len = can_cc_dlc2len(FIELD_GET(RX_HEADER0_DLC_MSK, header0));
	}

	if (header0 & RX_HEADER0_B_EXT) {
		/* The received frame is an Extended format frame */
		cf->can_id = FIELD_GET(RX_HEADER1_EXTID, header1);
		cf->can_id |= (FIELD_GET(RX_HEADER0_ID_0_6, header0) |
				(FIELD_GET(RX_HEADER1_ID_7_10, header1) << 7) << 18);
		cf->can_id |= CAN_EFF_FLAG;
	} else {
		/* The received frame is a standard format frame */
		cf->can_id = FIELD_GET(RX_HEADER0_ID_0_6, header0) |
				(FIELD_GET(RX_HEADER1_ID_7_10, header1) << 7);
	}
	if (header0 & RX_HEADER0_B_RTR)
		cf->can_id |= CAN_RTR_FLAG;

	memcpy(cf->data, rx_frame->data, cf->len);

	if (!(cf->can_id & CAN_RTR_FLAG))
		stats->rx_bytes += cf->len;
	stats->rx_packets++;

	netif_receive_skb(skb);

	return 1;
}

static int arasan_canfd_poll(struct napi_struct *napi, int quota)
{
	struct net_device *ndev = napi->dev;
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	u32 buf_cnt, ier;
	int type, work_done = 0;
	unsigned long addr;

	while ((buf_cnt = arasan_canfd_read(priv, USED_BUFS_PTR_COUNT)) > 0 &&
	       (work_done < quota)) {
		addr = arasan_canfd_read(priv, USED_BUFS_PTR);
		type = FIELD_GET(TYPE_MASK, addr);
		addr &= ~TYPE_MASK;
		switch (type) {
		case TYPE_RX:
			work_done += arasan_canfd_rx(ndev, addr);
			arasan_canfd_write(priv, RX_PTR_BUFS, addr);
			break;
		case TYPE_TX:
			work_done += arasan_canfd_tx(ndev, addr);
			break;
		case TYPE_TX_ABORT:
			netdev_err(ndev, "tx abort\n");
			break;
		default:
			netdev_err(ndev, "Unknown type: %d\n", type);
			break;
		}
	}

	if (work_done < quota) {
		if (napi_complete_done(napi, work_done)) {
			ier = arasan_canfd_read(priv, INTERRUPT_MASK0);
			ier |= USED_BUFFERS_NOT_EMPTY;
			arasan_canfd_write(priv, INTERRUPT_MASK0, ier);
		}
	}
	return work_done;
}

static irqreturn_t arasan_canfd_isr(int irq, void *dev_id)
{
	struct net_device *ndev = (struct net_device *)dev_id;
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	u32 isr_errors, isr, ier;

	isr = arasan_canfd_read(priv, INTERRUPTS_FROM_HW);
	if (!isr)
		return IRQ_NONE;

	arasan_canfd_write(priv, INTERRUPTS_FROM_HW, isr);

	ier = GENMASK(31, 0);
	ier &= ~(GOOD_RX_FRAME | TX_FIFO_EMPTY | CLTX_GOOD | FDTX_GOOD | USED_BUFFERS_NOT_EMPTY);
	isr_errors = isr & ier;
	if (isr_errors)
		arasan_canfd_err_interrupt(ndev, isr_errors);

	if (isr & USED_BUFFERS_NOT_EMPTY) {
		ier = arasan_canfd_read(priv, INTERRUPT_MASK0);
		ier &= ~USED_BUFFERS_NOT_EMPTY;
		arasan_canfd_write(priv, INTERRUPT_MASK0, ier);
		napi_schedule(&priv->napi);
	}

	return IRQ_HANDLED;
}

static void arasan_canfd_chip_stop(struct net_device *ndev)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);

	arasan_canfd_write(priv, OPERATIONAL, 0);
	priv->can.state = CAN_STATE_STOPPED;
}

static int arasan_canfd_open(struct net_device *ndev)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	int ret;

	ret = phy_power_on(priv->transceiver);
	if (ret)
		return ret;

	ret = pm_runtime_get_sync(priv->dev);
	if (ret < 0) {
		netdev_err(ndev, "%s: pm_runtime_get failed(%d)\n", __func__, ret);
		goto err;
	}

	ret = request_irq(ndev->irq, arasan_canfd_isr, 0, ndev->name, ndev);
	if (ret < 0) {
		netdev_err(ndev, "Failed to request irq\n");
		goto err;
	}

	ret = open_candev(ndev);
	if (ret)
		goto err_irq;

	ret = arasan_canfd_chip_start(ndev);
	if (ret < 0) {
		netdev_err(ndev, "arasan_canfd_chip_start failed!\n");
		goto err_candev;
	}

	napi_enable(&priv->napi);
	netif_start_queue(ndev);

	return 0;

err_candev:
	close_candev(ndev);
err_irq:
	free_irq(ndev->irq, ndev);
err:
	pm_runtime_put(priv->dev);
	phy_power_off(priv->transceiver);

	return ret;
}

static int arasan_canfd_close(struct net_device *ndev)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	napi_disable(&priv->napi);
	arasan_canfd_chip_stop(ndev);
	free_irq(ndev->irq, ndev);
	close_candev(ndev);

	pm_runtime_put(priv->dev);
	phy_power_off(priv->transceiver);

	return 0;
}

static int arasan_canfd_get_berr_counter(const struct net_device *ndev,
					 struct can_berr_counter *bec)
{
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	int ret;
	u32 val;

	ret = pm_runtime_get_sync(priv->dev);
	if (ret < 0) {
		netdev_err(ndev, "%s: pm_runtime_get failed(%d)\n", __func__, ret);
		pm_runtime_put(priv->dev);
		return ret;
	}

	val = arasan_canfd_read(priv, STATUS);
	bec->txerr = FIELD_GET(STATUS_TEC_MASK, val);
	bec->rxerr = FIELD_GET(STATUS_REC_MASK, val);

	pm_runtime_put(priv->dev);

	return 0;
}

static void arasan_canfd_init(struct arasan_canfd_priv *priv)
{
	u32 val;
	int i;

	/* soft reset */
	val = arasan_canfd_read(priv, CONTROL);
	val |= (SOFTRESET | DMA_SOFTRESET);
	arasan_canfd_write(priv, CONTROL, val);
	udelay(1);
	val &= ~(SOFTRESET | DMA_SOFTRESET);
	arasan_canfd_write(priv, CONTROL, val);

	/* setup features */
	val |= LVL_ISOFD;
	val &= ~DISABLE_ECC;
	arasan_canfd_write(priv, CONTROL, val);
	arasan_canfd_write(priv, SELF_ADDRESS, 0);
	arasan_canfd_write(priv, ALLOWED_RETRANSMITS, 0);
	arasan_canfd_write(priv, WATCHDOG_LIMIT, 0);

	/* setup base registers */
	arasan_canfd_write(priv, R_BASE_ADDR,
			   priv->buf_dma + offsetof(struct can_buffer, r_buffer));
	arasan_canfd_write(priv, R_ECC_ADDR,
			   priv->buf_dma + offsetof(struct can_buffer, ecc_buffer.read));
	arasan_canfd_write(priv, W_BASE_ADDR,
			   priv->buf_dma + offsetof(struct can_buffer, w_buffer));
	arasan_canfd_write(priv, W_ECC_ADDR,
			   priv->buf_dma + offsetof(struct can_buffer, ecc_buffer.write));

	/* setup RX buffers */
	for (i = 0; i < CAN_MAX_RX_QUEUE; i++)
		arasan_canfd_write(priv, RX_PTR_BUFS, priv->buf_dma +
				   offsetof(struct can_buffer, w_buffer.rx_frames[i]));

	/* setup filter buffers */
	arasan_canfd_write(priv, NUMBER_OF_PAIRS, 0);
	arasan_canfd_write(priv, BASE_ADDRESS,
			   priv->buf_dma + offsetof(struct can_buffer, r_buffer.rx_filters[0]));
}

static const struct net_device_ops arasan_canfd_netdev_ops = {
	.ndo_open	= arasan_canfd_open,
	.ndo_stop	= arasan_canfd_close,
	.ndo_start_xmit	= arasan_canfd_start_xmit,
};

static const struct ethtool_ops arasan_canfd_ethtool_ops = {
	.get_ts_info = ethtool_op_get_ts_info,
};

static int arasan_canfd_suspend(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);

	if (netif_running(ndev)) {
		netif_stop_queue(ndev);
		netif_device_detach(ndev);
		arasan_canfd_chip_stop(ndev);
	}

	return pm_runtime_force_suspend(dev);
}

static int arasan_canfd_resume(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret) {
		dev_err(dev, "pm_runtime_force_resume failed on resume\n");
		return ret;
	}

	arasan_canfd_init(priv);

	if (netif_running(ndev)) {
		ret = arasan_canfd_chip_start(ndev);
		if (ret) {
			dev_err(dev, "arasan_canfd_chip_start failed on resume\n");
			return ret;
		}

		netif_device_attach(ndev);
		netif_start_queue(ndev);
	}

	return 0;
}

static int arasan_canfd_runtime_suspend(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct arasan_canfd_priv *priv = netdev_priv(ndev);

	clk_disable_unprepare(priv->sys_clk);
	clk_disable_unprepare(priv->can_clk);

	return 0;
}

static int arasan_canfd_runtime_resume(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct arasan_canfd_priv *priv = netdev_priv(ndev);
	int ret;

	ret = clk_prepare_enable(priv->sys_clk);
	if (ret) {
		dev_err(dev, "Cannot enable clock.\n");
		return ret;
	}
	ret = clk_prepare_enable(priv->can_clk);
	if (ret) {
		dev_err(dev, "Cannot enable clock.\n");
		clk_disable_unprepare(priv->sys_clk);
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops arasan_canfd_dev_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(arasan_canfd_suspend, arasan_canfd_resume)
	RUNTIME_PM_OPS(arasan_canfd_runtime_suspend, arasan_canfd_runtime_resume, NULL)
};

static int arasan_canfd_probe(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct arasan_canfd_priv *priv;
	struct phy *transceiver;
	void __iomem *addr;
	int ret;

	addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(addr))
		return PTR_ERR(addr);

	ndev = alloc_candev(sizeof(struct arasan_canfd_priv), CAN_MAX_TX_QUEUE);
	if (!ndev)
		return -ENOMEM;
	priv = netdev_priv(ndev);

	priv->sys_rst = devm_reset_control_get(&pdev->dev, "sys");
	if (IS_ERR(priv->sys_rst)) {
		dev_err(&pdev->dev, "failed to get sys reset\n");
		ret = PTR_ERR(priv->sys_rst);
		goto err_free;
	}
	reset_control_assert(priv->sys_rst);
	reset_control_deassert(priv->sys_rst);

	priv->can_rst = devm_reset_control_get(&pdev->dev, "can");
	if (IS_ERR(priv->can_rst)) {
		dev_err(&pdev->dev, "failed to get can reset\n");
		ret = PTR_ERR(priv->can_rst);
		goto err_reset_sys;
	}
	reset_control_assert(priv->can_rst);
	reset_control_deassert(priv->can_rst);

	priv->can_clk = devm_clk_get(&pdev->dev, "can");
	if (IS_ERR(priv->can_clk)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(priv->can_clk),
				    "failed to get can clock\n");
		goto err_reset;
	}

	priv->sys_clk = devm_clk_get(&pdev->dev, "sys");
	if (IS_ERR(priv->sys_clk)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(priv->sys_clk),
				    "failed to get sys clock\n");
		goto err_reset;
	}

	priv->ssp = DEFAULT_SSP;

	transceiver = devm_phy_optional_get(&pdev->dev, NULL);
	if (IS_ERR(transceiver)) {
		ret = PTR_ERR(transceiver);
		dev_err_probe(&pdev->dev, ret, "failed to get phy\n");
		goto err_reset;
	}
	priv->transceiver = transceiver;

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		goto err_reset;
	ndev->irq = ret;

	spin_lock_init(&priv->tx_lock);
	platform_set_drvdata(pdev, ndev);
	priv->dev = &pdev->dev;
	priv->base = addr;

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		netdev_err(ndev, "%s: pm_runtime_get failed(%d)\n", __func__, ret);
		goto err_disableclks;
	}

	priv->buf = dmam_alloc_coherent(&pdev->dev, sizeof(*priv->buf), &priv->buf_dma, GFP_KERNEL);
	if (!priv->buf) {
		ret = -ENOMEM;
		goto err_disableclks;
	}

	priv->can.bittiming_const = &arasan_canfd_bittiming_const;
	priv->can.fd.data_bittiming_const = &arasan_canfd_data_bittiming_const;
	priv->can.do_set_mode = arasan_canfd_do_set_mode;
	priv->can.do_get_berr_counter = arasan_canfd_get_berr_counter;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_BERR_REPORTING |
				CAN_CTRLMODE_FD;
	priv->can.clock.freq = clk_get_rate(priv->can_clk);

	ndev->flags |= IFF_ECHO;
	SET_NETDEV_DEV(ndev, &pdev->dev);
	ndev->netdev_ops = &arasan_canfd_netdev_ops;
	ndev->ethtool_ops = &arasan_canfd_ethtool_ops;

	netif_napi_add_weight(ndev, &priv->napi, arasan_canfd_poll, ARASAN_CANFD_NAPI_WEIGHT);

	arasan_canfd_init(priv);

	ret = register_candev(ndev);
	if (ret) {
		dev_err(&pdev->dev, "fail to register failed (err=%d)\n", ret);
		goto err_napidel;
	}

	of_can_transceiver(ndev);
	pm_runtime_put(&pdev->dev);

	return 0;

err_napidel:
	netif_napi_del(&priv->napi);
err_disableclks:
	pm_runtime_put(priv->dev);
	pm_runtime_disable(&pdev->dev);
err_reset:
	reset_control_assert(priv->can_rst);
err_reset_sys:
	reset_control_assert(priv->sys_rst);
err_free:
	free_candev(ndev);

	return ret;
}

static void arasan_canfd_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct arasan_canfd_priv *priv = netdev_priv(ndev);

	unregister_candev(ndev);
	netif_napi_del(&priv->napi);
	pm_runtime_disable(&pdev->dev);
	reset_control_assert(priv->sys_rst);
	reset_control_assert(priv->can_rst);
	free_candev(ndev);
}

static const struct of_device_id arasan_canfd_of_match[] = {
	{ .compatible = "arasan,canfd" },
	{},
};
MODULE_DEVICE_TABLE(of, arasan_canfd_of_match);

static struct platform_driver arasan_canfd_driver = {
	.probe = arasan_canfd_probe,
	.remove = arasan_canfd_remove,
	.driver	= {
		.name = KBUILD_MODNAME,
		.pm = pm_ptr(&arasan_canfd_dev_pm_ops),
		.of_match_table	= arasan_canfd_of_match,
	},
};
module_platform_driver(arasan_canfd_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jisheng Zhang <jszhang@kernel.org>");
MODULE_DESCRIPTION("Arasan CANFD driver");
