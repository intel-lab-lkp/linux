/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Dept(DEPendency Tracker) - runtime dependency tracker internal header
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2020 LG Electronics, Inc., Byungchul Park
 *  Copyright (c) 2024 SK hynix, Inc., Byungchul Park
 */

#ifndef __DEPT_INTERNAL_H
#define __DEPT_INTERNAL_H

#ifdef CONFIG_DEPT
#include <linux/percpu.h>

struct dept_pool {
	const char			*name;

	/*
	 * object size
	 */
	size_t				obj_sz;

	/*
	 * the remaining number of the object in spool
	 */
	int				obj_nr;

	/*
	 * the number of the object in spool
	 */
	int				tot_nr;

	/*
	 * accumulated amount of memory used by the object in byte
	 */
	atomic_t			acc_sz;

	/*
	 * offset of ->pool_node
	 */
	size_t				node_off;

	/*
	 * pointer to the pool
	 */
	void				*spool; /* static pool */
	void				*rpool; /* reserved pool */
	struct llist_head		boot_pool;
	struct llist_head __percpu	*lpool; /* local pool */
};

enum object_t {
#define OBJECT(id, nr) OBJECT_##id,
	#include "dept_object.h"
#undef OBJECT
	OBJECT_NR,
};

extern struct list_head dept_classes;
extern struct dept_pool dept_pool[];

#endif
#endif /* __DEPT_INTERNAL_H */
