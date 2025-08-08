// SPDX-License-Identifier: GPL-2.0
/*
 * Lightning Mountain centralized DMA controller driver
 *
 * Copyright (c) 2016 - 2020 Intel Corporation.
 * Copyright (c) 2020 - 2025 Maxlinear Inc.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/of_dma.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "../dmaengine.h"
#include "../virt-dma.h"
#include "lgm-dma.h"

enum {
	DMA_ARG_CHAN_ID,
	DMA_ARG_DESC_CNT,
	DMA_ARG_PORT_ID,
	DMA_ARG_BURST_SZ,
};

static inline bool ldma_dma_v22(struct ldma_dev *d)
{
	if (d->ver == DMA_VER22)
		return true;
	else
		return false;
}

struct ldma_ops *ldma_dev_get_ops(struct ldma_dev *d)
{
	if (ldma_dma_v22(d))
		return &ldma_cdma_ops;
	else
		return &ldma_hdma_ops;
}

static inline bool ldma_chan_tx(struct ldma_chan *c)
{
	return !!(c->flags & DMA_TX_CH);
}

static void ldma_dev_reset(struct ldma_dev *d)

{
	unsigned long flags;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, DMA_CTRL_RST, DMA_CTRL_RST, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_pkt_arb_cfg(struct ldma_dev *d, bool enable)
{
	unsigned long flags;
	u32 mask = DMA_CTRL_PKTARB;
	u32 val = enable ? DMA_CTRL_PKTARB : 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_sram_desc_cfg(struct ldma_dev *d, bool enable)
{
	unsigned long flags;
	u32 mask = DMA_CTRL_DSRAM_PATH;
	u32 val = enable ? DMA_CTRL_DSRAM_PATH : 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_chan_flow_ctl_cfg(struct ldma_dev *d, bool enable)
{
	unsigned long flags;
	u32 mask, val;

	if (d->type != DMA_TYPE_TX)
		return;

	mask = DMA_CTRL_CH_FL;
	val = enable ? DMA_CTRL_CH_FL : 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_global_polling_enable(struct ldma_dev *d)
{
	unsigned long flags;
	u32 mask = DMA_CPOLL_EN | DMA_CPOLL_CNT;
	u32 val = DMA_CPOLL_EN;

	val |= FIELD_PREP(DMA_CPOLL_CNT, d->pollcnt);

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CPOLL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_desc_fetch_on_demand_cfg(struct ldma_dev *d, bool enable)
{
	unsigned long flags;
	u32 mask, val;

	if (d->type == DMA_TYPE_MCPY)
		return;

	mask = DMA_CTRL_DS_FOD;
	val = enable ? DMA_CTRL_DS_FOD : 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_byte_enable_cfg(struct ldma_dev *d, bool enable)
{
	unsigned long flags;
	u32 mask = DMA_CTRL_ENBE;
	u32 val = enable ? DMA_CTRL_ENBE : 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_orrc_cfg(struct ldma_dev *d)
{
	unsigned long flags;
	u32 val = 0;
	u32 mask;

	if (d->type == DMA_TYPE_RX || !d->orrc)
		return;

	mask = DMA_ORRC_EN | DMA_ORRC_ORRCNT;
	val = DMA_ORRC_EN | FIELD_PREP(DMA_ORRC_ORRCNT, d->orrc);

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_ORRC);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_df_tout_cfg(struct ldma_dev *d, bool enable, int tcnt)
{
	u32 mask = DMA_CTRL_DESC_TMOUT_CNT_V31;
	unsigned long flags;
	u32 val;

	if (enable)
		val = DMA_CTRL_DESC_TMOUT_EN_V31 | FIELD_PREP(DMA_CTRL_DESC_TMOUT_CNT_V31, tcnt);
	else
		val = 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_dburst_wr_cfg(struct ldma_dev *d, bool enable)
{
	unsigned long flags;
	u32 mask, val;

	if (d->type != DMA_TYPE_RX && d->type != DMA_TYPE_MCPY)
		return;

	mask = DMA_CTRL_DBURST_WR;
	val = enable ? DMA_CTRL_DBURST_WR : 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_vld_fetch_ack_cfg(struct ldma_dev *d, bool enable)
{
	unsigned long flags;
	u32 mask, val;

	if (d->type != DMA_TYPE_TX)
		return;

	mask = DMA_CTRL_VLD_DF_ACK;
	val = enable ? DMA_CTRL_VLD_DF_ACK : 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_dev_drb_cfg(struct ldma_dev *d, int enable)
{
	unsigned long flags;
	u32 mask = DMA_CTRL_DRB;
	u32 val = enable ? DMA_CTRL_DRB : 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, mask, val, DMA_CTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static int ldma_dev_cfg(struct ldma_dev *d)
{
	bool enable;

	ldma_dev_pkt_arb_cfg(d, true);
	ldma_dev_global_polling_enable(d);

	enable = !!(d->flags & DMA_DFT_DRB);
	ldma_dev_drb_cfg(d, enable);

	enable = !!(d->flags & DMA_EN_BYTE_EN);
	ldma_dev_byte_enable_cfg(d, enable);

	enable = !!(d->flags & DMA_CHAN_FLOW_CTL);
	ldma_dev_chan_flow_ctl_cfg(d, enable);

	enable = !!(d->flags & DMA_DESC_FOD);
	ldma_dev_desc_fetch_on_demand_cfg(d, enable);

	enable = !!(d->flags & DMA_DESC_IN_SRAM);
	ldma_dev_sram_desc_cfg(d, enable);

	enable = !!(d->flags & DMA_DBURST_WR);
	ldma_dev_dburst_wr_cfg(d, enable);

	enable = !!(d->flags & DMA_VALID_DESC_FETCH_ACK);
	ldma_dev_vld_fetch_ack_cfg(d, enable);

	if (ldma_dma_v22(d)) {
		ldma_dev_orrc_cfg(d);
		ldma_dev_df_tout_cfg(d, true, DMA_DFT_DESC_TCNT);
	}

	dev_dbg(d->dev, "%s Controller 0x%08x configuration done\n",
		d->name, readl(d->base + DMA_CTRL));

	return 0;
}

static int ldma_chan_cctrl_cfg(struct ldma_chan *c, u32 val)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 class_low, class_high;
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	reg = readl(d->base + DMA_CCTRL);
	/* Read from hardware */
	if (reg & DMA_CCTRL_DIR_TX)
		c->flags |= DMA_TX_CH;
	else
		c->flags |= DMA_RX_CH;

	/* Keep the class value unchanged */
	class_low = FIELD_GET(DMA_CCTRL_CLASS, reg);
	class_high = FIELD_GET(DMA_CCTRL_CLASSH, reg);
	val &= ~DMA_CCTRL_CLASS;
	val |= FIELD_PREP(DMA_CCTRL_CLASS, class_low);
	val &= ~DMA_CCTRL_CLASSH;
	val |= FIELD_PREP(DMA_CCTRL_CLASSH, class_high);
	writel(val, d->base + DMA_CCTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);

	return 0;
}

