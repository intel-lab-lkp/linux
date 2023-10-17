/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RW_HINT_H
#define _LINUX_RW_HINT_H

#include <linux/build_bug.h>
#include <linux/compiler_attributes.h>

/* Block storage write lifetime hint values. */
enum rw_hint {
	WRITE_LIFE_NOT_SET	= 0, /* RWH_WRITE_LIFE_NOT_SET */
	WRITE_LIFE_NONE		= 1, /* RWH_WRITE_LIFE_NONE */
	WRITE_LIFE_SHORT	= 2, /* RWH_WRITE_LIFE_SHORT */
	WRITE_LIFE_MEDIUM	= 3, /* RWH_WRITE_LIFE_MEDIUM */
	WRITE_LIFE_LONG		= 4, /* RWH_WRITE_LIFE_LONG */
	WRITE_LIFE_EXTREME	= 5, /* RWH_WRITE_LIFE_EXTREME */
} __packed;

static_assert(sizeof(enum rw_hint) == 1);

#endif /* _LINUX_RW_HINT_H */
