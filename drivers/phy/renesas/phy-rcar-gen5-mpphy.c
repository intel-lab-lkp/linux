// SPDX-License-Identifier: GPL-2.0-only
/*
 * Renesas Multi-Protocol PHY device driver
 *
 * Copyright (C) 2025-2026 Renesas Electronics Corporation
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/types.h>

#include <dt-bindings/phy/phy.h>

#define MPPHY_NUM_CHANNELS		4

/* Common registers */
#define MPPHY_CMNCNT1			0x80000
#define MPPHY_CMNCNT2			0x80004

/* Channel register base and offsets */
#define MPPHY_CHAN_BASE(ch)		(0x81000 + (ch) * 0x1000)
#define MPPHY_PXCNTXT1(ch)		(MPPHY_CHAN_BASE(ch) + 0x4)
#define MPPHY_PXCNTXT2(ch)		(MPPHY_CHAN_BASE(ch) + 0x8)
#define MPPHY_PXTEST(ch)		(MPPHY_CHAN_BASE(ch) + 0xc)
#define MPPHY_PXREFCLK(ch)		(MPPHY_CHAN_BASE(ch) + 0x14)
#define MPPHY_PXRXREQ1(ch)		(MPPHY_CHAN_BASE(ch) + 0x24)
#define MPPHY_PXRXCNT(ch)		(MPPHY_CHAN_BASE(ch) + 0x38)
#define MPPHY_PXSRAMCNT(ch)		(MPPHY_CHAN_BASE(ch) + 0x40)
#define MPPHY_PXTXREQ(ch)		(MPPHY_CHAN_BASE(ch) + 0x44)

#define MPPHY_PCS0REG1			0x85000
#define MPPHY_PCS0REG5			0x85010

/* TCA (Type-C Adapter) Register Offsets within MP-PHY base */
#define TCA_OFFSET(ch)			(0x90000 + (((ch) & 1) ? 0x10000 : 0))
#define TCA_VBUS_CTRL			0x40

/* PCS0REG1 register bits */
#define MPPHY_PCS0REG1_VAL		BIT(16)

/* PXTEST register bit */
#define MPPHY_PXTEST_BIT		BIT(0)

/* PXRXCNT register reset value */
#define MPPHY_PXRXCNT_RESET_VAL		0x202

/* PXSRAMCNT register bits */
#define SRAM_EXT_LD_DONE		0x10

/* PXREFCLK register value */
#define MPPHY_PXREFCLK_VAL_ETH		0x55

/* Firmware update */
#define MPPHY_FW_BASE			0x10000
#define MPPHY_FW_CH_OFFSET		0x20000
#define MPPHY_FW_NAME			"rcar_gen5_mp_phy.bin"

struct mp_phy_chan_priv {
	struct phy *phy;
	enum phy_mode protocol_id;
	bool initialized;
};

struct mp_phy_priv {
	void __iomem *base;
	struct device *dev;
	struct dev_pm_domain_list *pd_list;
	struct reset_control_bulk_data resets[MPPHY_NUM_CHANNELS + 1];
	struct clk_bulk_data *clks;
	int num_clks;
	const struct firmware *fw;
	struct regmap *map;
	struct mp_phy_chan_priv chan[MPPHY_NUM_CHANNELS];
	u32 num_lanes[MPPHY_NUM_CHANNELS];
	u32 write_cntxt1;
	u32 cmncnt[2];
	u8 sramcnt[MPPHY_NUM_CHANNELS];
};

#define MPPHY_PX_RD_RANGE(n)						\
	regmap_reg_range(MPPHY_PXCNTXT1(n), MPPHY_CHAN_BASE(n)),	\
	regmap_reg_range(MPPHY_PXREFCLK(n), MPPHY_PXREFCLK(n)),		\
	regmap_reg_range(MPPHY_PXRXREQ1(n), MPPHY_PXRXREQ1(n)),		\
	regmap_reg_range(MPPHY_PXRXCNT(n), MPPHY_PXRXCNT(n)),		\
	regmap_reg_range(MPPHY_PXSRAMCNT(n), MPPHY_PXTXREQ(n))