static void ldma_chan_irq_init(struct ldma_chan *c)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	unsigned long flags;
	u32 enofs, crofs;
	u32 cn_bit;

	if (c->nr < MAX_LOWER_CHANS) {
		enofs = DMA_IRNEN;
		crofs = DMA_IRNCR;
	} else {
		enofs = DMA_IRNEN1;
		crofs = DMA_IRNCR1;
	}

	cn_bit = BIT(c->nr & MASK_LOWER_CHANS);
	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);

	/* Clear all interrupts and disabled it */
	writel(0, d->base + DMA_CIE);
	writel(DMA_CI_ALL, d->base + DMA_CIS);

	ldma_update_bits(d, cn_bit, 0, enofs);
	writel(cn_bit, d->base + crofs);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void ldma_chan_irq_en(struct ldma_chan *c, bool en)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 enofs, crofs;
	u32 cn_bit, val;

	if (ldma_chan_is_hw_desc(c))
		return;

	if (c->nr < MAX_LOWER_CHANS) {
		enofs = DMA_IRNEN;
		crofs = DMA_IRNCR;
		cn_bit = BIT(c->nr);
	} else {
		enofs = DMA_IRNEN1;
		crofs = DMA_IRNCR1;
		cn_bit = BIT(c->nr - MAX_LOWER_CHANS);
	}

	writel(cn_bit, d->base + crofs);
	val = en ? cn_bit : 0;
	ldma_update_bits(d, cn_bit, val, enofs);
	val = en ? DMA_CI_EOP : 0;
	ldma_update_bits(d, DMA_CI_EOP, val, DMA_CIE);
	dev_dbg(d->dev, "irq: %d, CIE: 0x%x\n", en, readl(d->base + DMA_CIE));
}

