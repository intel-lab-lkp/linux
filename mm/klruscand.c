// SPDX-License-Identifier: GPL-2.0-only
#include <linux/memcontrol.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/slab.h>
#include <linux/sched/clock.h>
#include <linux/memory-tiers.h>
#include <linux/sched/mm.h>
#include <linux/sched.h>
#include <linux/pghot.h>

#include "internal.h"

#define KLRUSCAND_INTERVAL_MS 100
#define BATCH_SIZE (2 << 16)

static struct task_struct *scan_thread;
static unsigned long pfn_batch[BATCH_SIZE];
static int batch_index;

static void flush_cb(void)
{
	int i = 0;

	for (; i < batch_index; i++) {
		u64 pfn = pfn_batch[i];

		pghot_record_access((unsigned long)pfn, NUMA_NO_NODE,
					PGHOT_PGTABLE_SCAN, jiffies);

		if (i % 16 == 0)
			cond_resched();
	}
	batch_index = 0;
}

static int accessed_cb(unsigned long pfn)
{
	if (batch_index >= BATCH_SIZE)
		return -EAGAIN;

	pfn_batch[batch_index++] = pfn;
	return 0;
}

static int klruscand_run(void *unused)
{
	struct lru_gen_mm_walk *walk;

	walk = kzalloc(sizeof(*walk),
		       __GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN);
	if (!walk)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		unsigned long next_wake_time;
		long sleep_time;
		struct mem_cgroup *memcg;
		int flags;
		int nid;

		next_wake_time = jiffies + msecs_to_jiffies(KLRUSCAND_INTERVAL_MS);

		for_each_node_state(nid, N_MEMORY) {
			pg_data_t *pgdat = NODE_DATA(nid);
			struct reclaim_state rs = { 0 };

			if (node_is_toptier(nid))
				continue;

			rs.mm_walk = walk;
			set_task_reclaim_state(current, &rs);
			flags = memalloc_noreclaim_save();

			memcg = mem_cgroup_iter(NULL, NULL, NULL);
			do {
				struct lruvec *lruvec =
					mem_cgroup_lruvec(memcg, pgdat);
				unsigned long max_seq =
					READ_ONCE((lruvec)->lrugen.max_seq);

				lru_gen_scan_lruvec(lruvec, max_seq,
						    accessed_cb, flush_cb);
				cond_resched();
			} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

			memalloc_noreclaim_restore(flags);
			set_task_reclaim_state(current, NULL);
			memset(walk, 0, sizeof(*walk));
		}

		sleep_time = next_wake_time - jiffies;
		if (sleep_time > 0 && sleep_time != MAX_SCHEDULE_TIMEOUT)
			schedule_timeout_idle(sleep_time);
	}
	kfree(walk);
	return 0;
}

static int __init klruscand_init(void)
{
	struct task_struct *task;

	task = kthread_run(klruscand_run, NULL, "klruscand");

	if (IS_ERR(task)) {
		pr_err("Failed to create klruscand kthread\n");
		return PTR_ERR(task);
	}

	scan_thread = task;
	return 0;
}
module_init(klruscand_init);
