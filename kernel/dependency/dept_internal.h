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
#include <linux/dept.h>
#include <linux/percpu.h>
#include <linux/llist.h>
#include <linux/types.h>

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

struct dept_ecxt;
struct dept_iecxt {
	struct dept_ecxt		*ecxt;
	int				enirq;
	/*
	 * for preventing to add a new ecxt
	 */
	bool				staled;
};

struct dept_wait;
struct dept_iwait {
	struct dept_wait		*wait;
	int				irq;
	/*
	 * for preventing to add a new wait
	 */
	bool				staled;
	bool				touched;
};

struct dept_class {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * unique information about the class
			 */
			const char	*name;
			unsigned long	key;
			int		sub_id;

			/*
			 * for BFS
			 */
			unsigned int	bfs_gen;
			struct dept_class *bfs_parent;
			struct list_head bfs_node;

			/*
			 * for hashing this object
			 */
			struct hlist_node hash_node;

			/*
			 * for linking all classes
			 */
			struct list_head all_node;

			/*
			 * for associating its dependencies
			 */
			struct list_head dep_head;
			struct list_head dep_rev_head;

			/*
			 * for tracking IRQ dependencies
			 */
			struct dept_iecxt iecxt[DEPT_CXT_IRQS_NR];
			struct dept_iwait iwait[DEPT_CXT_IRQS_NR];

			/*
			 * classified by a map embedded in task_struct,
			 * not an explicit map
			 */
			bool		sched_map;
		};
	};
};

struct dept_stack {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * backtrace entries
			 */
			unsigned long	raw[DEPT_MAX_STACK_ENTRY];
			int nr;
		};
	};
};

struct dept_ecxt {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * function that entered to this ecxt
			 */
			const char	*ecxt_fn;

			/*
			 * event function
			 */
			const char	*event_fn;

			/*
			 * associated class
			 */
			struct dept_class *class;

			/*
			 * flag indicating which IRQ has been
			 * enabled within the event context
			 */
			unsigned long	enirqf;

			/*
			 * where the IRQ-enabled happened
			 */
			unsigned long	enirq_ip[DEPT_CXT_IRQS_NR];
			struct dept_stack *enirq_stack[DEPT_CXT_IRQS_NR];

			/*
			 * where the event context started
			 */
			unsigned long	ecxt_ip;
			struct dept_stack *ecxt_stack;

			/*
			 * where the event triggered
			 */
			unsigned long	event_ip;
			struct dept_stack *event_stack;
		};
	};
};

struct dept_wait {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * function causing this wait
			 */
			const char	*wait_fn;

			/*
			 * the associated class
			 */
			struct dept_class *class;

			/*
			 * which IRQ the wait was placed in
			 */
			unsigned long	irqf;

			/*
			 * where the IRQ wait happened
			 */
			unsigned long	irq_ip[DEPT_CXT_IRQS_NR];
			struct dept_stack *irq_stack[DEPT_CXT_IRQS_NR];

			/*
			 * where the wait happened
			 */
			unsigned long	wait_ip;
			struct dept_stack *wait_stack;

			/*
			 * whether this wait is for commit in scheduler
			 */
			bool		sched_sleep;
		};
	};
};

struct dept_dep {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * key data of dependency
			 */
			struct dept_ecxt *ecxt;
			struct dept_wait *wait;

			/*
			 * This object can be referred without dept_lock
			 * held but with IRQ disabled, e.g. for hash
			 * lookup. So deferred deletion is needed.
			 */
			struct rcu_head rh;

			/*
			 * for hashing this object
			 */
			struct hlist_node hash_node;

			/*
			 * for linking to a class object
			 */
			struct list_head dep_node;
			struct list_head dep_rev_node;
		};
	};
};

struct dept_hash {
	/*
	 * hash table
	 */
	struct hlist_head		*table;

	/*
	 * size of the table e.i. 2^bits
	 */
	int				bits;
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
