// SPDX-License-Identifier: GPL-2.0+
/*
 * AMD x86 platform driver for AMD HW platforms using ISP4
 *
 * Copyright 2025 Advanced Micro Devices, Inc.
 */

#include <linux/acpi.h>
#include <linux/gpio/machine.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/kernel.h>

#define AMDISP_I2C_BUS		99

#define AMDISP_ACPI_CAM_HID     "OMNI5C10"

static struct gpiod_lookup_table isp_gpio_table = {
	.dev_id = "amd_isp_capture",
	.table = {
		GPIO_LOOKUP("AMDI0030:00", 85, "enable_isp", GPIO_ACTIVE_HIGH),
		{ }
	},
};

static struct gpiod_lookup_table isp_sensor_gpio_table = {
	.dev_id = "ov05c",
	.table = {
		GPIO_LOOKUP("amdisp-pinctrl", 0, "sensor0_enable", GPIO_ACTIVE_HIGH),
		{ }
	},
};

static struct i2c_board_info sensor_info = {
	.dev_name = "ov05c",
	I2C_BOARD_INFO("ov05c", 0x10),
};

static int __init amd_isp_init(void)
{
	int ret;

	/* check for valid platform before configuring isp4 board resources */
	if (!acpi_dev_found(AMDISP_ACPI_CAM_HID))
		return -ENODEV;

	gpiod_add_lookup_table(&isp_gpio_table);
	gpiod_add_lookup_table(&isp_sensor_gpio_table);

	ret = i2c_register_board_info(AMDISP_I2C_BUS, &sensor_info, 1);
	if (ret)
		pr_err("%s: cannot register i2c board devices:%s",
		       __func__, sensor_info.dev_name);

	return ret;
}

module_init(amd_isp_init);

MODULE_AUTHOR("Benjamin Chan <benjamin.chan@amd.com>");
MODULE_AUTHOR("Pratap Nirujogi <pratap.nirujogi@amd.com>");
MODULE_DESCRIPTION("AMD ISP4 Platform parameters");
MODULE_LICENSE("GPL v2");
