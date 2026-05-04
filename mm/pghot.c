// SPDX-License-Identifier: GPL-2.0
/*
 * Maintains information about hot pages from slower tier nodes and
 * promotes them.
 *
 * Per-PFN hotness information is stored for lower tier nodes in
 * mem_section.
 *
 * In the default mode, a single byte (u8) is used to store
 * the frequency of access and last access time. Promotions are done
 * to a default toptier NID.
 *
 * In the precision mode, 4 bytes are used to store the frequency
 * of access, last access time and the accessing NID.
 *
 * A kernel thread named kmigrated is provided to migrate or promote
 * the hot pages. kmigrated runs for each lower tier node. It iterates
 * over the node's PFNs and  migrates pages marked for migration into
 * their targeted nodes.
 *
 * Migration rate-limiting and dynamic threshold logic implementations
 * were moved from NUMA Balancing mode 2.
 */
#include <linux/mm.h>
#include <linux/migrate.h>
#include <linux/memory.h>
#include <linux/memory-tiers.h>
#include <linux/pghot.h>

unsigned int pghot_target_nid = PGHOT_DEFAULT_NODE;
unsigned int pghot_src_enabled;
unsigned int pghot_freq_threshold = PGHOT_DEFAULT_FREQ_THRESHOLD;
unsigned int kmigrated_sleep_ms = KMIGRATED_DEFAULT_SLEEP_MS;
unsigned int kmigrated_batch_nr = KMIGRATED_DEFAULT_BATCH_NR;

unsigned int sysctl_pghot_freq_window = PGHOT_DEFAULT_FREQ_WINDOW;

/* Restrict the NUMA promotion throughput (MB/s) for each target node. */
static unsigned int sysctl_pghot_promote_rate_limit = 65536;

#define KMIGRATED_MIGRATION_ADJUST_STEPS	16
#define KMIGRATED_PROMOTION_THRESHOLD_WINDOW	60000

DEFINE_STATIC_KEY_FALSE(pghot_src_hwhints);
DEFINE_STATIC_KEY_FALSE(pghot_src_hintfaults);

#ifdef CONFIG_SYSCTL
static const struct ctl_table pghot_sysctls[] = {
	{
		.procname       = "pghot_promote_freq_window_ms",
		.data           = &sysctl_pghot_freq_window,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec_minmax,
		.extra1         = SYSCTL_ZERO,
	},
	{
		.procname	= "pghot_promote_rate_limit_MBps",
		.data		= &sysctl_pghot_promote_rate_limit,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	},
	{
		.procname	= "numa_balancing_promote_rate_limit_MBps",
		.data		= &sysctl_pghot_promote_rate_limit,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	},
};
#endif

static bool kmigrated_started __ro_after_init;

/**
 * pghot_record_access() - Record page accesses from lower tier memory
 * for the purpose of tracking page hotness and subsequent promotion.
 *
 * @pfn: PFN of the page
 * @nid: Target NID to where the page needs to be migrated in precision
 *       mode but unused in default mode
 * @src: The identifier of the sub-system that reports the access
 * @now: Access time in jiffies
 *
 * Updates the NID (in precision mode only), frequency and time of access
 * and marks the page as ready for migration if the frequency crosses a
 * threshold. The pages marked for migration are migrated by kmigrated
 * kernel thread.
 *
 * Return: 0 on success and -EINVAL on failure to record the access.
 */
