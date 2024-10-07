// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL838x, RTL839x, RTL930x & RTL931x SerDes PHY driver
 * Copyright (c) 2024 Markus Stockhausen <markus.stockhausen@gmx.de>
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#include "phy-rtk-otto-serdes.h"

/* common helpers */

static inline bool rtsds_invalid_reg(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	return (sid >= ctrl->conf->sds_cnt || page >= ctrl->conf->page_cnt || reg > 31);
}

static inline bool rtsds_invalid_sds(struct rtsds_ctrl *ctrl, u32 sid)
{
	return (sid >= ctrl->conf->sds_cnt);
}

static inline bool rtsds_readonly(struct rtsds_ctrl *ctrl, u32 sid)
{
	return !(ctrl->sds_mask & BIT(sid));
}

static int rtsds_hwmode_to_phymode(struct rtsds_ctrl *ctrl, int hwmode)
{
	for (int m = 0; m < PHY_INTERFACE_MODE_MAX; m++)
		if (ctrl->conf->mode_map[m] == hwmode)
			return m;

	return PHY_INTERFACE_MODE_MAX;
}

static const char *rtsds_events[RTSDS_EVENT_CNT] = {
	[RTSDS_EVENT_SETUP] = "setup",
};

static int rtsds_run_event(struct rtsds_ctrl *ctrl, u32 sid, int evt)
{
	struct rtsds_seq *seq;
	int ret, step = 1, delay = 0;

	if (evt >= RTSDS_EVENT_CNT || sid >= ctrl->conf->sds_cnt)
		return -EINVAL;

	seq = ctrl->conf->sequence[evt];

	if (!seq)
		return 0;

	while (seq->action != RTSDS_SEQ_STOP) {
		if (!(seq->ports & BIT(sid)) ||
		    ((seq->mode != PHY_INTERFACE_MODE_NA) &&
		     (seq->mode != ctrl->sds[sid].mode))) {
			step++;	seq++;
			continue;
		}

		if (seq->action == RTSDS_SEQ_WAIT)
			delay = seq->val;

		if (delay)
			usleep_range(delay << 10, (delay << 10) + 1000);

		if (seq->action == RTSDS_SEQ_MASK) {
			ret = ctrl->conf->mask(ctrl, sid, seq->page,
					 seq->reg, seq->val, seq->mask);
			if (ret) {
				dev_err(ctrl->dev, "sequence %s failed at step %d",
					rtsds_events[evt], step);
				return -EIO;
			}
		}

		step++; seq++;
	}

	return 0;
}

/* common RTL838x and RTL839x helpers */

static void rtsds_83xx_digital_reset(struct rtsds_ctrl *ctrl, u32 sid, u32 cnt)
{
	/* software reset */
	for (u32 i = sid; i <= sid + cnt; i++)
		ctrl->conf->mask(ctrl, i, RTSDS_PAGE_SDS,
				 0x03, RTSDS_BITS_SOFT_RST, RTSDS_BITS_SOFT_RST);
	usleep_range(100000, 101000);
	for (u32 i = sid; i <= sid + cnt; i++)
		ctrl->conf->mask(ctrl, i, RTSDS_PAGE_SDS,
				 0x03, 0x0000, RTSDS_BITS_SOFT_RST);
	/* RX/TX reset */
	for (u32 i = sid; i <= sid + cnt; i++) {
		ctrl->conf->mask(ctrl, i, RTSDS_PAGE_SDS,
				 0x00, 0x0000, RTSDS_BITS_SDS_EN);
		ctrl->conf->mask(ctrl, i, RTSDS_PAGE_SDS,
				 0x00, RTSDS_BITS_SDS_EN, RTSDS_BITS_SDS_EN);
	}
}

/* common RTL930x and RTL931x helpers */

static int rtsds_93xx_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	int cnt = 100, cmd = (sid << 2) | (page << 7) | (reg << 13) | RTSDS_93XX_SDS_READ;

	iowrite32(cmd, ctrl->base);
	while (--cnt && (ioread32(ctrl->base) & RTSDS_93XX_SDS_BUSY))
		usleep_range(50, 60);

	return cnt ? ioread32(ctrl->base + 4) & 0xffff : -EIO;
}

static int rtsds_93xx_mask(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg, u32 val, u32 mask)
{
	int oldval, cnt = 100, cmd = (sid << 2) | (page << 7) | (reg << 13) | RTSDS_93XX_SDS_WRITE;

	if (mask != 0xffff) {
		oldval = rtsds_93xx_read(ctrl, sid, page, reg);
		if (oldval < 0)
			return -EIO;
		oldval &= ~mask;
		val |= oldval;
	}

	iowrite32(val, ctrl->base + 4);
	iowrite32(cmd, ctrl->base);

	while (--cnt && (ioread32(ctrl->base) & RTSDS_93XX_SDS_BUSY))
		usleep_range(50, 60);

	return cnt ? 0 : -EIO;
}

/*
 * The RTL838x has 6 SerDes. The 16 bit registers start at 0xbb00e780 and are mapped directly into
 * 32 bit memory addresses. High 16 bits are always empty. A "lower memory block serves pages 0/3
 * a higher one pages 1/2.
 */

static int rtsds_838x_reg_offset(u32 sid, u32 page, u32 reg)
{
	if (page == 0 || page == 3)
		return (sid << 9) + (page << 7) + (reg << 2);
	else
		return 0xb80 + (sid << 8) + (page << 7) + (reg << 2);
}

static int rtsds_838x_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	int offs;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	offs = rtsds_838x_reg_offset(sid, page, reg);

	/* read twice for link status latch */
	if (page == RTSDS_PAGE_FIB && reg == 1)
		ioread32(ctrl->base + offs);

	return ioread32(ctrl->base + offs);
}

static int rtsds_838x_mask(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg, u32 val, u32 mask)
{
	int offs;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	offs = rtsds_838x_reg_offset(sid, page, reg);

	/* read twice for link status latch */
	if (page == RTSDS_PAGE_FIB && reg == 1)
		ioread32(ctrl->base + offs);

	iomask32(mask, val, ctrl->base + offs);

	return 0;
}

static void rtsds_838x_rx_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	/* reset with a selftest by enabling/disabling RX_EN_SELF */
	rtsds_838x_mask(ctrl, sid, 0x01, 0x09, 0x0200, 0x0200);
	usleep_range(100000, 101000);
	rtsds_838x_mask(ctrl, sid, 0x01, 0x09, 0x0000, 0x0200);
}

static void rtsds_838x_cmu_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	rtsds_838x_mask(ctrl, sid, 0x01, 0x00, 0x4040, 0xffff);
	rtsds_838x_mask(ctrl, sid, 0x01, 0x00, 0x4740, 0xffff);
	rtsds_838x_mask(ctrl, sid, 0x01, 0x00, 0x47c0, 0xffff);
	rtsds_838x_mask(ctrl, sid, 0x01, 0x00, 0x4000, 0xffff);
}