static const struct regmap_range mp_phy_readable_range[] = {
	regmap_reg_range(MPPHY_CMNCNT1, MPPHY_CMNCNT2),
	MPPHY_PX_RD_RANGE(0), MPPHY_PX_RD_RANGE(1),
	MPPHY_PX_RD_RANGE(2), MPPHY_PX_RD_RANGE(3),
	regmap_reg_range(MPPHY_PCS0REG1, MPPHY_PCS0REG1),
	regmap_reg_range(MPPHY_PCS0REG5, MPPHY_PCS0REG5),
	regmap_reg_range(TCA_OFFSET(0) + TCA_VBUS_CTRL, TCA_OFFSET(0) + TCA_VBUS_CTRL),
	regmap_reg_range(TCA_OFFSET(1) + TCA_VBUS_CTRL, TCA_OFFSET(1) + TCA_VBUS_CTRL),
};

static const struct regmap_access_table mp_phy_readable_table = {
	.yes_ranges = mp_phy_readable_range,
	.n_yes_ranges = ARRAY_SIZE(mp_phy_readable_range),
};

#define MPPHY_PX_WR_RANGE(n)						\
	regmap_reg_range(MPPHY_PXCNTXT1(n), MPPHY_CHAN_BASE(n)),	\
	regmap_reg_range(MPPHY_PXREFCLK(n), MPPHY_PXREFCLK(n)),		\
	regmap_reg_range(MPPHY_PXRXCNT(n), MPPHY_PXRXCNT(n)),		\
	regmap_reg_range(MPPHY_PXSRAMCNT(n), MPPHY_PXTXREQ(n))

static const struct regmap_range mp_phy_writeable_range[] = {
	regmap_reg_range(MPPHY_FW_BASE, MPPHY_FW_BASE + 4 * MPPHY_FW_CH_OFFSET),
	regmap_reg_range(MPPHY_CMNCNT1, MPPHY_CMNCNT2),
	MPPHY_PX_WR_RANGE(0), MPPHY_PX_WR_RANGE(1),
	MPPHY_PX_WR_RANGE(2), MPPHY_PX_WR_RANGE(3),
	regmap_reg_range(MPPHY_PCS0REG1, MPPHY_PCS0REG1),
	regmap_reg_range(MPPHY_PCS0REG5, MPPHY_PCS0REG5),
	regmap_reg_range(TCA_OFFSET(0) + TCA_VBUS_CTRL, TCA_OFFSET(0) + TCA_VBUS_CTRL),
	regmap_reg_range(TCA_OFFSET(1) + TCA_VBUS_CTRL, TCA_OFFSET(1) + TCA_VBUS_CTRL),
};

static const struct regmap_access_table mp_phy_writeable_table = {
	.yes_ranges = mp_phy_writeable_range,
	.n_yes_ranges = ARRAY_SIZE(mp_phy_writeable_range),
};

#define MPPHY_PX_VL_RANGE(n)						\
	regmap_reg_range(MPPHY_PXRXREQ1(n), MPPHY_PXRXREQ1(n)),		\
	regmap_reg_range(MPPHY_PXSRAMCNT(n), MPPHY_PXTXREQ(n))

static const struct regmap_range mp_phy_volatile_range[] = {
	MPPHY_PX_VL_RANGE(0), MPPHY_PX_VL_RANGE(1),
	MPPHY_PX_VL_RANGE(2), MPPHY_PX_VL_RANGE(3),
	regmap_reg_range(MPPHY_PCS0REG1, MPPHY_PCS0REG1),
	regmap_reg_range(MPPHY_PCS0REG5, MPPHY_PCS0REG5),
};

static const struct regmap_access_table mp_phy_volatile_table = {
	.yes_ranges = mp_phy_volatile_range,
	.n_yes_ranges = ARRAY_SIZE(mp_phy_volatile_range),
};

static const struct regmap_config mp_phy_regmap_config = {
	.fast_io	= true,
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= TCA_OFFSET(1) + TCA_VBUS_CTRL,
	.name		= "mpphy",
	.rd_table	= &mp_phy_readable_table,
	.wr_table	= &mp_phy_writeable_table,
	.volatile_table	= &mp_phy_volatile_table,
	.cache_type	= REGCACHE_MAPLE,
};