static void ldma_chan_set_class(struct ldma_chan *c, u32 val)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 class_val;

	if (d->type == DMA_TYPE_MCPY || val > DMA_MAX_CLASS)
		return;

	/* 3 bits low */
	class_val = FIELD_PREP(DMA_CCTRL_CLASS, val & 0x7);
	/* 2 bits high */
	class_val |= FIELD_PREP(DMA_CCTRL_CLASSH, (val >> 3) & 0x3);

	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, DMA_CCTRL_CLASS | DMA_CCTRL_CLASSH, class_val,
			 DMA_CCTRL);
}

int ldma_chan_on(struct ldma_chan *c)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	unsigned long flags;

	if (c->onoff == DMA_CH_ON)
		return 0;

	/* If descriptors not configured, not allow to turn on channel */
	if (WARN_ON(!c->desc_init))
		return -EINVAL;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, DMA_CCTRL_ON, DMA_CCTRL_ON, DMA_CCTRL);
	ldma_chan_irq_en(c, true);
	spin_unlock_irqrestore(&d->dev_lock, flags);

	c->onoff = DMA_CH_ON;

	return 0;
}

static int ldma_chan_off(struct ldma_chan *c)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	unsigned long flags;
	u32 val;
	int ret;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, DMA_CCTRL_ON, 0, DMA_CCTRL);
	ldma_chan_irq_en(c, false);
	spin_unlock_irqrestore(&d->dev_lock, flags);

	ret = readl_poll_timeout_atomic(d->base + DMA_CCTRL, val,
					!(val & DMA_CCTRL_ON), 0, 10000);
	if (ret)
		return ret;

	c->onoff = DMA_CH_OFF;

	return 0;
}

void ldma_chan_desc_hw_cfg(struct ldma_chan *c, dma_addr_t desc_base,
			   int desc_num)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	unsigned long flags;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	writel(lower_32_bits(desc_base), d->base + DMA_CDBA);

	/* Higher 4 bits of 36 bit addressing */
	if (IS_ENABLED(CONFIG_64BIT)) {
		u32 hi = upper_32_bits(desc_base) & HIGH_4_BITS;

		ldma_update_bits(d, DMA_CDBA_MSB,
				 FIELD_PREP(DMA_CDBA_MSB, hi), DMA_CCTRL);
	}
	writel(desc_num, d->base + DMA_CDLEN);
	spin_unlock_irqrestore(&d->dev_lock, flags);

	c->desc_init = true;
}

int ldma_chan_reset(struct ldma_chan *c)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	unsigned long flags;
	u32 val;
	int ret;

	ret = ldma_chan_off(c);
	if (ret)
		return ret;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, DMA_CCTRL_RST, DMA_CCTRL_RST, DMA_CCTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);

	ret = readl_poll_timeout_atomic(d->base + DMA_CCTRL, val,
					!(val & DMA_CCTRL_RST), 0, 10000);
	if (ret)
		return ret;

	c->rst = 1;
	c->desc_init = false;

	return 0;
}

static void ldma_chan_byte_offset_cfg(struct ldma_chan *c, u32 boff_len)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 mask = DMA_C_BOFF_EN | DMA_C_BOFF_BOF_LEN;
	u32 val;

	if (boff_len > 0 && boff_len <= DMA_CHAN_BOFF_MAX)
		val = FIELD_PREP(DMA_C_BOFF_BOF_LEN, boff_len) | DMA_C_BOFF_EN;
	else
		val = 0;

	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, mask, val, DMA_C_BOFF);
}

static void ldma_chan_data_endian_cfg(struct ldma_chan *c, bool enable,
				      u32 endian_type)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 mask = DMA_C_END_DE_EN | DMA_C_END_DATAENDI;
	u32 val;

	if (enable)
		val = DMA_C_END_DE_EN | FIELD_PREP(DMA_C_END_DATAENDI, endian_type);
	else
		val = 0;

	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, mask, val, DMA_C_ENDIAN);
}

