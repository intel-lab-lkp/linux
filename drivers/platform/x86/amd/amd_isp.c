/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/gpio/machine.h>

#define AMDISP_I2C_BUS		99

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

	gpiod_add_lookup_table(&isp_gpio_table);
	gpiod_add_lookup_table(&isp_sensor_gpio_table);

	ret = i2c_register_board_info(AMDISP_I2C_BUS, &sensor_info, 1);
	if (ret)
		pr_err("%s: cannot register i2c board devices:%s",
		       __func__, sensor_info.dev_name);

	return ret;
}

arch_initcall(amd_isp_init);

MODULE_AUTHOR("Benjamin Chan <benjamin.chan@amd.com>");
MODULE_AUTHOR("Pratap Nirujogi <pratap.nirujogi@amd.com>");
MODULE_DESCRIPTION("AMD ISP Platform parameters");
MODULE_LICENSE("GPL and additional rights");