int pghot_record_access(unsigned long pfn, int nid, int src, unsigned long now)
{
	struct mem_section *ms;
	struct folio *folio;
	phi_t *phi, *hot_map;
	struct page *page;
	int src_nid;

	if (!kmigrated_started)
		return 0;

	if (!pghot_nid_valid(nid))
		return -EINVAL;

	switch (src) {
	case PGHOT_HINTFAULTS:
		if (!static_branch_unlikely(&pghot_src_hintfaults))
			return 0;
		count_vm_event(PGHOT_RECORDED_HINTFAULTS);
		break;
	case PGHOT_HWHINTS:
		if (!static_branch_unlikely(&pghot_src_hwhints))
			return 0;
		count_vm_event(PGHOT_RECORDED_HWHINTS);
		break;
	default:
		return -EINVAL;
	}

	src_nid = pfn_to_nid(pfn);
	if (src_nid == nid)
		return 0;

	/*
	 * Record only accesses from lower tiers.
	 */
	if (node_is_toptier(src_nid))
		return 0;

	/*
	 * Reject the non-migratable pages right away.
	 */
	page = pfn_to_online_page(pfn);
	if (!page || is_zone_device_page(page))
		return 0;

	folio = page_folio(page);
	if (!folio_try_get(folio))
		return 0;

	if (unlikely(page_folio(page) != folio))
		goto out;

	if (!folio_test_lru(folio))
		goto out;

	/* Get the hotness slot corresponding to the 1st PFN of the folio */
	pfn = folio_pfn(folio);
	ms = __pfn_to_section(pfn);
	if (!ms || !ms->hot_map)
		goto out;

	hot_map = (phi_t *)(((unsigned long)(ms->hot_map)) & ~PGHOT_SECTION_HOT_MASK);
	phi = &hot_map[pfn % PAGES_PER_SECTION];

	count_vm_event(PGHOT_RECORDED_ACCESSES);

	/*
	 * Update the hotness parameters.
	 */
	if (pghot_update_record(phi, nid, now)) {
		set_bit(PGHOT_SECTION_HOT_BIT, (unsigned long *)&ms->hot_map);
		set_bit(PGDAT_KMIGRATED_ACTIVATE, &page_pgdat(page)->flags);
	}
out:
	folio_put(folio);
	return 0;
}

/*
 * For memory tiering mode, if there are enough free pages (more than
 * enough watermark defined here) in fast memory node, to take full
 * advantage of fast memory capacity, all recently accessed slow
 * memory pages will be migrated to fast memory node without
 * considering hot threshold.
 */
static bool pgdat_free_space_enough(struct pglist_data *pgdat)
{
	int z;
	unsigned long enough_wmark;

	enough_wmark = max(1UL * 1024 * 1024 * 1024 >> PAGE_SHIFT,
			   pgdat->node_present_pages >> 4);
	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
			continue;

		if (zone_watermark_ok(zone, 0,
				      promo_wmark_pages(zone) + enough_wmark,
				      ZONE_MOVABLE, 0))
			return true;
	}
	return false;
}

/*
 * For memory tiering mode, too high promotion/demotion throughput may
 * hurt application latency.  So we provide a mechanism to rate limit
 * the number of pages that are tried to be promoted.
 */
static bool kmigrated_promotion_rate_limit(struct pglist_data *pgdat, unsigned long rate_limit,
					   int nr, unsigned int now_ms)
{
	unsigned long nr_cand;
	unsigned int start;

	mod_node_page_state(pgdat, PGPROMOTE_CANDIDATE, nr);
	nr_cand = node_page_state(pgdat, PGPROMOTE_CANDIDATE);
	start = pgdat->nbp_rl_start;
	if (now_ms - start > MSEC_PER_SEC &&
	    cmpxchg(&pgdat->nbp_rl_start, start, now_ms) == start)
		pgdat->nbp_rl_nr_cand = nr_cand;
	if (nr_cand - pgdat->nbp_rl_nr_cand >= rate_limit)
		return true;
	return false;
}

