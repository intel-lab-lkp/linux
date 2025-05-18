// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL838x, RTL839x, RTL930x & RTL931x SerDes PHY driver
 * Copyright (c) 2025 Markus Stockhausen <markus.stockhausen@gmx.de>
 */

#include <linux/crc32.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "phy-rtk-otto-serdes.h"

/* common helpers */

static inline const char *rtsds_phy_modes(phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_NA:
		return "off";
	case RTSDS_REALTEK_MODE_HSGMII:
		return "realtek-hsgmii"; /* See rtsds_phy_set_mode() */
	default:
		return phy_modes(interface);
	}
}

static inline int rtsds_write_bits_pos(struct regmap *map, unsigned int reg,
				       unsigned int bitpos, unsigned int mask, unsigned int val)
{
	return regmap_write_bits(map, reg, mask << bitpos, val << bitpos);
}

static inline int rtsds_read_bits_pos(struct regmap *map, unsigned int reg,
				      unsigned int bitpos, unsigned int mask, unsigned int *val)
{
	int ret = regmap_read(map, reg, val);

	*val = (*val >> bitpos) & mask;

	return ret;
}

static inline bool rtsds_invalid_reg(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	return (sid >= ctrl->cfg->sds_cnt || page >= ctrl->cfg->page_cnt || reg > 31);
}

static inline bool rtsds_invalid_sds(struct rtsds_ctrl *ctrl, u32 sid)
{
	return (sid >= ctrl->cfg->sds_cnt);
}

static inline bool rtsds_invalid_mask(u32 mask, u32 val)
{
	return (val & mask) != val;
}

static int rtsds_hwmode_to_phymode(struct rtsds_ctrl *ctrl, int hwmode)
{
	for (int m = 0; m < PHY_INTERFACE_MODE_MAX; m++)
		if (ctrl->cfg->mode_map[m] == hwmode)
			return m;

	return PHY_INTERFACE_MODE_MAX;
}

static void rtsds_check_and_fix_mode(struct rtsds_ctrl *ctrl, u32 sid)
{
	int mode;

	mutex_lock(&ctrl->lock);
	mode = rtsds_hwmode_to_phymode(ctrl, ctrl->cfg->get_hwmode(ctrl, sid));
	if (unlikely(ctrl->sds[sid].mode != mode)) {
		/* Ouch, this should not occur. Nevertheless sync to keep driver operational. */
		dev_err(ctrl->dev, "driver mode %s out of sync with hardware mode %s, fixing\n",
			rtsds_phy_modes(ctrl->sds[sid].mode), rtsds_phy_modes(mode));
		ctrl->sds[sid].mode = mode;
	}
	mutex_unlock(&ctrl->lock);
}

static void rtsds_get_chip(struct rtsds_ctrl *ctrl)
{
	u32 val, act, reg;

	if (ctrl->cfg->family == RTSDS_838X_CFG_FAMILY) {
		reg = RTSDS_838X_MODEL_NAME_INFO_REG;
		act = RTSDS_83XX_SDS_CHIP_INFO_EN;
	} else if (ctrl->cfg->family == RTSDS_839X_CFG_FAMILY) {
		reg = RTSDS_839X_MODEL_NAME_INFO_REG;
		act = RTSDS_83XX_SDS_CHIP_INFO_EN;
	} else {
		reg = RTSDS_93XX_REG_MODEL_NAME_INFO;
		act = RTSDS_93XX_SDS_CHIP_INFO_EN;
	}

	regmap_read(ctrl->regmap, reg, &val);
	ctrl->soc.model_id = FIELD_GET(RTSDS_MODEL_ID_MASK, val);
	ctrl->soc.model_version = FIELD_GET(RTSDS_MODEL_VERSION_MASK, val);

	regmap_write(ctrl->regmap, reg + 4, act);
	regmap_read(ctrl->regmap, reg + 4, &val);
	ctrl->soc.chip_id = FIELD_GET(RTSDS_CHIP_ID_MASK, val);

	if (ctrl->cfg->family == RTSDS_838X_CFG_FAMILY ||
	    ctrl->cfg->family == RTSDS_839X_CFG_FAMILY)
		ctrl->soc.chip_version = FIELD_GET(RTSDS_83XX_SDS_CHIP_RL_ID_MASK, val);
	else
		ctrl->soc.chip_version = FIELD_GET(RTSDS_93XX_SDS_CHIP_RL_ID_MASK, val);

	snprintf(ctrl->soc.model_name, sizeof(ctrl->soc.model_name),
		 "RTL%04X%c", ctrl->soc.model_id,
		 ctrl->soc.model_version ? ctrl->soc.model_version + 64 : 0);

	snprintf(ctrl->soc.chip_name, sizeof(ctrl->soc.chip_name),
		 "%04X%c", ctrl->soc.chip_id,
		 ctrl->soc.chip_version ? ctrl->soc.chip_version + 64 : 0);
}

/*
 * A Realtek Otto SerDes is configured/patched by writing specific values into its registers.
 * These values are bound to the individual hardware and the transceivers that are connected to
 * it. Depending on the model, some of this might be integrated into the bootloader. To fully
 * support different configurations allow the driver to load firmware files and run patch
 * sequences.
 *
 * A firmware file contains a head, a directory and at the end the raw patch data. See
 * structure rtsds_fw_head, rtsds_fw_dir an rtsds_fw_seq for more details.
 *
 * header
 *	(u32) magic = 0x83009300, see RTSDS_FW_MAGIC
 *	(u32) CRC checksum of the following data
 *	(u32) filesize
 *	(u32) directory size = number of sequences
 *
 * directory with one or more blocks consisting of
 *	(u32) id of the sequence. See RTSDS_FW_EVT_xxx
 *	(u32) offset in bytes of patch data for this directory entry
 *	(u32) length in bytes of patch data for this directory entry
 *	(u32) future use to avoid structure breakage
 *
 * patch data with one ore more blocks consisting of
 *	(u16) action to process. See RTSDS_FW_OP_xxx
 *	(u16) mode for which the command is to be executed. See RTSDS_FW_MODE_xxx
 *	(u16) SerDes ports bitmask for which the command is to be executed
 *	(u16) page for action
 *	(u16) register for action
 *	(u16) value for action
 *	(u16) mask for write operations
 *	(u16) future use to avoid structure breakage
 */

static const char *rtsds_fw_events[RTSDS_FW_EVT_MAX] = {
	[RTSDS_FW_EVT_SETUP]		= "setup",
	[RTSDS_FW_EVT_INIT]		= "init",
	[RTSDS_FW_EVT_POWER_ON]		= "power-on",
	[RTSDS_FW_EVT_PRE_SET_MODE]	= "pre-set-mode",
	[RTSDS_FW_EVT_POST_SET_MODE]	= "post-set-mode",
	[RTSDS_FW_EVT_PRE_RESET]	= "pre-reset",
	[RTSDS_FW_EVT_POST_RESET]	= "post-reset",
	[RTSDS_FW_EVT_PRE_POWER_OFF]	= "pre-power-off",
	[RTSDS_FW_EVT_POST_POWER_OFF]	= "post-power-off",
};

