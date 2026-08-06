// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ambarella CV75 pinctrl data
 *
 * Copyright (C) 2026, Ambarella, Inc.
 */

#include <linux/kernel.h>

#include "pinctrl-ambarella.h"

static const u32 cv75_uart0tx_pinmux[] = {
	AMBA_PINMUX(44, 1),
};

static const u32 cv75_uart0rx_pinmux[] = {
	AMBA_PINMUX(45, 1),
};

static const struct ambpin_group_desc cv75_pin_groups[] = {
	{
		.name = "uart0tx",
		.pinmux = cv75_uart0tx_pinmux,
		.num_pins = ARRAY_SIZE(cv75_uart0tx_pinmux),
	},
	{
		.name = "uart0rx",
		.pinmux = cv75_uart0rx_pinmux,
		.num_pins = ARRAY_SIZE(cv75_uart0rx_pinmux),
	},
};

static const char * const cv75_uart0_groups[] = {
	"uart0tx",
	"uart0rx",
};

static const struct ambpin_function cv75_pin_functions[] = {
	{
		.name = "uart0",
		.groups = cv75_uart0_groups,
		.num_groups = ARRAY_SIZE(cv75_uart0_groups),
	},
};

const struct amb_pinctrl_data ambarella_cv75_pinctrl_data = {
	.ds0 = {
		0x314, 0x320, 0x32c,
	},
	.ds1 = {
		0x318, 0x324, 0x330,
	},
	.ds2 = {
		0x31c, 0x328, 0x334,
	},
	.pull_en = {
		0x60, 0x64, 0x68,
	},
	.pull_dir = {
		0x7c, 0x80, 0x84,
	},
	.have_ds2 = true,
	.clk_au_dedicated_pin = 96,
	.groups = cv75_pin_groups,
	.nr_groups = ARRAY_SIZE(cv75_pin_groups),
	.functions = cv75_pin_functions,
	.nr_functions = ARRAY_SIZE(cv75_pin_functions),
};
