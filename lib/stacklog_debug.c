// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/stackdepot.h>
#include <linux/debugfs.h>
#include <linux/stacklog_debug.h>

#define STACKDEPTH 32
#define BUFSZ 4096

#define LIST_ENTRIES 512
DEFINE_SPINLOCK(stack_lock);
depot_stack_handle_t stack_list[LIST_ENTRIES];
int head, tail;

void stacklog_debug_save(void)
{
	unsigned long entries[STACKDEPTH];
	depot_stack_handle_t stack_hash;
	unsigned long flags;
	unsigned int n;
	int i;

	n = stack_trace_save(entries, ARRAY_SIZE(entries), 1);
	stack_hash = stack_depot_save(entries, n, GFP_NOWAIT);
	if (!stack_hash)
		return;

	spin_lock_irqsave(&stack_lock, flags);
	for (i = head; i < tail; i++)
		if (stack_list[i % LIST_ENTRIES] == stack_hash)
			goto out;

	stack_list[(tail++ % LIST_ENTRIES)] = stack_hash;

	if (tail % LIST_ENTRIES == head % LIST_ENTRIES)
		head++;

	if (tail >= 2 * LIST_ENTRIES) {
		head %= LIST_ENTRIES;
		tail %= LIST_ENTRIES;
		if (tail < head)
			tail += LIST_ENTRIES;
	}
out:
	spin_unlock_irqrestore(&stack_lock, flags);
}

#ifdef CONFIG_DEBUG_FS
static int stacklog_stats_show(struct seq_file *s, void *unused)
{
	char *buf = kmalloc(BUFSZ, GFP_NOWAIT);
	unsigned int nr_entries;
	unsigned long flags;
	int i, start, stop;

	if (!buf)
		return -ENOMEM;

	spin_lock_irqsave(&stack_lock, flags);
	start = head;
	stop = tail;
	spin_unlock_irqrestore(&stack_lock, flags);

	if (start == stop)
		goto out;

	for (i = start; i < stop; i++) {
		unsigned long *ent;
		u32 hash;

		/*
		 * We avoid holdings the lock over the entire loop
		 * just to be careful as we don't want to trip a
		 * call path that calls back into stacklog_debug_save
		 * which would deadlock, so hold the lock minimally
		 * (and be ok with the data changing between loop
		 * iterations).
		 */
		spin_lock_irqsave(&stack_lock, flags);
		hash = stack_list[i % LIST_ENTRIES];
		spin_unlock_irqrestore(&stack_lock, flags);

		nr_entries = stack_depot_fetch(hash, &ent);
		stack_trace_snprint(buf, BUFSZ, ent, nr_entries, 0);
		seq_printf(s, "[idx: %i hash: %ld]====================\n%s\n\n",
			   i - start, (long)hash, buf);
	}
out:
	kfree(buf);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stacklog_stats);

static int __init stacklog_debug_init(void)
{
	debugfs_create_file("stacklog_debug", 0400, NULL, NULL,
			    &stacklog_stats_fops);
	return 0;
}

late_initcall(stacklog_debug_init);
#endif
