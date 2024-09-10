/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RW_HINT_H
#define _LINUX_RW_HINT_H

#include <linux/build_bug.h>
#include <linux/compiler_attributes.h>
#include <uapi/linux/fcntl.h>

/* Block storage write lifetime hint values. */
enum rw_lifetime_hint {
	WRITE_LIFE_NOT_SET	= RWH_WRITE_LIFE_NOT_SET,
	WRITE_LIFE_NONE		= RWH_WRITE_LIFE_NONE,
	WRITE_LIFE_SHORT	= RWH_WRITE_LIFE_SHORT,
	WRITE_LIFE_MEDIUM	= RWH_WRITE_LIFE_MEDIUM,
	WRITE_LIFE_LONG		= RWH_WRITE_LIFE_LONG,
	WRITE_LIFE_EXTREME	= RWH_WRITE_LIFE_EXTREME,
} __packed;

/* Sparse ignores __packed annotations on enums, hence the #ifndef below. */
#ifndef __CHECKER__
static_assert(sizeof(enum rw_lifetime_hint) == 1);
#endif

#define WRITE_HINT_TYPE_BIT	BIT(7)
#define WRITE_HINT_VAL_MASK	(WRITE_HINT_TYPE_BIT - 1)
#define WRITE_HINT_TYPE(h)	(((h) & WRITE_HINT_TYPE_BIT) ? \
				TYPE_RW_PLACEMENT_HINT : TYPE_RW_LIFETIME_HINT)
#define WRITE_HINT_VAL(h)	((h) & WRITE_HINT_VAL_MASK)

#define WRITE_PLACEMENT_HINT(h)	(((h) & WRITE_HINT_TYPE_BIT) ? \
				 WRITE_HINT_VAL(h) : 0)
#define WRITE_LIFETIME_HINT(h)	(((h) & WRITE_HINT_TYPE_BIT) ? \
				 0 : WRITE_HINT_VAL(h))

#define PLACEMENT_HINT_TYPE	WRITE_HINT_TYPE_BIT
#define MAX_PLACEMENT_HINT_VAL	(WRITE_HINT_VAL_MASK - 1)
#endif /* _LINUX_RW_HINT_H */
