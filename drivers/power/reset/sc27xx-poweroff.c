// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Spreadtrum Communications Inc.
 * Copyright (C) 2018 Linaro Ltd.
 */

#include <linux/cpu.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/syscore_ops.h>

#define SC2730_PWR_PD_HW	0x1820
#define SC2730_SLP_CTRL		0x1a48
#define SC2730_LDO_XTL_EN	BIT(2)

#define SC2731_PWR_PD_HW	0xc2c
#define SC2731_SLP_CTRL		0xdf0
#define SC2731_LDO_XTL_EN	BIT(3)

#define SC27XX_PWR_OFF_EN	BIT(0)

struct sc27xx_poweroff_reg_info {
	u32 poweroff_reg;
	u32 slp_ctrl_reg;
	u32 ldo_xtl_en;
};

struct sc27xx_poweroff_data {
	struct regmap *regmap;
	const struct sc27xx_poweroff_reg_info *regs;
};

/*
 * On Spreadtrum platform, we need power off system through external SC27xx
 * series PMICs, and it is one similar SPI bus mapped by regmap to access PMIC,
 * which is not fast io access.
 *
 * So before stopping other cores, we need release other cores' resource by
 * taking cpus down to avoid racing regmap or spi mutex lock when poweroff
 * system through PMIC.
 */
static void sc27xx_poweroff_shutdown(void)
{
#ifdef CONFIG_HOTPLUG_CPU
	int cpu;

	for_each_online_cpu(cpu) {
		if (cpu != smp_processor_id())
			remove_cpu(cpu);
	}
#endif
}

static struct syscore_ops poweroff_syscore_ops = {
	.shutdown = sc27xx_poweroff_shutdown,
};

static int sc27xx_poweroff_do_poweroff(struct sys_off_data *off_data)
{
	struct sc27xx_poweroff_data *data = off_data->cb_data;

	/* Disable the external subsys connection's power firstly */
	regmap_write(data->regmap, data->regs->slp_ctrl_reg,
		     data->regs->ldo_xtl_en);

	regmap_write(data->regmap, data->regs->poweroff_reg,
		     SC27XX_PWR_OFF_EN);

	mdelay(1000);

	pr_emerg("Unable to poweroff system\n");

	return NOTIFY_DONE;
}

static int sc27xx_poweroff_probe(struct platform_device *pdev)
{
	struct sc27xx_poweroff_data *data;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regs = of_device_get_match_data(&pdev->dev);
	if (!data->regs)
		return -EINVAL;

	data->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!data->regmap)
		return -ENODEV;

	register_syscore_ops(&poweroff_syscore_ops);

	return devm_register_sys_off_handler(&pdev->dev,
					     SYS_OFF_MODE_POWER_OFF,
					     SYS_OFF_PRIO_DEFAULT,
					     sc27xx_poweroff_do_poweroff,
					     data);
}

static const struct sc27xx_poweroff_reg_info sc2730_pwr_regs = {
	.poweroff_reg = SC2730_PWR_PD_HW,
	.slp_ctrl_reg = SC2730_SLP_CTRL,
	.ldo_xtl_en = SC2730_LDO_XTL_EN,
};

static const struct sc27xx_poweroff_reg_info sc2731_pwr_regs = {
	.poweroff_reg = SC2731_PWR_PD_HW,
	.slp_ctrl_reg = SC2731_SLP_CTRL,
	.ldo_xtl_en = SC2731_LDO_XTL_EN,
};

static const struct of_device_id sc27xx_poweroff_of_match[] = {
	{ .compatible = "sprd,sc2730-poweroff", .data = &sc2730_pwr_regs },
	{ .compatible = "sprd,sc2731-poweroff", .data = &sc2731_pwr_regs },
	{ }
};
MODULE_DEVICE_TABLE(of, sc27xx_poweroff_of_match);

static struct platform_driver sc27xx_poweroff_driver = {
	.probe = sc27xx_poweroff_probe,
	.driver = {
		.name = "sc27xx-poweroff",
		.of_match_table = sc27xx_poweroff_of_match,
	},
};
module_platform_driver(sc27xx_poweroff_driver);

MODULE_DESCRIPTION("Power off driver for SC27XX PMIC Device");
MODULE_AUTHOR("Baolin Wang <baolin.wang@unisoc.com>");
MODULE_LICENSE("GPL v2");
