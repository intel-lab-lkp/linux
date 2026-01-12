// SPDX-License-Identifier: GPL-2.0
/*
 * QEMU Virt Machine System Controller Driver
 *
 * Copyright (C) 2026 Kuan-Wei Chiu <visitorckw@gmail.com>
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

/* Registers */
#define VIRT_CTRL_REG_FEATURES	0x00
#define VIRT_CTRL_REG_CMD	0x04

/* Commands */
#define CMD_NOOP	0
#define CMD_RESET	1
#define CMD_HALT	2
#define CMD_PANIC	3

struct qemu_virt_ctrl {
	void __iomem *base;
	struct notifier_block restart_nb;
};

static void __iomem *qemu_virt_ctrl_base;

static void qemu_virt_ctrl_power_off(void)
{
	if (qemu_virt_ctrl_base)
		iowrite32be(CMD_HALT, qemu_virt_ctrl_base + VIRT_CTRL_REG_CMD);
}

static int qemu_virt_ctrl_restart(struct notifier_block *nb, unsigned long action,
				  void *data)
{
	struct qemu_virt_ctrl *vc = container_of(nb, struct qemu_virt_ctrl, restart_nb);

	iowrite32be(CMD_RESET, vc->base + VIRT_CTRL_REG_CMD);

	return NOTIFY_DONE;
}

static int qemu_virt_ctrl_probe(struct platform_device *pdev)
{
	struct qemu_virt_ctrl *vc;
	int ret;

	vc = devm_kzalloc(&pdev->dev, sizeof(*vc), GFP_KERNEL);
	if (!vc)
		return -ENOMEM;

	vc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vc->base))
		return PTR_ERR(vc->base);

	qemu_virt_ctrl_base = vc->base;

	vc->restart_nb.notifier_call = qemu_virt_ctrl_restart;
	vc->restart_nb.priority = 128;

	ret = register_restart_handler(&vc->restart_nb);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register restart handler\n");

	if (!pm_power_off)
		pm_power_off = qemu_virt_ctrl_power_off;

	platform_set_drvdata(pdev, vc);

	return 0;
}

static void qemu_virt_ctrl_remove(struct platform_device *pdev)
{
	struct qemu_virt_ctrl *vc = platform_get_drvdata(pdev);

	unregister_restart_handler(&vc->restart_nb);

	if (pm_power_off == qemu_virt_ctrl_power_off)
		pm_power_off = NULL;
}

static struct platform_driver qemu_virt_ctrl_driver = {
	.probe = qemu_virt_ctrl_probe,
	.remove = qemu_virt_ctrl_remove,
	.driver = {
		.name = "qemu-virt-ctrl",
	},
};
module_platform_driver(qemu_virt_ctrl_driver);

MODULE_AUTHOR("Kuan-Wei Chiu <visitorckw@gmail.com>");
MODULE_DESCRIPTION("QEMU Virt Machine System Controller Driver");
MODULE_LICENSE("GPL");
