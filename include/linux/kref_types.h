/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _KREF_TYPES_H_
#define _KREF_TYPES_H_

#include <linux/refcount_types.h>

struct kref {
	refcount_t refcount;
};

#endif /* _KREF_TYPES_H_ */
