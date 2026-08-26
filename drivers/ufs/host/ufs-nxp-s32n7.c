// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-3-Clause)
/*
 * Copyright 2026 NXP
 *
 */

#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ufshcd-pltfrm.h"
#include "ufshcd-dwc.h"
#include "ufshci-dwc.h"

/* SCM Register Offsets. */
#define SCM_ONE_US_TICK			0x2CU
#define SCM_STATUS			0x30

/* SCM_ONE_US_TICK Register. */
#define SCM_ONE_US_TICK_MASK		GENMASK(8, 0)

/* SCM_MPHY_RAM_CONFIG_STATUS Register Fields. */
#define SCM_STATUS_PHY_RESET_MASK		BIT(0)
#define SCM_STATUS_SRAM_BYPASS_MASK		BIT(1)
#define SCM_STATUS_SRAM_INIT_DONE_MASK		BIT(16)

#define SCM_STATUS_PHY_RESET(val)		\
	FIELD_PREP(SCM_STATUS_PHY_RESET_MASK, val)

#define SCM_STATUS_SRAM_BYPASS(val)		\
	FIELD_PREP(SCM_STATUS_SRAM_BYPASS_MASK, val)

/* Timeout values. */
#define CFG_MPHY_INIT_TIMEOUT_VALUE_US	3000000

/* Clock validation limits. */
#define MAX_VALID_ONE_US_TICK		SCM_ONE_US_TICK_MASK

/* CPort definitions */
#define CPORT_0				0

/* Hibern8 state poll timeout. */
#define HBRN8_POLL_TOUT_MS      1000

/**
 * enum mphy_boot_mode - MPHY boot mode options
 * @MPHY_BOOT_NONE: Skip MPHY initialization
 * @MPHY_BOOT_ROM: Running MPHY from internal FW ROM
 */
enum mphy_boot_mode {
	MPHY_BOOT_NONE = 0,
	MPHY_BOOT_ROM = 1,
};

/**
 * struct s32n_ufs - S32N7 UFS host controller data
 * @hba: UFS host controller instance
 * @reg_scm: SCM register base address
 * @core_clk: core reference clock used to derive ONE_US_TICK/HCLKDIV
 * @mphy_boot_mode: MPHY boot mode configuration
 */
struct s32n_ufs {
	struct ufs_hba *hba;
	void __iomem *reg_scm;
	struct clk *core_clk;
	enum mphy_boot_mode mphy_boot_mode;
};

/**
 * struct phy_reg_cfg - PHY register configuration entry
 * @reg: Register offset/address
 * @val: Value to write
 */
struct phy_reg_cfg {
	u32 reg;
	u32 val;
};

/**
 * ufs_s32n_phy_write_sequence - Write a sequence of PHY registers
 * @hba: UFS host controller instance
 * @cfg: Array of register configurations
 * @count: Number of entries in the array
 *
 * Return: 0 on success, error code on failure
 */
static int ufs_s32n_phy_write_sequence(struct ufs_hba *hba,
				       const struct phy_reg_cfg *cfg,
				       size_t count)
{
	int ret;
	size_t i;

	for (i = 0; i < count; i++) {
		ret = ufshcd_dwc_phy_reg_write(hba, cfg[i].reg, cfg[i].val);
		if (ret) {
			dev_err(hba->dev,
				"Failed to write PHY reg 0x%x = 0x%x (step %zu)\n",
				cfg[i].reg, cfg[i].val, i);
			return ret;
		}
	}

	return 0;
}

static unsigned long ufs_s32n_calculate_us_tick(unsigned long clk_rate)
{
	return clk_rate / USEC_PER_SEC;
}

