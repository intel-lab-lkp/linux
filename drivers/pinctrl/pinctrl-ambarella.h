/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Ambarella pinctrl SoC data
 *
 * Copyright (C) 2012-2026, Ambarella, Inc.
 */

#ifndef _PINCTRL_AMBARELLA_H
#define _PINCTRL_AMBARELLA_H

#include <linux/types.h>

#include <linux/pinctrl/pinctrl.h>

#define AMBA_MAX_BANKS			8

#define AMBA_PINMUX(pin, alt)		(((alt) << 12) | (pin))
#define AMBA_PINMUX_TO_PIN(mux)		((mux) & 0xfff)
#define AMBA_PINMUX_TO_ALT(mux)		(((mux) >> 12) & 0x7)

struct amb_pinmux_group {
	const char *name;
	const u32 *mux;
	unsigned int nmux;
};

struct amb_pinctrl_data {
	const struct amb_pinmux_group *groups;
	const struct pinfunction *functions;
	unsigned int ngroups;
	unsigned int nfunctions;
	unsigned int nr_banks;
	unsigned int npins;
	unsigned int ds0[AMBA_MAX_BANKS];
	unsigned int ds1[AMBA_MAX_BANKS];
	unsigned int ds2[AMBA_MAX_BANKS];
	unsigned int pull_en[AMBA_MAX_BANKS];
	unsigned int pull_dir[AMBA_MAX_BANKS];
	bool have_ds2;
};

extern const struct amb_pinctrl_data ambarella_cv75_pinctrl_data;

#endif /* _PINCTRL_AMBARELLA_H */
