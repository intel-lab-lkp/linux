/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Definitions related to LGM DMA.
 *
 * Copyright (c) 2025 Maxlinear Inc.
 */

#ifndef _LGM_DMA_H
#define _LGM_DMA_H

enum ldma_chan_on_off {
	DMA_CH_OFF = 0,
	DMA_CH_ON = 1,
};

enum {
	DMA_TYPE_TX = 0,
	DMA_TYPE_RX,
	DMA_TYPE_MCPY,
};

enum {
	DMA_IN_HW_MODE,
	DMA_IN_SW_MODE,
};

struct ldma_dev;
struct ldma_port;
struct ldma_chan;

struct ldma_ops {
	/* DMA control level init */
	int (*dma_ctrl_init)(struct ldma_dev *d);
	/* DMA port level init */
	int (*dma_port_init)(struct ldma_dev *d, struct ldma_port *p);
	/* DMA channel level init */
	int (*dma_chan_init)(struct ldma_dev *d, struct ldma_chan *c);
	/* DMA interrupt init */
	int (*dma_irq_init)(struct ldma_dev *d, struct platform_device *pdev);
	/* DMA callback API init */
	void (*dma_func_init)(struct ldma_dev *d, struct dma_device *dma_dev);
};

/* Instance specific data */
struct ldma_inst_data {
	bool			desc_in_sram;
	bool			chan_fc;
	bool			desc_fod; /* Fetch On Demand */
	bool			valid_desc_fetch_ack;
	u32			orrc; /* Outstanding read count */
	const char		*name;
	u32			type;
};

struct ldma_chan {
	struct virt_dma_chan	vchan;
	struct ldma_port	*port; /* back pointer */
	char			name[8]; /* Channel name */
	int			nr; /* Channel id in hardware */
	u32			flags; /* central way or channel based way */
	enum ldma_chan_on_off	onoff;
	dma_addr_t		desc_phys;
	void			*desc_base; /* Virtual address */
	u32			desc_cnt; /* Number of descriptors */
	int			rst;
	u32			hdrm_len;
	bool			hdrm_csum;
	u32			boff_len;
	u32			data_endian;
	u32			desc_endian;
	bool			pden;
	bool			desc_rx_np;
	bool			data_endian_en;
	bool			desc_endian_en;
	bool			abc_en;
	bool			desc_init;
	void			*priv;
};

struct ldma_port {
	struct ldma_dev		*ldev; /* back pointer */
	u32			portid;
	u32			rxbl;
	u32			txbl;
	u32			rxendi;
	u32			txendi;
	u32			pkt_drop;
};

struct ldma_dev {
	struct device		*dev;
	void __iomem		*base;
	struct reset_control	*rst;
	struct clk		*core_clk;
	struct dma_device	dma_dev;
	u32			ver;
	int			irq;
	struct ldma_port	*ports;
	struct ldma_chan	*chans; /* channel list on this DMA or port */
	spinlock_t		dev_lock; /* Controller register exclusive */
	u32			chan_nrs;
	u32			port_nrs;
	u32			channels_mask;
	u32			flags;
	u32			pollcnt;
	u32			orrc; /* Outstanding read count */
	int			type;
	const char		*name;
	const struct ldma_inst_data *inst;
	const struct ldma_ops	*ops;
};

extern struct ldma_ops ldma_cdma_ops;
extern struct ldma_ops ldma_hdma_ops;

#define DRIVER_NAME			"lgm-dma"

#define DMA_ID				0x0008
#define DMA_ID_REV			GENMASK(7, 0)
#define DMA_ID_PNR			GENMASK(19, 16)
#define DMA_ID_CHNR			GENMASK(26, 20)
#define DMA_ID_DW_128B			BIT(27)
#define DMA_ID_AW_36B			BIT(28)
#define DMA_VER32			0x32
#define DMA_VER31			0x31
#define DMA_VER22			0x0A