static int ufs_s32n_configure_clocks(struct ufs_hba *hba)
{
	struct s32n_ufs *ufs = ufshcd_get_variant(hba);
	struct device *dev = hba->dev;
	unsigned long clk_rate = 0;
	unsigned long one_us_tick;

	if (!ufs->core_clk) {
		dev_err(dev, "Invalid core_clk.\n");
		return -EINVAL;
	}

	clk_rate = clk_get_rate(ufs->core_clk);
	if (!clk_rate) {
		dev_err(dev, "Failed to get valid clock rate.\n");
		return -EINVAL;
	}

	one_us_tick = ufs_s32n_calculate_us_tick(clk_rate);
	if (one_us_tick == 0 || one_us_tick > MAX_VALID_ONE_US_TICK) {
		dev_err(dev, "Invalid one_us_tick value: %lu (clk_rate: %lu Hz).\n",
			one_us_tick, clk_rate);
		return -EINVAL;
	}

	dev_dbg(dev, "Core clock rate: %lu Hz, one_us_tick = %lu.\n",
		 clk_rate, one_us_tick);

	/*
	 * Configure the micro-second tick rate generator based on core
	 * clock rate.
	 */
	writel(one_us_tick, ufs->reg_scm + SCM_ONE_US_TICK);
	ufshcd_dwc_program_clk_div(hba, one_us_tick);

	return 0;
}

static int ufs_s32n_phy_initial_calib(struct ufs_hba *hba)
{
	static const struct phy_reg_cfg initial_calib[] = {
		{ FAST_FLAGS(0), 0x6 },
		{ FAST_FLAGS(1), 0x6 },
		{ RX_DAC_CTRL_OVRD(0), 0x1 },
		{ RX_DAC_CTRL_OVRD(1), 0x1 },
		{ RX_DAC_CTRL(0), 0x8E },
		{ RX_DAC_CTRL(1), 0x91 },
		{ RX_DAC_CTRL_SEL(0), 0x1 },
		{ RX_DAC_CTRL_SEL(1), 0x1 },
		{ RX_DAC_CTRL_EN(0), 0x1 },
		{ RX_DAC_CTRL_EN(1), 0x1 },
		{ RX_DAC_CTRL_OVRD(0), 0x0 },
		{ RX_DAC_CTRL_OVRD(1), 0x0 },
		{ RX_DAC_CTRL_OVRD(0), 0x1 },
		{ RX_DAC_CTRL_OVRD(1), 0x1 },
		{ RX_DAC_CTRL(0), 0x71 },
		{ RX_DAC_CTRL(1), 0x7F },
		{ RX_DAC_CTRL_SEL(0), 0x2 },
		{ RX_DAC_CTRL_SEL(1), 0x2 },
		{ RX_DAC_CTRL_EN(0), 0x1 },
		{ RX_DAC_CTRL_EN(1), 0x1 },
		{ RX_DAC_CTRL_OVRD(0), 0x0 },
		{ RX_DAC_CTRL_OVRD(1), 0x0 },
		{ FW_CALIB_CCFG(0), 0x100 },
		{ FW_CALIB_CCFG(1), 0x100 },
	};
	int ret;

	/* Clock Control */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CBREFCLKCTRL2, 0), 0x80);
	if (ret)
		return ret;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VS_MPHYCFGUPDT, 0), 0x1);
	if (ret)
		return ret;

	/* Initial calibration sequence */
	return ufs_s32n_phy_write_sequence(hba, initial_calib,
					   ARRAY_SIZE(initial_calib));
}

