// SPDX-License-Identifier: GPL-2.0
/*
 * SpacemiT K1 PCIe and PCIe/USB 3 combo PHY driver
 *
 * Copyright (C) 2025 by RISCstar Solutions Corporation.  All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <dt-bindings/phy/phy.h>

/*
 * Three PCIe ports are supported in the SpacemiT K1 SoC, and this driver
 * supports their PHYs.
 *
 * The PHY for PCIe port A is different from the PHYs for ports B and C:
 * - It has one lane, while ports B and C have two
 * - It is a combo PHY can be used for PCIe or USB 3
 * - It can automatically calibrate PCIe TX and RX termination settings
 *
 * The PHY functionality for PCIe ports B and C is identical:
 * - They have two PCIe lanes (but can be restricted to 1 via Device Tree)
 * - They are used for PCIe only
 * - They are configured using TX and RX values computed for port A
 *
 * A given board is designed to use the combo PHY for either PCIe or USB 3.
 * Whether the combo PHY is configured for PCIe or USB 3 is specified in
 * Device Tree using a phandle plus an argument.  The argument indicates
 * the type (either PHY_TYPE_PCIE or PHY_TYPE_USB3).
 *
 * Each PHY depends on clocks and resets provided by the controller
 * hardware (PCIe or USB) it is associated with.  The controller drivers
 * are required to enable any clocks and de-assert any resets that affect
 * PHY operation.
 *
 * PCIe PHYs must be programmed with RX and TX calibration values.  The
 * combo PHY is the only one that can determine these values.  They are
 * determined by temporarily enabling the combo PHY in PCIe mode at probe
 * time (if necessary).  This calibration only needs to be done once, and
 * when it has completed the TX and RX values are saved.
 *
 * To allow the combo PHY to be enabled for calibration, the resets and
 * clocks it uses in PCIe mode must be supplied.
 */

struct k1_pcie_phy {
	struct device *dev;		/* PHY provider device */
	struct phy *phy;
	void __iomem *regs;
	u32 pcie_lanes;			/* Max unless limited by DT */
	/* The remaining fields are only used for the combo PHY */
	u32 type;			/* PHY_TYPE_PCIE or PHY_TYPE_USB3 */
	struct regmap *pmu;
};

#define CALIBRATION_TIMEOUT		500000	/* microseconds */
#define PLL_TIMEOUT			500000	/* microseconds */
#define POLL_DELAY			500	/* microseconds */

/* Selecting the combo PHY operating mode requires PMU regmap access */
#define SYSCON_PMU			"spacemit,syscon-pmu"

/* PMU space, for selecting between PCIe and USB3 mode on the combo PHY */

#define PMUA_USB_PHY_CTRL0			0x0110
#define COMBO_PHY_SEL			BIT(3)	/* 0: PCIe; 1: USB3 */

#define PCIE_CLK_RES_CTRL			0x03cc
#define PCIE_APP_HOLD_PHY_RST		BIT(30)

/* PHY register space */

/* Offset between lane 0 and lane 1 registers when there are two */
#define PHY_LANE_OFFSET				0x0400

#define PCIE_PU_ADDR_CLK_CFG			0x0008
#define PLL_READY			BIT(0)		/* read-only */
#define CFG_RXCLK_EN			BIT(3)
#define CFG_TXCLK_EN			BIT(4)
#define CFG_PCLK_EN			BIT(5)
#define CFG_PIPE_PCLK_EN		BIT(6)
#define CFG_INTERNAL_TIMER_ADJ		GENMASK(10, 7)
#define TIMER_ADJ_USB		0x2
#define TIMER_ADJ_PCIE		0x6
#define CFG_SW_PHY_INIT_DONE		BIT(11)	/* We set after PLL config */

#define PCIE_RC_DONE_STATUS			0x0018
#define CFG_FORCE_RCV_RETRY		BIT(10)