#define DMA_CTRL			0x0010
#define DMA_CTRL_RST			BIT(0)
#define DMA_CTRL_DSRAM_PATH		BIT(1)
#define DMA_CTRL_DBURST_WR		BIT(3)
#define DMA_CTRL_VLD_DF_ACK		BIT(4)
#define DMA_CTRL_CH_FL			BIT(6)
#define DMA_CTRL_DS_FOD			BIT(7)
#define DMA_CTRL_DRB			BIT(8)
#define DMA_CTRL_ENBE			BIT(9)
#define DMA_CTRL_DESC_TMOUT_CNT_V31	GENMASK(27, 16)
#define DMA_CTRL_DESC_TMOUT_EN_V31	BIT(30)
#define DMA_CTRL_PKTARB			BIT(31)

#define DMA_CPOLL			0x0014
#define DMA_CPOLL_CNT			GENMASK(15, 4)
#define DMA_CPOLL_EN			BIT(31)

#define DMA_CS				0x0018
#define DMA_CS_MASK			GENMASK(5, 0)

#define DMA_CCTRL			0x001C
#define DMA_CCTRL_ON			BIT(0)
#define DMA_CCTRL_RST			BIT(1)
#define DMA_CCTRL_CH_POLL_EN		BIT(2)
#define DMA_CCTRL_CH_ABC		BIT(3) /* Adaptive Burst Chop */
#define DMA_CDBA_MSB			GENMASK(7, 4)
#define DMA_CCTRL_DIR_TX		BIT(8)
#define DMA_CCTRL_CLASS			GENMASK(11, 9)
#define DMA_CCTRL_CLASSH		GENMASK(19, 18)
#define DMA_CCTRL_WR_NP_EN		BIT(21)
#define DMA_CCTRL_PDEN			BIT(23)
#define DMA_MAX_CLASS			(SZ_32 - 1)

#define DMA_CDBA			0x0020
#define DMA_CDLEN			0x0024
#define DMA_CIS				0x0028
#define DMA_CIE				0x002C
#define DMA_CI_EOP			BIT(1)
#define DMA_CI_DUR			BIT(2)
#define DMA_CI_DESCPT			BIT(3)
#define DMA_CI_CHOFF			BIT(4)
#define DMA_CI_RDERR			BIT(5)
#define DMA_CI_ALL							\
	(DMA_CI_EOP | DMA_CI_DUR | DMA_CI_DESCPT | DMA_CI_CHOFF | DMA_CI_RDERR)

#define DMA_PS				0x0040
#define DMA_PCTRL			0x0044
#define DMA_PCTRL_RXBL16		BIT(0)
#define DMA_PCTRL_TXBL16		BIT(1)
#define DMA_PCTRL_RXBL			GENMASK(3, 2)
#define DMA_PCTRL_RXBL_8		3
#define DMA_PCTRL_TXBL			GENMASK(5, 4)
#define DMA_PCTRL_TXBL_8		3
#define DMA_PCTRL_PDEN			BIT(6)
#define DMA_PCTRL_RXBL32		BIT(7)
#define DMA_PCTRL_RXENDI		GENMASK(9, 8)
#define DMA_PCTRL_TXENDI		GENMASK(11, 10)
#define DMA_PCTRL_TXBL32		BIT(15)
#define DMA_PCTRL_MEM_FLUSH		BIT(16)

#define DMA_IRNEN1			0x00E8
#define DMA_IRNCR1			0x00EC
#define DMA_IRNEN			0x00F4
#define DMA_IRNCR			0x00F8
#define DMA_C_DP_TICK			0x100
#define DMA_C_DP_TICK_TIKNARB		GENMASK(15, 0)
#define DMA_C_DP_TICK_TIKARB		GENMASK(31, 16)

#define DMA_C_HDRM			0x110
/*
 * If header mode is set in DMA descriptor,
 *   If bit 30 is disabled, HDR_LEN must be configured according to channel
 *     requirement.
 *   If bit 30 is enabled(checksum with header mode), HDR_LEN has no need to
 *     be configured. It will enable check sum for switch
 * If header mode is not set in DMA descriptor,
 *   This register setting doesn't matter
 */
#define DMA_C_HDRM_HDR_SUM		BIT(30)

#define DMA_C_BOFF			0x120
#define DMA_C_BOFF_BOF_LEN		GENMASK(7, 0)
#define DMA_C_BOFF_EN			BIT(31)