static int mp_phy_reg_wait(struct mp_phy_priv *priv, u32 offs, u32 mask, u32 expected)
{
	u32 val;
	int ret;

	ret = regmap_read_poll_timeout(priv->map, offs, val,
				       (val & mask) == expected,
				       0, 1000000);
	if (ret) {
		dev_err(priv->dev,
			"Timeout waiting for offset: 0x%x, mask: 0x%x, expected: 0x%x\n",
			offs, mask, expected);
	}

	return ret;
}

static void mp_phy_update_firmware(struct phy *phy)
{
	struct mp_phy_priv *priv = phy_get_drvdata(phy);
	int i;

	for (i = 0; i < priv->fw->size; i += 2) {
		writew(priv->fw->data[i] | (priv->fw->data[i + 1] << 8),
		       priv->base + MPPHY_FW_BASE + (MPPHY_FW_CH_OFFSET * phy->id) + i);
	}
}

static int mp_phy_exit(struct phy *phy)
{
	struct mp_phy_priv *priv = phy_get_drvdata(phy);
	struct mp_phy_chan_priv *chan = &priv->chan[phy->id];

	if (!chan->initialized)
		return 0;

	chan->initialized = false;

	pm_runtime_put_sync(priv->pd_list->pd_devs[phy->id]);

	return 0;
}

static int mp_phy_init_ethernet(struct phy *phy)
{
	struct mp_phy_priv *priv = phy_get_drvdata(phy);

	mp_phy_update_firmware(phy);

	regmap_write(priv->map, MPPHY_PXRXCNT(phy->id), MPPHY_PXRXCNT_RESET_VAL);
	regmap_set_bits(priv->map, MPPHY_PXREFCLK(phy->id), MPPHY_PXREFCLK_VAL_ETH);
	regmap_clear_bits(priv->map, MPPHY_PXRXCNT(phy->id), BIT(9) | BIT(1));
	regmap_set_bits(priv->map, MPPHY_PXTXREQ(phy->id), BIT(19) | BIT(3));

	return 0;
}

