/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock - Namespace hooks
 *
 * Copyright © 2026 Cloudflare
 */

#ifndef _SECURITY_LANDLOCK_NS_H
#define _SECURITY_LANDLOCK_NS_H

#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/compiler_attributes.h>
#include <linux/ns/ns_common_types.h>
#include <linux/types.h>

#include "limits.h"

/* _LANDLOCK_NS_CLONE_NEWCGROUP, */
#define _LANDLOCK_NS_ENUM(struct_name, flag) _LANDLOCK_NS_##flag,

/* _LANDLOCK_NS_CLONE_NEWCGROUP = 0, */
enum {
	FOR_EACH_NS_TYPE(_LANDLOCK_NS_ENUM) _LANDLOCK_NUM_NS_TYPES,
};

static_assert(_LANDLOCK_NUM_NS_TYPES == LANDLOCK_NUM_PERM_NS);

/*
 * case CLONE_NEWCGROUP:
 *         return BIT_ULL(_LANDLOCK_NS_CLONE_NEWCGROUP);
 */
/* clang-format off */
#define _LANDLOCK_NS_CASE(struct_name, flag) \
	case flag: \
		return BIT_ULL(_LANDLOCK_NS_##flag);
/* clang-format on */

static inline __attribute_const__ u64
landlock_ns_type_to_bit(const unsigned long ns_type)
{
	switch (ns_type) {
		FOR_EACH_NS_TYPE(_LANDLOCK_NS_CASE)
	default:
		WARN_ON_ONCE(1);
		return 0;
	}
}

/*
 * if (ns_types & CLONE_NEWCGROUP)
 *         bits |= BIT_ULL(_LANDLOCK_NS_CLONE_NEWCGROUP);
 */
/* clang-format off */
#define _LANDLOCK_NS_CONVERT(struct_name, flag) \
	do { \
		if (ns_types & (flag)) \
			bits |= BIT_ULL(_LANDLOCK_NS_##flag); \
	} while (0);
/* clang-format on */

static inline __attribute_const__ u64
landlock_ns_types_to_bits(const u64 ns_types)
{
	u64 bits = 0;

	WARN_ON_ONCE(ns_types & ~CLONE_NS_ALL);
	FOR_EACH_NS_TYPE(_LANDLOCK_NS_CONVERT)
	return bits;
}

__init void landlock_add_ns_hooks(void);

#endif /* _SECURITY_LANDLOCK_NS_H */