#define DMA_ORRC			0x190
#define DMA_ORRC_ORRCNT			GENMASK(8, 4)
#define DMA_ORRC_EN			BIT(31)

#define DMA_C_ENDIAN			0x200
#define DMA_C_END_DATAENDI		GENMASK(1, 0)
#define DMA_C_END_DE_EN			BIT(7)
#define DMA_C_END_DESENDI		GENMASK(9, 8)
#define DMA_C_END_DES_EN		BIT(16)

/* DMA controller capability */
#define DMA_ADDR_36BIT			BIT(0)
#define DMA_DATA_128BIT			BIT(1)
#define DMA_CHAN_FLOW_CTL		BIT(2)
#define DMA_DESC_FOD			BIT(3)
#define DMA_DESC_IN_SRAM		BIT(4)
#define DMA_EN_BYTE_EN			BIT(5)
#define DMA_DBURST_WR			BIT(6)
#define DMA_VALID_DESC_FETCH_ACK	BIT(7)
#define DMA_DFT_DRB			BIT(8)
#define DMA_CHAN_HW_DESC		BIT(9)

#define DMA_ORRC_MAX_CNT		16
#define DMA_DFT_POLL_CNT		SZ_4
#define DMA_DFT_BURST_V22		SZ_2
#define DMA_BURSTL_8DW			SZ_8
#define DMA_BURSTL_16DW			SZ_16
#define DMA_BURSTL_32DW			SZ_32
#define DMA_DFT_BURST			DMA_BURSTL_16DW
#define DMA_MAX_DESC_NUM		(SZ_8K - 1)
#define DMA_CHAN_BOFF_MAX		(SZ_256 - 1)
#define DMA_DFT_ENDIAN			0

#define DMA_DFT_DESC_TCNT		50
#define DMA_HDR_LEN_MAX			(SZ_16K - 1)

/* DMA flags */
#define DMA_TX_CH			BIT(0)
#define DMA_RX_CH			BIT(1)
#define CHAN_IN_USE			BIT(2)

/* Descriptor fields */
#define DESC_BYTE_OFF			GENMASK(25, 23)
#define DESC_EOP			BIT(28)
#define DESC_SOP			BIT(29)
#define DESC_C				BIT(30)
#define DESC_OWN			BIT(31)

#define DMA_CHAN_RST			1
#define DMA_MAX_SIZE			(BIT(16) - 1)
#define MAX_LOWER_CHANS			32
#define MASK_LOWER_CHANS		GENMASK(4, 0)
#define DMA_OWN				1
#define HIGH_4_BITS			GENMASK(3, 0)
#define DMA_PKT_DROP_DIS		0

static inline struct ldma_chan *to_ldma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct ldma_chan, vchan.chan);
}

static inline struct ldma_dev *to_ldma_dev(struct dma_device *dma_dev)
{
	return container_of(dma_dev, struct ldma_dev, dma_dev);
}

static inline struct ldma_dev *chan_to_ldma_dev(struct ldma_chan *c)
{
	return to_ldma_dev(c->vchan.chan.device);
}

static inline bool ldma_chan_is_hw_desc(struct ldma_chan *c)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);

	return !!(d->flags & DMA_CHAN_HW_DESC);
}

static inline void
ldma_update_bits(struct ldma_dev *d, u32 mask, u32 val, u32 ofs)
{
	u32 old_val, new_val;

	old_val = readl(d->base +  ofs);
	new_val = (old_val & ~mask) | (val & mask);

	if (new_val != old_val)
		writel(new_val, d->base + ofs);
}

void ldma_chan_desc_hw_cfg(struct ldma_chan *c, dma_addr_t desc_base,
			   int desc_num);
int ldma_terminate_all(struct dma_chan *chan);
int ldma_resume_chan(struct dma_chan *chan);
int ldma_pause_chan(struct dma_chan *chan);
int ldma_chan_reset(struct ldma_chan *c);
int ldma_chan_on(struct ldma_chan *c);
struct ldma_ops *ldma_dev_get_ops(struct ldma_dev *d);

#endif