#define PCIE_RC_CAL_REG2			0x0020
#define RC_CAL_TOGGLE			BIT(22)
#define CLKSEL				GENMASK(31, 29)
#define CLKSEL_24M		0x3

#define PCIE_PU_PLL_1				0x0048
#define REF_100_WSSC			BIT(12)	/* 1: input is 100MHz, SSC */
#define FREF_SEL			GENMASK(15, 13)
#define FREF_24M		0x1
#define SSC_DEP_SEL			GENMASK(19, 16)
#define SSC_DEP_NONE		0x0
#define SSC_DEP_5000PPM		0xa
#define SSC_MODE			GENMASK(21, 20)
#define SSC_MODE_DOWN_SPREAD	0x3
#define SSC_OFFSET			GENMASK(23, 22)
#define SSC_OFFSET_0_PPM	0x0

#define PCIE_PU_PLL_2				0x004c
#define GEN_REF100			BIT(4)	/* 1: generate 100MHz clk */

#define PCIE_RX_REG1				0x0050
#define EN_RTERM			BIT(3)
#define AFE_RTERM_REG			GENMASK(11, 8)

#define PCIE_RX_REG2				0x0054
#define RX_RTERM_SEL			BIT(5)	/* 0: use AFE_RTERM_REG value */

#define PCIE_LTSSM_DIS_ENTRY			0x005c
#define CFG_REFCLK_MODE			GENMASK(9, 8)
#define RFCLK_MODE_DRIVER	0x1
#define OVRD_REFCLK_MODE		BIT(10)	/* 1: use CFG_RFCLK_MODE */

#define PCIE_TX_REG1				0x0064
#define TX_RTERM_REG			GENMASK(15, 12)
#define TX_RTERM_SEL			BIT(25)	/* 1: use TX_RTERM_REG */

#define USB3_TEST_CTRL				0x0068

#define PCIE_RCAL_RESULT			0x0084	/* Port A PHY only */
#define RTERM_VALUE_RX			GENMASK(3, 0)
#define RTERM_VALUE_TX			GENMASK(7, 4)
#define R_TUNE_DONE			BIT(10)

static u32 k1_phy_rterm = ~0;     /* Invalid initial value */

/* Save the RX and TX receiver termination values */
static void k1_phy_rterm_set(u32 val)
{
	k1_phy_rterm = val & (RTERM_VALUE_RX | RTERM_VALUE_TX);
}

static bool k1_phy_rterm_valid(void)
{
	/* Valid if no bits outside those we care about are set */
	return !(k1_phy_rterm & ~(RTERM_VALUE_RX | RTERM_VALUE_TX));
}

static u32 k1_phy_rterm_rx(void)
{
	return FIELD_GET(RTERM_VALUE_RX, k1_phy_rterm);
}

static u32 k1_phy_rterm_tx(void)
{
	return FIELD_GET(RTERM_VALUE_TX, k1_phy_rterm);
}

/* Only the combo PHY has a PMU pointer defined */
static bool k1_phy_port_a(struct k1_pcie_phy *k1_phy)
{
	return !!k1_phy->pmu;
}

/*
 * Select PCIe or USB 3 mode for the combo PHY.  Return 1 if the bit
 * was changed, 0 if it was not, or a negative error value otherwise.
 */
static int k1_combo_phy_sel(struct k1_pcie_phy *k1_phy, bool usb3)
{
	int ret;

	ret = regmap_test_bits(k1_phy->pmu, PMUA_USB_PHY_CTRL0, COMBO_PHY_SEL);
	if (ret < 0)
		return ret;

	/* If it's already in the desired state, we're done */
	if (!!ret == usb3)
		return 0;

	/* Change the bit */
	ret = regmap_assign_bits(k1_phy->pmu, PMUA_USB_PHY_CTRL0,
				 COMBO_PHY_SEL, usb3);

	return ret < 0 ? ret : 1;
}

