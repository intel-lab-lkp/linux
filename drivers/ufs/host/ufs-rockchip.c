// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rockchip UFS Host Controller driver
 *
 * Copyright (C) 2024 Rockchip Electronics Co.Ltd.
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <soc/rockchip/rockchip_sip.h>

#include <ufs/ufshcd.h>
#include <ufs/unipro.h>
#include "ufshcd-pltfrm.h"
#include "ufs-rockchip.h"

static int ufs_rockchip_hce_enable_notify(struct ufs_hba *hba,
					 enum ufs_notify_change_status status)
{
	int err = 0;

	if (status == POST_CHANGE)
		err = ufshcd_vops_phy_initialization(hba);

	return err;
}

static void ufs_rockchip_set_pm_lvl(struct ufs_hba *hba)
{
	hba->rpm_lvl = UFS_PM_LVL_5;
	hba->spm_lvl = UFS_PM_LVL_5;
}

static int ufs_rockchip_rk3576_phy_init(struct ufs_hba *hba)
{
	struct ufs_rockchip_host *host = ufshcd_get_variant(hba);

	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(PA_LOCAL_TX_LCC_ENABLE, 0x0), 0x0);
	/* enable the mphy DME_SET cfg */
	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x200, 0x0), 0x40);
	for (int i = 0; i < 2; i++) {
		/* Configuration M-TX */
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0xaa, SEL_TX_LANE0 + i), 0x06);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0xa9, SEL_TX_LANE0 + i), 0x02);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0xad, SEL_TX_LANE0 + i), 0x44);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0xac, SEL_TX_LANE0 + i), 0xe6);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0xab, SEL_TX_LANE0 + i), 0x07);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x94, SEL_TX_LANE0 + i), 0x93);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x93, SEL_TX_LANE0 + i), 0xc9);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x7f, SEL_TX_LANE0 + i), 0x00);
		/* Configuration M-RX */
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x12, SEL_RX_LANE0 + i), 0x06);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x11, SEL_RX_LANE0 + i), 0x00);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x1d, SEL_RX_LANE0 + i), 0x58);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x1c, SEL_RX_LANE0 + i), 0x8c);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x1b, SEL_RX_LANE0 + i), 0x02);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x25, SEL_RX_LANE0 + i), 0xf6);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x2f, SEL_RX_LANE0 + i), 0x69);
	}
	/* disable the mphy DME_SET cfg */
	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x200, 0x0), 0x00);

	ufs_sys_writel(host->mphy_base, 0x80, 0x08C);
	ufs_sys_writel(host->mphy_base, 0xB5, 0x110);
	ufs_sys_writel(host->mphy_base, 0xB5, 0x250);

	ufs_sys_writel(host->mphy_base, 0x03, 0x134);
	ufs_sys_writel(host->mphy_base, 0x03, 0x274);

	ufs_sys_writel(host->mphy_base, 0x38, 0x0E0);
	ufs_sys_writel(host->mphy_base, 0x38, 0x220);

	ufs_sys_writel(host->mphy_base, 0x50, 0x164);
	ufs_sys_writel(host->mphy_base, 0x50, 0x2A4);

	ufs_sys_writel(host->mphy_base, 0x80, 0x178);
	ufs_sys_writel(host->mphy_base, 0x80, 0x2B8);

	ufs_sys_writel(host->mphy_base, 0x18, 0x1B0);
	ufs_sys_writel(host->mphy_base, 0x18, 0x2F0);

	ufs_sys_writel(host->mphy_base, 0x03, 0x128);
	ufs_sys_writel(host->mphy_base, 0x03, 0x268);

	ufs_sys_writel(host->mphy_base, 0x20, 0x12C);
	ufs_sys_writel(host->mphy_base, 0x20, 0x26C);

	ufs_sys_writel(host->mphy_base, 0xC0, 0x120);
	ufs_sys_writel(host->mphy_base, 0xC0, 0x260);

	ufs_sys_writel(host->mphy_base, 0x03, 0x094);

	ufs_sys_writel(host->mphy_base, 0x03, 0x1B4);
	ufs_sys_writel(host->mphy_base, 0x03, 0x2F4);

	ufs_sys_writel(host->mphy_base, 0xC0, 0x08C);
	usleep_range(1, 2);
	ufs_sys_writel(host->mphy_base, 0x00, 0x08C);

	usleep_range(200, 250);
	/* start link up */
	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(MIB_T_DBG_CPORT_TX_ENDIAN, 0), 0x0);
	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(MIB_T_DBG_CPORT_RX_ENDIAN, 0), 0x0);
	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(N_DEVICEID, 0), 0x0);
	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(N_DEVICEID_VALID, 0), 0x1);
	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(T_PEERDEVICEID, 0), 0x1);
	ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(T_CONNECTIONSTATE, 0), 0x1);

	return 0;
}

