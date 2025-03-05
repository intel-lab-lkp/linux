/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, NXP
 */

#ifndef OX05B1S_H
#define OX05B1S_H

#include <linux/types.h>

struct ox05b1s_reg {
	u32 addr;
	u32 data;
};

struct ox05b1s_reglist {
	const struct ox05b1s_reg *regs;
};

extern const struct ox05b1s_reglist ox05b1s_reglist_2592x1944[];

#endif /* OX05B1S_H */
