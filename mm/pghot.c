// SPDX-License-Identifier: GPL-2.0
/*
 * Maintains information about hot pages from slower tier nodes and
 * promotes them.
 *
 * Info about accessed pages are stored in hash lists indexed by PFN.
 * Info about pages that are hot enough to be promoted are stored in
 * a per-toptier-node max_heap.
 *
 * kpromoted is a kernel thread that runs on each toptier node and
 * promotes pages from max_heap.
 *
 * TODO:
 * - Compact pghot_info so that nid, time and frequency can fit
 * - Scalar hotness value as a function frequency and recency
 * - Possibility of moving migration rate limiting to kpromoted
 */
#include <linux/pghot.h>
#include <linux/kthread.h>
#include <linux/mmzone.h>
#include <linux/migrate.h>
#include <linux/memory-tiers.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/hashtable.h>
#include <linux/min_heap.h>

struct pghot_hash {
	struct hlist_head hash;
	spinlock_t lock;
};

static struct pghot_hash *phi_hash;
static int phi_hash_order;
static int phi_heap_entries;
static struct kmem_cache *phi_cache __ro_after_init;
static bool kpromoted_started __ro_after_init;

static bool phi_heap_less(const void *lhs, const void *rhs, void *args)
{
	return (*(struct pghot_info **)lhs)->frequency >
		(*(struct pghot_info **)rhs)->frequency;
}

static void phi_heap_swp(void *lhs, void *rhs, void *args)
{
	struct pghot_info **l = (struct pghot_info **)lhs;
	struct pghot_info **r = (struct pghot_info **)rhs;
	int lindex = l - (struct pghot_info **)args;
	int rindex = r - (struct pghot_info **)args;
	struct pghot_info *tmp = *l;

	*l = *r;
	*r = tmp;

	(*l)->heap_idx = lindex;
	(*r)->heap_idx = rindex;
}

static const struct min_heap_callbacks phi_heap_cb = {
	.less = phi_heap_less,
	.swp = phi_heap_swp,
};

static void phi_heap_update_entry(struct max_heap *phi_heap, struct pghot_info *phi)
{
	int orig_idx = phi->heap_idx;

	min_heap_sift_up(phi_heap, phi->heap_idx, &phi_heap_cb,
			 phi_heap->data);
	if (phi_heap->data[phi->heap_idx]->heap_idx == orig_idx)
		min_heap_sift_down(phi_heap, phi->heap_idx,
				   &phi_heap_cb, phi_heap->data);
}

static bool phi_heap_insert(struct max_heap *phi_heap, struct pghot_info *phi)
{
	if (phi_heap->nr >= phi_heap_entries)
		return false;

	phi->heap_idx = phi_heap->nr;
	min_heap_push(phi_heap, &phi, &phi_heap_cb, phi_heap->data);

	return true;
}

static bool phi_is_pfn_hot(struct pghot_info *phi)
{
	struct page *page = pfn_to_online_page(phi->pfn);
	unsigned long now = jiffies;
	struct folio *folio;

	if (!page || is_zone_device_page(page))
		return false;

	folio = page_folio(page);
	if (!folio_test_lru(folio)) {
		count_vm_event(KPROMOTED_NON_LRU);
		return false;
	}
	if (folio_nid(folio) == phi->nid) {
		count_vm_event(KPROMOTED_RIGHT_NODE);
		return false;
	}

	/* If the page was hot a while ago, don't promote */
	if ((now - phi->last_update) > 2 * msecs_to_jiffies(KPROMOTED_FREQ_WINDOW)) {
		count_vm_event(KPROMOTED_COLD_OLD);
		return false;
	}
	return true;
}

static struct folio *kpromoted_isolate_folio(struct pghot_info *phi)
{
	struct page *page = pfn_to_page(phi->pfn);
	struct folio *folio;

	if (!page)
		return NULL;