static int rtsds_838x_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_838x_rx_reset(ctrl, sid);
	rtsds_838x_cmu_reset(ctrl, sid);
	rtsds_83xx_digital_reset(ctrl, sid, 1);

	return 0;
}

static int rtsds_838x_set_mode(struct rtsds_ctrl *ctrl, u32 sid, int combomode)
{
	int shift, mode = RTSDS_MODE(combomode), submode = RTSDS_SUBMODE(combomode);

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	if (sid >= 4) {
		shift = (sid - 4) * 3;
		iomask32(0x7 << shift, (submode & 0x7) << shift, RTSDS_838X_INT_MODE_CTRL);
	} else if (submode != 0)
		return -EINVAL;

	shift = 25 - sid * 5;
	iomask32(0x1f << shift, (mode & 0x1f) << shift, RTSDS_838X_SDS_MODE_SEL);

	return 0;
}

static int rtsds_838x_get_mode(struct rtsds_ctrl *ctrl, u32 sid)
{
	int shift, mode, submode = 0;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	if (sid >= 4) {
		shift = (sid - 4) * 3;
		submode = (ioread32(RTSDS_838X_INT_MODE_CTRL) >> shift) & 0x7;
	}

	shift = 25 - sid * 5;
	mode = (ioread32(RTSDS_838X_SDS_MODE_SEL) >> shift) & 0x1f;

	return RTSDS_COMBOMODE(mode, submode);
}

/*
 * The RLT839x has 14 SerDes starting at 0xbb00a000. 0-7, 10, 11 are 5GBit, 8, 9, 12, 13 are
 * 10GIt. Two adjacent SerDes are tightly coupled and share a 1024 bytes register area. Per 32 bit
 * address two registers are stored. The first register is stored in the lower 2 bytes ("on the
 * right" due to big endian) and the second register in the upper 2 bytes. We know the following
 * register areas:
 *
 * - XSG0	(4 pages @ offset 0x000): for even SerDes
 * - XSG1	(4 pages @ offset 0x100): for odd SerDes
 * - TGRX	(4 pages @ offset 0x200): for even 10G SerDes
 * - ANA_RG	(2 pages @ offset 0x300): for even 5G SerDes
 * - ANA_RG	(2 pages @ offset 0x380): for odd 5G SerDes
 * - ANA_TG	(2 pages @ offset 0x300): for even 10G SerDes
 * - ANA_TG	(2 pages @ offset 0x380): for odd 10G SerDes
 *
 * The most consistent mapping that aligns to the RTL93xx devices is:
 *
 *		even 5G SerDes	odd 5G SerDes	even 10G SerDes	odd 10G SerDes
 * Page 0:	XSG0/0		XSG1/0		XSG0/0		XSG1/0
 * Page 1:	XSG0/1		XSG1/1		XSG0/1		XSG1/1
 * Page 2:	XSG0/2		XSG1/2		XSG0/2		XSG1/2
 * Page 3:	XSG0/3		XSG1/3		XSG0/3		XSG1/3
 * Page 4:	<zero>		<zero>		TGRX/0		<zero>
 * Page 5:	<zero>		<zero>		TGRX/1		<zero>
 * Page 6:	<zero>		<zero>		TGRX/2		<zero>
 * Page 7:	<zero>		<zero>		TGRX/3		<zero>
 * Page 8:	ANA_RG		ANA_RG		<zero>		<zero>
 * Page 9:	ANA_RG_EXT	ANA_RG_EXT	<zero>		<zero>
 * Page 10:	<zero>		<zero>		ANA_TG		ANA_TG
 * Page 11:	<zero>		<zero>		ANA_TG_EXT	ANA_TG_EXT
 */

static int rtsds_839x_reg_offset(u32 sid, u32 page, u32 reg)
{
	int offs = ((sid & 0xfe) << 9) + ((reg & 0xfe) << 1);

	if (page < 4) {
		offs += ((sid & 1) << 8) + (page << 6);
	} else if (page < 8) {
		if (sid != 8 && sid != 12)
			return -1;
		offs += 0x100 + (page << 6);
	} else if (page < 10) {
		if (sid == 8 || sid == 9 || sid == 12 || sid == 13)
			return -1;
		offs += 0x100 + ((sid & 1) << 7) + (page << 6);
	} else {
		if (sid != 8 && sid != 9 && sid != 12 && sid != 13)
			return -1;
		offs += 0x100 + ((sid & 1) << 7) + ((page - 2) << 6);
	}

	return offs;
}

static int rtsds_839x_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	int offs, shift = (reg << 4) & 0x10;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	offs = rtsds_839x_reg_offset(sid, page, reg);
	if (offs < 0)
		return 0;

	/* read twice for link status latch */
	if (page == RTSDS_PAGE_FIB && reg == 1)
		ioread32(ctrl->base + offs);

	return (ioread32(ctrl->base + offs) >> shift) & 0xffff;
}

static int rtsds_839x_mask(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg, u32 val, u32 mask)
{
	int oldval, offs;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	offs = rtsds_839x_reg_offset(sid, page, reg);
	if (offs < 0)
		return 0;

	/* read twice for link status latch */
	if (page == RTSDS_PAGE_FIB && reg == 1)
		ioread32(ctrl->base + offs);

	oldval = ioread32(ctrl->base + offs);
	val = reg & 1 ? (oldval & ~(mask << 16)) | (val << 16) : (oldval & ~mask) | val;
	iowrite32(val, ctrl->base + offs);

	return 0;
}

static int rtsds_839x_set_mode(struct rtsds_ctrl *ctrl, u32 sid, int combomode)
{
	int shift = (sid & 7) << 2, offs = (sid >> 1) & ~3;
	int mode = RTSDS_MODE(combomode), submode = RTSDS_SUBMODE(combomode);

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_839x_mask(ctrl, sid, RTSDS_PAGE_SDS, 0x04, (submode << 12) & 0xf000, 0xf000);
	iomask32(0xf << shift, (mode & 0xf) << shift, RTSDS_839X_MAC_SERDES_IF_CTRL + offs);

	return 0;
}

static int rtsds_839x_get_mode(struct rtsds_ctrl *ctrl, u32 sid)
{
	int mode, submode, shift = (sid & 7) << 2, offs = (sid >> 1) & ~3;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	submode = (rtsds_839x_read(ctrl, sid, RTSDS_PAGE_SDS, 0x04) >> 12) & 0xf;
	mode = (ioread32(RTSDS_839X_MAC_SERDES_IF_CTRL + offs) >> shift) & 0xf;

	return RTSDS_COMBOMODE(mode, submode);
}

static void rtsds_839x_rx_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	u32 i, page, reg, pre, post, mask, lo = sid & ~1, hi = sid | 1;

	/* reset with a selftest by enabling/disabling RX_EN_SELF */
	if (lo < 8 || lo == 10) {
		page = 0x08; reg = 0x14; pre = 0x0200; post = 0x0000; mask = 0x0201;
	} else {
		page = 0x0b; reg = 0x00; pre = 0x8000; post = 0x0000; mask = 0x8008;
	}

	for (i = lo; i <= hi; i++)
		rtsds_839x_mask(ctrl, i, page, reg, pre, mask);
	usleep_range(100000, 101000);
	for (i = lo; i <= hi; i++)
		rtsds_839x_mask(ctrl, i, page, reg, post, mask);
}

