/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Hardware identification
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_ID_H
#define MALI_C720_ID_H

#include <linux/types.h>

#define MALI_C720_ID_SIZE 0x1000

struct mali_c720;

enum mali_c720_version_id {
	MALI_C720_VERSION_ID_R0_P0,
	MALI_C720_VERSION_ID_R0_P1,
	MALI_C720_VERSION_ID_MAX,
};

struct mali_c720_hw_version {
	const char *name;
	enum mali_c720_version_id id;
	u16 part;
	u8 revision;
	u8 revand;
	u8 cmod;
};

int mali_c720_identify_hw(struct mali_c720 *mali_c720);

#endif /* MALI_C720_ID_H */
