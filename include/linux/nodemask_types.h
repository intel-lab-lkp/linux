/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_NODEMASK_TYPES_H
#define __LINUX_NODEMASK_TYPES_H

#include <linux/bitops.h>
#include <linux/numa.h>

struct nodemask {
	DECLARE_BITMAP(bits, MAX_NUMNODES);
};

typedef struct nodemask nodemask_t;

#endif /* __LINUX_NODEMASK_TYPES_H */