static void rtsds_839x_cmu_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	int hi = sid | 1;

	if (hi == 9 || hi == 13)
		return;

	rtsds_839x_mask(ctrl, hi, 0x09, 0x01, 0x0050, 0xffff);
	rtsds_839x_mask(ctrl, hi, 0x09, 0x01, 0x00f0, 0xffff);
	rtsds_839x_mask(ctrl, hi, 0x09, 0x01, 0x0000, 0xffff);
}

static int rtsds_839x_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	int lo = sid & ~1;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	/* Registers are shared between adjacent SerDes. Reset both. */
	rtsds_839x_cmu_reset(ctrl, sid);
	rtsds_839x_rx_reset(ctrl, sid);
	rtsds_83xx_digital_reset(ctrl, lo, 2);

	return 0;
}

/*
 * The RTL930x family has 12 SerdDes. They are accessed through two IO registers at 0xbb0003b0
 * which simulate commands to an internal MDIO bus. From the current observation there are 3 types
 * of SerDes:
 *
 * - SerDes 0,1 exist on the RLT9301 and 9302B and are QSGMII capable
 * - SerDes 2-9 are USXGMII capabable with either quad or single configuration
 * - SerDes 10-11 are 10GBase-R capable
 */

static int rtsds_930x_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	return rtsds_93xx_read(ctrl, sid, page, reg);
}

static int rtsds_930x_mask(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg, u32 val, u32 mask)
{
	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	return rtsds_93xx_mask(ctrl, sid, page, reg, val, mask);
}

static void rtsds_930x_mode_offset(int sid,
				   void __iomem __force **modereg, int *modeshift,
				   void __iomem __force **subreg, int *subshift)
{
	if (sid > 3) {
		*subreg = RTSDS_930X_SDS_SUBMODE_CTRL1;
		*subshift = (sid - 4) * 5;
	} else {
		*subreg = RTSDS_930X_SDS_SUBMODE_CTRL0;
		*subshift = (sid - 2) * 5;
	}

	if (sid < 4) {
		*modeshift = sid * 6;
		*modereg = RTSDS_930X_SDS_MODE_SEL_0;
	} else if (sid < 8) {
		*modeshift = (sid - 4) * 6;
		*modereg = RTSDS_930X_SDS_MODE_SEL_1;
	} else if (sid < 10) {
		*modeshift = (sid - 8) * 6;
		*modereg = RTSDS_930X_SDS_MODE_SEL_2;
	} else {
		*modeshift = (sid - 10) * 6;
		*modereg = RTSDS_930X_SDS_MODE_SEL_3;
	}
}

static int rtsds_930x_set_mode(struct rtsds_ctrl *ctrl, u32 sid, int combomode)
{
	int modeshift, subshift;
	int mode = RTSDS_MODE(combomode);
	int submode = RTSDS_SUBMODE(combomode);
	void __iomem __force *modereg;
	void __iomem __force *subreg;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_930x_mode_offset(sid, &modereg, &modeshift, &subreg, &subshift);
	if (sid >= 2 || sid <= 9)
		iomask32(0x1f << subshift, (submode & 0x1f) << subshift, subreg);
	else if (submode != 0)
		return -EINVAL;
	iomask32(0x1f << modeshift, (mode & 0x1f) << modeshift, modereg);

	return 0;
}

static int rtsds_930x_get_mode(struct rtsds_ctrl *ctrl, u32 sid)
{
	int modeshift, subshift, mode, submode = 0;
	void __iomem __force *modereg;
	void __iomem __force *subreg;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_930x_mode_offset(sid, &modereg, &modeshift, &subreg, &subshift);
	if (sid >= 2 || sid <= 9)
		submode = (ioread32(subreg) >> subshift) & 0x1f;
	mode = ioread32(modereg) >> modeshift & 0x1f;

	return RTSDS_COMBOMODE(mode, submode);
}

static int rtsds_930x_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	int modecur, modeoff = ctrl->conf->mode_map[PHY_INTERFACE_MODE_NA];

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	modecur = rtsds_930x_get_mode(ctrl, sid);

	/* It is enough to power off SerDes and set to old mode again */
	if (modecur != modeoff) {
		rtsds_930x_set_mode(ctrl, sid, modeoff);
		rtsds_930x_set_mode(ctrl, sid, modecur);
	}

	return 0;
}

/*
 * The RTL931x family has 14 "frontend" SerDes that are cascaded. All operations (e.g. reset) work
 * on this frontend view while their registers are distributed over a total of 32 background
 * SerDes. Two types of SerDes have been identified:
 *
 * A "even" SerDes with numbers 0, 1, 2, 4, 6, 8, 10, 12 works on two background SerDes. 64 analog
 * and 64 XGMII data pages are coming from a first background SerDes while another 64 XGMII pages
 * are served from a second SerDes.
 *
 * The "odd" SerDes with numbers 3, 5, 7, 9, 11 & 13 SerDes consist of a total of 3 background
 * SerDes (one analog and two XGMII) each with an own page/register set.
 *
 * To align this and improve readability the driver will simulate a total of 576 pages and mix
 * them as follows.
 *
 * frontend page		"even" frontend SerDes	"odd" frontend SerDes
 * page 0-63 (analog):		back SDS page 0-63	back SDS page 0-63
 * page 64-127 (XGMII1):	back SDS page 0-63	back SDS+1 page 0-63
 * page 128-191 (XGMII2):	back SDS+1 page 0-63	back SDS+2 page 0-63
 */

static int rtsds_931x_reg_offset(u32 sid, u32 page)
{
	int map[] = {0, 1, 2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23};
	int offs = map[sid];

	if ((sid & 1) && (sid != 1))
		offs += (page >> 6); /* distribute "odd" to 3 background SerDes */
	else if (page >= 128)
		offs += 1; /* "distribute "even" to 2 background SerDes */

	return offs;
}

static int rtsds_931x_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	int offs;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	offs = rtsds_931x_reg_offset(sid, page);
	if (offs < 0)
		return 0;

	return rtsds_93xx_read(ctrl, offs, page, reg);
}

static int rtsds_931x_mask(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg, u32 val, u32 mask)
{
	int offs;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	offs = rtsds_931x_reg_offset(sid, page);
	if (offs < 0)
		return 0;

	return rtsds_93xx_mask(ctrl, offs, page, reg, val, mask);
}

static int rtsds_931x_set_mode(struct rtsds_ctrl *ctrl, u32 sid, int combomode)
{
	int shift = (sid & 3) << 3, offs = sid & ~3;
	int mode = RTSDS_MODE(combomode);
	int submode = RTSDS_SUBMODE(combomode);

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_931x_mask(ctrl, sid, 0x1f, 0x09, (submode & 0x3f << 6), 0x0fc0);
	iomask32(0xff << shift, ((mode | RTSDS_931X_SDS_FORCE_SETUP) & 0xff) << shift,
		 RTSDS_931X_SERDES_MODE_CTRL + offs);

	return 0;
}

