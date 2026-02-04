/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_KASAN_TAGS_H
#define __ASM_KASAN_TAGS_H

#define KASAN_TAG_KERNEL	0xFF /* native kernel pointers tag */

#ifdef CONFIG_KASAN_HW_TAGS
#define KASAN_TAG_MIN		0xF0 /* minimum value for random tags */
#define KASAN_TAG_WIDTH		4
#else
#define KASAN_TAG_WIDTH		8
#endif

#endif /* ASM_KASAN_TAGS_H */