static int mp_phy_init_pcie4(struct phy *phy)
{
	struct mp_phy_priv *priv = phy_get_drvdata(phy);

	if (priv->num_lanes[phy->id] == 1 || priv->num_lanes[phy->id] == 2) {
		if (phy->id == 0) {
			regmap_set_bits(priv->map, MPPHY_PXCNTXT1(0), 0x2010002);
			regmap_set_bits(priv->map, MPPHY_PXCNTXT2(0), 0x2020201);
			regmap_set_bits(priv->map, MPPHY_PXTXREQ(0), 0x80004);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(0), 0x30);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(0), 0x4);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(0), 0x1);
			regmap_set_bits(priv->map, MPPHY_PXRXCNT(0), 0x202);
			regmap_clear_bits(priv->map, MPPHY_PXRXCNT(0), 0x202);
		} else if (phy->id == 1) {
			regmap_set_bits(priv->map, MPPHY_PXCNTXT1(2), 0x2010002);
			regmap_set_bits(priv->map, MPPHY_PXCNTXT2(2), 0x2020202);
			regmap_set_bits(priv->map, MPPHY_PXTXREQ(2), 0x8);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(2), 0x30);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(2), 0x4);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(2), 0x1);
			regmap_set_bits(priv->map, MPPHY_PXRXCNT(2), 0x202);
			regmap_clear_bits(priv->map, MPPHY_PXRXCNT(2), 0x202);
		}
	} else if (priv->num_lanes[phy->id] == 4) {
		if (phy->id == 0) {
			regmap_set_bits(priv->map, MPPHY_PXCNTXT1(0), 0x2010002);
			regmap_set_bits(priv->map, MPPHY_PXCNTXT2(0), 0x2020201);
			regmap_set_bits(priv->map, MPPHY_PXTXREQ(0), 0x8);
			regmap_set_bits(priv->map, MPPHY_PXCNTXT1(1), 0x2010002);
			regmap_set_bits(priv->map, MPPHY_PXCNTXT2(1), 0x2020201);
			regmap_set_bits(priv->map, MPPHY_PXTXREQ(1), 0x8);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(0), 0x30);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(0), 0x4);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(0), 0x1);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(1), 0x30);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(1), 0x4);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(1), 0x1);
			regmap_set_bits(priv->map, MPPHY_PXRXCNT(0), 0x202);
			regmap_set_bits(priv->map, MPPHY_PXRXCNT(1), 0x202);
			regmap_clear_bits(priv->map, MPPHY_PXRXCNT(0), 0x202);
			regmap_clear_bits(priv->map, MPPHY_PXRXCNT(1), 0x202);
		} else if (phy->id == 1) {
			regmap_set_bits(priv->map, MPPHY_PXCNTXT1(2), 0x2010002);
			regmap_set_bits(priv->map, MPPHY_PXCNTXT2(2), 0x2020202);
			regmap_set_bits(priv->map, MPPHY_PXTXREQ(2), 0x8);
			regmap_set_bits(priv->map, MPPHY_PXCNTXT1(3), 0x2010002);
			regmap_set_bits(priv->map, MPPHY_PXCNTXT2(3), 0x2020202);
			regmap_set_bits(priv->map, MPPHY_PXTXREQ(3), 0x8);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(2), 0x30);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(2), 0x4);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(2), 0x1);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(3), 0x30);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(3), 0x4);
			regmap_set_bits(priv->map, MPPHY_PXREFCLK(3), 0x1);

			regmap_set_bits(priv->map, MPPHY_PXRXCNT(2), 0x202);
			regmap_set_bits(priv->map, MPPHY_PXRXCNT(3), 0x202);
			regmap_clear_bits(priv->map, MPPHY_PXRXCNT(2), 0x202);
			regmap_clear_bits(priv->map, MPPHY_PXRXCNT(3), 0x202);
		}
	} else if (priv->num_lanes[phy->id] == 8) {
		regmap_write(priv->map, MPPHY_PXCNTXT1(0), 0x2010002);
		regmap_write(priv->map, MPPHY_PXCNTXT2(0), 0x2020201);
		regmap_write(priv->map, MPPHY_PXTXREQ(0), 0x8);
		regmap_write(priv->map, MPPHY_PXCNTXT1(1), 0x2010002);
		regmap_write(priv->map, MPPHY_PXCNTXT2(1), 0x2020202);
		regmap_write(priv->map, MPPHY_PXTXREQ(1), 0x8);
		regmap_write(priv->map, MPPHY_PXCNTXT1(2), 0x2010002);
		regmap_write(priv->map, MPPHY_PXCNTXT2(2), 0x2020202);
		regmap_write(priv->map, MPPHY_PXTXREQ(2), 0x8);
		regmap_write(priv->map, MPPHY_PXCNTXT1(3), 0x2010002);
		regmap_write(priv->map, MPPHY_PXCNTXT2(3), 0x2020202);
		regmap_write(priv->map, MPPHY_PXTXREQ(3), 0x8);

		regmap_set_bits(priv->map, MPPHY_PXRXCNT(0), 0x202);
		regmap_set_bits(priv->map, MPPHY_PXRXCNT(1), 0x202);
		regmap_set_bits(priv->map, MPPHY_PXRXCNT(2), 0x202);
		regmap_set_bits(priv->map, MPPHY_PXRXCNT(3), 0x202);

		regmap_set_bits(priv->map, MPPHY_PXRXCNT(0), 0x202);
		regmap_set_bits(priv->map, MPPHY_PXRXCNT(1), 0x202);
		regmap_set_bits(priv->map, MPPHY_PXRXCNT(2), 0x202);
		regmap_set_bits(priv->map, MPPHY_PXRXCNT(3), 0x202);

		regmap_clear_bits(priv->map, MPPHY_PXRXCNT(0), 0x202);
		regmap_clear_bits(priv->map, MPPHY_PXRXCNT(1), 0x202);
		regmap_clear_bits(priv->map, MPPHY_PXRXCNT(2), 0x202);
		regmap_clear_bits(priv->map, MPPHY_PXRXCNT(3), 0x202);
	}

	return 0;
}

