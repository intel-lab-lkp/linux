// SPDX-License-Identifier: GPL-2.0
/*
 * test_mempress_timer.c
 *
 * Simulates kernelspace memory allocation pressure via timers.
 * This module binds individual timers to each online CPU to independently
 * allocate and free physical pages concurrently. It serves as a workload
 * generator to reproduce and measure page allocation stalls under system
 * memory contention.
 *
 * Module Parameters:
 *  - allocs_per_iteration: Number of atomic page allocations executed
 *                          during each timer callback.
 *                          Default: 1000
 *  - test_duration_secs:   Total duration in seconds to run the pressure test
 *                          before automatically disengaging.
 *                          Default: 900 (15 minutes)
 *
 * Usage:
 * insmod test_mempress_timer.ko test_duration_secs=900 allocs_per_iteration=1000
 */

#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/skbuff.h>
#include <net/tcp.h>

struct mempress_timer {
	struct timer_list timer;
	struct list_head list;
	struct list_head page_list;
	int cpu;
};

static LIST_HEAD(timers);
static bool stop_timers;

static unsigned long interval = 2;

static unsigned long allocs_per_iteration = 1000;
module_param(allocs_per_iteration, long, 0444);

static unsigned long test_duration_secs = 900; /* 15 mins default */
module_param(test_duration_secs, ulong, 0444);

static unsigned long end_jiffies;

static void alloc_pages_atomics(struct mempress_timer *test)
{
	int i;
	struct page *page;

	for (i = 0; i < allocs_per_iteration; i++) {
		page = alloc_page(GFP_ATOMIC | __GFP_NOWARN);
		if (!page) {
			pr_warn("page allocation failure from cpu %d at iteration %d\n",
				test->cpu, i);
			break;
		}
		list_add(&page->lru, &test->page_list);
	}
}

static void free_pages_atomics(struct mempress_timer *test)
{
	struct list_head *page;
	struct list_head *iter;

	list_for_each_safe(page, iter, &test->page_list)
		__free_page(container_of(page, struct page, lru));
	INIT_LIST_HEAD(&test->page_list);
}

static void atomic_timer_allocator(struct timer_list *timer)
{
	struct mempress_timer *test = timer_container_of(test, timer, timer);
	bool is_expired = (test_duration_secs > 0 &&
			   time_after(jiffies, end_jiffies));

	if (list_empty(&test->page_list) && !is_expired)
		alloc_pages_atomics(test);
	else
		free_pages_atomics(test);

	if (!READ_ONCE(stop_timers) && !is_expired) {
		test->timer.expires = jiffies + interval;
		add_timer_on(&test->timer, test->cpu);
	} else if (is_expired) {
		pr_info_once("Duration (%lu secs) reached, stopping.\n", test_duration_secs);
	}
}

static int __init mempress_timers_init(void)
{
	int cpu;
	struct mempress_timer *test;

	if (test_duration_secs > 0)
		end_jiffies = jiffies + (test_duration_secs * HZ);

	for_each_online_cpu(cpu) {
		test = kzalloc(sizeof(*test), GFP_KERNEL | __GFP_NOFAIL);

		timer_setup(&test->timer, atomic_timer_allocator, 0);
		list_add(&test->list, &timers);
		INIT_LIST_HEAD(&test->page_list);
		test->cpu = cpu;

		/* For start, use 90 seconds. */
		test->timer.expires = jiffies + (90 * HZ);
		add_timer_on(&test->timer, test->cpu);
	}

	return 0;
}
module_init(mempress_timers_init);

static void mempress_timers_exit(void)
{
	struct mempress_timer *test, *n;

	pr_crit("exiting\n");
	stop_timers = true;

	list_for_each_entry_safe(test, n, &timers, list) {
		timer_delete_sync(&test->timer);
		list_del(&test->list);
		cond_resched();
		free_pages_atomics(test);
		kfree(test);
	}
}
module_exit(mempress_timers_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test memory pressure from timers");