static void ldma_chan_desc_endian_cfg(struct ldma_chan *c, bool enable,
				      u32 endian_type)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 mask = DMA_C_END_DES_EN | DMA_C_END_DESENDI;
	u32 val;

	if (enable)
		val = DMA_C_END_DES_EN | FIELD_PREP(DMA_C_END_DESENDI, endian_type);
	else
		val = 0;

	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, mask, val, DMA_C_ENDIAN);
}

static void ldma_chan_hdr_mode_cfg(struct ldma_chan *c, u32 hdr_len, bool csum)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 mask, val;

	/* NB, csum disabled, hdr length must be provided */
	if (!csum && (!hdr_len || hdr_len > DMA_HDR_LEN_MAX))
		return;

	mask = DMA_C_HDRM_HDR_SUM;
	val = DMA_C_HDRM_HDR_SUM;

	if (!csum && hdr_len)
		val = hdr_len;

	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, mask, val, DMA_C_HDRM);
}

static void ldma_chan_rxwr_np_cfg(struct ldma_chan *c, bool enable)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 mask, val;

	/* Only valid for RX channel */
	if (ldma_chan_tx(c))
		return;

	mask = DMA_CCTRL_WR_NP_EN;
	val = enable ? DMA_CCTRL_WR_NP_EN : 0;

	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, mask, val, DMA_CCTRL);
}

static void ldma_chan_abc_cfg(struct ldma_chan *c, bool enable)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	u32 mask, val;

	if (d->ver < DMA_VER32 || ldma_chan_tx(c))
		return;

	mask = DMA_CCTRL_CH_ABC;
	val = enable ? DMA_CCTRL_CH_ABC : 0;

	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	ldma_update_bits(d, mask, val, DMA_CCTRL);
}

static int ldma_port_cfg(struct ldma_port *p)
{
	unsigned long flags;
	struct ldma_dev *d;
	u32 reg;

	d = p->ldev;
	reg = FIELD_PREP(DMA_PCTRL_TXENDI, p->txendi);
	reg |= FIELD_PREP(DMA_PCTRL_RXENDI, p->rxendi);

	if (ldma_dma_v22(d)) {
		reg |= FIELD_PREP(DMA_PCTRL_TXBL, p->txbl);
		reg |= FIELD_PREP(DMA_PCTRL_RXBL, p->rxbl);
	} else {
		reg |= FIELD_PREP(DMA_PCTRL_PDEN, p->pkt_drop);

		if (p->txbl == DMA_BURSTL_32DW)
			reg |= DMA_PCTRL_TXBL32;
		else if (p->txbl == DMA_BURSTL_16DW)
			reg |= DMA_PCTRL_TXBL16;
		else
			reg |= FIELD_PREP(DMA_PCTRL_TXBL, DMA_PCTRL_TXBL_8);

		if (p->rxbl == DMA_BURSTL_32DW)
			reg |= DMA_PCTRL_RXBL32;
		else if (p->rxbl == DMA_BURSTL_16DW)
			reg |= DMA_PCTRL_RXBL16;
		else
			reg |= FIELD_PREP(DMA_PCTRL_RXBL, DMA_PCTRL_RXBL_8);
	}

	spin_lock_irqsave(&d->dev_lock, flags);
	writel(p->portid, d->base + DMA_PS);
	writel(reg, d->base + DMA_PCTRL);
	spin_unlock_irqrestore(&d->dev_lock, flags);

	reg = readl(d->base + DMA_PCTRL); /* read back */
	dev_dbg(d->dev, "Port Control 0x%08x configuration done\n", reg);

	return 0;
}

static int ldma_chan_cfg(struct ldma_chan *c)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	unsigned long flags;
	u32 reg;

	reg = c->pden ? DMA_CCTRL_PDEN : 0;
	reg |= c->onoff ? DMA_CCTRL_ON : 0;
	reg |= c->rst ? DMA_CCTRL_RST : 0;

	ldma_chan_cctrl_cfg(c, reg);
	ldma_chan_irq_init(c);

	if (ldma_dma_v22(d))
		return 0;

	spin_lock_irqsave(&d->dev_lock, flags);
	ldma_chan_set_class(c, c->nr);
	ldma_chan_byte_offset_cfg(c, c->boff_len);
	ldma_chan_data_endian_cfg(c, c->data_endian_en, c->data_endian);
	ldma_chan_desc_endian_cfg(c, c->desc_endian_en, c->desc_endian);
	ldma_chan_hdr_mode_cfg(c, c->hdrm_len, c->hdrm_csum);
	ldma_chan_rxwr_np_cfg(c, c->desc_rx_np);
	ldma_chan_abc_cfg(c, c->abc_en);
	spin_unlock_irqrestore(&d->dev_lock, flags);

	if (ldma_chan_is_hw_desc(c))
		ldma_chan_desc_hw_cfg(c, c->desc_phys, c->desc_cnt);

	return 0;
}