static int ufs_s32n_link_startup_pre_change(struct ufs_hba *hba)
{
	struct s32n_ufs *ufs = ufshcd_get_variant(hba);
	struct device *dev = hba->dev;
	int ret;
	u32 reg;

	/*
	 * There are two supported MPHY boot options implemented:
	 * - Running MPHY from internal FW ROM:
	 *  MPHY_RAM_CONFIG_STATUS.SRAM_BYPASS = 1
	 *  MPHY_RAM_CONFIG_STATUS.SRAM_EXT_LD_DONE = 0
	 * - Skip M-PHY initialization when it is handled externally.
	 */
	if (ufs->mphy_boot_mode == MPHY_BOOT_NONE) {
		dev_dbg(dev, "Skipping UFS MPHY init.\n");
		return 0;
	}

	ret = ufs_s32n_configure_clocks(hba);
	if (ret)
		return ret;

	/* Reset SCM.MPHY_RAM_CONFIG_STATUS to default value; keep MPHY in reset. */
	writel(SCM_STATUS_PHY_RESET(1), ufs->reg_scm + SCM_STATUS);
	if (ufs->mphy_boot_mode == MPHY_BOOT_ROM) {
		/* ROM Mode. */
		writel(SCM_STATUS_PHY_RESET(1) | SCM_STATUS_SRAM_BYPASS(1),
		       ufs->reg_scm + SCM_STATUS);
	}

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CBCRCTRL, 0), 0x1);
	if (ret)
		return ret;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VS_MPHYCFGUPDT, 0), 0x1);
	if (ret)
		return ret;

	/* Release PHY_RESET. */
	writel(readl(ufs->reg_scm + SCM_STATUS) & ~SCM_STATUS_PHY_RESET_MASK,
	       ufs->reg_scm + SCM_STATUS);

	/* Wait until SRAM_INIT_DONE = 1. */
	ret = readl_poll_timeout(ufs->reg_scm + SCM_STATUS, reg,
			reg & SCM_STATUS_SRAM_INIT_DONE_MASK,
			1000, CFG_MPHY_INIT_TIMEOUT_VALUE_US);
	if (ret) {
		dev_err(dev, "UFS MPHY init not done!\n");
		return ret;
	}

	/* Start of initial calibration */
	ret = ufs_s32n_phy_initial_calib(hba);
	if (ret) {
		dev_err(dev, "UFS MPHY initial calibration failed!\n");
		return ret;
	}

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VS_MPHYDISABLE, 0), 0x0);
	if (ret)
		return ret;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VS_MPHYCFGUPDT, 0), 0x1);
	if (ret)
		return ret;

	/* Set Local DeviceID. */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(N_DEVICEID, 0), 0x0);
	if (ret)
		return ret;

	/* End of Gear1 settings */

	return ufshcd_check_hibern8(hba, hba->lanes_per_direction,
				    HBRN8_POLL_TOUT_MS);
}

static int ufs_s32n_link_startup_post_change(struct ufs_hba *hba)
{
	static const struct ufshcd_dme_attr_val cport_setup[] = {
		{ UIC_ARG_MIB_SEL(T_CONNECTIONSTATE, CPORT_0),
				CPORT_IDLE, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(T_CPORTFLAGS, CPORT_0),
				CPORT_DEF_FLAGS, DME_LOCAL },
		{ UIC_ARG_MIB_SEL(T_CONNECTIONSTATE, CPORT_0),
				CPORT_CONNECTED, DME_LOCAL },
	};
	static const struct phy_reg_cfg post_calib[] = {
		{ RX_OVRD_IN_1(0), 0xc },
		{ RX_OVRD_IN_1(1), 0xc },
		{ RX_OVRD_IN_1(0), 0x8 },
		{ RX_OVRD_IN_1(1), 0x8 },
		{ RX_OVRD_IN_1(0), 0x0 },
		{ RX_OVRD_IN_1(1), 0x0 },
	};
	struct s32n_ufs *ufs = ufshcd_get_variant(hba);
	unsigned int data = 0;
	int ret;

	if (ufs->mphy_boot_mode == MPHY_BOOT_NONE)
		return 0;

	/* Set Connection State to IDLE (it allows CPort Attributes to be set). */
	ret = ufshcd_dwc_dme_set_attrs(hba, cport_setup, ARRAY_SIZE(cport_setup));
	if (ret)
		return ret;

	ret = ufshcd_dme_get(hba, UIC_ARG_MIB_SEL(T_CONNECTIONSTATE, CPORT_0), &data);
	if (ret)
		return ret;

	if (data != CPORT_CONNECTED)
		return -EIO;

	/* Post Link Startup Calibration sequence */
	ret = ufs_s32n_phy_write_sequence(hba, post_calib,
					   ARRAY_SIZE(post_calib));
	if (ret)
		return ret;

	/*
	 * Performing MPHY configuration for rate change:
	 * CB rate selection: 0 - rate A, 1 - rate B;
	 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CBRATESEL, 0), 0x1);
	if (ret)
		return ret;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VS_MPHYCFGUPDT, 0), 0x1);
	if (ret)
		return ret;

	/* End of Gear4 calibration */
	return 0;
}

static int ufs_s32n_link_startup_notify(struct ufs_hba *hba,
			     enum ufs_notify_change_status status)
{
	int err;

	if (status == PRE_CHANGE) {
		err = ufs_s32n_link_startup_pre_change(hba);
		if (err) {
			dev_err(hba->dev, "MPHY setup failed (%d).\n", err);
			return err;
		}
		return 0;
	}

