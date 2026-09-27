// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner B288 (sun8iw10p1) SoC pinctrl driver.
 *
 * Bank sizes and the interrupt mux derived from Allwinner's BSP driver
 * pinctrl-sun8iw10p1.c, which is:
 * Copyright (C) 2014 Jackie Hwang <huangshr@allwinnertech.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pinctrl/pinctrl.h>

#include "pinctrl-sunxi.h"

/* PH0 exists only on the BSP's FPGA platform and is omitted. */
static const u8 b288_nr_bank_pins[SUNXI_PINCTRL_MAX_BANKS] =
/*	  PA  PB  PC  PD  PE  PF  PG */
	{  0, 17, 15, 29,  0,  7, 12 };

/* Port C has no external interrupts. */
static const unsigned int b288_irq_bank_map[] = { 1, 3, 5, 6 };

static const u8 b288_irq_bank_muxes[SUNXI_PINCTRL_MAX_BANKS] =
/*	  PA  PB  PC  PD  PE  PF  PG */
	{  0,  6,  0,  6,  0,  6,  6 };

static struct sunxi_pinctrl_desc b288_pinctrl_data = {
	.irq_banks	= ARRAY_SIZE(b288_irq_bank_map),
	.irq_bank_map	= b288_irq_bank_map,
};

static int b288_pinctrl_probe(struct platform_device *pdev)
{
	return sunxi_pinctrl_dt_table_init(pdev, b288_nr_bank_pins,
					   b288_irq_bank_muxes,
					   &b288_pinctrl_data, 0);
}

static const struct of_device_id b288_pinctrl_match[] = {
	{ .compatible = "allwinner,sun8i-b288-pinctrl", },
	{}
};

static struct platform_driver b288_pinctrl_driver = {
	.probe	= b288_pinctrl_probe,
	.driver	= {
		.name		= "sun8i-b288-pinctrl",
		.of_match_table	= b288_pinctrl_match,
	},
};
builtin_platform_driver(b288_pinctrl_driver);