static const u8 rtsds_fw_modes[PHY_INTERFACE_MODE_MAX] = {
	[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_FW_MODE_1000BASEX,
	[PHY_INTERFACE_MODE_100BASEX]	= RTSDS_FW_MODE_100BASEX,
	[PHY_INTERFACE_MODE_10GBASER]	= RTSDS_FW_MODE_10GBASER,
	[PHY_INTERFACE_MODE_2500BASEX]	= RTSDS_FW_MODE_2500BASEX,
	[PHY_INTERFACE_MODE_NA]		= RTSDS_FW_MODE_ALL,
	[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_FW_MODE_QSGMII,
	[PHY_INTERFACE_MODE_QUSGMII]	= RTSDS_FW_MODE_QUSGMII,
	[PHY_INTERFACE_MODE_SGMII]	= RTSDS_FW_MODE_SGMII,
	[PHY_INTERFACE_MODE_USXGMII]	= RTSDS_FW_MODE_USXGMII,
	[PHY_INTERFACE_MODE_XGMII]	= RTSDS_FW_MODE_XGMII,
	[RTSDS_REALTEK_MODE_HSGMII]	= RTSDS_FW_MODE_HSGMII,
};

static int rtsds_fw_load(struct rtsds_ctrl *ctrl)
{
	struct rtsds_fw_head *h;
	const char *fwname;
	const char *msg;
	u32 checksum;

	if (device_property_read_string(ctrl->dev, "firmware-name", &fwname)) {
		dev_info(ctrl->dev, "firmware not configured, patching disabled\n");
		return -EACCES;
	}

	if (firmware_request_nowarn(&ctrl->firmware, fwname, ctrl->dev) < 0) {
		dev_err(ctrl->dev, "firmware %s not found, patching disabled\n", fwname);
		return -ENOENT;
	}

	if (ctrl->firmware->size < 16) {
		msg = "size to small";
		goto error;
	}

	h = (struct rtsds_fw_head *)ctrl->firmware->data;
	if (h->magic != RTSDS_FW_MAGIC) {
		msg = "magic mismatch";
		goto error;
	}

	if (h->filesize != ctrl->firmware->size) {
		msg = "size mismatch";
		goto error;
	}

	checksum = ~crc32(0xFFFFFFFFU, ctrl->firmware->data + 8, ctrl->firmware->size - 8);
	if (h->checksum != checksum) {
		msg = "checksum mismatch";
		goto error;
	}

	for (int i = 0; i < h->dirsize; i++)
		if ((ctrl->firmware->size <= h->dir[i].offset) ||
		    (ctrl->firmware->size < h->dir[i].offset + h->dir[i].len) ||
		    (h->dir[i].len % sizeof(struct rtsds_fw_seq))) {
			msg = "malformed sequence";
			goto error;
		}

	dev_info(ctrl->dev, "firmware %s: loaded with %zu bytes, %d sequences\n",
		 fwname, ctrl->firmware->size, h->dirsize);

	return 0;
error:
	dev_err(ctrl->dev, "firmware %s: %s, patching disabled\n", fwname, msg);
	ctrl->firmware = NULL;
	return -EINVAL;
}

static void rtsds_fw_get_sequence(struct rtsds_ctrl *ctrl, int evt,
				  struct rtsds_fw_seq **seq, int *cnt)
{
	struct rtsds_fw_head *h;

	*seq = NULL;
	*cnt = 0;

	if (!ctrl->firmware)
		return;

	h = (struct rtsds_fw_head *)ctrl->firmware->data;
	for (int i = 0; i < h->dirsize; i++)
		if (h->dir[i].evtid == evt) {
			*seq = (struct rtsds_fw_seq *)(ctrl->firmware->data + h->dir[i].offset);
			*cnt = h->dir[i].len / sizeof(struct rtsds_fw_seq);
		}
}

static int rtsds_fw_run_event(struct rtsds_ctrl *ctrl, u32 sid, int evt)
{
	int cnt, ret, step = 0, delay = 0, mode = rtsds_fw_modes[ctrl->sds[sid].mode];
	struct rtsds_fw_seq *seq;

	if (evt >= RTSDS_FW_EVT_MAX ||
	    sid >= ctrl->cfg->sds_cnt ||
	    mode == RTSDS_FW_MODE_UNDEFINED)
		return -EINVAL;

	rtsds_fw_get_sequence(ctrl, evt, &seq, &cnt);
	if (!seq)
		return 0;

	seq--;
	while (cnt) {
		step++;	seq++; cnt--;

		if (!(seq->ports & BIT(sid)) ||
		    (seq->mode != RTSDS_FW_MODE_ALL && seq->mode != mode))
			continue;

		if (seq->action == RTSDS_FW_OP_WAIT) {
			delay = seq->val;
			continue;
		}

		if (delay) {
			dev_dbg(ctrl->dev, "%s/%03d: SDS %02d WAIT(%d)\n",
				rtsds_fw_events[evt], step, sid, delay);

			usleep_range(delay << 10, (delay + 1) << 10);
		}

		if (seq->action == RTSDS_FW_OP_MASK) {
			dev_dbg(ctrl->dev,
				"%s/%03d: SDS %02d MASK(0x%04x, 0x%04x, 0x%04x, 0x%04x)\n",
				rtsds_fw_events[evt], step, sid,
				seq->page, seq->reg, seq->mask, seq->val);

			ret = ctrl->cfg->write_bits(ctrl, sid, seq->page,
						    seq->reg, seq->mask, seq->val);
			if (ret) {
				dev_err(ctrl->dev,
					"sequence %s failed for SerDes %d at step %d, rc=%d",
					rtsds_fw_events[evt], sid, step, ret);
				return -EIO;
			}
		}
	}

	return 0;
}

/* common RTL838x and RTL839x helpers */

static inline int rtsds_83xx_sds_5g(u32 sid)
{
	return (GENMASK(11, 10) | GENMASK(7, 0)) & BIT(sid);
}

static void rtsds_83xx_rx_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	u32 page, reg, bits;

	if (rtsds_83xx_sds_5g(sid)) {
		/* RTL838x or RTL839x 5G SerDes */
		page = RTSDS_SDS_EXT_PAGE;
		reg = 0x09;
		bits = RTSDS_RX_SELF_BIT;
	} else if (sid == 8 || sid == 12) {
		/* RTL839x 10G SerDes */
		page = RTSDS_ANA_TG_EXT_PAGE;
		reg = 0x00;
		bits = RTSDS_RX_SELF_10G_BIT;
	} else
		return;

	ctrl->cfg->write_bits(ctrl, sid, page, reg, bits, bits);
	usleep_range(100000, 101000);
	ctrl->cfg->write_bits(ctrl, sid, page, reg, bits, 0);
}

static void rtsds_83xx_digital_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	int bits;

	/* soft reset */
	bits = RTSDS_SOFT_RST_BIT;
	ctrl->cfg->write_bits(ctrl, sid, RTSDS_SDS_PAGE, 0x03, bits, bits);
	usleep_range(100000, 101000);
	ctrl->cfg->write_bits(ctrl, sid, RTSDS_SDS_PAGE, 0x03, bits, 0);

	/* SerDes RX/TX reset */
	bits = RTSDS_SDS_EN_RX_BIT | RTSDS_SDS_EN_TX_BIT;
	ctrl->cfg->write_bits(ctrl, sid, RTSDS_SDS_PAGE, 0x00, bits, 0);
	ctrl->cfg->write_bits(ctrl, sid, RTSDS_SDS_PAGE, 0x00, bits, bits);
}