	/* POST_CHANGE */
	err = ufshcd_dwc_link_is_up(hba);
	if (err) {
		dev_err(hba->dev, "Link is not up.\n");
		return err;
	}

	err = ufs_s32n_link_startup_post_change(hba);
	if (err)
		dev_err(hba->dev, "Connection setup failed (%d).\n", err);

	return err;
}

static int ufs_s32n_init(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *np = dev->of_node;
	const char *mphy_boot_mode;
	struct s32n_ufs *ufs;
	int ret;

	ufs = devm_kzalloc(dev, sizeof(*ufs), GFP_KERNEL);
	if (!ufs)
		return -ENOMEM;

	ufs->hba = hba;
	ufs->mphy_boot_mode = MPHY_BOOT_NONE;

	hba->quirks |= UFSHCD_QUIRK_BROKEN_AUTO_HIBERN8;
	hba->quirks |= UFSHCD_QUIRK_PERFORM_LINK_STARTUP_ONCE;
	hba->spm_lvl = UFS_PM_LVL_5;

	ret = of_property_read_string(np, "nxp,mphy-boot-mode", &mphy_boot_mode);
	if (ret || !mphy_boot_mode) {
		dev_dbg(dev,
			"nxp,mphy-boot-mode property not found. UFS MPHY init will be skipped.\n");
		goto init_out;
	}

	if (!strcmp(mphy_boot_mode, "rom")) {
		ufs->mphy_boot_mode = MPHY_BOOT_ROM;
	} else {
		return dev_err_probe(dev, -EINVAL,
				     "Unrecognized nxp,mphy-boot-mode property. UFS MPHY init will be skipped.\n");
	}

	ufs->reg_scm = devm_platform_ioremap_resource_byname(pdev, "scm");
	if (IS_ERR(ufs->reg_scm))
		return dev_err_probe(dev, PTR_ERR(ufs->reg_scm),
				     "ioremap failed for SCM registers.\n");

	ufs->core_clk = devm_clk_get(dev, "core_clk");
	if (IS_ERR(ufs->core_clk))
		return dev_err_probe(dev, PTR_ERR(ufs->core_clk),
				     "Failed to get core clock.\n");

init_out:
	ufshcd_set_variant(hba, ufs);

	return 0;
}

static const struct ufs_hba_variant_ops ufs_hba_s32n79_vops = {
	.name = "s32n79",
	.init = ufs_s32n_init,
	.link_startup_notify = ufs_s32n_link_startup_notify,
};

static const struct of_device_id ufs_s32n7_match[] = {
	{
		.compatible = "nxp,s32n79-ufshc",
		.data = &ufs_hba_s32n79_vops,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ufs_s32n7_match);

static int ufs_s32n7_probe(struct platform_device *pdev)
{
	const struct ufs_hba_variant_ops *vops;

	vops = device_get_match_data(&pdev->dev);
	if (!vops)
		return -ENODEV;

	return ufshcd_pltfrm_init(pdev, vops);
}

static void ufs_s32n7_remove(struct platform_device *pdev)
{
	ufshcd_pltfrm_remove(pdev);
}

static const struct dev_pm_ops ufs_s32n7_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ufshcd_system_suspend, ufshcd_system_resume)
	SET_RUNTIME_PM_OPS(ufshcd_runtime_suspend, ufshcd_runtime_resume, NULL)
	.prepare	= ufshcd_suspend_prepare,
	.complete	= ufshcd_resume_complete,
};

static struct platform_driver ufs_s32n7_driver = {
	.probe = ufs_s32n7_probe,
	.remove = ufs_s32n7_remove,
	.driver	= {
		.name	= "ufs-s32n7",
		.pm	= &ufs_s32n7_pm_ops,
		.of_match_table	= ufs_s32n7_match,
	},
};

module_platform_driver(ufs_s32n7_driver);

MODULE_AUTHOR("Larisa Grigore <larisa.grigore@oss.nxp.com>");
MODULE_DESCRIPTION("NXP S32N7 UFS Host Controller platform driver");
MODULE_LICENSE("Dual BSD/GPL");