static void ldma_dev_init(struct ldma_dev *d)
{
	unsigned long ch_mask = (unsigned long)d->channels_mask;
	struct ldma_port *p;
	struct ldma_chan *c;
	int i;
	u32 j;

	spin_lock_init(&d->dev_lock);
	ldma_dev_reset(d);
	ldma_dev_cfg(d);

	/* DMA port initialization */
	for (i = 0; i < d->port_nrs; i++) {
		p = &d->ports[i];
		ldma_port_cfg(p);
	}

	/* DMA channel initialization */
	for_each_set_bit(j, &ch_mask, d->chan_nrs) {
		c = &d->chans[j];
		ldma_chan_cfg(c);
	}
}

static int ldma_parse_dt(struct ldma_dev *d)
{
	struct fwnode_handle *fwnode = dev_fwnode(d->dev);

	if (fwnode_property_read_bool(fwnode, "intel,dma-byte-en"))
		d->flags |= DMA_EN_BYTE_EN;

	if (fwnode_property_read_bool(fwnode, "intel,dma-dburst-wr"))
		d->flags |= DMA_DBURST_WR;

	if (fwnode_property_read_bool(fwnode, "intel,dma-drb"))
		d->flags |= DMA_DFT_DRB;

	if (!fwnode_property_read_bool(fwnode, "intel,dma-sw-desc"))
		d->flags |= DMA_CHAN_HW_DESC;

	if (fwnode_property_read_u32(fwnode, "intel,dma-poll-cnt",
				     &d->pollcnt))
		d->pollcnt = DMA_DFT_POLL_CNT;

	if (fwnode_property_read_u32(fwnode, "dma-channel-mask",
				     &d->channels_mask))
		d->channels_mask = GENMASK(d->chan_nrs - 1, 0);

	if (d->inst->orrc)
		d->orrc = d->inst->orrc;

	if (d->inst->chan_fc)
		d->flags |= DMA_CHAN_FLOW_CTL;

	if (d->flags & DMA_CHAN_HW_DESC) {
		if (d->inst->desc_fod)
			d->flags |= DMA_DESC_FOD;

		if (d->inst->desc_in_sram)
			d->flags |= DMA_DESC_IN_SRAM;
	}

	if (d->inst->valid_desc_fetch_ack)
		d->flags |= DMA_VALID_DESC_FETCH_ACK;

	d->type = d->inst->type;
	d->name = d->inst->name;

	return 0;
}

int ldma_terminate_all(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);

	vchan_free_chan_resources(&c->vchan);

	return ldma_chan_reset(c);
}

int ldma_resume_chan(struct dma_chan *dma_chan)
{
	return ldma_chan_on(to_ldma_chan(dma_chan));
}

int ldma_pause_chan(struct dma_chan *dma_chan)
{
	return ldma_chan_off(to_ldma_chan(dma_chan));
}

static u32
chan_burst_len(struct ldma_chan *c, struct ldma_port *p, u32 burst)
{
	struct ldma_dev *d = p->ldev;

	if (ldma_dma_v22(d))
		return ilog2(burst);
	else
		return burst;
}

static void
update_burst_len(struct ldma_chan *c, struct ldma_port *p, u32 burst)
{
	if (ldma_chan_tx(c))
		p->txbl = chan_burst_len(c, p, burst);
	else
		p->rxbl = chan_burst_len(c, p, burst);
}

static int
update_client_configs(struct of_dma *ofdma, struct of_phandle_args *spec)
{
	struct ldma_dev *d = ofdma->of_dma_data;
	u32 chan_id =  spec->args[0];
	u32 port_id =  spec->args[1];
	u32 burst = spec->args[2];
	struct ldma_port *p;
	struct ldma_chan *c;

	if (chan_id >= d->chan_nrs || port_id >= d->port_nrs)
		return 0;

	p = &d->ports[port_id];
	c = &d->chans[chan_id];
	c->port = p;

	update_burst_len(c, p, burst);

	ldma_port_cfg(p);

	return 1;
}