static int ufs_rockchip_common_init(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct ufs_rockchip_host *host;
	int err;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	/* system control register for hci */
	host->ufs_sys_ctrl = devm_platform_ioremap_resource_byname(pdev, "hci_grf");
	if (IS_ERR(host->ufs_sys_ctrl))
		return dev_err_probe(dev, PTR_ERR(host->ufs_sys_ctrl),
					"cannot ioremap for hci system control register\n");

	/* system control register for mphy */
	host->ufs_phy_ctrl = devm_platform_ioremap_resource_byname(pdev, "mphy_grf");
	if (IS_ERR(host->ufs_phy_ctrl))
		return dev_err_probe(dev, PTR_ERR(host->ufs_phy_ctrl),
				"cannot ioremap for mphy system control register\n");

	/* mphy base register */
	host->mphy_base = devm_platform_ioremap_resource_byname(pdev, "mphy");
	if (IS_ERR(host->mphy_base))
		return dev_err_probe(dev, PTR_ERR(host->mphy_base),
				"cannot ioremap for mphy base register\n");

	host->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(host->rst))
		return dev_err_probe(dev, PTR_ERR(host->rst),
				"failed to get reset control\n");

	reset_control_assert(host->rst);
	usleep_range(1, 2);
	reset_control_deassert(host->rst);

	host->ref_out_clk = devm_clk_get_enabled(dev, "ref_out");
	if (IS_ERR(host->ref_out_clk))
		return dev_err_probe(dev, PTR_ERR(host->ref_out_clk),
				"ref_out unavailable\n");

	host->rst_gpio = devm_gpiod_get(&pdev->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(host->rst_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(host->rst_gpio),
				"invalid reset-gpios property in node\n");

	host->clks[0].id = "core";
	host->clks[1].id = "pclk";
	host->clks[2].id = "pclk_mphy";
	err = devm_clk_bulk_get_optional(dev, UFS_MAX_CLKS, host->clks);
	if (err)
		return dev_err_probe(dev, err, "failed to get clocks\n");

	err = clk_bulk_prepare_enable(UFS_MAX_CLKS, host->clks);
	if (err)
		return dev_err_probe(dev, err, "failed to enable clocks\n");

	host->hba = hba;

	ufshcd_set_variant(hba, host);

	return 0;
}

static int ufs_rockchip_rk3576_init(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct ufs_rockchip_host *host = ufshcd_get_variant(hba);
	int ret;

	hba->quirks = UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING |
		      UFSHCI_QUIRK_DME_RESET_ENABLE_AFTER_HCE;

	/* Enable BKOPS when suspend */
	hba->caps |= UFSHCD_CAP_AUTO_BKOPS_SUSPEND;
	/* Enable putting device into deep sleep */
	hba->caps |= UFSHCD_CAP_DEEPSLEEP;
	/* Enable devfreq of UFS */
	hba->caps |= UFSHCD_CAP_CLK_SCALING;
	/* Enable WriteBooster */
	hba->caps |= UFSHCD_CAP_WB_EN;

	host->pd_id = RK3576_PD_UFS;

	ret = ufs_rockchip_common_init(hba);
	if (ret)
		return dev_err_probe(dev, ret, "ufs common init fail\n");

	return 0;
}

static int ufs_rockchip_device_reset(struct ufs_hba *hba)
{
	struct ufs_rockchip_host *host = ufshcd_get_variant(hba);

	/* Active the reset-gpios */
	gpiod_set_value_cansleep(host->rst_gpio, 1);
	usleep_range(20, 25);

	/* Inactive the reset-gpios */
	gpiod_set_value_cansleep(host->rst_gpio, 0);
	usleep_range(20, 25);

	return 0;
}

