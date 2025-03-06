// SPDX-License-Identifier: GPL-2.0
/*
 * kpromoted is a kernel thread that runs on each node that has CPU i,e.,
 * on regular nodes.
 *
 * Maintains list of hot pages from lower tiers and promotes them.
 */
#include <linux/kpromoted.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/mmzone.h>
#include <linux/migrate.h>
#include <linux/memory-tiers.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cpuhotplug.h>
#include <linux/hashtable.h>

static DEFINE_HASHTABLE(page_hotness_hash, KPROMOTED_HASH_ORDER);
static struct mutex page_hotness_lock[1UL << KPROMOTED_HASH_ORDER];

static int kpromote_page(struct page_hotness_info *phi)
{
	struct page *page = pfn_to_page(phi->pfn);
	struct folio *folio;
	int ret;

	if (!page)
		return 1;

	folio = page_folio(page);
	ret = migrate_misplaced_folio_prepare(folio, NULL, phi->hot_node);
	if (ret)
		return 1;

	return migrate_misplaced_folio(folio, phi->hot_node);
}

static int page_should_be_promoted(struct page_hotness_info *phi)
{
	struct page *page = pfn_to_online_page(phi->pfn);
	unsigned long now = jiffies;
	struct folio *folio;

	if (!page || is_zone_device_page(page))
		return false;

	folio = page_folio(page);
	if (!folio_test_lru(folio)) {
		count_vm_event(KPROMOTED_MIG_NON_LRU);
		return false;
	}
	if (folio_nid(folio) == phi->hot_node) {
		count_vm_event(KPROMOTED_MIG_RIGHT_NODE);
		return false;
	}

	/* If the page was hot a while ago, don't promote */
	if ((now - phi->last_update) > 2 * msecs_to_jiffies(KPROMOTED_FREQ_WINDOW)) {
		count_vm_event(KPROMOTED_MIG_COLD_OLD);
		return false;
	}

	/* If the page hasn't been accessed enough number of times, don't promote */
	if (phi->frequency < KPRMOTED_FREQ_THRESHOLD) {
		count_vm_event(KPROMOTED_MIG_COLD_NOT_ACCESSED);
		return false;
	}
	return true;
}

/*
 * Go thro' page hotness information and migrate pages if required.
 *
 * Promoted pages are not longer tracked in the hot list.
 * Cold pages are pruned from the list as well.
 *
 * TODO: Batching could be done
 */
static void kpromoted_migrate(pg_data_t *pgdat)
{
	int nid = pgdat->node_id;
	struct page_hotness_info *phi;
	struct hlist_node *tmp;
	int nr_bkts = HASH_SIZE(page_hotness_hash);
	int bkt;

	for (bkt = 0; bkt < nr_bkts; bkt++) {
		mutex_lock(&page_hotness_lock[bkt]);
		hlist_for_each_entry_safe(phi, tmp, &page_hotness_hash[bkt], hnode) {
			if (phi->hot_node != nid)
				continue;

			if (page_should_be_promoted(phi)) {
				count_vm_event(KPROMOTED_MIG_CANDIDATE);
				if (!kpromote_page(phi)) {
					count_vm_event(KPROMOTED_MIG_PROMOTED);
					hlist_del_init(&phi->hnode);
					kfree(phi);
				}
			} else {
				/*
				 * Not a suitable page or cold page, stop tracking it.
				 * TODO: Identify cold pages and drive demotion?
				 */
				count_vm_event(KPROMOTED_MIG_DROPPED);
				hlist_del_init(&phi->hnode);
				kfree(phi);
			}
		}
		mutex_unlock(&page_hotness_lock[bkt]);
	}
}

static struct page_hotness_info *__kpromoted_lookup(unsigned long pfn, int bkt)
{
	struct page_hotness_info *phi;

	hlist_for_each_entry(phi, &page_hotness_hash[bkt], hnode) {
		if (phi->pfn == pfn)
			return phi;
	}
	return NULL;
}

static struct page_hotness_info *kpromoted_lookup(unsigned long pfn, int bkt, unsigned long now)
{
	struct page_hotness_info *phi;

	phi = __kpromoted_lookup(pfn, bkt);
	if (!phi) {
		phi = kzalloc(sizeof(struct page_hotness_info), GFP_KERNEL);
		if (!phi)
			return ERR_PTR(-ENOMEM);

		phi->pfn = pfn;
		phi->frequency = 1;
		phi->last_update = now;
		phi->recency = now;
		hlist_add_head(&phi->hnode, &page_hotness_hash[bkt]);
		count_vm_event(KPROMOTED_RECORD_ADDED);
	} else {
		count_vm_event(KPROMOTED_RECORD_EXISTS);
	}
	return phi;
}