	folio = page_folio(page);
	if (migrate_misplaced_folio_prepare(folio, NULL, phi->nid))
		return NULL;
	else
		return folio;
}

static struct pghot_info *phi_alloc(unsigned long pfn)
{
	struct pghot_info *phi;

	phi = kmem_cache_zalloc(phi_cache, GFP_NOWAIT);
	if (!phi)
		return NULL;

	phi->pfn = pfn;
	phi->heap_idx = -1;
	return phi;
}

static inline void phi_free(struct pghot_info *phi)
{
	kmem_cache_free(phi_cache, phi);
}

static int phi_heap_extract(pg_data_t *pgdat, int batch_count, int freq_th,
			    struct list_head *migrate_list, int *count)
{
	spinlock_t *phi_heap_lock = &pgdat->heap_lock;
	struct max_heap *phi_heap = &pgdat->heap;
	int max_retries = 10;
	int bkt, i = 0;

	if (batch_count < 0 || !migrate_list || !count || freq_th < 1 ||
	    freq_th > KPRMOTED_FREQ_THRESHOLD)
		return -EINVAL;

	*count = 0;
	for (i = 0; i < batch_count; i++) {
		struct pghot_info *top = NULL;
		bool should_continue = false;
		struct folio *folio;
		int retries = 0;

		while (retries < max_retries) {
			spin_lock(phi_heap_lock);
			if (phi_heap->nr > 0 && phi_heap->data[0]->frequency >= freq_th) {
				should_continue = true;
				bkt = hash_min(phi_heap->data[0]->pfn, phi_hash_order);
				top = phi_heap->data[0];
			}
			spin_unlock(phi_heap_lock);

			if (!should_continue)
				goto done;

			spin_lock(&phi_hash[bkt].lock);
			spin_lock(phi_heap_lock);
			if (phi_heap->nr == 0 || phi_heap->data[0] != top ||
			    phi_heap->data[0]->frequency < freq_th) {
				spin_unlock(phi_heap_lock);
				spin_unlock(&phi_hash[bkt].lock);
				retries++;
				continue;
			}

			top = phi_heap->data[0];
			hlist_del_init(&top->hnode);

			phi_heap->nr--;
			if (phi_heap->nr > 0) {
				phi_heap->data[0] = phi_heap->data[phi_heap->nr];
				phi_heap->data[0]->heap_idx = 0;
				min_heap_sift_down(phi_heap, 0, &phi_heap_cb,
						   phi_heap->data);
			}

			spin_unlock(phi_heap_lock);
			spin_unlock(&phi_hash[bkt].lock);

			if (!phi_is_pfn_hot(top)) {
				count_vm_event(KPROMOTED_DROPPED);
				goto skip;
			}

			folio = kpromoted_isolate_folio(top);
			if (folio) {
				list_add(&folio->lru, migrate_list);
				(*count)++;
			}
skip:
			phi_free(top);
			break;
		}
		if (retries >= max_retries) {
			pr_warn("%s: Too many retries\n", __func__);
			break;
		}

	}
done:
	return 0;
}

static void phi_heap_add_or_adjust(struct pghot_info *phi)
{
	pg_data_t *pgdat = NODE_DATA(phi->nid);
	struct max_heap *phi_heap = &pgdat->heap;

	spin_lock(&pgdat->heap_lock);
	if (phi->heap_idx >= 0 && phi->heap_idx < phi_heap->nr &&
	    phi_heap->data[phi->heap_idx] == phi) {
		/* Entry exists in heap */
		if (phi->frequency < KPRMOTED_FREQ_THRESHOLD) {
			/* Below threshold, remove from the heap */
			phi_heap->nr--;
			if (phi->heap_idx < phi_heap->nr) {
				phi_heap->data[phi->heap_idx] =
					phi_heap->data[phi_heap->nr];
				phi_heap->data[phi->heap_idx]->heap_idx =
					phi->heap_idx;
				min_heap_sift_down(phi_heap, phi->heap_idx,
						   &phi_heap_cb, phi_heap->data);
			}
			phi->heap_idx = -1;

		} else {
			/* Update position in heap */
			phi_heap_update_entry(phi_heap, phi);
		}
	} else if (phi->frequency >= KPRMOTED_FREQ_THRESHOLD) {
		/* Add to the heap */
		if (phi_heap_insert(phi_heap, phi))
			count_vm_event(PGHOT_RECORDS_HEAP);
	}
	spin_unlock(&pgdat->heap_lock);
}