static int mp_phy_init_usb(struct phy *phy)
{
	struct mp_phy_priv *priv = phy_get_drvdata(phy);
	int ret;

	ret = mp_phy_reg_wait(priv, MPPHY_PXSRAMCNT(phy->id), 0x20, 0x20);
	if (ret)
		return ret;

	mp_phy_update_firmware(phy);
	regmap_set_bits(priv->map, MPPHY_PXSRAMCNT(phy->id), SRAM_EXT_LD_DONE);
	ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1(phy->id), 0x2, 0);
	if (ret)
		return ret;

	return 0;
}

static int mp_phy_init(struct phy *phy)
{
	struct mp_phy_priv *priv = phy_get_drvdata(phy);
	struct mp_phy_chan_priv *chan = &priv->chan[phy->id];
	int ret;

	/*
	 * Note: Current source code support for Ethernet, PCIe
	 * initialization is based on the bare metal code shared
	 * by the board team.
	 */
	ret = pm_runtime_get_sync(priv->pd_list->pd_devs[phy->id]);
	if (ret < 0) {
		dev_err(priv->dev,
			"Failed to power on domain for channel %d: %d\n",
			phy->id, ret);
		return ret;
	}

	/* Check if initialized with same protocol then skip */
	if (chan->initialized)
		return 0;

	if (chan->protocol_id == PHY_MODE_PCIE)
		ret = mp_phy_init_pcie4(phy);
	else if (chan->protocol_id == PHY_MODE_ETHERNET)
		ret = mp_phy_init_ethernet(phy);
	else
		ret = mp_phy_init_usb(phy);
	if (ret)
		return ret;

	chan->initialized = true;
	dev_dbg(priv->dev,
		"Channel %d successfully initialized for protocol %d\n",
		phy->id, chan->protocol_id);

	return 0;
}

static int mp_phy_power_on(struct phy *phy)
{
	struct mp_phy_priv *priv = phy_get_drvdata(phy);
	struct mp_phy_chan_priv *chan = &priv->chan[phy->id];
	int ret;

	if (!chan->initialized) {
		dev_err(priv->dev, "Channel %d not initialized\n", phy->id);
		return -EINVAL;
	}

	/*
	 * The datasheet describes initialization procedure without full
	 * information about the registers. Therefore, the source code is
	 * based on the bare metal code shared by the board team.
	 */
	if (chan->protocol_id == PHY_MODE_PCIE) {
		if (priv->num_lanes[phy->id] == 1 || priv->num_lanes[phy->id] == 2) {
			ret = mp_phy_reg_wait(priv, MPPHY_PXSRAMCNT(phy->id), 0x20, 0x20);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1(phy->id), 0x2, 0);
			if (ret)
				return ret;
		} else if (priv->num_lanes[phy->id] == 4) {
			ret = mp_phy_reg_wait(priv, MPPHY_PXSRAMCNT(2 * phy->id), 0x20, 0x20);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXSRAMCNT((2 * phy->id) + 1), 0x20, 0x20);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1(2 * phy->id), 0x2, 0);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1((2 * phy->id) + 1), 0x2, 0);
			if (ret)
				return ret;
		} else if (priv->num_lanes[phy->id] == 8) {
			ret = mp_phy_reg_wait(priv, MPPHY_PXSRAMCNT(0), 0x20, 0x20);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXSRAMCNT(1), 0x20, 0x20);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXSRAMCNT(2), 0x20, 0x20);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXSRAMCNT(3), 0x20, 0x20);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1(0), 0x2, 0);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1(1), 0x2, 0);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1(2), 0x2, 0);
			if (ret)
				return ret;
			ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1(3), 0x2, 0);
			if (ret)
				return ret;
		}
	} else if (chan->protocol_id == PHY_MODE_ETHERNET) {
		regmap_set_bits(priv->map, MPPHY_PXSRAMCNT(phy->id), SRAM_EXT_LD_DONE);
		ret = mp_phy_reg_wait(priv, MPPHY_PXRXREQ1(phy->id), 0x2, 0);
		if (ret)
			return ret;
	} else {	/* USB */
		regmap_write(priv->map, TCA_OFFSET(phy->id) + TCA_VBUS_CTRL, 0x3e);
	}

	return 0;
}