static void k1_pcie_phy_init_pll(struct k1_pcie_phy *k1_phy,
				 void __iomem *regs, bool pcie)
{
	void __iomem *virt;
	u32 timer_adj;
	u32 ssc_dep;
	u32 val;

	if (pcie) {
		timer_adj = TIMER_ADJ_PCIE;
		ssc_dep = SSC_DEP_NONE;
	} else {
		timer_adj = TIMER_ADJ_USB;
		ssc_dep = SSC_DEP_5000PPM;
	}

	/*
	 * Disable 100 MHz input reference with spread-spectrum
	 * clocking and select the 24 MHz clock input frequency
	 */
	virt = k1_phy->regs + PCIE_PU_PLL_1;
	val = readl(virt);
	val &= ~REF_100_WSSC;

	val &= ~FREF_SEL;
	val |= FIELD_PREP(FREF_SEL, FREF_24M);

	val &= ~SSC_DEP_SEL;
	val |= FIELD_PREP(SSC_DEP_SEL, ssc_dep);

	val &= ~SSC_MODE;
	val |= FIELD_PREP(SSC_MODE, SSC_MODE_DOWN_SPREAD);

	val &= ~SSC_OFFSET;
	val |= FIELD_PREP(SSC_OFFSET, SSC_OFFSET_0_PPM);
	writel(val, virt);

	if (pcie) {
		virt = regs + PCIE_PU_PLL_2;
		val = readl(virt);
		val |= GEN_REF100;	/* Enable 100 MHz PLL output clock */
		writel(val, virt);
	}

	/* Enable clocks and mark PLL initialization done */
	virt = regs + PCIE_PU_ADDR_CLK_CFG;
	val = readl(virt);
	val |= CFG_RXCLK_EN;
	val |= CFG_TXCLK_EN;
	val |= CFG_PCLK_EN;
	val |= CFG_PIPE_PCLK_EN;

	val &= ~CFG_INTERNAL_TIMER_ADJ;
	val |= FIELD_PREP(CFG_INTERNAL_TIMER_ADJ, timer_adj);

	val |= CFG_SW_PHY_INIT_DONE;
	writel(val, virt);
}

static int k1_pcie_pll_lock(struct k1_pcie_phy *k1_phy, bool pcie)
{
	u32 val = pcie ? CFG_FORCE_RCV_RETRY : 0;
	void __iomem *virt;

	writel(val, k1_phy->regs + PCIE_RC_DONE_STATUS);

	/*
	 * Wait for indication the PHY PLL is locked.  Lanes for ports
	 * B and C share a PLL, so it's enough to sample just lane 0.
	 */
	virt = k1_phy->regs + PCIE_PU_ADDR_CLK_CFG;	/* Lane 0 */

	return readl_poll_timeout(virt, val, val & PLL_READY,
				  POLL_DELAY, PLL_TIMEOUT);
}

