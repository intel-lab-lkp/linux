/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_REFCOUNT_H
#define _LINUX_REFCOUNT_H

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/limits.h>
#include <linux/refcount_types.h>
#include <linux/spinlock_types.h>

struct mutex;

enum refcount_saturation_type {
	REFCOUNT_ADD_NOT_ZERO_OVF,
	REFCOUNT_ADD_OVF,
	REFCOUNT_ADD_UAF,
	REFCOUNT_SUB_UAF,
	REFCOUNT_DEC_LEAK,
};

/* Make the generation of refcount_long_t easier. */
#define refcount_long_saturation_type refcount_saturation_type

#include <linux/refcount-impl.h>
#include <generated/refcount-long.h>

#endif /* _LINUX_REFCOUNT_H */
