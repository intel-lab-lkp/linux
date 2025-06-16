// SPDX-License-Identifier: GPL-2.0
/*
 * kmigrated is a kernel thread that runs for each node that has
 * memory. It iterates over the node's PFNs and  migrates pages
 * marked for migration into their targeted nodes.
 *
 * kmigrated depends on PAGE_EXTENSION to find out the pages that
 * need to be migrated. In addition to a few fields that could be
 * used by hot page promotion logic to store and evaluate the page
 * hotness information, the extended page flags is field is extended
 * to store the target NID for migration.
 */
#include <linux/mm.h>
#include <linux/migrate.h>
#include <linux/cpuhotplug.h>
#include <linux/page_ext.h>

#define KMIGRATE_DELAY	MSEC_PER_SEC
#define KMIGRATE_BATCH	512

static int page_ext_xchg_nid(struct page_ext *page_ext, int nid)
{
	unsigned long old_flags, flags;
	int old_nid;

	old_flags = READ_ONCE(page_ext->flags);
	do {
		flags = old_flags;
		old_nid = (flags >> PAGE_EXT_MIG_NID_SHIFT) & PAGE_EXT_MIG_NID_MASK;

		flags &= ~(PAGE_EXT_MIG_NID_MASK << PAGE_EXT_MIG_NID_SHIFT);
		flags |= (nid & PAGE_EXT_MIG_NID_MASK) << PAGE_EXT_MIG_NID_SHIFT;
	} while (unlikely(!try_cmpxchg(&page_ext->flags, &old_flags, flags)));

	return old_nid;
}

/*
 * Marks the page as ready for migration.
 *
 * @pfn: PFN of the page
 * @nid: Target NID to were the page needs to be migrated
 *
 * The request for migration is noted by setting PAGE_EXT_MIGRATE_READY
 * in the extended page flags which the kmigrated thread would check.
 */
int kmigrated_add_pfn(unsigned long pfn, int nid)
{
	struct page *page;
	struct page_ext *page_ext;

	page = pfn_to_page(pfn);
	if (!page)
		return -EINVAL;

	page_ext = page_ext_get(page);
	if (unlikely(!page_ext))
		return -EINVAL;

	page_ext_xchg_nid(page_ext, nid);
	test_and_set_bit(PAGE_EXT_MIGRATE_READY, &page_ext->flags);
	page_ext_put(page_ext);

	set_bit(PGDAT_KMIGRATED_ACTIVATE, &page_pgdat(page)->flags);
	return 0;
}

/*
 * If the page has been marked ready for migration, return
 * the NID to which it needs to be migrated to.
 *
 * If not return NUMA_NO_NODE.
 */
static int kmigrated_get_nid(struct page *page)
{
	struct page_ext *page_ext;
	int nid = NUMA_NO_NODE;

	page_ext = page_ext_get(page);
	if (unlikely(!page_ext))
		return nid;

	if (!test_and_clear_bit(PAGE_EXT_MIGRATE_READY, &page_ext->flags))
		goto out;

	nid = page_ext_xchg_nid(page_ext, nid);
out:
	page_ext_put(page_ext);
	return nid;
}

/*
 * Walks the PFNs of the zone, isolates and migrates them in batches.
 */
static void kmigrated_walk_zone(unsigned long start_pfn, unsigned long end_pfn,
				int src_nid)
{
	int nid, cur_nid = NUMA_NO_NODE;
	LIST_HEAD(migrate_list);
	int batch_count = 0;
	struct folio *folio;
	struct page *page;
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_online_page(pfn);
		if (!page)
			continue;

		if (page_to_nid(page) != src_nid)
			continue;

		/*
		 * TODO: Take care of folio_nr_pages() increment
		 * to pfn count.
		 */
		folio = page_folio(page);
		if (!folio_test_lru(folio))
			continue;

		nid = kmigrated_get_nid(page);
		if (nid == NUMA_NO_NODE)
			continue;

		if (page_to_nid(page) == nid)
			continue;

		if (migrate_misplaced_folio_prepare(folio, NULL, nid))
			continue;

		if (cur_nid != NUMA_NO_NODE)
			cur_nid = nid;

		if (++batch_count >= KMIGRATE_BATCH || cur_nid != nid) {
			migrate_misplaced_folios_batch(&migrate_list, cur_nid);
			cur_nid = nid;
			batch_count = 0;
			cond_resched();
		}
		list_add(&folio->lru, &migrate_list);
	}
	if (!list_empty(&migrate_list))
		migrate_misplaced_folios_batch(&migrate_list, cur_nid);
}

static void kmigrated_do_work(pg_data_t *pgdat)
{
	struct zone *zone;
	int zone_idx;

	clear_bit(PGDAT_KMIGRATED_ACTIVATE, &pgdat->flags);
	for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
		zone = &pgdat->node_zones[zone_idx];

		if (!populated_zone(zone))
			continue;

		if (zone_is_zone_device(zone))
			continue;

		kmigrated_walk_zone(zone->zone_start_pfn, zone_end_pfn(zone),
				    pgdat->node_id);
	}
}

static inline bool kmigrated_work_requested(pg_data_t *pgdat)
{
	return test_bit(PGDAT_KMIGRATED_ACTIVATE, &pgdat->flags);
}

static void kmigrated_wait_work(pg_data_t *pgdat)
{
	long timeout = msecs_to_jiffies(KMIGRATE_DELAY);

	wait_event_timeout(pgdat->kmigrated_wait,
			   kmigrated_work_requested(pgdat), timeout);
}

/*
 * Per-node kthread that iterates over its PFNs and migrates the
 * pages that have been marked for migration.
 */
static int kmigrated(void *p)
{
	pg_data_t *pgdat = (pg_data_t *)p;

	while (!kthread_should_stop()) {
		kmigrated_wait_work(pgdat);
		kmigrated_do_work(pgdat);
	}
	return 0;
}

static void kmigrated_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);

	if (pgdat->kmigrated)
		return;

	pgdat->kmigrated = kthread_create(kmigrated, pgdat, "kmigrated%d", nid);
	if (IS_ERR(pgdat->kmigrated)) {
		pr_err("Failed to start kmigrated for node %d\n", nid);
		pgdat->kmigrated = NULL;
	} else {
		wake_up_process(pgdat->kmigrated);
	}
}

static int __init kmigrated_init(void)
{
	int nid;

	for_each_node_state(nid, N_MEMORY)
		kmigrated_run(nid);

	return 0;
}

subsys_initcall(kmigrated_init)