static struct pghot_info *phi_lookup(unsigned long pfn, int bkt)
{
	struct pghot_info *phi;

	hlist_for_each_entry(phi, &phi_hash[bkt].hash, hnode) {
		if (phi->pfn == pfn)
			return phi;
	}
	return NULL;
}

/*
 * Called by subsystems that generate page hotness/access information.
 *
 *  @pfn: The PFN of the memory accessed
 *  @nid: The accessing NUMA node ID
 *  @src: The temperature source (sub-system) that generated the
 *        access info
 *  @time: The access time in jiffies
 *
 * Maintains the access records per PFN, classifies them as
 * hot based on subsequent accesses and finally hands over
 * them to kpromoted for migration.
 */
int pghot_record_access(u64 pfn, int nid, int src, unsigned long now)
{
	struct pghot_info *phi;
	struct page *page;
	struct folio *folio;
	int bkt;
	bool new_entry = false, new_window = false;

	if (!kpromoted_started)
		return -EINVAL;

	count_vm_event(PGHOT_RECORDED_ACCESSES);

	switch (src) {
	case PGHOT_HW_HINTS:
		count_vm_event(PGHOT_RECORD_HWHINTS);
		break;
	case PGHOT_PGTABLE_SCAN:
		count_vm_event(PGHOT_RECORD_PGTSCANS);
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Record only accesses from lower tiers.
	 */
	if (node_is_toptier(pfn_to_nid(pfn)))
		return 0;

	/*
	 * Reject the non-migratable pages right away.
	 */
	page = pfn_to_online_page(pfn);
	if (!page || is_zone_device_page(page))
		return 0;

	folio = page_folio(page);
	if (!folio_test_lru(folio))
		return 0;

	bkt = hash_min(pfn, phi_hash_order);
	spin_lock(&phi_hash[bkt].lock);
	phi = phi_lookup(pfn, bkt);
	if (!phi) {
		phi = phi_alloc(pfn);
		if (!phi)
			goto out;
		new_entry = true;
	}

	if (((now - phi->last_update) > msecs_to_jiffies(KPROMOTED_FREQ_WINDOW)) ||
	    (nid != NUMA_NO_NODE && phi->nid != nid))
		new_window = true;

	if (new_entry || new_window) {
		/* New window */
		phi->frequency = 1; /* TODO: Factor in the history */
	} else
		phi->frequency++;
	phi->last_update = now;
	phi->nid = (nid == NUMA_NO_NODE) ? KPROMOTED_DEFAULT_NODE : nid;

	if (new_entry) {
		/* Insert the new entry into hash table */
		hlist_add_head(&phi->hnode, &phi_hash[bkt].hash);
		count_vm_event(PGHOT_RECORDS_HASH);
	} else {
		/* Add/update the position in heap */
		phi_heap_add_or_adjust(phi);
	}
out:
	spin_unlock(&phi_hash[bkt].lock);
	return 0;
}

/*
 * Extract the hot page records and batch-migrate the
 * hot pages.
 */
static void kpromoted_migrate(pg_data_t *pgdat)
{
	int count, ret;
	LIST_HEAD(migrate_list);

	/*
	 * Extract the top N elements from the heap that match
	 * the requested hotness threshold.
	 *
	 * PFNs ineligible from migration standpoint are removed
	 * from the heap and hash.
	 *
	 * Folios eligible for migration are isolated and returned
	 * in @migrate_list.
	 */
	ret = phi_heap_extract(pgdat, KPRMOTED_MIGRATE_BATCH,
			       KPRMOTED_FREQ_THRESHOLD, &migrate_list, &count);
	if (ret)
		return;

	if (!list_empty(&migrate_list))
		migrate_misplaced_folios_batch(&migrate_list, pgdat->node_id);
}

static int kpromoted(void *p)
{
	pg_data_t *pgdat = (pg_data_t *)p;

	while (!kthread_should_stop()) {
		wait_event_timeout(pgdat->kpromoted_wait, false,
				   msecs_to_jiffies(KPROMOTE_DELAY));
		kpromoted_migrate(pgdat);
	}
	return 0;
}

static int kpromoted_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret = 0;

	if (!node_is_toptier(nid))
		return 0;

	if (!pgdat->phi_buf) {
		pgdat->phi_buf = vzalloc_node(phi_heap_entries * sizeof(struct pghot_info *),
					      nid);
		if (!pgdat->phi_buf)
			return -ENOMEM;

		min_heap_init(&pgdat->heap, pgdat->phi_buf, phi_heap_entries);
		spin_lock_init(&pgdat->heap_lock);
	}

	if (!pgdat->kpromoted)
		pgdat->kpromoted = kthread_create_on_node(kpromoted, pgdat, nid,
							  "kpromoted%d", nid);
	if (IS_ERR(pgdat->kpromoted)) {
		ret = PTR_ERR(pgdat->kpromoted);
		pgdat->kpromoted = NULL;
		pr_info("Failed to start kpromoted%d, ret %d\n", nid, ret);
	} else {
		wake_up_process(pgdat->kpromoted);
	}
	return ret;
}