static void rtsds_83xx_cmu_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	int mask, bpos, hi = sid | 1;

	if (ctrl->cfg->family == RTSDS_838X_CFG_FAMILY) {
		/*
		 * 5G SerDes sequence for SDS_EXT_REG0. This is some very magic flipping of
		 * fields FRC_PDOWN, VAL_RXEN, FRC_RXEN, VAL_CMUEN & FRC_CMUEN. See functions
		 * rtl8380_serdes_rst() or _dal_maple_mac_serdes_rst() in GPL dumps.
		 */
		ctrl->cfg->write_bits(ctrl, sid, RTSDS_SDS_EXT_PAGE, 0x00,
				      RTSDS_FULL_REG_MASK, 0x4040);
		ctrl->cfg->write_bits(ctrl, sid, RTSDS_SDS_EXT_PAGE, 0x00,
				      RTSDS_FULL_REG_MASK, 0x4740);
		ctrl->cfg->write_bits(ctrl, sid, RTSDS_SDS_EXT_PAGE, 0x00,
				      RTSDS_FULL_REG_MASK, 0x47c0);
		ctrl->cfg->write_bits(ctrl, sid, RTSDS_SDS_EXT_PAGE, 0x00,
				      RTSDS_FULL_REG_MASK, 0x4000);
	} else if (rtsds_83xx_sds_5g(sid)) {
		/*
		 * 5G SerDes sequence for undocumented shared CMU register in odd SerDes.
		 * See function rtl839x_serdes_rst() or sequence rtl839x_serdes_rst[] in GPL
		 * dumps. Bits 4, 5 are for even SerDes, bits 6, 7 are for odd SerDes.
		 */
		if (sid & 1) {
			mask = GENMASK(7, 6); bpos = 6;
		} else {
			mask = GENMASK(5, 4); bpos = 4;
		}
		ctrl->cfg->write_bits(ctrl, hi, RTSDS_ANA_RG_EXT_PAGE, 0x01, mask, 1 << bpos);
		ctrl->cfg->write_bits(ctrl, hi, RTSDS_ANA_RG_EXT_PAGE, 0x01, mask, 3 << bpos);
		ctrl->cfg->write_bits(ctrl, hi, RTSDS_ANA_RG_EXT_PAGE, 0x01, mask, 0);
	} else {
		/*
		 * 10G SerDes sequence for undocumented shared CMU register in odd SerDes.
		 * See function rtl839x_serdes_rst() or sequences rtl839x_serdes8/12_rst[]
		 * in GPL dumps. Bits 0, 1 are for even SerDes, bits 2, 3 are for odd SerDes.
		 */
		if (sid & 1) {
			mask = GENMASK(3, 2); bpos = 2;
		} else {
			mask = GENMASK(1, 0); bpos = 0;
		}
		ctrl->cfg->write_bits(ctrl, hi, RTSDS_ANA_TG_EXT_PAGE, 0x1d, mask, 1 << bpos);
		usleep_range(500000, 501000);
		ctrl->cfg->write_bits(ctrl, hi, RTSDS_ANA_TG_EXT_PAGE, 0x1d, mask, 3 << bpos);
		ctrl->cfg->write_bits(ctrl, hi, RTSDS_ANA_TG_EXT_PAGE, 0x1d, mask, 0);
	}
}

static int rtsds_83xx_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_83xx_rx_reset(ctrl, sid);
	rtsds_83xx_cmu_reset(ctrl, sid);
	rtsds_83xx_digital_reset(ctrl, sid);

	return 0;
}

/* common RTL930x and RTL931x helpers */

static inline int rtsds_rt93xx_io(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg, u32 cmd)
{
	int ret, val = FIELD_PREP(RTSDS_93XX_SDS_CMD_SID_MASK, sid) |
		       FIELD_PREP(RTSDS_93XX_SDS_CMD_PAGE_MASK, page) |
		       FIELD_PREP(RTSDS_93XX_SDS_CMD_REG_MASK, reg) |
		       RTSDS_93XX_SDS_CMD_BUSY | cmd;

	regmap_write(ctrl->regmap, ctrl->regbase, val);
	ret = regmap_read_poll_timeout(ctrl->regmap, ctrl->regbase, val,
				       !(val & RTSDS_93XX_SDS_CMD_BUSY), 30, 1000);

	if (ret < 0) {
		dev_err(ctrl->dev, "SerDes I/O timed out\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int rtsds_93xx_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	u32 val;

	if (rtsds_rt93xx_io(ctrl, sid, page, reg, RTSDS_93XX_SDS_CMD_READ))
		return -ETIMEDOUT;

	regmap_read(ctrl->regmap, ctrl->regbase + 4, &val);

	return val & RTSDS_FULL_REG_MASK;
}

static int rtsds_93xx_write_bits(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg,
				 u32 mask, u32 val)
{
	if (mask != RTSDS_FULL_REG_MASK) {
		int oldval = rtsds_93xx_read(ctrl, sid, page, reg);

		if (oldval < 0)
			return -EIO;
		val |= oldval & ~mask;
	}

	regmap_write(ctrl->regmap, ctrl->regbase + 4, val);

	return rtsds_rt93xx_io(ctrl, sid, page, reg, RTSDS_93XX_SDS_CMD_WRITE);
}

static int rtsds_93xx_reset(struct rtsds_ctrl *ctrl, u32 sid)
{
	int ret;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	if (ctrl->sds[sid].mode == PHY_INTERFACE_MODE_NA)
		return 0;

	ret = ctrl->cfg->set_hwmode(ctrl, sid, ctrl->sds[sid].mode, PHY_INTERFACE_MODE_NA);
	if (!ret)
		return ret;

	return ctrl->cfg->set_hwmode(ctrl, sid, PHY_INTERFACE_MODE_NA, ctrl->sds[sid].mode);
}

/*
 * The RTL838x has 6 SerDes. The 16 bit registers start at 0xbb00e780 and are mapped directly into
 * 32 bit memory addresses. High 16 bits are always empty. A "lower" memory block serves pages 0/3
 * a "higher" memory block pages 1/2.
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
	int offs, val;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	offs = rtsds_838x_reg_offset(sid, page, reg);

	/* read twice for link status latch */
	if (page == RTSDS_FIB_PAGE && reg == 0x01)
		regmap_read(ctrl->regmap, ctrl->regbase + offs, &val);

	regmap_read(ctrl->regmap, ctrl->regbase + offs, &val);

	return val & RTSDS_FULL_REG_MASK;
}

static int rtsds_838x_write_bits(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg,
				 u32 mask, u32 val)
{
	int offs;

	if (rtsds_invalid_reg(ctrl, sid, page, reg) || rtsds_invalid_mask(mask, val))
		return -EINVAL;

	offs = rtsds_838x_reg_offset(sid, page, reg);

	/* read twice for link status latch */
	if (page == RTSDS_FIB_PAGE && reg == 0x01)
		regmap_read(ctrl->regmap, ctrl->regbase + offs, &val);

	regmap_write_bits(ctrl->regmap, ctrl->regbase + offs, mask, val);

	return 0;
}

static int rtsds_838x_has_submode(u32 sid)
{
	/* only SerDes 4 and 5 can have fiber modes */
	return GENMASK(5, 4) & BIT(sid);
}

static int rtsds_838x_set_hwmode(struct rtsds_ctrl *ctrl, u32 sid, int old, int new)
{
	int combomode = ctrl->cfg->mode_map[new];
	int submode = RTSDS_SUBMODE(combomode);
	int mode = RTSDS_MODE(combomode);

	if (rtsds_invalid_sds(ctrl, sid) || !combomode)
		return -EINVAL;

	if (rtsds_838x_has_submode(sid))
		rtsds_write_bits_pos(ctrl->regmap,
				     RTSDS_838X_INT_MODE_CTRL_REG,
				     RTSDS_838X_SDS_SUBMODE_BPOS(sid),
				     RTSDS_838X_SDS_SUBMODE_MASK, submode);
	else if (submode != 0)
		return -EINVAL;

	rtsds_write_bits_pos(ctrl->regmap,
			     RTSDS_838X_SDS_MODE_SEL_REG,
			     RTSDS_838X_SDS_MODE_BPOS(sid),
			     RTSDS_838X_SDS_MODE_MASK, mode);

	return 0;
}

static int rtsds_838x_get_hwmode(struct rtsds_ctrl *ctrl, u32 sid)
{
	int mode, submode = 0;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_read_bits_pos(ctrl->regmap,
			    RTSDS_838X_SDS_MODE_SEL_REG,
			    RTSDS_838X_SDS_MODE_BPOS(sid),
			    RTSDS_838X_SDS_MODE_MASK, &mode);

	if (rtsds_838x_has_submode(sid))
		rtsds_read_bits_pos(ctrl->regmap,
				    RTSDS_838X_INT_MODE_CTRL_REG,
				    RTSDS_838X_SDS_SUBMODE_BPOS(sid),
				    RTSDS_838X_SDS_SUBMODE_MASK, &submode);

	return RTSDS_COMBOMODE(mode, submode);
}

/*
 * The RTL839x has 14 SerDes starting at 0xbb00a000. 0-7, 10, 11 are 5GBit, 8, 9, 12, 13 are
 * 10 GBit. Two adjacent SerDes are tightly coupled and share a 1024 bytes register area. Per 32
 * bit address two registers are stored. The first register is stored in the lower 2 bytes ("on
 * the right" due to big endian) and the second register in the upper 2 bytes. The following
 * register areas are known:
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
	int offs = ((sid & 0xfe) << 9) + ((reg & 0xfe) << 1) + (page << 6);
	int sds5g = rtsds_83xx_sds_5g(sid);

	if (page < 4)
		return offs + ((sid & 1) << 8);
	else if ((page & 4) && (sid == 8 || sid == 12))
		return offs + 0x100;
	else if (page >= 8 && page <= 9 && sds5g)
		return offs + 0x100 + ((sid & 1) << 7);
	else if (page >= 10 && !sds5g)
		return offs + 0x80 + ((sid & 1) << 7);

	return -EINVAL; /* hole */
}