static void kmigrated_promotion_adjust_threshold(struct pglist_data *pgdat,
						 unsigned long rate_limit, unsigned int ref_th,
						 unsigned int now_ms)
{
	unsigned int start, th_period, unit_th, th;
	unsigned long nr_cand, ref_cand, diff_cand;

	th_period = KMIGRATED_PROMOTION_THRESHOLD_WINDOW;
	start = pgdat->nbp_th_start;
	if (now_ms - start > th_period &&
	    cmpxchg(&pgdat->nbp_th_start, start, now_ms) == start) {
		ref_cand = rate_limit *
			KMIGRATED_PROMOTION_THRESHOLD_WINDOW / MSEC_PER_SEC;
		nr_cand = node_page_state(pgdat, PGPROMOTE_CANDIDATE);
		diff_cand = nr_cand - pgdat->nbp_th_nr_cand;
		unit_th = ref_th * 2 / KMIGRATED_MIGRATION_ADJUST_STEPS;
		th = pgdat->nbp_threshold ? : ref_th;
		if (diff_cand > ref_cand * 11 / 10)
			th = max(th - unit_th, unit_th);
		else if (diff_cand < ref_cand * 9 / 10)
			th = min(th + unit_th, ref_th * 2);
		pgdat->nbp_th_nr_cand = nr_cand;
		pgdat->nbp_threshold = th;
	}
}

static bool kmigrated_should_migrate_memory(unsigned long nr_pages, int nid,
					    unsigned long time)
{
	struct pglist_data *pgdat;
	unsigned long rate_limit;
	unsigned int th, def_th;
	unsigned int now_ms = jiffies_to_msecs(jiffies); /* Based on full-width jiffies */
	unsigned long now = jiffies;

	pgdat = NODE_DATA(nid);
	if (pgdat_free_space_enough(pgdat)) {
		/* workload changed, reset hot threshold */
		pgdat->nbp_threshold = 0;
		mod_node_page_state(pgdat, PGPROMOTE_CANDIDATE_NRL, nr_pages);
		return true;
	}

	def_th = sysctl_pghot_freq_window;
	rate_limit = MB_TO_PAGES(sysctl_pghot_promote_rate_limit);
	kmigrated_promotion_adjust_threshold(pgdat, rate_limit, def_th, now_ms);

	th = pgdat->nbp_threshold ? : def_th;
	if (pghot_access_latency(time, now) >= th)
		return false;

	return !kmigrated_promotion_rate_limit(pgdat, rate_limit, nr_pages, now_ms);
}

static int pghot_get_hotness(unsigned long pfn, int *nid, int *freq,
			     unsigned long *time)
{
	phi_t *phi, *hot_map;
	struct mem_section *ms;

	ms = __pfn_to_section(pfn);
	if (!ms || !ms->hot_map)
		return -EINVAL;

	hot_map = (phi_t *)(((unsigned long)(ms->hot_map)) & ~PGHOT_SECTION_HOT_MASK);
	phi = &hot_map[pfn % PAGES_PER_SECTION];

	return pghot_get_record(phi, nid, freq, time);
}

/*
 * Walks the PFNs of the zone, isolates and migrates them in batches.
 */
static void kmigrated_walk_zone(unsigned long start_pfn, unsigned long end_pfn,
				int src_nid)
{
	struct mem_cgroup *cur_memcg = NULL;
	int cur_nid = NUMA_NO_NODE;
	LIST_HEAD(migrate_list);
	int batch_count = 0;
	struct folio *folio;
	struct page *page;
	unsigned long pfn;

	pfn = start_pfn;
	do {
		int nid = NUMA_NO_NODE, nr = 1;
		struct mem_cgroup *memcg;
		unsigned long time = 0;
		int freq = 0;

		if (!pfn_valid(pfn))
			goto out_next;

		page = pfn_to_online_page(pfn);
		if (!page)
			goto out_next;

		folio = page_folio(page);
		if (!folio_try_get(folio))
			goto out_next;

		if (unlikely(page_folio(page) != folio)) {
			folio_put(folio);
			goto out_next;
		}

		nr = folio_nr_pages(folio);
		if (folio_nid(folio) != src_nid) {
			folio_put(folio);
			goto out_next;
		}

		if (!folio_test_lru(folio)) {
			folio_put(folio);
			goto out_next;
		}

		if (pghot_get_hotness(pfn, &nid, &freq, &time)) {
			folio_put(folio);
			goto out_next;
		}

		if (nid == NUMA_NO_NODE)
			nid = pghot_target_nid;

		if (folio_nid(folio) == nid) {
			folio_put(folio);
			goto out_next;
		}

		if (!kmigrated_should_migrate_memory(nr, nid, time)) {
			folio_put(folio);
			goto out_next;
		}

		if (migrate_misplaced_folio_prepare(folio, NULL, nid)) {
			folio_put(folio);
			goto out_next;
		}

		memcg = folio_memcg(folio);
		if (cur_nid == NUMA_NO_NODE) {
			cur_nid = nid;
			cur_memcg = memcg;
		}

		/* If NID or memcg changed, flush the previous batch first */
		if (cur_nid != nid || cur_memcg != memcg) {
			if (!list_empty(&migrate_list))
				promote_misplaced_memcg_folios(&migrate_list, cur_nid);
			cur_nid = nid;
			cur_memcg = memcg;
			batch_count = 0;
			cond_resched();
		}

		list_add(&folio->lru, &migrate_list);
		folio_put(folio);

		if (++batch_count > kmigrated_batch_nr) {
			promote_misplaced_memcg_folios(&migrate_list, cur_nid);
			batch_count = 0;
			cond_resched();
		}
out_next:
		pfn += nr;
	} while (pfn < end_pfn);
	if (!list_empty(&migrate_list))
		promote_misplaced_memcg_folios(&migrate_list, cur_nid);
}

