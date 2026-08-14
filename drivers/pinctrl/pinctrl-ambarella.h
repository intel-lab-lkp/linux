/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Ambarella pinctrl common data definitions
 *
 * Copyright (C) 2012-2026, Ambarella, Inc.
 */

#ifndef _PINCTRL_AMBARELLA_H
#define _PINCTRL_AMBARELLA_H

#include <linux/types.h>

#define AMBA_MAX_BANKS			8

#define AMBA_PINMUX(pin, alt)		(((alt) << 12) | (pin))
#define AMBA_PINMUX_TO_PIN(mux)		((mux) & 0xfff)
#define AMBA_PINMUX_TO_ALT(mux)		(((mux) >> 12) & 0xf)

struct ambpin_group_desc {
	const char		*name;
	const u32		*pinmux;
	unsigned int		num_pins;
};

struct ambpin_function {
	const char		*name;
	const char * const	*groups;
	unsigned int		num_groups;
};

struct amb_pinctrl_data {
	unsigned int		ds0[AMBA_MAX_BANKS];
	unsigned int		ds1[AMBA_MAX_BANKS];
	unsigned int		ds2[AMBA_MAX_BANKS];
	unsigned int		pull_en[AMBA_MAX_BANKS];
	unsigned int		pull_dir[AMBA_MAX_BANKS];
	bool			have_ds2;
	u32			hsm_domain_id;
	u32			clk_au_dedicated_pin;
	unsigned int		nr_banks;
	unsigned int		npins;
	const struct ambpin_group_desc *groups;
	unsigned int		nr_groups;
	const struct ambpin_function *functions;
	unsigned int		nr_functions;
};

extern const struct amb_pinctrl_data ambarella_cv75_pinctrl_data;

#endif /* _PINCTRL_AMBARELLA_H */