static int rtsds_839x_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	int offs;
	u32 val;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	offs = rtsds_839x_reg_offset(sid, page, reg);
	if (offs < 0)
		return 0;

	/* read twice for link status latch */
	if (page == RTSDS_FIB_PAGE && reg == 0x01)
		regmap_read(ctrl->regmap, ctrl->regbase + offs, &val);

	rtsds_read_bits_pos(ctrl->regmap, ctrl->regbase + offs,
			    RTSDS_839X_SDS_RW_BPOS(reg),
			    RTSDS_FULL_REG_MASK, &val);

	return val;
}

static int rtsds_839x_write_bits(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg,
				 u32 mask, u32 val)
{
	int offs;

	if (rtsds_invalid_reg(ctrl, sid, page, reg) || rtsds_invalid_mask(mask, val))
		return -EINVAL;

	offs = rtsds_839x_reg_offset(sid, page, reg);
	if (offs < 0)
		return 0;

	/* read twice for link status latch */
	if (page == RTSDS_FIB_PAGE && reg == 0x01)
		regmap_read(ctrl->regmap, ctrl->regbase + offs, &val);

	rtsds_write_bits_pos(ctrl->regmap, ctrl->regbase + offs,
			     RTSDS_839X_SDS_RW_BPOS(reg),
			     mask, val);

	return 0;
}

static int rtsds_839x_set_hwmode(struct rtsds_ctrl *ctrl, u32 sid, int old, int new)
{
	int combomode = ctrl->cfg->mode_map[new];
	int submode = RTSDS_SUBMODE(combomode);
	int mode = RTSDS_MODE(combomode);

	if (rtsds_invalid_sds(ctrl, sid) || !combomode)
		return -EINVAL;

	rtsds_write_bits_pos(ctrl->regmap,
			     RTSDS_839X_MAC_SDS_IF_CTL_REG(sid),
			     RTSDS_839X_SDS_MODE_BPOS(sid),
			     RTSDS_839X_SDS_MODE_MASK, mode);

	rtsds_839x_write_bits(ctrl, sid, RTSDS_SDS_PAGE, 0x04,
			      RTSDS_839X_SDS_SUBMODE_MASK,
			      FIELD_PREP(RTSDS_839X_SDS_SUBMODE_MASK, submode));

	return 0;
}

static int rtsds_839x_get_hwmode(struct rtsds_ctrl *ctrl, u32 sid)
{
	int mode, submode;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_read_bits_pos(ctrl->regmap,
			    RTSDS_839X_MAC_SDS_IF_CTL_REG(sid),
			    RTSDS_839X_SDS_MODE_BPOS(sid),
			    RTSDS_839X_SDS_MODE_MASK, &mode);

	submode = FIELD_GET(RTSDS_839X_SDS_SUBMODE_MASK,
			    rtsds_839x_read(ctrl, sid, RTSDS_SDS_PAGE, 0x04));

	return RTSDS_COMBOMODE(mode, submode);
}

/*
 * The RTL930x family has 12 SerdDes of three types. They are accessed through two IO registers at
 * 0xbb0003b0 which simulate commands to an internal MDIO bus:
 *
 * - SerDes 0-1 exist on the RTL9301 and 9302B and are QSGMII capable
 * - SerDes 2-9 are USXGMII capabable with either quad or single configuration
 * - SerDes 10-11 are 10GBase-R capable
 */

static int rtsds_930x_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	return rtsds_93xx_read(ctrl, sid, page, reg);
}

static int rtsds_930x_write_bits(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg,
				 u32 mask, u32 val)
{
	if (rtsds_invalid_reg(ctrl, sid, page, reg) || rtsds_invalid_mask(mask, val))
		return -EINVAL;

	return rtsds_93xx_write_bits(ctrl, sid, page, reg, mask, val);
}

static void rtsds_930x_get_mode_regs(int sid,
				     u32 *modereg, int *modebpos,
				     u32 *subreg, int *subbpos)
{
	if (sid > 3) {
		*subreg = RTSDS_930X_SDS_SUBMODE_CTRL1_REG;
		*subbpos = (sid - 4) * 5;
	} else {
		*subreg = RTSDS_930X_SDS_SUBMODE_CTRL0_REG;
		*subbpos = (sid - 2) * 5;
	}

	if (sid < 4) {
		*modereg = RTSDS_930X_SDS_MODE_SEL_0_REG;
		*modebpos = sid * 6;
	} else if (sid < 8) {
		*modereg = RTSDS_930X_SDS_MODE_SEL_1_REG;
		*modebpos = (sid - 4) * 6;
	} else if (sid < 10) {
		*modereg = RTSDS_930X_SDS_MODE_SEL_2_REG;
		*modebpos = (sid - 8) * 6;
	} else {
		*modereg = RTSDS_930X_SDS_MODE_SEL_3_REG;
		*modebpos = (sid - 10) * 6;
	}
}