static int rtsds_931x_get_mode(struct rtsds_ctrl *ctrl, u32 sid)
{
	int mode, submode, shift = (sid & 3) << 3, offs = sid & ~3;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	submode = (rtsds_931x_read(ctrl, sid, 0x1f, 0x09) >> 6) & 0x3f;
	mode = (ioread32(RTSDS_931X_SERDES_MODE_CTRL + offs) >> shift) & 0x1f;

	return RTSDS_COMBOMODE(mode, submode);
}

static int rtsds_931x_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	int pwr, modecur, modeoff = ctrl->conf->mode_map[PHY_INTERFACE_MODE_NA];

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	modecur = rtsds_931x_get_mode(ctrl, sid);

	if (modecur != modeoff) {
		/* reset with mode switch cycle while being powered off */
		pwr = ioread32(RTSDS_931X_PS_SERDES_OFF_MODE_CTRL);
		iowrite32(pwr | BIT(sid), RTSDS_931X_PS_SERDES_OFF_MODE_CTRL);
		rtsds_931x_set_mode(ctrl, sid, modeoff);
		rtsds_931x_set_mode(ctrl, sid, modecur);
		iowrite32(pwr, RTSDS_931X_PS_SERDES_OFF_MODE_CTRL);
	}

	return 0;
}

int rtsds_read(struct phy *phy, u32 page, u32 reg)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;

	return ctrl->conf->read(ctrl, sid, page, reg);
}

int rtsds_mask(struct phy *phy, u32 page, u32 reg, u32 val, u32 mask)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;

	if (rtsds_readonly(ctrl, sid))
		return 0;

	return ctrl->conf->mask(ctrl, sid, page, reg, val, mask);
}

int rtsds_write(struct phy *phy, u32 page, u32 reg, u32 val)
{
	return rtsds_mask(phy, page, reg, val, 0xffff);
}

static int rtsds_phy_init(struct phy *phy)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;
	int ret;

	if (rtsds_readonly(ctrl, sid))
		return 0;

	mutex_lock(&ctrl->lock);
//	ret = rtsds_run_event(ctrl, sid, RTSDS_EVENT_INIT);
	mutex_unlock(&ctrl->lock);

	if (ret)
		dev_err(ctrl->dev, "init failed for SerDes %d\n", sid);

	return ret;
}

static int rtsds_phy_power_on(struct phy *phy)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;
	int ret;

	if (rtsds_readonly(ctrl, sid))
		return 0;

	mutex_lock(&ctrl->lock);
//	ret = rtsds_run_event(ctrl, sid, RTSDS_EVENT_POWER_ON);
	mutex_unlock(&ctrl->lock);

	if (ret)
		dev_err(ctrl->dev, "power on failed for SerDes %d\n", sid);

	return ret;
}

static int rtsds_phy_power_off(struct phy *phy)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;
	int ret;

	if (rtsds_readonly(ctrl, sid))
		return 0;

	mutex_lock(&ctrl->lock);
	if (!ret)
		ret = ctrl->conf->set_mode(ctrl, sid, ctrl->conf->mode_map[PHY_INTERFACE_MODE_NA]);
	mutex_unlock(&ctrl->lock);

	if (ret)
		dev_err(ctrl->dev, "power off failed for SerDes %d\n", sid);

	return ret;
}

static int rtsds_phy_set_mode_int(struct rtsds_ctrl *ctrl, u32 sid, int phymode, int hwmode)
{
	int ret;

	mutex_lock(&ctrl->lock);
	ret = ctrl->conf->set_mode(ctrl, sid, hwmode);
	if (!ret)
		ctrl->sds[sid].mode = phymode;
	mutex_unlock(&ctrl->lock);

	if (ret)
		dev_err(ctrl->dev, "set mode failed for SerDes %d\n", sid);

	return ret;
}

static int rtsds_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;

	if (rtsds_readonly(ctrl, sid))
		return 0;

	if (mode != PHY_MODE_ETHERNET)
		return -EINVAL;

	return rtsds_phy_set_mode_int(ctrl, sid, submode, ctrl->conf->mode_map[submode]);
}

static int rtsds_phy_reset_int(struct rtsds_ctrl *ctrl, u32 sid)
{
	int ret;

	mutex_lock(&ctrl->lock);
	ret = ctrl->conf->reset(ctrl, sid);
	mutex_unlock(&ctrl->lock);

	if (ret)
		dev_err(ctrl->dev, "reset failed for SerDes %d\n", sid);

	return ret;
}

static int rtsds_phy_reset(struct phy *phy)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;

	if (rtsds_readonly(ctrl, sid))
		return 0;

	return rtsds_phy_reset_int(ctrl, sid);
}

static const struct phy_ops rtsds_phy_ops = {
	.init		= rtsds_phy_init,
	.power_on	= rtsds_phy_power_on,
	.power_off	= rtsds_phy_power_off,
	.reset		= rtsds_phy_reset,
	.set_mode	= rtsds_phy_set_mode,
	.owner		= THIS_MODULE,
};

/*
 * The SerDes offer a lot of magic that sill needs to be uncovered. To help further development
 * provide some basic debugging about registers, modes and polarity. The mode can be changed on
 * the fly and executes the normal setter including events.
 */

#ifdef CONFIG_DEBUG_FS

#define RTSDS_PAGE_NAMES 48

static const char *rtsds_page_name[RTSDS_PAGE_NAMES] = {
	[0] = "SDS",		[1] = "SDS_EXT",
	[2] = "FIB",		[3] = "FIB_EXT",
	[4] = "DTE",		[5] = "DTE_EXT",
	[6] = "TGX",		[7] = "TGX_EXT",
	[8] = "ANA_RG",		[9] = "ANA_RG_EXT",
	[10] = "ANA_TG",	[11] = "ANA_TG_EXT",
	[31] = "ANA_WDIG",
	[32] = "ANA_MISC",	[33] = "ANA_COM",
	[34] = "ANA_SP",	[35] = "ANA_SP_EXT",
	[36] = "ANA_1G",	[37] = "ANA_1G_EXT",
	[38] = "ANA_2G",	[39] = "ANA_2G_EXT",
	[40] = "ANA_3G",	[41] = "ANA_3G_EXT",
	[42] = "ANA_5G",	[43] = "ANA_5G_EXT",
	[44] = "ANA_6G",	[45] = "ANA_6G_EXT",
	[46] = "ANA_10G",	[47] = "ANA_10G_EXT",
};

static ssize_t rtsds_dbg_mode_show(struct seq_file *seqf, void *unused)
{
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	int mode, sid = macro->sid;

	mutex_lock(&ctrl->lock);
	mode = ctrl->conf->get_mode(ctrl, sid);
	mutex_unlock(&ctrl->lock);

	seq_printf(seqf, "hw mode: 0x%X\n", mode);
	seq_puts(seqf, "phy mode: ");

	if (ctrl->sds[sid].mode == PHY_INTERFACE_MODE_NA)
		seq_puts(seqf, "off\n");
	else
		seq_printf(seqf, "%s\n", phy_modes(ctrl->sds[sid].mode));

	return 0;
}