static const struct phy_ops mp_phy_ops = {
	.init		= mp_phy_init,
	.exit		= mp_phy_exit,
	.power_on	= mp_phy_power_on,
	.owner		= THIS_MODULE,
};

static struct phy *mp_phy_xlate(struct device *dev,
				const struct of_phandle_args *args)
{
	struct mp_phy_priv *priv = dev_get_drvdata(dev);
	int id;

	if (args->args_count > 1) {
		dev_err(dev, "Invalid args_count: %d\n", args->args_count);
		return ERR_PTR(-EINVAL);
	}

	if (args->args_count >= 1)
		id = args->args[0];
	else
		id = 0;

	return priv->chan[id].phy;
}

static int mp_phy_parse_dt(struct platform_device *pdev, struct mp_phy_priv *priv)
{
	struct device *dev = &pdev->dev;
	bool need_fw = false;
	u32 ext_ref_clk[4];
	u32 out_ref_clk[4];
	u32 phy_type[8];
	int i, ret;

	ret = device_property_read_u32_array(dev, "renesas,phy-type", phy_type, 8);
	if (ret < 0)
		return dev_err_probe(dev, -EINVAL, "Failed to read PHY configuration\n");

	ret = device_property_read_u32_array(dev, "renesas,external-ref-clock",
					     ext_ref_clk, 4);
	if (ret < 0)
		return dev_err_probe(dev, -EINVAL, "Failed to read PHY clock configuration\n");

	ret = device_property_read_u32_array(dev, "renesas,output-repeat-ref-clock",
					     out_ref_clk, 4);
	if (ret < 0)
		return dev_err_probe(dev, -EINVAL, "Failed to read PHY clock out configuration\n");

	ret = device_property_read_u32_array(dev, "renesas,num-lanes", priv->num_lanes, 4);
	if (ret < 0)
		return dev_err_probe(dev, -EINVAL, "Failed to read PHY lane configuration\n");

	/* Port 0 can be either PCIe4 channel 0 lanes 0,1 or Ethernet 0-1 */
	if (!((phy_type[0] == PHY_TYPE_PCIE && phy_type[1] == 0) ||
	      (phy_type[0] == PHY_TYPE_XPCS && phy_type[1] == 0))) {
		return dev_err_probe(dev, -EINVAL, "Incorrect PHY port 0 configuration\n");
	}

	/* Port 1 can be either PCIe4 channel 0 lanes 2,3 or Ethernet 2-3 */
	if (!((phy_type[2] == PHY_TYPE_PCIE && phy_type[3] == 0) ||
	      (phy_type[2] == PHY_TYPE_XPCS && phy_type[3] == 1))) {
		return dev_err_probe(dev, -EINVAL, "Incorrect PHY port 1 configuration\n");
	}

	/*
	 * Port 2 can be either PCIe4 channel 0 lanes 4,5 or Ethernet 4-5 or
	 * PCIe4 channel 1 lanes 0,1 or USB3.2 channel 0
	 */
	if (!((phy_type[4] == PHY_TYPE_PCIE && phy_type[5] == 0) ||
	      (phy_type[4] == PHY_TYPE_PCIE && phy_type[5] == 1) ||
	      (phy_type[4] == PHY_TYPE_XPCS && phy_type[5] == 2) ||
	      (phy_type[4] == PHY_TYPE_USB3 && phy_type[5] == 0))) {
		return dev_err_probe(dev, -EINVAL, "Incorrect PHY port 2 configuration\n");
	}

	/*
	 * Port 3 can be either PCIe4 channel 0 lanes 6,7 or Ethernet 6-7 or
	 * PCIe4 channel 1 lanes 2,3 or USB3.2 channel 1
	 */
	if (!((phy_type[6] == PHY_TYPE_PCIE && phy_type[7] == 0) ||
	      (phy_type[6] == PHY_TYPE_PCIE && phy_type[7] == 1) ||
	      (phy_type[6] == PHY_TYPE_XPCS && phy_type[7] == 3) ||
	      (phy_type[6] == PHY_TYPE_USB3 && phy_type[7] == 1))) {
		return dev_err_probe(dev, -EINVAL, "Incorrect PHY port 3 configuration\n");
	}

	priv->cmncnt[0] = 0;
	priv->cmncnt[1] = 0x33330000;	/* All res_{ack,req}_in_sel are 1 */

	for (i = 0; i < MPPHY_NUM_CHANNELS; i++) {
		priv->chan[i].initialized = false;
		priv->chan[i].protocol_id = PHY_MODE_INVALID;

		if (phy_type[2 * i] == PHY_TYPE_PCIE) {
			priv->chan[i].protocol_id = PHY_MODE_PCIE;
			priv->sramcnt[i] = 0xf;
			if (phy_type[(2 * i) + 1] == 0)	/* Channel 0 */
				priv->cmncnt[0] |= 0x0 << (i * 8);
			else				/* Channel 1 */
				priv->cmncnt[0] |= 0x2 << (i * 8);
		} else if (phy_type[2 * i] == PHY_TYPE_XPCS) {
			priv->chan[i].protocol_id = PHY_MODE_ETHERNET;
			priv->sramcnt[i] = 0x0;
			priv->write_cntxt1 |= BIT(i);
			priv->cmncnt[0] |= 0x1 << (i * 8);
			need_fw = true;
		} else if (phy_type[2 * i] == PHY_TYPE_USB3) {
			priv->chan[i].protocol_id = PHY_MODE_USB_OTG;
			priv->sramcnt[i] = 0x9;
			priv->cmncnt[0] |= 0x3 << (i * 8);
			need_fw = true;
		} else {
			/* Cannot be reached. */
			return -EINVAL;
		}

		if (ext_ref_clk[i])
			priv->cmncnt[1] |= BIT(i * 4);

		if (out_ref_clk[i])
			priv->cmncnt[1] |= BIT((i * 4) + 1);
	}

	if (!need_fw)
		return 0;

	return request_firmware(&priv->fw, MPPHY_FW_NAME, dev);
}

