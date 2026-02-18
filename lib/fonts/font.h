/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LIB_FONTS_FONT_H
#define _LIB_FONTS_FONT_H

#include <linux/font.h>

#if defined(CONFIG_FONT_PEARL_8x8)
extern const struct font_desc font_pearl_8x8;
#endif
#if defined(CONFIG_FONT_6x11)
extern const struct font_desc font_vga_6x11;
#endif
#if defined(CONFIG_FONT_7x14)
extern const struct font_desc font_7x14;
#endif
#if defined(CONFIG_FONT_10x18)
extern const struct font_desc font_10x18;
#endif
#if defined(CONFIG_FONT_SUN8x16)
extern const struct font_desc font_sun_8x16;
#endif
#if defined(CONFIG_FONT_SUN12x22)
extern const struct font_desc font_sun_12x22;
#endif
#if defined(CONFIG_FONT_ACORN_8x8)
extern const struct font_desc font_acorn_8x8;
#endif
#if defined(CONFIG_FONT_MINI_4x6)
extern const struct font_desc font_mini_4x6;
#endif
#if defined(CONFIG_FONT_6x10)
extern const struct font_desc font_6x10;
#endif
#if defined(CONFIG_FONT_TER16x32)
extern const struct font_desc font_ter_16x32;
#endif
#if defined(CONFIG_FONT_6x8)
extern const struct font_desc font_6x8;
#endif
#if defined(CONFIG_FONT_TER10x18)
extern const struct font_desc font_ter_10x18;
#endif

#define FONT_EXTRA_WORDS 4

struct font_data {
	unsigned int extra[FONT_EXTRA_WORDS];
	unsigned char data[];
} __packed;

#endif