static ssize_t rtsds_dbg_mode_write(struct file *file, const char __user *userbuf,
				size_t count, loff_t *ppos)
{
	struct seq_file *seqf = file->private_data;
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	int ret, hwmode, phymode, sid = macro->sid;

	ret = kstrtou32_from_user(userbuf, count, 16, &hwmode);
	if (ret)
		return ret;
	/*
	 * Allow to set arbitrary modes into the SerDes to improve error analysis. Accept that
	 * this might confuse the internal state tracking.
	 */
	phymode = rtsds_hwmode_to_phymode(ctrl, hwmode);
	rtsds_phy_set_mode_int(ctrl, sid, phymode, hwmode);

	return count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(rtsds_dbg_mode);

static ssize_t rtsds_dbg_reset_show(struct seq_file *seqf, void *unused)
{
	return 0;
}

static ssize_t rtsds_dbg_reset_write(struct file *file, const char __user *userbuf,
				size_t count, loff_t *ppos)
{
	struct seq_file *seqf = file->private_data;
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	int ret, reset, sid = macro->sid;

	ret = kstrtou32_from_user(userbuf, count, 10, &reset);
	if (ret || reset != 1)
		return ret;

	rtsds_phy_reset_int(ctrl, sid);

	return count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(rtsds_dbg_reset);

static int rtsds_dbg_registers_show(struct seq_file *seqf, void *unused)
{
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 page = 0, reg, sid = macro->sid;

	seq_printf(seqf, "%*s", 12, "");
	for (int i = 0; i < 32; i++)
		seq_printf(seqf, "   %02X", i);

	while (page < ctrl->conf->page_cnt) {
		if (page < RTSDS_PAGE_NAMES && rtsds_page_name[page])
			seq_printf(seqf, "\n%*s: ", -11, rtsds_page_name[page]);
		else if (page == 64 || page == 128)
			seq_printf(seqf, "\n\nXGMII_%d    : ", page >> 8);
		else
			seq_printf(seqf, "\nPAGE_%03d   : ", page);
		for (reg = 0; reg < 32; reg++)
			seq_printf(seqf, "%04X ", ctrl->conf->read(ctrl, sid, page, reg));

		page++;
	}
	seq_puts(seqf, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtsds_dbg_registers);

static int rtsds_dbg_polarity_show(struct seq_file *seqf, void *unused)
{
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 reg, sid = macro->sid;

	reg = ctrl->conf->read(ctrl, sid, RTSDS_PAGE_SDS, 0);

	seq_puts(seqf, "tx polarity: ");
	seq_puts(seqf, reg & RTSDS_BITS_INV_HSO ? "inverse" : "normal");
	seq_puts(seqf, "\nrx polarity: ");
	seq_puts(seqf, reg & RTSDS_BITS_INV_HSI ? "inverse" : "normal");
	seq_puts(seqf, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtsds_dbg_polarity);

static void rtsds_dbg_init(struct rtsds_ctrl *ctrl, u32 sid)
{
	debugfs_create_file("mode", 0600, ctrl->sds[sid].phy->debugfs,
			    &ctrl->sds[sid].phy->dev, &rtsds_dbg_mode_fops);

	debugfs_create_file("reset", 0200, ctrl->sds[sid].phy->debugfs,
			    &ctrl->sds[sid].phy->dev, &rtsds_dbg_reset_fops);

	debugfs_create_file("polarity", 0400, ctrl->sds[sid].phy->debugfs,
			    &ctrl->sds[sid].phy->dev, &rtsds_dbg_polarity_fops);

	debugfs_create_file("registers", 0400, ctrl->sds[sid].phy->debugfs,
			    &ctrl->sds[sid].phy->dev, &rtsds_dbg_registers_fops);
}
#endif /* CONFIG_DEBUG_FS */

static void rtsds_setup(struct rtsds_ctrl *ctrl)
{
	int hwmode, ret;

	for (u32 sid = 0; sid < ctrl->conf->sds_cnt; sid++) {
		if (ctrl->sds_mask & BIT(sid)) {
			/* power off controlled SerDes */
			hwmode = ctrl->conf->mode_map[PHY_INTERFACE_MODE_NA];
			ret = ctrl->conf->set_mode(ctrl, sid, hwmode);
			if (!ret)
				ret = rtsds_run_event(ctrl, sid, RTSDS_EVENT_SETUP);
			if (ret)
				dev_err(ctrl->dev, "setup failed for SerDes %d\n", sid);
		}
		/* in any case sync back hardware status */
		hwmode = ctrl->conf->get_mode(ctrl, sid);
		ctrl->sds[sid].mode = rtsds_hwmode_to_phymode(ctrl, hwmode);
	}
}

static struct phy *rtsds_simple_xlate(struct device *dev,
				      const struct of_phandle_args *args)
{
	struct rtsds_ctrl *ctrl = dev_get_drvdata(dev);
	int sid, sid2, min_port, max_port;

	/*
	 * Some Realtek Ethernet transceivers (e.g. RLT8218B) will be attached via a bonded 2x
	 * QSGMII link to two SerDes. Others (e.g. RTL8218D) allow to make use of single XGMII or
	 * dual QSGMII links. When a switch port tries to lookup the SerDes it is attached to
	 * honour that by an enhanced mapping. Allow two possible configuration options. Either
	 * standalone or linked to another. E.g.
	 *
	 * Single: port@24 { phys = <&serdes 4 -1 MinPort MaxPort>; };
	 * Dual:   port@24 { phys = <&serdes 4  5 MinPort MaxPort>; };
	 *
	 * This function will return the primary PHY of the link. The secondary can be identified
	 * later on by the link attribute in the controller structure.
	 */

	if (args->args_count != 4)
		return ERR_PTR(-EINVAL);

	sid = args->args[0];
	if (sid < 0 || sid >= ctrl->conf->sds_cnt)
		return ERR_PTR(-EINVAL);

	sid2 = args->args[1];
	if (sid2 < -1 || sid2 >= ctrl->conf->sds_cnt)
		return ERR_PTR(-EINVAL);

	/*
	 * Additionally to a linked SerDes also get the ports whose traffic is going through this
	 * SerDes. As of now the driver does not care about that but later on it will be helpful.
	 */

	min_port = args->args[2];
	if (min_port < 0)
		return ERR_PTR(-EINVAL);

	max_port = args->args[3];
	if (max_port < min_port)
		return ERR_PTR(-EINVAL);

	ctrl->sds[sid].link = sid2;
	if (sid2 >= 0)
		ctrl->sds[sid2].link = sid;

	ctrl->sds[sid].min_port = min_port;
	ctrl->sds[sid].max_port = max_port;

	return ctrl->sds[sid].phy;
}

static int rtsds_phy_create(struct rtsds_ctrl *ctrl, u32 sid)
{
	struct rtsds_macro *macro;

	ctrl->sds[sid].phy = devm_phy_create(ctrl->dev, NULL, &rtsds_phy_ops);
	if (IS_ERR(ctrl->sds[sid].phy))
		return PTR_ERR(ctrl->sds[sid].phy);

	macro = devm_kzalloc(ctrl->dev, sizeof(*macro), GFP_KERNEL);
	if (!macro)
		return -ENOMEM;

	macro->sid = sid;
	macro->ctrl = ctrl;
	phy_set_drvdata(ctrl->sds[sid].phy, macro);

	ctrl->sds[sid].link = -1;
	ctrl->sds[sid].min_port = -1;
	ctrl->sds[sid].max_port = -1;

#ifdef CONFIG_DEBUG_FS
	rtsds_dbg_init(ctrl, sid);
#endif
	return 0;
}

static int rtsds_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct rtsds_ctrl *ctrl;
	int ret;

	if (!np)
		return -EINVAL;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctrl->base)) {
		dev_err(dev, "failed to map SerDes memory\n");
		return PTR_ERR(ctrl->base);
	}

	ctrl->dev = dev;
	ctrl->conf = (struct rtsds_conf *)of_device_get_match_data(dev);

	ret = of_property_read_u32(np, "realtek,controlled-ports", &ctrl->sds_mask);
	if (ret)
		ctrl->sds_mask = GENMASK(ctrl->conf->sds_cnt, 0);

	for (u32 sid = 0; sid < ctrl->conf->sds_cnt; sid++) {
		ret = rtsds_phy_create(ctrl, sid);
		if (ret) {
			dev_err(dev, "failed to create PHY for SerDes %d\n", sid);
			return ret;
		}
	}

	mutex_init(&ctrl->lock);
	dev_set_drvdata(dev, ctrl);
	provider = devm_of_phy_provider_register(dev, rtsds_simple_xlate);
	rtsds_setup(ctrl);
	dev_info(dev, "initialized (%d SerDes, %d pages, 32 registers, mask 0x%04x)",
		 ctrl->conf->sds_cnt, ctrl->conf->page_cnt, ctrl->sds_mask);

	return PTR_ERR_OR_ZERO(provider);
}

#define MSK_ALL(a, b, c, d, e)	{ RTSDS_SEQ_MASK, PHY_INTERFACE_MODE_NA, a, b, c, d, e }
#define MSK_QSG(a, b, c, d, e)	{ RTSDS_SEQ_MASK, PHY_INTERFACE_MODE_QSGMII, a, b, c, d, e }
#define WAIT_MS(v)		{ RTSDS_SEQ_WAIT, 0, 0, 0, 0, v, 0 }
#define END_SEQ			{ RTSDS_SEQ_STOP, 0, 0, 0, 0, 0, 0 }

static struct rtsds_seq rtsds_838x_seq_setup[] = {
	WAIT_MS(1),
	/* all ports */
	MSK_ALL(0x003f, 0x02, 0x19, 0x0303, 0xffff),	MSK_ALL(0x003f, 0x01, 0x02, 0x85fa, 0xffff),
	/* ports 0-3 */
	MSK_ALL(0x0005, 0x01, 0x11, 0xb7c9, 0xffff),	MSK_ALL(0x000a, 0x01, 0x11, 0x4208, 0xffff),
	MSK_ALL(0x0005, 0x01, 0x12, 0xab8e, 0xffff),	MSK_ALL(0x000a, 0x01, 0x12, 0xc208, 0xffff),
	MSK_ALL(0x0005, 0x01, 0x13, 0x24ab, 0xffff),	MSK_ALL(0x0001, 0x01, 0x03, 0xf46f, 0xffff),
	MSK_ALL(0x0002, 0x01, 0x0a, 0x80c7, 0xffff),	MSK_ALL(0x0004, 0x01, 0x03, 0xf46d, 0xffff),
	MSK_ALL(0x000f, 0x01, 0x0b, 0x0482, 0xffff),	MSK_ALL(0x000f, 0x01, 0x0e, 0xfcc2, 0xffff),
	MSK_ALL(0x000f, 0x00, 0x01, 0x0f00, 0xffff),	MSK_ALL(0x000f, 0x00, 0x02, 0x7060, 0xffff),
	MSK_ALL(0x000f, 0x01, 0x06, 0x20d8, 0xffff),
	/* port 4 */
	MSK_ALL(0x0010, 0x01, 0x06, 0x20d8, 0xffff),	MSK_ALL(0x0010, 0x01, 0x0a, 0x00c3, 0xffff),
	MSK_ALL(0x0010, 0x01, 0x0b, 0x1482, 0xffff),	MSK_ALL(0x0010, 0x01, 0x0e, 0xfcc2, 0xffff),
	MSK_ALL(0x0010, 0x01, 0x11, 0xb7c9, 0xffff),	MSK_ALL(0x0010, 0x01, 0x12, 0xab8e, 0xffff),
	/* port 5 */
	MSK_ALL(0x0020, 0x01, 0x03, 0x0000, 0xffff),	MSK_ALL(0x0020, 0x01, 0x04, 0xdccc, 0xffff),
	MSK_ALL(0x0020, 0x01, 0x05, 0x0000, 0xffff),	MSK_ALL(0x0020, 0x01, 0x06, 0x3600, 0xffff),
	MSK_ALL(0x0020, 0x01, 0x07, 0x0003, 0xffff),	MSK_ALL(0x0020, 0x01, 0x08, 0x79aa, 0xffff),
	MSK_ALL(0x0020, 0x01, 0x09, 0x8c64, 0xffff),	MSK_ALL(0x0020, 0x01, 0x0a, 0x00c3, 0xffff),
	MSK_ALL(0x0020, 0x01, 0x0b, 0x1482, 0xffff),	MSK_ALL(0x0020, 0x01, 0x0e, 0xf002, 0xffff),
	MSK_ALL(0x0020, 0x02, 0x1b, 0x04bf, 0xffff),	MSK_ALL(0x0020, 0x02, 0x18, 0x14aa, 0xffff),
	MSK_ALL(0x0020, 0x02, 0x1b, 0x04bf, 0xffff),	END_SEQ,
};

static struct rtsds_seq rtsds_839x_seq_setup[] = {
	/* ports 0-7, 10-11 (5G) */
	MSK_ALL(0x0cff, 0x08, 0x11, 0x0000, 0x8000),	MSK_ALL(0x0cff, 0x08, 0x11, 0x1000, 0x7000),
	MSK_ALL(0x0cff, 0x08, 0x11, 0x0400, 0x0e00),	MSK_ALL(0x0cff, 0x08, 0x11, 0x00c0, 0x01c0),
	MSK_ALL(0x0cff, 0x08, 0x16, 0x0000, 0x8000),	MSK_ALL(0x0cff, 0x08, 0x08, 0x0000, 0x0008),
	MSK_ALL(0x0cff, 0x08, 0x07, 0x0000, 0x0200),	MSK_ALL(0x0cff, 0x08, 0x07, 0x0000, 0x0100),
	MSK_ALL(0x0cff, 0x08, 0x14, 0x0002, 0x0002),	MSK_ALL(0x0cff, 0x08, 0x15, 0xc000, 0xf000),
	MSK_ALL(0x0cff, 0x08, 0x16, 0x0000, 0x1000),	MSK_ALL(0x0cff, 0x08, 0x18, 0x0006, 0x003f),
	MSK_ALL(0x0cff, 0x08, 0x08, 0x0000, 0x0040),	MSK_ALL(0x0cff, 0x08, 0x08, 0x0000, 0x0800),
	MSK_ALL(0x0cff, 0x08, 0x08, 0x8000, 0xf000),	MSK_ALL(0x0cff, 0x08, 0x08, 0x0400, 0x0780),
	/* ports 10-11 (clock edge)			ports 4, 10 (full swing)) */
	MSK_ALL(0x0c00, 0x00, 0x07, 0x4000, 0x4000),	MSK_ALL(0x0410, 0x08, 0x08, 0x8c6a, 0xffff),
	/* ports 8-9, 12-13 (10G digital patch) */
	MSK_ALL(0x3300, 0x0a, 0x00, 0x5800, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x01, 0x4000, 0xffff),
	MSK_ALL(0x1100, 0x0a, 0x02, 0x5400, 0xffff),	MSK_ALL(0x2200, 0x0a, 0x02, 0x5000, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x03, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x04, 0x0000, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x05, 0x4000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x06, 0x4000, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x07, 0xffff, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x08, 0xffff, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x09, 0x806f, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x0a, 0x0004, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x0b, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x0c, 0x0000, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x0d, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x0e, 0x0a00, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x0f, 0x2000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x10, 0xf00e, 0xffff),
	MSK_ALL(0x1100, 0x0a, 0x11, 0xf04a, 0xffff),	MSK_ALL(0x2200, 0x0a, 0x11, 0xfdab, 0xffff),
	MSK_ALL(0x1100, 0x0a, 0x12, 0x97b3, 0xffff),	MSK_ALL(0x2200, 0x0a, 0x12, 0x96ea, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x13, 0x5318, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x14, 0x0f03, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x15, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x16, 0x0000, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x17, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x18, 0x0000, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x19, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x1a, 0xffff, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x1b, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x1c, 0x1203, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x1d, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x1e, 0xa052, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x1f, 0x9a00, 0xffff),	MSK_ALL(0x3300, 0x0b, 0x00, 0x00f5, 0xffff),
	MSK_ALL(0x3300, 0x0b, 0x01, 0xf000, 0xffff),	MSK_ALL(0x1100, 0x0b, 0x02, 0x41ff, 0xffff),
	MSK_ALL(0x2200, 0x0b, 0x02, 0x4079, 0xffff),	MSK_ALL(0x3300, 0x0b, 0x03, 0x0000, 0xffff),
	MSK_ALL(0x1100, 0x0b, 0x04, 0x39ff, 0xffff),	MSK_ALL(0x2200, 0x0b, 0x04, 0x93fa, 0xffff),
	MSK_ALL(0x3300, 0x0b, 0x05, 0x3340, 0xffff),	MSK_ALL(0x1100, 0x0b, 0x06, 0x40aa, 0xffff),
	MSK_ALL(0x2200, 0x0b, 0x06, 0x4280, 0xffff),	MSK_ALL(0x3300, 0x0b, 0x07, 0x0000, 0xffff),
	MSK_ALL(0x3300, 0x0b, 0x08, 0x801f, 0xffff),	MSK_ALL(0x3300, 0x0b, 0x09, 0x0000, 0xffff),
	MSK_ALL(0x3300, 0x0b, 0x0a, 0x619c, 0xffff),	MSK_ALL(0x3300, 0x0b, 0x0b, 0xffed, 0xffff),
	MSK_ALL(0x3300, 0x0b, 0x0c, 0x29ff, 0xffff),	MSK_ALL(0x3300, 0x0b, 0x0d, 0x29ff, 0xffff),
	MSK_ALL(0x1100, 0x0b, 0x0e, 0x4e10, 0xffff),	MSK_ALL(0x2200, 0x0b, 0x0e, 0x4c50, 0xffff),
	MSK_ALL(0x1100, 0x0b, 0x0f, 0x4e10, 0xffff),	MSK_ALL(0x2200, 0x0b, 0x0f, 0x4c50, 0xffff),
	MSK_ALL(0x3300, 0x0b, 0x10, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0b, 0x11, 0x0000, 0xffff),
	MSK_ALL(0x3300, 0x00, 0x0c, 0x08ec, 0xffff),	MSK_ALL(0x2200, 0x0b, 0x1f, 0x003f, 0xffff),
	MSK_ALL(0x3300, 0x00, 0x07, 0x4000, 0x4000),
	/* ports 8-9, 12-13 (10G analog patch) */
	MSK_ALL(0x3300, 0x0b, 0x09, 0x417f, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x1c, 0x0000, 0x0200),
	MSK_ALL(0x3300, 0x0a, 0x1c, 0x0000, 0x1c00),	MSK_ALL(0x3300, 0x0a, 0x1c, 0x0028, 0x0038),
	MSK_ALL(0x3300, 0x0a, 0x1c, 0x0000, 0x01c0),	MSK_ALL(0x3300, 0x0a, 0x1c, 0x0002, 0x0007),
	MSK_ALL(0x3300, 0x0b, 0x01, 0xc440, 0xffff),	MSK_ALL(0x1100, 0x0b, 0x06, 0x0000, 0x0008),
	MSK_ALL(0x3300, 0x0a, 0x05, 0x8000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x06, 0x8000, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x0a, 0x0000, 0xffff),	MSK_ALL(0x3300, 0x0a, 0x1e, 0x0002, 0xffff),
	MSK_ALL(0x3300, 0x0a, 0x1f, 0xbe00, 0xffff),	MSK_ALL(0x1100, 0x0b, 0x0e, 0x0000, 0x0400),
	MSK_ALL(0x1100, 0x0b, 0x0f, 0x0000, 0x0400),	MSK_ALL(0x1100, 0x0b, 0x0e, 0x0000, 0x4000),
	MSK_ALL(0x1100, 0x0b, 0x0f, 0x0000, 0x4000),	MSK_ALL(0x3300, 0x0a, 0x10, 0x0000, 0x0020),
	MSK_ALL(0x3300, 0x0b, 0x09, 0x0000, 0x0100),	MSK_ALL(0x3300, 0x0a, 0x03, 0xf000, 0xf000),
	MSK_ALL(0x3300, 0x0a, 0x1f, 0x3000, 0x3000),	MSK_ALL(0x3300, 0x0a, 0x1f, 0x0e00, 0x0e00),
	MSK_ALL(0x3300, 0x0b, 0x01, 0x8000, 0x8000),	MSK_ALL(0x3300, 0x0b, 0x01, 0x4000, 0x4000),
	MSK_ALL(0x3300, 0x0b, 0x01, 0x0000, 0x2000),	MSK_ALL(0x3300, 0x0b, 0x01, 0x0000, 0x1000),
	MSK_ALL(0x3300, 0x0b, 0x01, 0x0400, 0x0e00),	MSK_ALL(0x3300, 0x0b, 0x01, 0x0080, 0x01c0),
	MSK_ALL(0x3300, 0x0b, 0x01, 0x0000, 0x0038),	MSK_ALL(0x3300, 0x0b, 0x01, 0x0000, 0x0007),
	MSK_ALL(0x3300, 0x0b, 0x0c, 0x0200, 0x0200),	MSK_ALL(0x3300, 0x0b, 0x0d, 0x0200, 0x0200),
	MSK_ALL(0x3300, 0x0b, 0x08, 0x0020, 0x0020),	MSK_ALL(0x3300, 0x0b, 0x08, 0x0000, 0x0040),
	MSK_ALL(0x3300, 0x0a, 0x1c, 0x0000, 0x8000),	MSK_ALL(0x3300, 0x0a, 0x10, 0x0000, 0xf000),
	MSK_ALL(0x3300, 0x0a, 0x13, 0x0000, 0x0010),	MSK_ALL(0x3300, 0x0a, 0x13, 0x0000, 0x0200),
	MSK_ALL(0x3300, 0x0a, 0x13, 0x0008, 0x000f),	MSK_ALL(0x3300, 0x0a, 0x13, 0x0100, 0x01e0),
	END_SEQ,
};

static const struct rtsds_conf rtsds_838x_conf = {
	.sds_cnt	= RTSDS_838X_SDS_CNT,
	.page_cnt	= RTSDS_838X_PAGE_CNT,
	.mask		= rtsds_838x_mask,
	.read		= rtsds_838x_read,
	.reset		= rtsds_838x_reset,
	.set_mode	= rtsds_838x_set_mode,
	.get_mode	= rtsds_838x_get_mode,
	.mode_map = {
		[PHY_INTERFACE_MODE_NA]		= RTSDS_COMBOMODE(0x00, 0x00),
		[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_COMBOMODE(0x04, 0x01),
		[PHY_INTERFACE_MODE_100BASEX]	= RTSDS_COMBOMODE(0x05, 0x01),
		[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_COMBOMODE(0x06, 0x00),
	},
	.sequence = {
		[RTSDS_EVENT_SETUP]		= rtsds_838x_seq_setup,
	}
};

static const struct rtsds_conf rtsds_839x_conf = {
	.sds_cnt	= RTSDS_839X_SDS_CNT,
	.page_cnt	= RTSDS_839X_PAGE_CNT,
	.mask		= rtsds_839x_mask,
	.read		= rtsds_839x_read,
	.reset		= rtsds_839x_reset,
	.set_mode	= rtsds_839x_set_mode,
	.get_mode	= rtsds_839x_get_mode,
	.mode_map = {
		[PHY_INTERFACE_MODE_NA]		= RTSDS_COMBOMODE(0x00, 0x00),
		[PHY_INTERFACE_MODE_10GBASER]	= RTSDS_COMBOMODE(0x01, 0x00),
		[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_COMBOMODE(0x07, 0x00),
		[PHY_INTERFACE_MODE_100BASEX]	= RTSDS_COMBOMODE(0x08, 0x00),
		[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_COMBOMODE(0x06, 0x00),
		[PHY_INTERFACE_MODE_SGMII]	= RTSDS_COMBOMODE(0x07, 0x05),
	},
	.sequence = {
		[RTSDS_EVENT_SETUP]		= rtsds_839x_seq_setup,
	}
};

static const struct rtsds_conf rtsds_930x_conf = {
	.sds_cnt	= RTSDS_930X_SDS_CNT,
	.page_cnt	= RTSDS_930X_PAGE_CNT,
	.mask		= rtsds_930x_mask,
	.read		= rtsds_930x_read,
	.reset		= rtsds_930x_reset,
	.set_mode	= rtsds_930x_set_mode,
	.get_mode	= rtsds_930x_get_mode,
	.mode_map = {
		[PHY_INTERFACE_MODE_NA]		= RTSDS_COMBOMODE(0x1f, 0x00),
		[PHY_INTERFACE_MODE_10GBASER]	= RTSDS_COMBOMODE(0x1a, 0x00),
		[PHY_INTERFACE_MODE_2500BASEX]  = RTSDS_COMBOMODE(0x16, 0x00),
		[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_COMBOMODE(0x04, 0x00),
		[PHY_INTERFACE_MODE_USXGMII]	= RTSDS_COMBOMODE(0x0d, 0x00),
		[PHY_INTERFACE_MODE_QUSGMII]	= RTSDS_COMBOMODE(0x0d, 0x02),
		[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_COMBOMODE(0x06, 0x00),
	},
	.sequence = {
		[RTSDS_EVENT_SETUP]		= NULL,
	}
};

static const struct rtsds_conf rtsds_931x_conf = {
	.sds_cnt	= RTSDS_931X_SDS_CNT,
	.page_cnt	= RTSDS_931X_PAGE_CNT,
	.mask		= rtsds_931x_mask,
	.read		= rtsds_931x_read,
	.reset		= rtsds_931x_reset,
	.set_mode	= rtsds_931x_set_mode,
	.get_mode	= rtsds_931x_get_mode,
	.mode_map = {
		[PHY_INTERFACE_MODE_NA]		= RTSDS_COMBOMODE(0x1f, 0x3f),
		[PHY_INTERFACE_MODE_10GBASER]	= RTSDS_COMBOMODE(0x1f, 0x35),
		[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_COMBOMODE(0x1f, 0x39),
		[PHY_INTERFACE_MODE_USXGMII]	= RTSDS_COMBOMODE(0x0d, 0x00),
		[PHY_INTERFACE_MODE_XGMII]	= RTSDS_COMBOMODE(0x0a, 0x00),
		[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_COMBOMODE(0x06, 0x00),
	},
	.sequence = {
		[RTSDS_EVENT_SETUP]		= NULL,
	}
};

static const struct of_device_id rtsds_compatible_ids[] = {
	{ .compatible = "realtek,rtl8380-serdes",
	  .data = &rtsds_838x_conf,
	},
	{ .compatible = "realtek,rtl8390-serdes",
	  .data = &rtsds_839x_conf,
	},
	{ .compatible = "realtek,rtl9300-serdes",
	  .data = &rtsds_930x_conf,
	},
	{ .compatible = "realtek,rtl9310-serdes",
	  .data = &rtsds_931x_conf,
	},
	{},
};
MODULE_DEVICE_TABLE(of, rtsds_compatible_ids);

static struct platform_driver rtsds_platform_driver = {
	.probe		= rtsds_probe,
	.driver		= {
		.name	= "realtek,otto-serdes",
		.of_match_table = of_match_ptr(rtsds_compatible_ids),
	},
};

module_platform_driver(rtsds_platform_driver);

MODULE_AUTHOR("Markus Stockhausen <markus.stockhausen@gmx.de>");
MODULE_DESCRIPTION("SerDes driver for Realtek RTL83xx, RTL93xx switch SoCs");
MODULE_LICENSE("GPL");