static int rtsds_930x_has_submode(u32 sid)
{
	/* only SerDes 2-9 allow GMII 10G modes */
	return GENMASK(9, 2) & BIT(sid);
}

static int rtsds_930x_set_hwmode(struct rtsds_ctrl *ctrl, u32 sid, int old, int new)
{
	int combomode = ctrl->cfg->mode_map[new];
	u32 modereg, modebpos, subreg, subbpos;
	int submode = RTSDS_SUBMODE(combomode);
	int mode = RTSDS_MODE(combomode);

	if (rtsds_invalid_sds(ctrl, sid) || !combomode)
		return -EINVAL;

	rtsds_930x_get_mode_regs(sid, &modereg, &modebpos, &subreg, &subbpos);

	if (rtsds_930x_has_submode(sid))
		rtsds_write_bits_pos(ctrl->regmap, subreg, subbpos,
				     RTSDS_930X_SDS_SUBMODE_MASK, submode);
	else if (submode != 0)
		return -EINVAL;

	rtsds_write_bits_pos(ctrl->regmap, modereg, modebpos, RTSDS_930X_SDS_MODE_MASK, mode);

	return 0;
}

static int rtsds_930x_get_hwmode(struct rtsds_ctrl *ctrl, u32 sid)
{
	u32 modereg, modebpos, subreg, subbpos, mode, submode = 0;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_930x_get_mode_regs(sid, &modereg, &modebpos, &subreg, &subbpos);

	rtsds_read_bits_pos(ctrl->regmap, modereg, modebpos, RTSDS_930X_SDS_MODE_MASK, &mode);

	if (rtsds_930x_has_submode(sid))
		rtsds_read_bits_pos(ctrl->regmap, subreg, subbpos,
				    RTSDS_930X_SDS_SUBMODE_MASK, &submode);

	return RTSDS_COMBOMODE(mode, submode);
}

/*
 * The RTL931x family has 14 "frontend" SerDes that are cascaded. All operations (e.g. reset) work
 * on this frontend view while their registers are distributed over a total of 32 background
 * SerDes. Two types of SerDes exist:
 *
 * An "even" SerDes with numbers 0, 1, 2, 4, 6, 8, 10, 12 works on two background SerDes. 64 analog
 * and 64 XGMII data pages are coming from a first background SerDes while another 64 XGMII pages
 * are served from a second SerDes.
 *
 * The "odd" SerDes with numbers 3, 5, 7, 9, 11 & 13 SerDes consist of a total of 3 background
 * SerDes (one analog and two XGMII) each with an own page/register set.
 *
 * Align this for readability by simulating a total of 576 pages and mix them as follows.
 *
 * frontend page		"even" frontend SerDes		"odd" frontend SerDes
 * page 0x000-0x03f (analog):	page 0x000-0x03f back SDS	page 0x000-0x03f back SDS
 * page 0x100-0x13f (XGMII1):	page 0x000-0x03f back SDS	page 0x000-0x03f back SDS+1
 * page 0x200-0x23f (XGMII2):	page 0x000-0x03f back SDS+1	page 0x000-0x03f back SDS+2
 */

static int rtsds_931x_get_backing_sds(u32 sid, u32 page)
{
	int map[] = {0, 1, 2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23};
	int back = map[sid];

	if (page & 0xc0)
		return -EINVAL; /* hole */

	if ((sid & 1) && (sid != 1))
		back += (page >> 8); /* distribute "odd" to 3 background SerDes */
	else if (page >= 512)
		back += 1; /* distribute "even" to 2 background SerDes */

	return back;
}

static int rtsds_931x_read(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg)
{
	int back;

	if (rtsds_invalid_reg(ctrl, sid, page, reg))
		return -EINVAL;

	back = rtsds_931x_get_backing_sds(sid, page);

	return back < 0 ? 0 : rtsds_93xx_read(ctrl, back, page & 0x3f, reg);
}

static int rtsds_931x_write_bits(struct rtsds_ctrl *ctrl, u32 sid, u32 page, u32 reg,
				 u32 mask, u32 val)
{
	int back;

	if (rtsds_invalid_reg(ctrl, sid, page, reg) || rtsds_invalid_mask(mask, val))
		return -EINVAL;

	back = rtsds_931x_get_backing_sds(sid, page);

	return back < 0 ? 0 : rtsds_93xx_write_bits(ctrl, back, page & 0x3f, reg, mask, val);
}

static int rtsds_931x_set_hwmode(struct rtsds_ctrl *ctrl, u32 sid, int old, int new)
{
	int combomode = ctrl->cfg->mode_map[new];
	int submode = RTSDS_SUBMODE(combomode);
	int mode = RTSDS_MODE(combomode);

	if (rtsds_invalid_sds(ctrl, sid) || !combomode)
		return -EINVAL;

	if (old != PHY_INTERFACE_MODE_NA && new == PHY_INTERFACE_MODE_NA)
		regmap_write_bits(ctrl->regmap, RTSDS_931X_PS_SDS_OFF_MODE_CTRL_REG,
				  BIT(sid), BIT(sid));

	rtsds_write_bits_pos(ctrl->regmap,
			     RTSDS_931X_SERDES_MODE_CTRL_REG(sid),
			     RTSDS_931X_SDS_MODE_BPOS(sid),
			     RTSDS_931X_SDS_MODE_WRITE_MASK,
			     RTSDS_931X_SDS_MODE_FORCE_SETUP | mode);

	rtsds_931x_write_bits(ctrl, sid, 0x1f, 0x09,
			      RTSDS_931X_SDS_SUBMODE_MASK,
			      FIELD_PREP(RTSDS_931X_SDS_SUBMODE_MASK, submode));

	if (old == PHY_INTERFACE_MODE_NA && new != PHY_INTERFACE_MODE_NA)
		regmap_write_bits(ctrl->regmap, RTSDS_931X_PS_SDS_OFF_MODE_CTRL_REG,
				  BIT(sid), 0);

	return 0;
}

static int rtsds_931x_get_hwmode(struct rtsds_ctrl *ctrl, u32 sid)
{
	int mode, submode;

	if (rtsds_invalid_sds(ctrl, sid))
		return -EINVAL;

	rtsds_read_bits_pos(ctrl->regmap,
			     RTSDS_931X_SERDES_MODE_CTRL_REG(sid),
			     RTSDS_931X_SDS_MODE_BPOS(sid),
			     RTSDS_931X_SDS_MODE_READ_MASK, &mode);

	submode = FIELD_GET(RTSDS_931X_SDS_SUBMODE_MASK, rtsds_931x_read(ctrl, sid, 0x1f, 0x09));

	return RTSDS_COMBOMODE(mode, submode);
}

/* phy controller functions */