static int k1_pcie_phy_init_pcie(struct k1_pcie_phy *k1_phy)
{
	u32 rx_rterm = k1_phy_rterm_rx();
	u32 tx_rterm = k1_phy_rterm_tx();
	void __iomem *virt;
	void __iomem *regs;
	u32 val;
	int ret;
	int i;

	/* For the combo PHY, set PHY to PCIe mode */
	if (k1_phy_port_a(k1_phy)) {
		ret = k1_combo_phy_sel(k1_phy, false);
		if (ret < 0)
			return ret;
	}

	regs = k1_phy->regs;
	for (i = 0; i < k1_phy->pcie_lanes; i++) {
		virt = regs + PCIE_RX_REG1;
		val = readl(virt);

		/* Set RX analog front-end receiver termination value */
		val &= ~AFE_RTERM_REG;
		val |= FIELD_PREP(AFE_RTERM_REG, rx_rterm);

		/* And enable refclock receiver termination */
		val |= EN_RTERM;
		writel(val, virt);

		virt = regs + PCIE_RX_REG2;
		val = readl(virt);
		/* Use PCIE_RX_REG1 AFE_RTERM_REG value */
		val &= ~RX_RTERM_SEL;
		writel(val, virt);

		virt = regs + PCIE_TX_REG1;
		val = readl(virt);

		/* Set TX driver termination value */
		val &= ~TX_RTERM_REG;
		val |= FIELD_PREP(TX_RTERM_REG, tx_rterm);

		/* Use PCIE_TX_REG1 TX_RTERM_REG value */
		val |= TX_RTERM_SEL;
		writel(val, virt);

		virt = regs + PCIE_RC_CAL_REG2;
		val = readl(virt);

		/* Set the input clock to 24 MHz, and clear RC_CAL_TOGGLE */
		val &= CLKSEL;
		val |= FIELD_PREP(CLKSEL, CLKSEL_24M);

		val &= ~RC_CAL_TOGGLE;
		writel(val, virt);
		/* Trigger recalibration by setting RC_CAL_TOGGLE again */
		val |= RC_CAL_TOGGLE;
		writel(val, virt);

		virt = regs + PCIE_LTSSM_DIS_ENTRY;
		val = readl(virt);
		/* Override the reference clock; set to refclk driver mode */
		val |= OVRD_REFCLK_MODE;

		val &= ~CFG_REFCLK_MODE;
		val |= FIELD_PREP(CFG_REFCLK_MODE, RFCLK_MODE_DRIVER);

		writel(val, virt);

		k1_pcie_phy_init_pll(k1_phy, regs, true);

		regs += PHY_LANE_OFFSET;	/* Next lane */
	}

	return k1_pcie_pll_lock(k1_phy, true);
}

/* Only called for combo PHY */
static int k1_pcie_phy_init_usb3_host(struct k1_pcie_phy *k1_phy)
{
	int ret;

	ret = k1_combo_phy_sel(k1_phy, true);
	if (ret < 0)
		return ret;

	/* We're not doing any testing */
	writel(0, k1_phy->regs + USB3_TEST_CTRL);

	k1_pcie_phy_init_pll(k1_phy, k1_phy->regs, false);

	return k1_pcie_pll_lock(k1_phy, false);
}

static int k1_pcie_phy_init(struct phy *phy)
{
	struct k1_pcie_phy *k1_phy = phy_get_drvdata(phy);

	if (k1_phy_port_a(k1_phy) && k1_phy->type == PHY_TYPE_USB3)
		return k1_pcie_phy_init_usb3_host(k1_phy);

	return k1_pcie_phy_init_pcie(k1_phy);
}

static const struct phy_ops k1_pcie_phy_ops = {
	.init		= k1_pcie_phy_init,
	.owner		= THIS_MODULE,
};

/*
 * Get values needed for calibrating PHYs operating in PCIe mode.  Only
 * the combo PHY is able to do this, and its calibration values are used
 * for configuring all PCIe PHYs.
 *
 * We always need to de-assert the (PCIe) global reset on the combo PHY,
 * because the USB driver depends on it.  If used for PCIe, the driver
 * will (also) de-assert this, but by leaving it de-asserted for the
 * combo PHY, the USB driver doesn't have to do this.
 * of this first.
 *
 * In addition, we guarantee the APP_HOLD_PHY_RESET bit is clear for the
 * combo PHY, so the USB driver doesn't have to manage that either.  The
 * PCIe driver is free to change this bit for normal operation.
 *
 * Calibration only needs to be done once.  It's possible calibration has
 * already completed (e.g., it might have happened in the boot loader, or
 * -EPROBE_DEFER might result in this function being called again).  So we
 * check that early too, to avoid doing it more than once.
 *
 * Otherwise we temporarily power up the PHY, wait for the hardware to
 * indicate calibration is done, grab the value, then shut the PHY down
 * again.
 */
