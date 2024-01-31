// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instrument's System Control Interface (TI-SCI) reboot driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 *	Andrew Davis <afd@ti.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#include <linux/soc/ti/ti_sci_protocol.h>

static int ti_sci_reboot_handler(struct sys_off_data *data)
{
	const struct ti_sci_handle *sci = data->cb_data;
	const struct ti_sci_core_ops *core_ops = &sci->ops.core_ops;

	core_ops->reboot_device(sci);

	return NOTIFY_DONE;
}

static int ti_sci_reboot_probe(struct platform_device *pdev)
{
	const struct ti_sci_handle *sci;
	int err;

	sci = devm_ti_sci_get_handle(&pdev->dev);
	if (IS_ERR(sci))
		return PTR_ERR(sci);

	err = devm_register_sys_off_handler(&pdev->dev,
					    SYS_OFF_MODE_RESTART,
					    SYS_OFF_PRIO_LOW,
					    ti_sci_reboot_handler,
					    (void *)sci);
	if (err)
		return dev_err_probe(&pdev->dev, err, "Cannot register restart handler\n");

	return 0;
}

static const struct of_device_id ti_sci_reboot_of_match[] = {
	{ .compatible = "ti,sci-reboot", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ti_sci_reboot_of_match);

static struct platform_driver ti_sci_reboot_driver = {
	.probe = ti_sci_reboot_probe,
	.driver = {
		.name = "ti-sci-reboot",
		.of_match_table = ti_sci_reboot_of_match,
	},
};
module_platform_driver(ti_sci_reboot_driver);

MODULE_AUTHOR("Andrew Davis <afd@ti.com>");
MODULE_DESCRIPTION("TI System Control Interface (TI SCI) Reboot driver");
MODULE_LICENSE("GPL");