static void kmigrated_do_work(pg_data_t *pgdat)
{
	unsigned long section_nr, s_begin, start_pfn;
	struct mem_section *ms;
	int nid;

	clear_bit(PGDAT_KMIGRATED_ACTIVATE, &pgdat->flags);
	s_begin = next_present_section_nr(-1);
	for_each_present_section_nr(s_begin, section_nr) {
		start_pfn = section_nr_to_pfn(section_nr);
		ms = __nr_to_section(section_nr);

		if (!pfn_valid(start_pfn))
			continue;

		nid = pfn_to_nid(start_pfn);
		if (node_is_toptier(nid) || nid != pgdat->node_id)
			continue;

		if (!test_and_clear_bit(PGHOT_SECTION_HOT_BIT, (unsigned long *)&ms->hot_map))
			continue;

		kmigrated_walk_zone(start_pfn, start_pfn + PAGES_PER_SECTION,
				    pgdat->node_id);
	}
}

static inline bool kmigrated_work_requested(pg_data_t *pgdat)
{
	return test_bit(PGDAT_KMIGRATED_ACTIVATE, &pgdat->flags);
}

/*
 * Per-node kthread that iterates over its PFNs and migrates the
 * pages that have been marked for migration.
 */
static int kmigrated(void *p)
{
	pg_data_t *pgdat = p;

	while (!kthread_should_stop()) {
		long timeout = msecs_to_jiffies(READ_ONCE(kmigrated_sleep_ms));

		if (wait_event_timeout(pgdat->kmigrated_wait, kmigrated_work_requested(pgdat),
				       timeout))
			kmigrated_do_work(pgdat);
	}
	return 0;
}

static int kmigrated_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret;

	if (!pgdat->kmigrated) {
		pgdat->kmigrated = kthread_create_on_node(kmigrated, pgdat, nid,
							  "kmigrated%d", nid);
		if (IS_ERR(pgdat->kmigrated)) {
			ret = PTR_ERR(pgdat->kmigrated);
			pgdat->kmigrated = NULL;
			pr_err("Failed to start kmigrated%d, ret %d\n", nid, ret);
			return ret;
		}
		pr_info("pghot: Started kmigrated thread for node %d\n", nid);
	}
	wake_up_process(pgdat->kmigrated);
	return 0;
}

static void pghot_free_hot_map(struct mem_section *ms)
{
	kfree((void *)((unsigned long)ms->hot_map & ~PGHOT_SECTION_HOT_MASK));
	ms->hot_map = NULL;
}

static int pghot_alloc_hot_map(struct mem_section *ms, int nid)
{
	ms->hot_map = kcalloc_node(PAGES_PER_SECTION, PGHOT_RECORD_SIZE, GFP_KERNEL,
				   nid);
	if (!ms->hot_map)
		return -ENOMEM;
	return 0;
}