static int mp_phy_probe(struct platform_device *pdev)
{
	static const char *const pd_names[] = { "mpp0", "mpp1", "mpp2", "mpp3" };
	const struct dev_pm_domain_attach_data pd_attach_data = {
		.pd_names = pd_names,
		.num_pd_names = ARRAY_SIZE(pd_names),
		.pd_flags = 0,
	};
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct mp_phy_priv *priv;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	ret = mp_phy_parse_dt(pdev, priv);
	if (ret)
		return ret;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return dev_err_probe(dev, PTR_ERR(priv->base), "Failed to map PHY registers\n");

	priv->map = devm_regmap_init_mmio(dev, priv->base, &mp_phy_regmap_config);
	if (IS_ERR(priv->map))
		return PTR_ERR(priv->map);

	priv->num_clks = devm_clk_bulk_get_all(dev, &priv->clks);
	if (priv->num_clks < 0)
		return dev_err_probe(dev, priv->num_clks, "Failed to get PHY clocks\n");
	if (priv->num_clks != 5)
		return dev_err_probe(dev, -ENODEV, "Failed to get all PHY clocks\n");

	/*
	 * The reset ID order here does matters, reset_control_bulk_assert()
	 * asserts these resets in this order, with mpphy02 reset being
	 * asserted first, reset_control_bulk_deassert() deasserts these
	 * resets in reverse order, with mpphy02 being reset being
	 * deasserted last. This is the behavior the hardware expects.
	 */
	priv->resets[0].id = "mpphy02";
	priv->resets[1].id = "mpphy01";
	priv->resets[2].id = "mpphy11";
	priv->resets[3].id = "mpphy21";
	priv->resets[4].id = "mpphy31";
	ret = devm_reset_control_bulk_get_exclusive(&pdev->dev, ARRAY_SIZE(priv->resets),
						    priv->resets);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get PHY resets\n");

	platform_set_drvdata(pdev, priv);

	ret = dev_pm_domain_attach_list(dev, &pd_attach_data, &priv->pd_list);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to attach power domains\n");

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable PHY runtime PM\n");

	provider = devm_of_phy_provider_register(dev, mp_phy_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider), "Failed to register PHY provider\n");

	for (i = 0; i < MPPHY_NUM_CHANNELS; i++) {
		priv->chan[i].phy = devm_phy_create(dev, NULL, &mp_phy_ops);
		if (IS_ERR(priv->chan[i].phy)) {
			return dev_err_probe(dev, PTR_ERR(priv->chan[i].phy),
					     "Failed to create PHY %d\n", i);
		}

		priv->chan[i].phy->id = i;
		phy_set_drvdata(priv->chan[i].phy, priv);
	}

	return pm_runtime_resume_and_get(dev);
}

