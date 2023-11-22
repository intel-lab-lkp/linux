/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2017 BayLibre, SAS
 * Copyright (C) 2023 JetHome
 *
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 * Author: Viacheslav Bocharov <adeep@lexina.in>
 */

#ifndef _MESON_GX_SOCINFO_INTERNAL_H_
#define _MESON_GX_SOCINFO_INTERNAL_H_

#include <linux/bitfield.h>

#define AO_SEC_SD_CFG8		0xe0
#define AO_SEC_SOCINFO_OFFSET	AO_SEC_SD_CFG8

#define SOCINFO_MAJOR	GENMASK(31, 24)
#define SOCINFO_PACK	GENMASK(23, 16)
#define SOCINFO_MINOR	GENMASK(15, 8)
#define SOCINFO_MISC	GENMASK(7, 0)

static const struct meson_gx_soc_id {
	const char *name;
	unsigned int id;
} soc_ids[] = {
	{ "GXBB", 0x1f },
	{ "GXTVBB", 0x20 },
	{ "GXL", 0x21 },
	{ "GXM", 0x22 },
	{ "TXL", 0x23 },
	{ "TXLX", 0x24 },
	{ "AXG", 0x25 },
	{ "GXLX", 0x26 },
	{ "TXHD", 0x27 },
	{ "G12A", 0x28 },
	{ "G12B", 0x29 },
	{ "SM1", 0x2b },
	{ "A1", 0x2c },
};


static const struct meson_gx_package_id {
	const char *name;
	unsigned int major_id;
	unsigned int pack_id;
	unsigned int pack_mask;
} soc_packages[] = {
	{ "S905", 0x1f, 0, 0x20 }, /* pack_id != 0x20 */
	{ "S905H", 0x1f, 0x3, 0xf }, /* pack_id & 0xf == 0x3 */
	{ "S905M", 0x1f, 0x20, 0xf0 }, /* pack_id == 0x20 */
	{ "S905D", 0x21, 0, 0xf0 },
	{ "S905X", 0x21, 0x80, 0xf0 },
	{ "S905W", 0x21, 0xa0, 0xf0 },
	{ "S905L", 0x21, 0xc0, 0xf0 },
	{ "S905M2", 0x21, 0xe0, 0xf0 },
	{ "S805X", 0x21, 0x30, 0xf0 },
	{ "S805Y", 0x21, 0xb0, 0xf0 },
	{ "S912", 0x22, 0, 0x0 }, /* Only S912 is known for GXM */
	{ "962X", 0x24, 0x10, 0xf0 },
	{ "962E", 0x24, 0x20, 0xf0 },
	{ "A113X", 0x25, 0x37, 0xff },
	{ "A113X", 0x25, 0x43, 0xff },
	{ "A113D", 0x25, 0x22, 0xff },
	{ "S905D2", 0x28, 0x10, 0xf0 },
	{ "S905Y2", 0x28, 0x30, 0xf0 },
	{ "S905X2", 0x28, 0x40, 0xf0 },
	{ "A311D", 0x29, 0x10, 0xf0 },
	{ "S922X", 0x29, 0x40, 0xf0 },
	{ "S905D3", 0x2b, 0x4, 0xf5 },
	{ "S905X3", 0x2b, 0x5, 0xf5 },
	{ "S905X3", 0x2b, 0x10, 0x3f },
	{ "S905D3", 0x2b, 0x30, 0x3f },
	{ "A113L", 0x2c, 0x0, 0xf8 },
};


static inline unsigned int socinfo_to_major(u32 socinfo)
{
	return FIELD_GET(SOCINFO_MAJOR, socinfo);
}

static inline unsigned int socinfo_to_minor(u32 socinfo)
{
	return FIELD_GET(SOCINFO_MINOR, socinfo);
}

static inline unsigned int socinfo_to_pack(u32 socinfo)
{
	return FIELD_GET(SOCINFO_PACK, socinfo);
}

static inline unsigned int socinfo_to_misc(u32 socinfo)
{
	return FIELD_GET(SOCINFO_MISC, socinfo);
}

#endif /* _MESON_GX_SOCINFO_INTERNAL_H_ */