static struct dma_chan *ldma_xlate(struct of_phandle_args *spec,
				   struct of_dma *ofdma)
{
	struct ldma_dev *d = ofdma->of_dma_data;
	u32 chan_id =  spec->args[0];
	int ret;

	if (!spec->args_count)
		return NULL;

	/* if args_count is 1 driver use default settings */
	if (spec->args_count > 1) {
		ret = update_client_configs(ofdma, spec);
		if (!ret)
			return NULL;
	}

	return dma_get_slave_channel(&d->chans[chan_id].vchan.chan);
}

static int ldma_irq_init(struct ldma_dev *d, struct platform_device *pdev)
{
	return d->ops->dma_irq_init(d, pdev);
}

static void ldma_clk_disable(void *data)
{
	struct ldma_dev *d = data;

	clk_disable_unprepare(d->core_clk);
	reset_control_assert(d->rst);
}

/* Initialize DMA controller, port, channel structures */
static int ldma_init(struct ldma_dev *d)
{
	struct ldma_chan *c;
	struct ldma_port *p;
	unsigned long ch_mask;
	int i, ret;

	ret = d->ops->dma_ctrl_init(d);
	if (ret)
		return ret;

	d->ports = devm_kcalloc(d->dev, d->port_nrs, sizeof(*p), GFP_KERNEL);
	if (!d->ports)
		return -ENOMEM;

	/* Channels Initializations */
	d->chans = devm_kcalloc(d->dev, d->chan_nrs, sizeof(*c), GFP_KERNEL);
	if (!d->chans)
		return -ENOMEM;

	for (i = 0; i < d->port_nrs; i++) {
		p = &d->ports[i];
		p->portid = i;
		ret = d->ops->dma_port_init(d, p);
		if (ret)
			return ret;
	}

	ch_mask = (unsigned long)d->channels_mask;
	for_each_set_bit(i, &ch_mask, d->chan_nrs) {
		c = &d->chans[i];
		c->nr = i;
		ret = d->ops->dma_chan_init(d, c);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct ldma_inst_data dma0 = {
	.name = "dma0",
	.chan_fc = false,
	.desc_fod = false,
	.desc_in_sram = false,
	.valid_desc_fetch_ack = false,
};

static const struct ldma_inst_data dma2tx = {
	.name = "dma2tx",
	.type = DMA_TYPE_TX,
	.orrc = 16,
	.chan_fc = true,
	.desc_fod = true,
	.desc_in_sram = true,
	.valid_desc_fetch_ack = true,
};

static const struct ldma_inst_data dma1rx = {
	.name = "dma1rx",
	.type = DMA_TYPE_RX,
	.orrc = 16,
	.chan_fc = false,
	.desc_fod = true,
	.desc_in_sram = true,
	.valid_desc_fetch_ack = false,
};

static const struct ldma_inst_data dma1tx = {
	.name = "dma1tx",
	.type = DMA_TYPE_TX,
	.orrc = 16,
	.chan_fc = true,
	.desc_fod = true,
	.desc_in_sram = true,
	.valid_desc_fetch_ack = true,
};

static const struct ldma_inst_data dma0tx = {
	.name = "dma0tx",
	.type = DMA_TYPE_TX,
	.orrc = 16,
	.chan_fc = true,
	.desc_fod = true,
	.desc_in_sram = true,
	.valid_desc_fetch_ack = true,
};

static const struct ldma_inst_data dma3 = {
	.name = "dma3",
	.type = DMA_TYPE_MCPY,
	.orrc = 16,
	.chan_fc = false,
	.desc_fod = false,
	.desc_in_sram = true,
	.valid_desc_fetch_ack = false,
};

static const struct ldma_inst_data toe_dma30 = {
	.name = "toe_dma30",
	.type = DMA_TYPE_MCPY,
	.orrc = 16,
	.chan_fc = false,
	.desc_fod = false,
	.desc_in_sram = true,
	.valid_desc_fetch_ack = true,
};

static const struct ldma_inst_data toe_dma31 = {
	.name = "toe_dma31",
	.type = DMA_TYPE_MCPY,
	.orrc = 16,
	.chan_fc = false,
	.desc_fod = false,
	.desc_in_sram = true,
	.valid_desc_fetch_ack = true,
};

static const struct of_device_id intel_ldma_match[] = {
	{ .compatible = "intel,lgm-cdma", .data = &dma0},
	{ .compatible = "intel,lgm-dma2tx", .data = &dma2tx},
	{ .compatible = "intel,lgm-dma1rx", .data = &dma1rx},
	{ .compatible = "intel,lgm-dma1tx", .data = &dma1tx},
	{ .compatible = "intel,lgm-dma0tx", .data = &dma0tx},
	{ .compatible = "intel,lgm-dma3", .data = &dma3},
	{ .compatible = "intel,lgm-toe-dma30", .data = &toe_dma30},
	{ .compatible = "intel,lgm-toe-dma31", .data = &toe_dma31},
	{}
};

static int intel_ldma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dma_device *dma_dev;
	struct ldma_dev *d;
	u32 id, bitn = 32;
	int ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	/* Link controller to platform device */
	d->dev = &pdev->dev;

	d->inst = device_get_match_data(dev);
	if (!d->inst) {
		dev_err(dev, "No device match found!\n");
		return -ENODEV;
	}

	d->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->base))
		return PTR_ERR(d->base);

	/* Power up and reset the dma engine, some DMAs always on?? */
	d->core_clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(d->core_clk))
		return PTR_ERR(d->core_clk);

	d->rst = devm_reset_control_get_optional(dev, NULL);
	if (IS_ERR(d->rst))
		return PTR_ERR(d->rst);

	clk_prepare_enable(d->core_clk);
	reset_control_deassert(d->rst);

	ret = devm_add_action_or_reset(dev, ldma_clk_disable, d);
	if (ret) {
		dev_err(dev, "Failed to devm_add_action_or_reset, %d\n", ret);
		return ret;
	}

	id = readl(d->base + DMA_ID);
	d->chan_nrs = FIELD_GET(DMA_ID_CHNR, id);
	d->port_nrs = FIELD_GET(DMA_ID_PNR, id);
	d->ver = FIELD_GET(DMA_ID_REV, id);

	if (id & DMA_ID_AW_36B)
		d->flags |= DMA_ADDR_36BIT;

	if (IS_ENABLED(CONFIG_64BIT) && (id & DMA_ID_AW_36B))
		bitn = 36;

	if (id & DMA_ID_DW_128B)
		d->flags |= DMA_DATA_128BIT;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(bitn));
	if (ret) {
		dev_err(dev, "No usable DMA configuration\n");
		return ret;
	}

	d->ops = ldma_dev_get_ops(d);

	ret = ldma_parse_dt(d);
	if (ret)
		return ret;

	ret = ldma_irq_init(d, pdev);
	if (ret)
		return ret;

	dma_dev = &d->dma_dev;
	dma_dev->dev = &pdev->dev;

	dma_cap_zero(dma_dev->cap_mask);
	dma_cap_set(DMA_SLAVE, dma_dev->cap_mask);

	/* Channel initializations */
	INIT_LIST_HEAD(&dma_dev->channels);

	/* init dma callback functions */
	d->ops->dma_func_init(d, dma_dev);

	ret = ldma_init(d);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, d);

	ldma_dev_init(d);

	ret = dma_async_device_register(dma_dev);
	if (ret) {
		dev_err(dev, "Failed to register slave DMA engine device\n");
		return ret;
	}

	ret = of_dma_controller_register(pdev->dev.of_node, ldma_xlate, d);
	if (ret) {
		dev_err(dev, "Failed to register of DMA controller\n");
		dma_async_device_unregister(dma_dev);
		return ret;
	}

	dev_info(dev, "Init done - DMA: %s: rev: %x, ports: %d channels: %d\n",
		 d->name, d->ver, d->port_nrs, d->chan_nrs);

	return 0;
}

static struct platform_driver intel_ldma_driver = {
	.probe = intel_ldma_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = intel_ldma_match,
	},
};

/*
 * Perform this driver as device_initcall to make sure initialization happens
 * before its DMA clients of some are platform specific and also to provide
 * registered DMA channels and DMA capabilities to clients before their
 * initialization.
 */
builtin_platform_driver(intel_ldma_driver);