static void pghot_offline_sec_hotmap(unsigned long start_pfn,
				     unsigned long nr_pages)
{
	unsigned long start, end, pfn;
	struct mem_section *ms;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION) {
		ms = __pfn_to_section(pfn);
		if (!ms || !ms->hot_map)
			continue;

		pghot_free_hot_map(ms);
	}
}

static int pghot_online_sec_hotmap(unsigned long start_pfn,
				   unsigned long nr_pages)
{
	int nid = pfn_to_nid(start_pfn);
	unsigned long start, end, pfn;
	struct mem_section *ms;
	int fail = 0;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	for (pfn = start; !fail && pfn < end; pfn += PAGES_PER_SECTION) {
		ms = __pfn_to_section(pfn);
		if (!ms || ms->hot_map)
			continue;

		fail = pghot_alloc_hot_map(ms, nid);
	}

	if (!fail)
		return 0;

	/* rollback */
	end = pfn - PAGES_PER_SECTION;
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION) {
		ms = __pfn_to_section(pfn);
		if (ms && ms->hot_map)
			pghot_free_hot_map(ms);
	}
	return -ENOMEM;
}

static int pghot_memhp_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;
	int ret = 0;

	switch (action) {
	case MEM_GOING_ONLINE:
		ret = pghot_online_sec_hotmap(mn->start_pfn, mn->nr_pages);
		break;
	case MEM_OFFLINE:
	case MEM_CANCEL_ONLINE:
		pghot_offline_sec_hotmap(mn->start_pfn, mn->nr_pages);
		break;
	}

	return notifier_from_errno(ret);
}

static struct notifier_block pghot_mem_notifier = {
	.notifier_call = pghot_memhp_callback,
	.priority = DEFAULT_CALLBACK_PRI,
};

static void pghot_destroy_hot_map(void)
{
	unsigned long section_nr, s_begin;
	struct mem_section *ms;

	s_begin = next_present_section_nr(-1);
	for_each_present_section_nr(s_begin, section_nr) {
		ms = __nr_to_section(section_nr);
		pghot_free_hot_map(ms);
	}

	unregister_memory_notifier(&pghot_mem_notifier);
}

static int pghot_setup_hot_map(void)
{
	unsigned long section_nr, s_begin, start_pfn;
	struct mem_section *ms;
	int nid, ret;

	ret = register_memory_notifier(&pghot_mem_notifier);
	if (ret)
		return ret;

	s_begin = next_present_section_nr(-1);
	for_each_present_section_nr(s_begin, section_nr) {
		ms = __nr_to_section(section_nr);
		start_pfn = section_nr_to_pfn(section_nr);
		nid = pfn_to_nid(start_pfn);

		if (node_is_toptier(nid) || !pfn_valid(start_pfn))
			continue;

		if (pghot_alloc_hot_map(ms, nid))
			goto out_free_hot_map;
	}
	return 0;

out_free_hot_map:
	pghot_destroy_hot_map();
	return -ENOMEM;
}

static int __init pghot_init(void)
{
	pg_data_t *pgdat;
	int nid, ret;

	ret = pghot_setup_hot_map();
	if (ret)
		return ret;

	for_each_node_state(nid, N_MEMORY) {
		if (node_is_toptier(nid))
			continue;

		ret = kmigrated_run(nid);
		if (ret)
			goto out_stop_kthread;
	}
	register_sysctl_init("vm", pghot_sysctls);
	pghot_debug_init();

	kmigrated_started = true;
	return 0;

out_stop_kthread:
	for_each_node_state(nid, N_MEMORY) {
		pgdat = NODE_DATA(nid);
		if (pgdat->kmigrated) {
			kthread_stop(pgdat->kmigrated);
			pgdat->kmigrated = NULL;
		}
	}
	pghot_destroy_hot_map();
	return ret;
}

late_initcall_sync(pghot_init)
