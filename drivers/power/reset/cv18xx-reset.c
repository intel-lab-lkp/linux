// SPDX-License-Identifier: GPL-2.0-only

#include <linux/io.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

/* RTCSYS_CTRL registers */
#define RTC_CTRL_UNLOCKKEY		0x04
#define RTC_CTRL0			0x08
#define  REQ_PWR_CYC			BIT(3)
#define  REQ_WARM_RST			BIT(4)

/* RTCSYS_CORE registers */
#define RTC_EN_PWR_CYC_REQ		0xC8
#define RTC_EN_WARM_RST_REQ		0xCC

static struct regmap *rtcsys_ctrl_regs;
static struct regmap *rtcsys_core_regs;

static int cv18xx_restart_handler(struct sys_off_data *data)
{
	u32 reg_en = RTC_EN_WARM_RST_REQ;
	u32 request = 0xFFFF0800;

	if (data->mode == REBOOT_COLD) {
		reg_en = RTC_EN_PWR_CYC_REQ;
		request |= REQ_PWR_CYC;
	} else {
		request |= REQ_WARM_RST;
	}

	/* Enable reset request */
	regmap_write(rtcsys_core_regs, reg_en, 1);
	/* Enable CTRL0 register access */
	regmap_write(rtcsys_ctrl_regs, RTC_CTRL_UNLOCKKEY, 0xAB18);
	/* Request reset */
	regmap_write(rtcsys_ctrl_regs, RTC_CTRL0, request);

	return NOTIFY_DONE;
}

static int cv18xx_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	if (!np)
		return -ENODEV;

	rtcsys_ctrl_regs = syscon_regmap_lookup_by_phandle(np, "sophgo,rtcsys-ctrl");
	if (IS_ERR(rtcsys_ctrl_regs))
		return dev_err_probe(dev, PTR_ERR(rtcsys_ctrl_regs),
				     "sophgo,rtcsys-ctrl lookup failed\n");

	rtcsys_core_regs = syscon_regmap_lookup_by_phandle(np, "sophgo,rtcsys-core");
	if (IS_ERR(rtcsys_core_regs))
		return dev_err_probe(dev, PTR_ERR(rtcsys_core_regs),
				     "sophgo,rtcsys-core lookup failed\n");

	ret = devm_register_restart_handler(&pdev->dev, cv18xx_restart_handler, NULL);
	if (ret)
		dev_err(&pdev->dev, "Cannot register restart handler (%pe)\n", ERR_PTR(ret));
	return ret;
}

static const struct of_device_id cv18xx_reset_of_match[] = {
	{ .compatible = "sophgo,cv1800-reset" },
	{}
};
MODULE_DEVICE_TABLE(platform, cv18xx_reset_of_match);

static struct platform_driver cv18xx_reset_driver = {
	.probe = cv18xx_reset_probe,
	.driver = {
		.name = "cv18xx-reset",
		.of_match_table = cv18xx_reset_of_match,
	},
};
module_platform_driver(cv18xx_reset_driver);

MODULE_AUTHOR("Alexander Sverdlin <alexander.sverdlin@gmail.com>");
MODULE_DESCRIPTION("Cvitek CV18xx/Sophgo SG2000 Reset Driver");
MODULE_ALIAS("platform:cv18xx-reset");
