/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_DEFERRED_TYPES_H
#define _LINUX_UNWIND_USER_DEFERRED_TYPES_H

#include <asm/local64.h>
#include <asm/local.h>

struct unwind_cache {
	unsigned int		nr_entries;
	unsigned long		entries[];
};

struct unwind_task_info {
	struct unwind_cache	*cache;
	struct callback_head	work;
	local64_t		timestamp;
	local_t			pending;
};

#endif /* _LINUX_UNWIND_USER_DEFERRED_TYPES_H */
