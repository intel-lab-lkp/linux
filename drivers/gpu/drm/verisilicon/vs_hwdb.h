/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Icenowy Zheng <uwu@icenowy.me>
 */

#ifndef _VS_HWDB_H_
#define _VS_HWDB_H_

#include <linux/regmap.h>
#include <linux/types.h>

struct vs_formats {
	const u32 *array;
	unsigned int num;
};

enum vs_dc_family {
	VS_DC_FAMILY_DC8000,
	VS_DC_FAMILY_DCULTRA_LITE,
};

/**
 * struct vs_dc_info - per-SoC DC platform data
 * @family:		DC IP family (DC8000, DCUltra Lite, etc.)
 * @display_count:	number of display outputs (0 = auto-detect from DT/HW)
 * @has_chip_id:	whether chip identity registers exist
 * @has_config_ex:	whether CONFIG_EX commit mechanism exists
 * @regmap_cfg:		regmap configuration for this variant
 * @formats:		supported pixel formats (NULL = auto-detect from chip ID)
 */
struct vs_dc_info {
	enum vs_dc_family family;
	u32 display_count;
	bool has_chip_id;
	bool has_config_ex;
	const struct regmap_config *regmap_cfg;
	const struct vs_formats *formats;
};

struct vs_chip_identity {
	u32 model;
	u32 revision;
	u32 customer_id;

	u32 display_count;
	const struct vs_formats *formats;
};

extern const struct vs_formats vs_formats_no_yuv444;

int vs_fill_chip_identity(struct regmap *regs,
			  struct vs_chip_identity *ident);

#endif /* _VS_HWDB_H_ */