static int k1_pcie_combo_phy_calibrate(struct k1_pcie_phy *k1_phy)
{
	struct reset_control_bulk_data data[] = {
		{ .id = "dbi", },
		{ .id = "mstr", },
		{ .id = "slv", },
	};
	size_t data_size = ARRAY_SIZE(data);
	struct reset_control *global_reset;
	struct device *dev = k1_phy->dev;
	struct clk_bulk_data *clocks;
	void __iomem *virt;
	bool mode_changed;
	u32 clock_count;
	int ret = 0;
	int val;

	/* We always de-assert the global reset and leave it that way */
	global_reset = devm_reset_control_get_shared_deasserted(dev, "global");
	if (IS_ERR(global_reset))
		return PTR_ERR(global_reset);

	/*
	 * We also guarantee the APP_HOLD_PHY_RESET bit is clear.  If an
	 * error occurs we can't go on, but otherwise we can leave this
	 * bit clear even if an error happens below.
	 */
	ret = regmap_assign_bits(k1_phy->pmu, PCIE_CLK_RES_CTRL,
				 PCIE_APP_HOLD_PHY_RST, false);
	if (ret < 0)
		return ret;

	/* If the receiver termination value is valid, nothing more to do */
	if (k1_phy_rterm_valid())
		return 0;

	/* If the calibration already completed (e.g. by U-Boot), we're done */
	virt = k1_phy->regs + PCIE_RCAL_RESULT;
	val = readl(virt);
	if (val & R_TUNE_DONE)
		goto done;

	/* Make sure the PHY is configured for PCIe */
	ret = k1_combo_phy_sel(k1_phy, false);
	if (ret < 0)
		return ret;
	mode_changed = ret > 0;

	/* Get and enable all clocks */
	ret = clk_bulk_get_all(dev, &clocks);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ENOENT;
	clock_count = ret;

	ret = clk_bulk_prepare_enable(clock_count, clocks);
	if (ret)
		goto out_put_clocks;

	/* Get the (not "global") PCIe application resets */
	ret = reset_control_bulk_get_shared(dev, data_size, data);
	if (ret)
		goto out_disable_clocks;

	/* De-assert the PCIe application resets */
	ret = reset_control_bulk_deassert(data_size, data);
	if (ret)
		goto out_put_resets;

	/*
	 * This is the core activity here.  Wait for the hardware to
	 * signal that it has completed calibration/tuning.  Once it
	 * has, the register value will contain the values we'll
	 * use to configure PCIe PHYs.
	 */
	ret = readl_poll_timeout(virt, val, val & R_TUNE_DONE,
				 POLL_DELAY, CALIBRATION_TIMEOUT);

	/* Clean up.  We're done with the resets and clocks */
	reset_control_bulk_assert(data_size, data);
out_put_resets:
	reset_control_bulk_put(data_size, data);
out_disable_clocks:
	clk_bulk_disable_unprepare(clock_count, clocks);
out_put_clocks:
	clk_bulk_put_all(clock_count, clocks);

	/* If we changed the mode, restore the original state */
	if (mode_changed)
		(void)k1_combo_phy_sel(k1_phy, true);
done:
	/* If we got the value without timing out, set k1_phy_rterm */
	if (!ret)
		k1_phy_rterm_set(val);

	return ret;
}

static struct phy *
k1_pcie_combo_phy_xlate(struct device *dev, const struct of_phandle_args *args)
{
	struct k1_pcie_phy *k1_phy = dev_get_drvdata(dev);
	u32 type;

	/* The argument specifying the PHY mode is required */
	if (args->args_count != 1)
		return ERR_PTR(-EINVAL);

	/* We only support PCIe and USB3 mode */
	type = args->args[0];
	if (type != PHY_TYPE_PCIE && type != PHY_TYPE_USB3)
		return ERR_PTR(-EINVAL);

	/* This PHY can only be used once */
	if (k1_phy->type != PHY_NONE)
		return ERR_PTR(-EBUSY);

	k1_phy->type = type;

	return k1_phy->phy;
}