static int __init pghot_init(void)
{
	unsigned int hash_size;
	size_t hash_entries;
	size_t nr_pages = 0;
	pg_data_t *pgdat;
	int i, nid, ret;

	/*
	 * Arrive at the hash and heap sizes based on the
	 * number of pages present in the lower tier nodes.
	 */
	for_each_node_state(nid, N_MEMORY) {
		if (!node_is_toptier(nid))
			nr_pages += NODE_DATA(nid)->node_present_pages;
	}

	if (!nr_pages)
		return 0;

	hash_entries = nr_pages * PGHOT_HASH_PCT / 100;
	hash_size = hash_entries / PGHOT_HASH_ENTRIES;
	phi_hash_order = ilog2(hash_size);

	phi_hash = vmalloc(sizeof(struct pghot_hash) * hash_size);
	if (!phi_hash) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < hash_size; i++) {
		INIT_HLIST_HEAD(&phi_hash[i].hash);
		spin_lock_init(&phi_hash[i].lock);
	}

	phi_cache = KMEM_CACHE(pghot_info, 0);
	if (unlikely(!phi_cache)) {
		ret = -ENOMEM;
		goto out;
	}

	phi_heap_entries = hash_entries * PGHOT_HEAP_PCT / 100;
	for_each_node_state(nid, N_CPU) {
		ret = kpromoted_run(nid);
		if (ret)
			goto out_stop_kthread;
	}

	kpromoted_started = true;
	pr_info("pghot: Started page hotness monitoring and promotion thread\n");
	pr_info("pghot: nr_pages %ld hash_size %d hash_entries %ld hash_order %d heap_entries %d\n",
	       nr_pages, hash_size, hash_entries, phi_hash_order, phi_heap_entries);
	return 0;

out_stop_kthread:
	for_each_node_state(nid, N_CPU) {
		pgdat = NODE_DATA(nid);
		if (pgdat->kpromoted) {
			kthread_stop(pgdat->kpromoted);
			pgdat->kpromoted = NULL;
			vfree(pgdat->phi_buf);
		}
	}
out:
	kmem_cache_destroy(phi_cache);
	vfree(phi_hash);
	return ret;
}

late_initcall(pghot_init)