static int rtsds_phy_init(struct phy *phy)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;
	int ret;

	dev_dbg(ctrl->dev, "init SerDes %d\n", sid);

	mutex_lock(&ctrl->lock);
	ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_INIT);
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

	dev_dbg(ctrl->dev, "power on SerDes %d\n", sid);

	mutex_lock(&ctrl->lock);
	ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_POWER_ON);
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

	dev_dbg(ctrl->dev, "power off SerDes %d\n", sid);

	mutex_lock(&ctrl->lock);
	ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_PRE_POWER_OFF);
	if (!ret)
		ret = ctrl->cfg->set_hwmode(ctrl, sid,
					    ctrl->sds[sid].mode, PHY_INTERFACE_MODE_NA);
	if (!ret) {
		ctrl->sds[sid].mode = PHY_INTERFACE_MODE_NA;
		ctrl->sds[sid].speed = SPEED_UNKNOWN;
		ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_POST_POWER_OFF);
	}
	mutex_unlock(&ctrl->lock);

	if (ret)
		dev_err(ctrl->dev, "power off failed for SerDes %d\n", sid);

	return ret;
}

static int rtsds_phy_set_mode_int(struct rtsds_ctrl *ctrl, u32 sid, int old, int new)
{
	int ret;

	rtsds_check_and_fix_mode(ctrl, sid);

	if (ctrl->sds[sid].mode == new)
		return 0;

	dev_dbg(ctrl->dev, "switch SerDes %d from %s to %s (hw mode 0x%X)\n",
		sid, rtsds_phy_modes(old), rtsds_phy_modes(new), ctrl->cfg->mode_map[new]);

	mutex_lock(&ctrl->lock);
	ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_PRE_SET_MODE);
	if (!ret)
		ret = ctrl->cfg->set_hwmode(ctrl, sid, ctrl->sds[sid].mode, new);
	if (!ret) {
		ctrl->sds[sid].mode = new;
		ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_POST_SET_MODE);
	}
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
	int new = submode;

	if (mode != PHY_MODE_ETHERNET)
		return -EINVAL;
	/*
	 * The Realtek SerDes distinguishes between 2500base-x and a propietary 2.5GBit SGMII
	 * mode (aka HSGMII) that is acively used in the wild (e.g. Zyxel XGS1210-12). To provide
	 * this non-standard mode follow /drivers/phy/qualcomm/phy-qcom-sgmii-eth.c that switches
	 * between SGMII and HSGMII depending on the set speed. To simplify driver coding track
	 * the mode as RTSDS_REALTEK_MODE_HSGMII (reusing PHY_INTERFACE_MODE_INTERNAL). Activating
	 * this mode requires to set the SerDes mode to SGMII and the speed to 2500 before or
	 * after. E.g.
	 *
	 * phy_ops->set_speed(SPEED_2500);
	 * phy_ops->set_mode(PHY_INTERFACE_MODE_SGMII);
	 */
	if (new == PHY_INTERFACE_MODE_SGMII && ctrl->sds[sid].speed == SPEED_2500)
		new = RTSDS_REALTEK_MODE_HSGMII;

	return rtsds_phy_set_mode_int(ctrl, sid, ctrl->sds[sid].mode, new);
}

static int rtsds_phy_set_speed(struct phy *phy, int speed)
{
	struct rtsds_macro *macro = phy_get_drvdata(phy);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	u32 sid = macro->sid;

	mutex_lock(&ctrl->lock);
	ctrl->sds[sid].speed = speed;
	mutex_unlock(&ctrl->lock);

	/* See rtsds_phy_set_mode() */
	if (speed == SPEED_2500 && ctrl->sds[sid].mode == PHY_INTERFACE_MODE_SGMII)
		return rtsds_phy_set_mode(phy, PHY_MODE_ETHERNET, RTSDS_REALTEK_MODE_HSGMII);
	if (speed == SPEED_1000 && ctrl->sds[sid].mode == RTSDS_REALTEK_MODE_HSGMII)
		return rtsds_phy_set_mode(phy, PHY_MODE_ETHERNET, PHY_INTERFACE_MODE_SGMII);

	return 0;
}

static int rtsds_phy_reset_int(struct rtsds_ctrl *ctrl, u32 sid)
{
	int ret;

	dev_dbg(ctrl->dev, "reset SerDes %d\n", sid);

	mutex_lock(&ctrl->lock);
	ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_PRE_RESET);
	if (!ret)
		ret = ctrl->cfg->reset(ctrl, sid);
	if (!ret)
		ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_POST_RESET);
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

	return rtsds_phy_reset_int(ctrl, sid);
}

static const struct phy_ops rtsds_phy_ops = {
	.init		= rtsds_phy_init,
	.power_on	= rtsds_phy_power_on,
	.power_off	= rtsds_phy_power_off,
	.reset		= rtsds_phy_reset,
	.set_mode	= rtsds_phy_set_mode,
	.set_speed	= rtsds_phy_set_speed,
	.owner		= THIS_MODULE,
};

/*
 * The SerDes offer a lot of magic that sill needs to be uncovered. To help further development
 * provide some basic debugging about registers, modes, reset and polarity. All functions are
 * run under the global lock to avoid inconsistencies.
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

static int rtsds_dbg_mode_show(struct seq_file *seqf, void *unused)
{
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	int hwmode, sid = macro->sid;

	rtsds_check_and_fix_mode(ctrl, sid);

	mutex_lock(&ctrl->lock);
	hwmode = ctrl->cfg->get_hwmode(ctrl, sid);
	mutex_unlock(&ctrl->lock);


	seq_printf(seqf, "hw mode: 0x%X\n", hwmode);
	seq_printf(seqf, "phy mode: %s\n", rtsds_phy_modes(ctrl->sds[sid].mode));

	return 0;
}

static ssize_t rtsds_dbg_mode_write(struct file *file, const char __user *userbuf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *seqf = file->private_data;
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	int ret, hwmode, phymode, sid = macro->sid;
	struct rtsds_ctrl *ctrl = macro->ctrl;

	ret = kstrtou32_from_user(userbuf, count, 16, &hwmode);
	if (ret)
		return ret;

	/* Allow only modes we have under control. */
	phymode = rtsds_hwmode_to_phymode(ctrl, hwmode);
	if (phymode == PHY_INTERFACE_MODE_MAX)
		return -EINVAL;

	ret = rtsds_phy_set_mode_int(ctrl, sid, ctrl->sds[sid].mode, phymode);

	return ret ? ret : count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(rtsds_dbg_mode);

static int rtsds_dbg_reset_show(struct seq_file *seqf, void *unused)
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

	/* SerDes can be reset with "echo 1 > reset" */
	ret = kstrtou32_from_user(userbuf, count, 10, &reset);
	if (ret || reset != 1)
		return -EINVAL;

	ret = rtsds_phy_reset_int(ctrl, sid);

	return ret ? ret : count;
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

	mutex_lock(&ctrl->lock);
	while (page < ctrl->cfg->page_cnt) {
		if (page < RTSDS_PAGE_NAMES && rtsds_page_name[page])
			seq_printf(seqf, "\n%*s: ", -11, rtsds_page_name[page]);
		else if (page == 64 || page == 320) {
			page += 192;
			seq_printf(seqf, "\n\nXGMII_%d    : ", page >> 8);
		} else
			seq_printf(seqf, "\nPAGE_%03X   : ", page);
		for (reg = 0; reg < 0x20; reg++)
			seq_printf(seqf, "%04X ", ctrl->cfg->read(ctrl, sid, page, reg));
		page++;
	}
	seq_puts(seqf, "\n");
	mutex_unlock(&ctrl->lock);

	return 0;
}