/* Use the maximum number of PCIe lanes unless limited by Device Tree */
static u32 k1_pcie_num_lanes(struct k1_pcie_phy *k1_phy, bool port_a)
{
	struct device *dev = k1_phy->dev;
	u32 count = 0;
	u32 max;
	int ret;

	ret = of_property_read_u32(dev_of_node(dev), "num-lanes", &count);
	if (count == 1)
		return 1;

	if (count == 2 && !port_a)
		return 2;

	max = port_a ? 1 : 2;
	if (ret != -EINVAL)
		dev_warn(dev, "bad lane count %u for port; using %u\n",
			 count, max);

	return max;
}

static int k1_pcie_combo_phy_probe(struct k1_pcie_phy *k1_phy)
{
	struct device *dev = k1_phy->dev;
	struct regmap *regmap;
	int ret;

	/* Setting the PHY mode requires access to the PMU regmap */
	regmap = syscon_regmap_lookup_by_phandle(dev_of_node(dev), SYSCON_PMU);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "error getting PMU\n");
	k1_phy->pmu = regmap;

	ret = k1_pcie_combo_phy_calibrate(k1_phy);
	if (ret)
		return dev_err_probe(dev, ret, "calibration failed\n");

	/* Needed by k1_pcie_combo_phy_xlate(), which also sets k1_phy->type */
	dev_set_drvdata(dev, k1_phy);

	return 0;
}

static int k1_pcie_phy_probe(struct platform_device *pdev)
{
	struct phy *(*xlate)(struct device *, const struct of_phandle_args *);
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct k1_pcie_phy *k1_phy;
	bool probing_port_a;
	int ret;

	xlate = of_device_get_match_data(dev);
	probing_port_a = xlate == k1_pcie_combo_phy_xlate;

	/* Only the combo PHY can calibrate, so it must probe first */
	if (!k1_phy_rterm_valid() && !probing_port_a)
		return -EPROBE_DEFER;

	k1_phy = devm_kzalloc(dev, sizeof(*k1_phy), GFP_KERNEL);
	if (!k1_phy)
		return -ENOMEM;
	k1_phy->dev = dev;

	k1_phy->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(k1_phy->regs))
		return dev_err_probe(dev, PTR_ERR(k1_phy->regs),
				     "error mapping registers\n");

	k1_phy->pcie_lanes = k1_pcie_num_lanes(k1_phy, probing_port_a);

	if (probing_port_a) {
		ret = k1_pcie_combo_phy_probe(k1_phy);
		if (ret)
			return dev_err_probe(dev, ret,
					     "error probing combo phy\n");
	}

	k1_phy->phy = devm_phy_create(dev, NULL, &k1_pcie_phy_ops);
	if (IS_ERR(k1_phy->phy))
		return dev_err_probe(dev, PTR_ERR(k1_phy->phy),
				     "error creating phy\n");
	phy_set_drvdata(k1_phy->phy, k1_phy);

	provider = devm_of_phy_provider_register(dev, xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider),
				     "error registering provider\n");
	return 0;
}

static const struct of_device_id k1_pcie_phy_of_match[] = {
	{ .compatible = "spacemit,k1-combo-phy", k1_pcie_combo_phy_xlate, },
	{ .compatible = "spacemit,k1-pcie-phy", of_phy_simple_xlate, },
	{ },
};
MODULE_DEVICE_TABLE(of, k1_pcie_phy_of_match);

static struct platform_driver k1_pcie_phy_driver = {
	.probe	= k1_pcie_phy_probe,
	.driver = {
		.of_match_table	= k1_pcie_phy_of_match,
		.name = "k1-pcie-phy",
	}
};
module_platform_driver(k1_pcie_phy_driver);

MODULE_DESCRIPTION("SpacemiT K1 PCIe 3.0 and USB 3 PHY driver");
MODULE_LICENSE("GPL");