static const struct ufs_hba_variant_ops ufs_hba_rk3576_vops = {
	.name = "rk3576",
	.init = ufs_rockchip_rk3576_init,
	.device_reset = ufs_rockchip_device_reset,
	.hce_enable_notify = ufs_rockchip_hce_enable_notify,
	.phy_initialization = ufs_rockchip_rk3576_phy_init,
};

static const struct of_device_id ufs_rockchip_of_match[] = {
	{ .compatible = "rockchip,rk3576-ufshc", .data = &ufs_hba_rk3576_vops},
	{},
};
MODULE_DEVICE_TABLE(of, ufs_rockchip_of_match);

static int ufs_rockchip_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct ufs_hba_variant_ops *vops;
	struct ufs_hba *hba;
	int err;

	vops = device_get_match_data(dev);
	if (!vops)
		return dev_err_probe(dev, -EINVAL, "ufs_hba_variant_ops not defined.\n");

	err = ufshcd_pltfrm_init(pdev, vops);
	if (err)
		return dev_err_probe(dev, err, "ufshcd_pltfrm_init failed\n");

	hba = platform_get_drvdata(pdev);
	/* Set the default desired pm level in case no users set via sysfs */
	ufs_rockchip_set_pm_lvl(hba);

	return 0;
}

static void ufs_rockchip_remove(struct platform_device *pdev)
{
	struct ufs_hba *hba = platform_get_drvdata(pdev);
	struct ufs_rockchip_host *host = ufshcd_get_variant(hba);

	pm_runtime_forbid(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);
	ufshcd_remove(hba);
	ufshcd_dealloc_host(hba);
	clk_disable_unprepare(host->ref_out_clk);
}

static int ufs_rockchip_runtime_suspend(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct ufs_rockchip_host *host = ufshcd_get_variant(hba);
	struct generic_pm_domain *genpd = pd_to_genpd(dev->pm_domain);

	clk_disable_unprepare(host->ref_out_clk);

	/*
	 * Shouldn't power down if rpm_lvl is less than level 5.
	 * This flag will be passed down to platform power-domain driver
	 * which has the final decision.
	 */
	if (hba->rpm_lvl < UFS_PM_LVL_5)
		genpd->flags |= GENPD_FLAG_RPM_ALWAYS_ON;
	else
		genpd->flags &= ~GENPD_FLAG_RPM_ALWAYS_ON;

	return ufshcd_runtime_suspend(dev);
}

static int ufs_rockchip_runtime_resume(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct ufs_rockchip_host *host = ufshcd_get_variant(hba);
	int err;

	err = clk_prepare_enable(host->ref_out_clk);
	if (err) {
		dev_err(hba->dev, "failed to enable ref out clock %d\n", err);
		return err;
	}

	reset_control_assert(host->rst);
	usleep_range(1, 2);
	reset_control_deassert(host->rst);

	return ufshcd_runtime_resume(dev);
}

static int ufs_rockchip_system_suspend(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct ufs_rockchip_host *host = ufshcd_get_variant(hba);

	/* Pass down desired spm_lvl to Firmware */
	arm_smccc_smc(ROCKCHIP_SIP_SUSPEND_MODE, ROCKCHIP_SLEEP_PD_CONFIG,
			host->pd_id, hba->spm_lvl < 5 ? 1 : 0, 0, 0, 0, 0, NULL);

	return ufshcd_system_suspend(dev);
}

static const struct dev_pm_ops ufs_rockchip_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ufs_rockchip_system_suspend, ufshcd_system_resume)
	SET_RUNTIME_PM_OPS(ufs_rockchip_runtime_suspend, ufs_rockchip_runtime_resume, NULL)
	.prepare	 = ufshcd_suspend_prepare,
	.complete	 = ufshcd_resume_complete,
};

static struct platform_driver ufs_rockchip_pltform = {
	.probe = ufs_rockchip_probe,
	.remove = ufs_rockchip_remove,
	.driver = {
		.name = "ufshcd-rockchip",
		.pm = &ufs_rockchip_pm_ops,
		.of_match_table = ufs_rockchip_of_match,
	},
};
module_platform_driver(ufs_rockchip_pltform);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rockchip UFS Host Driver");