static ssize_t rtsds_dbg_registers_write(struct file *file, const char __user *userbuf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *seqf = file->private_data;
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	int ret, sid = macro->sid;
	u32 reg, page, val, mask;
	u64 data;

	/*
	 * Due to many different devices and limited regional hardware access for developers,
	 * improve analysis with write access to the SerDes registers. This allows testers to
	 * build new patch sequences from the command line without creating new firmware files
	 * and building new images.
	 *
	 * Input for a register change is a single hex value that concatenates the page with
	 * 12 bits, the register with 8 bits, the value and the mask with 16 bits each.
	 * E.g. "echo 0x10110f000300 > register" means "write 0x0300 with a mask of 0x0f00
	 * (leaving the other 12 bits as is) into register 0x11 of page 0x10".
	 */

	ret = kstrtou64_from_user(userbuf, count, 16, &data);
	if (ret)
		return ret;

	page = FIELD_GET(RTSDS_DEBUG_PAGE_MASK, data);
	reg = FIELD_GET(RTSDS_DEBUG_REG_MASK, data);
	mask = FIELD_GET(RTSDS_DEBUG_FIELD_MASK, data);
	val = FIELD_GET(RTSDS_DEBUG_VAL_MASK, data);

	if (rtsds_invalid_reg(ctrl, sid, page, reg) || rtsds_invalid_mask(mask, val))
		return -EINVAL;

	mutex_lock(&ctrl->lock);
	ret = ctrl->cfg->write_bits(ctrl, sid, page, reg, mask, val);
	mutex_unlock(&ctrl->lock);

	return ret ? ret : count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(rtsds_dbg_registers);

static int rtsds_dbg_polarity_show(struct seq_file *seqf, void *unused)
{
	struct rtsds_macro *macro = dev_get_drvdata(seqf->private);
	struct rtsds_ctrl *ctrl = macro->ctrl;
	int val, sid = macro->sid;

	mutex_lock(&ctrl->lock);
	val = ctrl->cfg->read(ctrl, sid, RTSDS_SDS_PAGE, 0x00);
	mutex_unlock(&ctrl->lock);

	if (val < 0)
		return -EIO;

	seq_puts(seqf, "tx polarity: ");
	seq_puts(seqf, val & RTSDS_INV_HSO_BIT ? "inverse" : "normal");
	seq_puts(seqf, "\nrx polarity: ");
	seq_puts(seqf, val & RTSDS_INV_HSI_BIT ? "inverse" : "normal");
	seq_puts(seqf, "\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rtsds_dbg_polarity);

static void rtsds_dbg_init(struct rtsds_ctrl *ctrl, u32 sid)
{
	struct device *dev = &ctrl->sds[sid].phy->dev;
	struct dentry *dir, *root;
	char dirname[32];

	root = debugfs_lookup("realtek_serdes", NULL);
	if (!root)
		root = debugfs_create_dir("realtek_serdes", NULL);

	snprintf(dirname, sizeof(dirname), "serdes.%d", sid);
	dir = debugfs_create_dir(dirname, root);

	debugfs_create_file("mode", 0600, dir, dev, &rtsds_dbg_mode_fops);
	debugfs_create_file("polarity", 0400, dir, dev, &rtsds_dbg_polarity_fops);
	debugfs_create_file("registers", 0600, dir, dev, &rtsds_dbg_registers_fops);
	debugfs_create_file("reset", 0200, dir, dev, &rtsds_dbg_reset_fops);
}

#endif /* CONFIG_DEBUG_FS */

static void rtsds_setup(struct rtsds_ctrl *ctrl)
{
	int hwmode, ret;

	for (u32 sid = 0; sid < ctrl->cfg->sds_cnt; sid++) {
		/* switch off SerDes */
		ret = ctrl->cfg->set_hwmode(ctrl, sid,
					    PHY_INTERFACE_MODE_MAX, PHY_INTERFACE_MODE_NA);
		if (!ret)
			ret = rtsds_fw_run_event(ctrl, sid, RTSDS_FW_EVT_SETUP);
		if (ret)
			dev_err(ctrl->dev, "setup failed for SerDes %d\n", sid);

		/* in any case sync back hardware status for consistency */
		hwmode = ctrl->cfg->get_hwmode(ctrl, sid);
		ctrl->sds[sid].mode = rtsds_hwmode_to_phymode(ctrl, hwmode);
		ctrl->sds[sid].speed = SPEED_UNKNOWN;
	}
}

static struct phy *rtsds_simple_xlate(struct device *dev,
				      const struct of_phandle_args *args)
{
	struct rtsds_ctrl *ctrl = dev_get_drvdata(dev);
	int sid, sid2, min_port, max_port;

	/*
	 * A SerDes is usually connected to Ethernet transceivers (e.g. RTL8218B). Some of them
	 * make use of multiple links, e.g. 2x QSGMII. Transceivers themselves can have multiple
	 * ports. To make the driver understand this topology when looked up from controller, use
	 * following translation for the first port of a transceiver package in the devicetree.
	 *
	 * Single port/single link: E.g. Switch port 8 connected to SerDes 1
	 *
	 * port@8 { phys = <&serdes 1  1 8 8>; };
	 * port@8 { phys = <&serdes 1 -1 8 8>; }; (alternative notation)
	 *
	 * Multiport/single link: E.g. Switch ports 16 to 23 connected to SerDes 2
	 *
	 * port@16 { phys = <&serdes 2  2 16 23>; };
	 * port@16 { phys = <&serdes 2 -1 16 23>; }; (alternative notation)
	 *
	 * Multiport/multilink: E.g. Switch ports 24 to 31 connected to SerDes 3 & 4
	 *
	 * port@24 { phys = <&serdes 3 4 24 27>; };
	 * port@28 { phys = <&serdes 4 3 28 31>; };
	 */

	if (args->args_count != 4)
		return ERR_PTR(-EINVAL);

	sid = args->args[0];
	sid2 = args->args[1];
	min_port = args->args[2];
	max_port = args->args[3];

	if (sid < 0 || sid >= ctrl->cfg->sds_cnt ||
	    sid2 < -1 || sid2 >= ctrl->cfg->sds_cnt ||
	    min_port < 0 || max_port < min_port)
		return ERR_PTR(-EINVAL);

	if (sid2 == sid)
		sid2 = -1;

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
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct rtsds_ctrl *ctrl;
	int ret;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(ctrl->regmap)) {
		dev_err(dev, "failed to get parent syscon regmap\n");
		return PTR_ERR(ctrl->regmap);
	}
	regcache_cache_bypass(ctrl->regmap, true);

	ret = device_property_read_u32(dev, "reg", &ctrl->regbase);
	if (ret) {
		dev_err(dev, "SerDes register base not defined\n");
		return ret;
	}

	ctrl->dev = dev;
	ctrl->cfg = (struct rtsds_cfg *)of_device_get_match_data(dev);

	for (u32 sid = 0; sid < ctrl->cfg->sds_cnt; sid++) {
		ret = rtsds_phy_create(ctrl, sid);
		if (ret) {
			dev_err(dev, "failed to create phy for SerDes %d\n", sid);
			return ret;
		}
	}

	mutex_init(&ctrl->lock);
	dev_set_drvdata(dev, ctrl);

	rtsds_get_chip(ctrl);
	rtsds_fw_load(ctrl);
	rtsds_setup(ctrl);

	dev_info(dev, "%s (chip %s) initialized. %d SerDes, %d pages, 32 registers.",
		 ctrl->soc.model_name, ctrl->soc.chip_name,
		 ctrl->cfg->sds_cnt, ctrl->cfg->page_cnt);

	provider = devm_of_phy_provider_register(dev, rtsds_simple_xlate);

	return PTR_ERR_OR_ZERO(provider);
}

static const struct rtsds_cfg rtsds_838x_cfg = {
	.family		= RTSDS_838X_CFG_FAMILY,
	.sds_cnt	= RTSDS_838X_CFG_SDS_CNT,
	.page_cnt	= RTSDS_838X_CFG_PAGE_CNT,
	.write_bits	= rtsds_838x_write_bits,
	.read		= rtsds_838x_read,
	.reset		= rtsds_83xx_reset,
	.set_hwmode	= rtsds_838x_set_hwmode,
	.get_hwmode	= rtsds_838x_get_hwmode,
	.mode_map = {
		[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_COMBOMODE(0x04, 0x01),
		[PHY_INTERFACE_MODE_100BASEX]	= RTSDS_COMBOMODE(0x05, 0x01),
		[PHY_INTERFACE_MODE_NA]		= RTSDS_COMBOMODE(0x00, 0x00),
		[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_COMBOMODE(0x06, 0x00),
		[PHY_INTERFACE_MODE_SGMII]	= RTSDS_COMBOMODE(0x02, 0x02),
		[RTSDS_REALTEK_MODE_HSGMII]	= RTSDS_COMBOMODE(0x12, 0x04),
	},
};

static const struct rtsds_cfg rtsds_839x_cfg = {
	.family		= RTSDS_839X_CFG_FAMILY,
	.sds_cnt	= RTSDS_839X_CFG_SDS_CNT,
	.page_cnt	= RTSDS_839X_CFG_PAGE_CNT,
	.write_bits	= rtsds_839x_write_bits,
	.read		= rtsds_839x_read,
	.reset		= rtsds_83xx_reset,
	.set_hwmode	= rtsds_839x_set_hwmode,
	.get_hwmode	= rtsds_839x_get_hwmode,
	.mode_map = {
		[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_COMBOMODE(0x07, 0x00),
		[PHY_INTERFACE_MODE_100BASEX]	= RTSDS_COMBOMODE(0x08, 0x00),
		[PHY_INTERFACE_MODE_10GBASER]	= RTSDS_COMBOMODE(0x01, 0x00),
		[PHY_INTERFACE_MODE_NA]		= RTSDS_COMBOMODE(0x00, 0x00),
		[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_COMBOMODE(0x06, 0x00),
	},
};

static const struct rtsds_cfg rtsds_930x_cfg = {
	.family		= RTSDS_930X_CFG_FAMILY,
	.sds_cnt	= RTSDS_930X_CFG_SDS_CNT,
	.page_cnt	= RTSDS_930X_CFG_PAGE_CNT,
	.write_bits	= rtsds_930x_write_bits,
	.read		= rtsds_930x_read,
	.reset		= rtsds_93xx_reset,
	.set_hwmode	= rtsds_930x_set_hwmode,
	.get_hwmode	= rtsds_930x_get_hwmode,
	.mode_map = {
		[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_COMBOMODE(0x04, 0x00),
		[PHY_INTERFACE_MODE_2500BASEX]	= RTSDS_COMBOMODE(0x16, 0x00),
		[PHY_INTERFACE_MODE_10GBASER]	= RTSDS_COMBOMODE(0x1a, 0x00),
		[PHY_INTERFACE_MODE_NA]		= RTSDS_COMBOMODE(0x1f, 0x00),
		[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_COMBOMODE(0x06, 0x00),
		[PHY_INTERFACE_MODE_QUSGMII]	= RTSDS_COMBOMODE(0x0d, 0x02),
		[PHY_INTERFACE_MODE_SGMII]	= RTSDS_COMBOMODE(0x02, 0x00),
		[PHY_INTERFACE_MODE_USXGMII]	= RTSDS_COMBOMODE(0x0d, 0x00),
		[PHY_INTERFACE_MODE_XGMII]	= RTSDS_COMBOMODE(0x10, 0x00),
		[RTSDS_REALTEK_MODE_HSGMII]	= RTSDS_COMBOMODE(0x12, 0x00),
	},
};

static const struct rtsds_cfg rtsds_931x_cfg = {
	.family		= RTSDS_931X_CFG_FAMILY,
	.sds_cnt	= RTSDS_931X_CFG_SDS_CNT,
	.page_cnt	= RTSDS_931X_CFG_PAGE_CNT,
	.write_bits	= rtsds_931x_write_bits,
	.read		= rtsds_931x_read,
	.reset		= rtsds_93xx_reset,
	.set_hwmode	= rtsds_931x_set_hwmode,
	.get_hwmode	= rtsds_931x_get_hwmode,
	.mode_map = {
		[PHY_INTERFACE_MODE_1000BASEX]	= RTSDS_COMBOMODE(0x1f, 0x39),
		[PHY_INTERFACE_MODE_10GBASER]	= RTSDS_COMBOMODE(0x1f, 0x35),
		[PHY_INTERFACE_MODE_NA]		= RTSDS_COMBOMODE(0x1f, 0x3f),
		[PHY_INTERFACE_MODE_QSGMII]	= RTSDS_COMBOMODE(0x06, 0x00),
		[PHY_INTERFACE_MODE_SGMII]	= RTSDS_COMBOMODE(0x02, 0x00),
		[PHY_INTERFACE_MODE_USXGMII]	= RTSDS_COMBOMODE(0x0d, 0x00),
		[PHY_INTERFACE_MODE_XGMII]	= RTSDS_COMBOMODE(0x0a, 0x00),
		[RTSDS_REALTEK_MODE_HSGMII]	= RTSDS_COMBOMODE(0x12, 0x00),
	},
};

static const struct of_device_id rtsds_compatible_ids[] = {
	{ .compatible = "realtek,rtl8380m-serdes",	.data = &rtsds_838x_cfg },
	{ .compatible = "realtek,rtl8392m-serdes",	.data = &rtsds_839x_cfg },
	{ .compatible = "realtek,rtl9302b-serdes",	.data = &rtsds_930x_cfg },
	{ .compatible = "realtek,rtl9311-serdes",	.data = &rtsds_931x_cfg },
	{},
};
MODULE_DEVICE_TABLE(of, rtsds_compatible_ids);

static struct platform_driver rtsds_platform_driver = {
	.probe		= rtsds_probe,
	.driver		= {
		.name	= "realtek,otto-serdes",
		.of_match_table = rtsds_compatible_ids,
	},
};

module_platform_driver(rtsds_platform_driver);

MODULE_AUTHOR("Markus Stockhausen <markus.stockhausen@gmx.de>");
MODULE_DESCRIPTION("SerDes driver for Realtek RTL83xx, RTL93xx switch SoCs");
MODULE_LICENSE("GPL");