static void mp_phy_remove(struct platform_device *pdev)
{
	struct mp_phy_priv *priv = dev_get_drvdata(&pdev->dev);
	struct device *dev = &pdev->dev;

	pm_runtime_put(dev);

	dev_pm_domain_detach_list(priv->pd_list);

	pm_runtime_disable(&pdev->dev);

	if (priv->fw)
		release_firmware(priv->fw);

	platform_set_drvdata(pdev, NULL);
}

static int mp_phy_suspend(struct device *dev)
{
	struct mp_phy_priv *priv = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < MPPHY_NUM_CHANNELS; i++)
		priv->chan[i].initialized = false;

	clk_bulk_disable_unprepare(priv->num_clks, priv->clks);

	dev_info(dev, "Multi-Protocol PHY suspended\n");

	return 0;
}

static int mp_phy_resume(struct device *dev)
{
	struct mp_phy_priv *priv = dev_get_drvdata(dev);
	int i, ret;

	ret = reset_control_bulk_assert(ARRAY_SIZE(priv->resets), priv->resets);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to assert PHY resets\n");

	ret = reset_control_bulk_deassert(ARRAY_SIZE(priv->resets), priv->resets);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to deassert PHY resets\n");

	ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable PHY clocks\n");

	/* Reload configuration */
	regmap_set_bits(priv->map, MPPHY_CMNCNT1, priv->cmncnt[0]);
	regmap_write(priv->map, MPPHY_CMNCNT2, priv->cmncnt[1]);

	for (i = 0; i < MPPHY_NUM_CHANNELS; i++) {
		regmap_set_bits(priv->map, MPPHY_PXTEST(i), MPPHY_PXTEST_BIT);
		regmap_write(priv->map, MPPHY_PXSRAMCNT(i), priv->sramcnt[i]);

		if (priv->write_cntxt1 & BIT(i)) {
			regmap_write(priv->map, MPPHY_CHAN_BASE(i) + 0x10c, 0xff0ff);
			regmap_write(priv->map, MPPHY_PXCNTXT1(i), 0x180023);
		}

		regmap_clear_bits(priv->map, MPPHY_PXTEST(i), MPPHY_PXTEST_BIT);
	}

	regmap_clear_bits(priv->map, MPPHY_PCS0REG1, MPPHY_PCS0REG1_VAL);
	regmap_clear_bits(priv->map, MPPHY_PCS0REG5, 0xff000000);

	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(mp_phy_pm_ops, mp_phy_suspend, mp_phy_resume, NULL);

static const struct of_device_id mp_phy_of_match[] = {
	{ .compatible = "renesas,rcar-gen5-mpphy" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mp_phy_of_match);

static struct platform_driver mp_phy_driver = {
	.probe	= mp_phy_probe,
	.remove	= mp_phy_remove,
	.driver	= {
		.name		= "renesas-mpphy",
		.of_match_table	= mp_phy_of_match,
		.pm		= pm_ptr(&mp_phy_pm_ops),
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_platform_driver(mp_phy_driver);

MODULE_AUTHOR("Thanh Quan");
MODULE_DESCRIPTION("Renesas Multi-Protocol PHY driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(MPPHY_FW_NAME);
