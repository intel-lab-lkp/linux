// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 UltraRISC Technology (Shanghai) Co., Ltd.
 *
 * Author: Jia Wang <wangjia@ultrarisc.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "pinctrl-ultrarisc.h"

static const struct pinctrl_pin_desc ur_dp1000_pins[] = {
	PINCTRL_PIN(0, "PA0"),
	PINCTRL_PIN(1, "PA1"),
	PINCTRL_PIN(2, "PA2"),
	PINCTRL_PIN(3, "PA3"),
	PINCTRL_PIN(4, "PA4"),
	PINCTRL_PIN(5, "PA5"),
	PINCTRL_PIN(6, "PA6"),
	PINCTRL_PIN(7, "PA7"),
	PINCTRL_PIN(8, "PA8"),
	PINCTRL_PIN(9, "PA9"),
	PINCTRL_PIN(10, "PA10"),
	PINCTRL_PIN(11, "PA11"),
	PINCTRL_PIN(12, "PA12"),
	PINCTRL_PIN(13, "PA13"),
	PINCTRL_PIN(14, "PA14"),
	PINCTRL_PIN(15, "PA15"),
	PINCTRL_PIN(16, "PB0"),
	PINCTRL_PIN(17, "PB1"),
	PINCTRL_PIN(18, "PB2"),
	PINCTRL_PIN(19, "PB3"),
	PINCTRL_PIN(20, "PB4"),
	PINCTRL_PIN(21, "PB5"),
	PINCTRL_PIN(22, "PB6"),
	PINCTRL_PIN(23, "PB7"),
	PINCTRL_PIN(24, "PC0"),
	PINCTRL_PIN(25, "PC1"),
	PINCTRL_PIN(26, "PC2"),
	PINCTRL_PIN(27, "PC3"),
	PINCTRL_PIN(28, "PC4"),
	PINCTRL_PIN(29, "PC5"),
	PINCTRL_PIN(30, "PC6"),
	PINCTRL_PIN(31, "PC7"),
	PINCTRL_PIN(32, "PD0"),
	PINCTRL_PIN(33, "PD1"),
	PINCTRL_PIN(34, "PD2"),
	PINCTRL_PIN(35, "PD3"),
	PINCTRL_PIN(36, "PD4"),
	PINCTRL_PIN(37, "PD5"),
	PINCTRL_PIN(38, "PD6"),
	PINCTRL_PIN(39, "PD7"),
	PINCTRL_PIN(40, "LPC0"),
	PINCTRL_PIN(41, "LPC1"),
	PINCTRL_PIN(42, "LPC2"),
	PINCTRL_PIN(43, "LPC3"),
	PINCTRL_PIN(44, "LPC4"),
	PINCTRL_PIN(45, "LPC5"),
	PINCTRL_PIN(46, "LPC6"),
	PINCTRL_PIN(47, "LPC7"),
	PINCTRL_PIN(48, "LPC8"),
	PINCTRL_PIN(49, "LPC9"),
	PINCTRL_PIN(50, "LPC10"),
	PINCTRL_PIN(51, "LPC11"),
	PINCTRL_PIN(52, "LPC12"),
};

static const struct ur_function_desc ur_dp1000_functions[] = {
	{ "gpio", UR_FUNC_DEF, true },
	{ "func0", UR_FUNC0, false },
	{ "func1", UR_FUNC1, false },
};

#define UR_DP1000_PORT(_name, _npins, _func, _conf, _modes) \
	{ .name = (_name), .npins = (_npins), .func_offset = (_func), \
	  .conf_offset = (_conf), .supported_modes = (_modes) }

static const struct ur_pinctrl_match_data ur_dp1000_match_data = {
	.pins = ur_dp1000_pins,
	.npins = ARRAY_SIZE(ur_dp1000_pins),
	.functions = ur_dp1000_functions,
	.num_functions = ARRAY_SIZE(ur_dp1000_functions),
	.num_ports = 5,
	.ports = {
		UR_DP1000_PORT("A", 16, 0x2c0, 0x310, UR_FUNC0 | UR_FUNC1),
		UR_DP1000_PORT("B", 8, 0x2c4, 0x318, UR_FUNC0 | UR_FUNC1),
		UR_DP1000_PORT("C", 8, 0x2c8, 0x31c, UR_FUNC0 | UR_FUNC1),
		UR_DP1000_PORT("D", 8, 0x2cc, 0x320, UR_FUNC0 | UR_FUNC1),
		UR_DP1000_PORT("LPC", 13, 0x2d0, 0x324, UR_FUNC0),
	},
};

static const struct of_device_id ur_pinctrl_of_match[] = {
	{ .compatible = "ultrarisc,dp1000-pinctrl", .data = &ur_dp1000_match_data, },
	{ }
};
MODULE_DEVICE_TABLE(of, ur_pinctrl_of_match);

static struct platform_driver ur_pinctrl_driver = {
	.driver = {
		.name = "ultrarisc-pinctrl-dp1000",
		.of_match_table = ur_pinctrl_of_match,
	},
	.probe = ur_pinctrl_probe,
};

module_platform_driver(ur_pinctrl_driver);

MODULE_DESCRIPTION("UltraRISC DP1000 pinctrl driver");
MODULE_LICENSE("GPL");
