/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_DEFERRED_TYPES_H
#define _LINUX_UNWIND_USER_DEFERRED_TYPES_H

#include <asm/local.h>

struct unwind_cache {
	unsigned int		nr_entries;
	unsigned long		entries[];
};


union unwind_task_id {
	struct {
		u32		cpu;
		u32		cnt;
	};
	u64			id;
};

struct unwind_task_info {
	struct unwind_cache	*cache;
	struct callback_head	work;
	unsigned long		unwind_mask;
	union unwind_task_id	id;
	local_t			pending;
};

#endif /* _LINUX_UNWIND_USER_DEFERRED_TYPES_H */
