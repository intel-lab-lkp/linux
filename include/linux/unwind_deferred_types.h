/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_DEFERRED_TYPES_H
#define _LINUX_UNWIND_USER_DEFERRED_TYPES_H

struct unwind_cache {
	unsigned int		nr_entries;
	unsigned long		entries[];
};

struct unwind_task_info {
	struct unwind_cache	*cache;
	struct callback_head	work;
	u64			timestamp;
	int			pending;
};

#endif /* _LINUX_UNWIND_USER_DEFERRED_TYPES_H */
