/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Backlight driver for Maxim MAX25014
 *
 * Copyright (C) 2025 GOcontroll B.V.
 * Author: Maud Spierings <maudspierings@gocontroll.com>
 */

#ifndef _MAX25014_H
#define _MAX25014_H

/**
 * struct max25014_platform_data
 * @initial_brightness : Initial value of the backlight brightness.
 * @iset : Value of the iset field which scales the amperage/limits it.
 * @strings : Which, out of four, led strings are in use.
 */
struct max25014_platform_data {
	uint32_t initial_brightness;
	uint32_t iset;
	uint32_t strings[4];
};

#endif