/*
 * Called by subsystems that generate page hotness/access information.
 *
 * Records the memory access info for futher action by kpromoted.
 */
int kpromoted_record_access(u64 pfn, int nid, int src, unsigned long now)
{
	struct page_hotness_info *phi;
	struct page *page;
	struct folio *folio;
	int ret, bkt;

	count_vm_event(KPROMOTED_RECORDED_ACCESSES);

	switch (src) {
	case KPROMOTED_HW_HINTS:
		count_vm_event(KPROMOTED_RECORD_HWHINTS);
		break;
	case KPROMOTED_PGTABLE_SCAN:
		count_vm_event(KPROMOTED_RECORD_PGTSCANS);
		break;
	default:
		break;
	}

	/*
	 * Record only accesses from lower tiers.
	 * Assuming node having CPUs as toptier for now.
	 */
	if (node_is_toptier(pfn_to_nid(pfn))) {
		count_vm_event(KPROMOTED_RECORD_TOPTIER);
		return 0;
	}

	page = pfn_to_online_page(pfn);
	if (!page || is_zone_device_page(page))
		return 0;

	folio = page_folio(page);
	if (!folio_test_lru(folio))
		return 0;

	bkt = hash_min(pfn, KPROMOTED_HASH_ORDER);
	mutex_lock(&page_hotness_lock[bkt]);
	phi = kpromoted_lookup(pfn, bkt, now);
	if (!phi) {
		ret = PTR_ERR(phi);
		goto out;
	}

	if ((phi->last_update - now) > msecs_to_jiffies(KPROMOTED_FREQ_WINDOW)) {
		/* New window */
		phi->frequency = 1; /* TODO: Factor in the history */
		phi->last_update = now;
	} else {
		phi->frequency++;
	}
	phi->recency = now;

	/*
	 * TODOs:
	 * 1. Source nid is hard-coded for some temperature sources
	 * 2. Take action if hot_node changes - may be a shared page?
	 * 3. Maintain node info for every access within the window?
	 */
	phi->hot_node = (nid == NUMA_NO_NODE) ? 1 : nid;
	mutex_unlock(&page_hotness_lock[bkt]);
out:
	return 0;
}

/*
 * Go through the accumulated mem_access_info and migrate
 * pages if required.
 */
static void kpromoted_do_work(pg_data_t *pgdat)
{
	kpromoted_migrate(pgdat);
}

static inline bool kpromoted_work_requested(pg_data_t *pgdat)
{
	return false;
}

static int kpromoted(void *p)
{
	pg_data_t *pgdat = (pg_data_t *)p;
	struct task_struct *tsk = current;
	long timeout = msecs_to_jiffies(KPROMOTE_DELAY);

	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);

	while (!kthread_should_stop()) {
		wait_event_timeout(pgdat->kpromoted_wait,
				   kpromoted_work_requested(pgdat), timeout);
		kpromoted_do_work(pgdat);
	}
	return 0;
}

static void kpromoted_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);

	if (pgdat->kpromoted)
		return;

	pgdat->kpromoted = kthread_run(kpromoted, pgdat, "kpromoted%d", nid);
	if (IS_ERR(pgdat->kpromoted)) {
		pr_err("Failed to start kpromoted on node %d\n", nid);
		pgdat->kpromoted = NULL;
	}
}

static int kpromoted_cpu_online(unsigned int cpu)
{
	int nid;

	for_each_node_state(nid, N_CPU) {
		pg_data_t *pgdat = NODE_DATA(nid);
		const struct cpumask *mask;

		mask = cpumask_of_node(pgdat->node_id);

		if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
			/* One of our CPUs online: restore mask */
			if (pgdat->kpromoted)
				set_cpus_allowed_ptr(pgdat->kpromoted, mask);
	}
	return 0;
}

static int __init kpromoted_init(void)
{
	int nid, ret, i;

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"mm/promotion:online",
					kpromoted_cpu_online, NULL);
	if (ret < 0) {
		pr_err("kpromoted: failed to register hotplug callbacks.\n");
		return ret;
	}

	for (i = 0; i < (1UL << KPROMOTED_HASH_ORDER); i++)
		mutex_init(&page_hotness_lock[i]);

	for_each_node_state(nid, N_CPU)
		kpromoted_run(nid);

	return 0;
}

subsys_initcall(kpromoted_init)
