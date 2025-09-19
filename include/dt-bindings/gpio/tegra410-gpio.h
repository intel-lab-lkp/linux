/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved. */

/*
 * This header provides constants for the nvidia,tegra410-gpio DT binding.
 *
 * The first cell in Tegra's GPIO specifier is the GPIO ID. The macros below
 * provide names for this.
 *
 * The second cell contains standard flag values specified in gpio.h.
 */

#ifndef _DT_BINDINGS_GPIO_TEGRA410_GPIO_H
#define _DT_BINDINGS_GPIO_TEGRA410_GPIO_H

#include <dt-bindings/gpio/gpio.h>

/* GPIOs implemented by main GPIO controller */
#define TEGRA410_MAIN_GPIO_PORT_A	0
#define TEGRA410_MAIN_GPIO_PORT_B	1
#define TEGRA410_MAIN_GPIO_PORT_C	2
#define TEGRA410_MAIN_GPIO_PORT_D	3
#define TEGRA410_MAIN_GPIO_PORT_E	4
#define TEGRA410_MAIN_GPIO_PORT_I	5
#define TEGRA410_MAIN_GPIO_PORT_J	6
#define TEGRA410_MAIN_GPIO_PORT_K	7
#define TEGRA410_MAIN_GPIO_PORT_L	8
#define TEGRA410_MAIN_GPIO_PORT_M	9
#define TEGRA410_MAIN_GPIO_PORT_N	10
#define TEGRA410_MAIN_GPIO_PORT_P	11
#define TEGRA410_MAIN_GPIO_PORT_Q	12
#define TEGRA410_MAIN_GPIO_PORT_R	13

#define TEGRA410_MAIN_GPIO(port, offset) \
	((TEGRA410_MAIN_GPIO_PORT_##port * 8) + (offset))

#endif
