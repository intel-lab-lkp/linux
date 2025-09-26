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

#define SC2730_SOFT_RST_HW	0x1824
#define SC2730_RST_STATUS	0x1bac
#define SC2730_SWRST_CTRL0	0x1bf8

#define SC2731_PWR_PD_HW	0xc2c
#define SC2731_SLP_CTRL		0xdf0
#define SC2731_LDO_XTL_EN	BIT(3)

#define SC27XX_PWR_OFF_EN	BIT(0)
#define SC27XX_SOFT_RST_EN	BIT(4)
#define SC27XX_RESET		BIT(0)

#define HWRST_STATUS_SECURITY		0x02
#define HWRST_STATUS_RECOVERY		0x20
#define HWRST_STATUS_NORMAL		0x40
#define HWRST_STATUS_ALARM		0x50
#define HWRST_STATUS_SLEEP		0x60
#define HWRST_STATUS_FASTBOOT		0x30
#define HWRST_STATUS_SPECIAL		0x70
#define HWRST_STATUS_PANIC		0x80
#define HWRST_STATUS_CFTREBOOT		0x90
#define HWRST_STATUS_AUTODLOADER	0xa0
#define HWRST_STATUS_IQMODE		0xb0
#define HWRST_STATUS_SPRDISK		0xc0
#define HWRST_STATUS_FACTORYTEST	0xe0
#define HWRST_STATUS_WATCHDOG		0xf0
#define HWRST_STATUS_MASK		0xf0

struct sc27xx_poweroff_reg_info {
	u32 poweroff_reg;
	u32 slp_ctrl_reg;
	u32 ldo_xtl_en;

	u32 reset_reg;
	u32 rst_sts_reg;
	u32 swrst_ctrl_reg;
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

static int sc27xx_restart(struct sys_off_data *off_data)
{
	struct sc27xx_poweroff_data *data = off_data->cb_data;
	u32 reboot_mode = 0;

	if (!off_data->cmd)
		reboot_mode = HWRST_STATUS_NORMAL;
	else if (!strcmp(off_data->cmd, "recovery"))
		reboot_mode = HWRST_STATUS_RECOVERY;
	else if (!strcmp(off_data->cmd, "alarm"))
		reboot_mode = HWRST_STATUS_ALARM;
	else if (!strcmp(off_data->cmd, "fastsleep"))
		reboot_mode = HWRST_STATUS_SLEEP;
	else if (!strcmp(off_data->cmd, "bootloader"))
		reboot_mode = HWRST_STATUS_FASTBOOT;
	else if (!strcmp(off_data->cmd, "panic"))
		reboot_mode = HWRST_STATUS_PANIC;
	else if (!strcmp(off_data->cmd, "special"))
		reboot_mode = HWRST_STATUS_SPECIAL;
	else if (!strcmp(off_data->cmd, "cftreboot"))
		reboot_mode = HWRST_STATUS_CFTREBOOT;
	else if (!strcmp(off_data->cmd, "autodloader"))
		reboot_mode = HWRST_STATUS_AUTODLOADER;
	else if (!strcmp(off_data->cmd, "iqmode"))
		reboot_mode = HWRST_STATUS_IQMODE;
	else if (!strcmp(off_data->cmd, "sprdisk"))
		reboot_mode = HWRST_STATUS_SPRDISK;
	else if (!strcmp(off_data->cmd, "tospanic"))
		reboot_mode = HWRST_STATUS_SECURITY;
	else if (!strcmp(off_data->cmd, "factorytest"))
		reboot_mode = HWRST_STATUS_FACTORYTEST;
	else
		reboot_mode = HWRST_STATUS_NORMAL;

	regmap_update_bits(data->regmap, data->regs->rst_sts_reg,
			   HWRST_STATUS_MASK, reboot_mode);

	regmap_set_bits(data->regmap, data->regs->swrst_ctrl_reg,
			SC27XX_SOFT_RST_EN);

	regmap_write(data->regmap, data->regs->reset_reg, SC27XX_RESET);

	mdelay(1000);

	pr_emerg("Unable to restart system\n");

	return NOTIFY_DONE;
}

static int sc27xx_poweroff_probe(struct platform_device *pdev)
{
	struct sc27xx_poweroff_data *data;
	int ret;

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

	if (data->regs->reset_reg) {
		ret = devm_register_sys_off_handler(&pdev->dev,
						    SYS_OFF_MODE_RESTART,
						    192, sc27xx_restart,
						    data);
		if (ret)
			return ret;
	}

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

	.reset_reg = SC2730_SOFT_RST_HW,
	.rst_sts_reg = SC2730_RST_STATUS,
	.swrst_ctrl_reg = SC2730_SWRST_CTRL0,
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
