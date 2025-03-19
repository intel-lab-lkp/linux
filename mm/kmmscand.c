// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>
#include <linux/rmap.h>
#include <linux/pagewalk.h>
#include <linux/page_ext.h>
#include <linux/page_idle.h>
#include <linux/page_table_check.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
#include <linux/kthread.h>
#include <linux/kmmscand.h>
#include <linux/memory-tiers.h>
#include <linux/mempolicy.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/cleanup.h>
#include <linux/minmax.h>

#include <asm/pgalloc.h>
#include "internal.h"
#include "mm_slot.h"

static struct task_struct *kmmscand_thread __read_mostly;
static DEFINE_MUTEX(kmmscand_mutex);
extern unsigned int sysctl_numa_balancing_scan_delay;

/*
 * Total VMA size to cover during scan.
 * Min: 256MB default: 1GB max: 4GB
 */
#define KMMSCAND_SCAN_SIZE_MIN	(256 * 1024 * 1024UL)
#define KMMSCAND_SCAN_SIZE_MAX	(4 * 1024 * 1024 * 1024UL)
#define KMMSCAND_SCAN_SIZE	(1 * 1024 * 1024 * 1024UL)

static unsigned long kmmscand_scan_size __read_mostly = KMMSCAND_SCAN_SIZE;

/*
 * Scan period for each mm.
 * Min: 500ms default: 2sec Max: 5sec
 */
#define KMMSCAND_SCAN_PERIOD_MAX	5000U
#define KMMSCAND_SCAN_PERIOD_MIN	500U
#define KMMSCAND_SCAN_PERIOD		2000U

static unsigned int kmmscand_mm_scan_period_ms __read_mostly = KMMSCAND_SCAN_PERIOD;

/* How long to pause between two scan and migration cycle */
static unsigned int kmmscand_scan_sleep_ms __read_mostly = 16;

/* Max number of mms to scan in one scan and migration cycle */
#define KMMSCAND_MMS_TO_SCAN	(4 * 1024UL)
static unsigned long kmmscand_mms_to_scan __read_mostly = KMMSCAND_MMS_TO_SCAN;

bool kmmscand_scan_enabled = true;
static bool need_wakeup;
static bool migrated_need_wakeup;

/* How long to pause between two migration cycles */
static unsigned int kmmmigrate_sleep_ms __read_mostly = 20;

static struct task_struct *kmmmigrated_thread __read_mostly;
static DEFINE_MUTEX(kmmmigrated_mutex);
static DECLARE_WAIT_QUEUE_HEAD(kmmmigrated_wait);
static unsigned long kmmmigrated_sleep_expire;

/* mm of the migrating folio entry */
static struct mm_struct *kmmscand_cur_migrate_mm;

/* Migration list is manipulated underneath because of mm_exit */
static bool  kmmscand_migration_list_dirty;

static unsigned long kmmscand_sleep_expire;
#define KMMSCAND_DEFAULT_TARGET_NODE	(0)
static int kmmscand_target_node = KMMSCAND_DEFAULT_TARGET_NODE;

static DEFINE_SPINLOCK(kmmscand_mm_lock);
static DEFINE_SPINLOCK(kmmscand_migrate_lock);
static DECLARE_WAIT_QUEUE_HEAD(kmmscand_wait);

#define KMMSCAND_SLOT_HASH_BITS 10
static DEFINE_READ_MOSTLY_HASHTABLE(kmmscand_slots_hash, KMMSCAND_SLOT_HASH_BITS);

static struct kmem_cache *kmmscand_slot_cache __read_mostly;

/* Per mm information collected to control VMA scanning */
struct kmmscand_mm_slot {
	struct mm_slot slot;
	/* Unit: ms. Determines how aften mm scan should happen. */
	unsigned int scan_period;
	unsigned long next_scan;
	/* Tracks how many useful pages obtained for migration in the last scan */
	unsigned long scan_delta;
	/* Determines how much VMA address space to be covered in the scanning */
	unsigned long scan_size;
	long address;
	bool is_scanned;
};

/* Data structure to keep track of current mm under scan */
struct kmmscand_scan {
	struct list_head mm_head;
	struct kmmscand_mm_slot *mm_slot;
};

struct kmmscand_scan kmmscand_scan = {
	.mm_head = LIST_HEAD_INIT(kmmscand_scan.mm_head),
};

/*
 * Data structure passed to control scanning and also collect
 * per memory node information
 */
struct kmmscand_scanctrl {
	struct list_head scan_list;
	unsigned long address;
};

struct kmmscand_scanctrl kmmscand_scanctrl;

struct kmmscand_migrate_list {
	struct list_head migrate_head;
};

struct kmmscand_migrate_list kmmscand_migrate_list = {
	.migrate_head = LIST_HEAD_INIT(kmmscand_migrate_list.migrate_head),
};

/* Per folio information used for migration */
struct kmmscand_migrate_info {
	struct list_head migrate_node;
	struct mm_struct *mm;
	struct folio *folio;
	unsigned long address;
};

static bool kmmscand_eligible_srcnid(int nid)
{
	if (!node_is_toptier(nid))
		return true;

	return false;
}

static int kmmscand_has_work(void)
{
	return !list_empty(&kmmscand_scan.mm_head);
}

static int kmmmigrated_has_work(void)
{
	if (!list_empty(&kmmscand_migrate_list.migrate_head))
		return true;
	return false;
}

static bool kmmscand_should_wakeup(void)
{
	bool wakeup =  kthread_should_stop() || need_wakeup ||
	       time_after_eq(jiffies, kmmscand_sleep_expire);
	if (need_wakeup)
		need_wakeup = false;

	return wakeup;
}

static bool kmmmigrated_should_wakeup(void)
{
	bool wakeup =  kthread_should_stop() || migrated_need_wakeup ||
	       time_after_eq(jiffies, kmmmigrated_sleep_expire);
	if (migrated_need_wakeup)
		migrated_need_wakeup = false;

	return wakeup;
}

static void kmmscand_wait_work(void)
{
	const unsigned long scan_sleep_jiffies =
		msecs_to_jiffies(kmmscand_scan_sleep_ms);

	if (!scan_sleep_jiffies)
		return;

	kmmscand_sleep_expire = jiffies + scan_sleep_jiffies;
	wait_event_timeout(kmmscand_wait,
			kmmscand_should_wakeup(),
			scan_sleep_jiffies);
	return;
}

static void kmmmigrated_wait_work(void)
{
	const unsigned long migrate_sleep_jiffies =
		msecs_to_jiffies(kmmmigrate_sleep_ms);

	if (!migrate_sleep_jiffies)
		return;

	kmmmigrated_sleep_expire = jiffies + migrate_sleep_jiffies;
	wait_event_timeout(kmmmigrated_wait,
			kmmmigrated_should_wakeup(),
			migrate_sleep_jiffies);
}

/*
 * Do not know what info to pass in the future to make
 * decision on taget node. Keep it void * now.
 */
static int kmmscand_get_target_node(void *data)
{
	return kmmscand_target_node;
}

extern bool migrate_balanced_pgdat(struct pglist_data *pgdat,
					unsigned long nr_migrate_pages);

/*XXX: Taken from migrate.c to avoid NUMAB mode=2 and NULL vma checks*/
static int kmmscand_migrate_misplaced_folio_prepare(struct folio *folio,
		struct vm_area_struct *vma, int node)
{
	int nr_pages = folio_nr_pages(folio);
	pg_data_t *pgdat = NODE_DATA(node);

	if (folio_is_file_lru(folio)) {
		/*
		 * Do not migrate file folios that are mapped in multiple
		 * processes with execute permissions as they are probably
		 * shared libraries.
		 *
		 * See folio_likely_mapped_shared() on possible imprecision
		 * when we cannot easily detect if a folio is shared.
		 */
		if (vma && (vma->vm_flags & VM_EXEC) &&
		    folio_likely_mapped_shared(folio))
			return -EACCES;
		/*
		 * Do not migrate dirty folios as not all filesystems can move
		 * dirty folios in MIGRATE_ASYNC mode which is a waste of
		 * cycles.
		 */
		if (folio_test_dirty(folio))
			return -EAGAIN;
	}

	/* Avoid migrating to a node that is nearly full */
	if (!migrate_balanced_pgdat(pgdat, nr_pages)) {
		int z;

		for (z = pgdat->nr_zones - 1; z >= 0; z--) {
			if (managed_zone(pgdat->node_zones + z))
				break;
		}

		/*
		 * If there are no managed zones, it should not proceed
		 * further.
		 */
		if (z < 0)
			return -EAGAIN;

		wakeup_kswapd(pgdat->node_zones + z, 0,
			      folio_order(folio), ZONE_MOVABLE);
		return -EAGAIN;
	}

	if (!folio_isolate_lru(folio))
		return -EAGAIN;

	node_stat_mod_folio(folio, NR_ISOLATED_ANON + folio_is_file_lru(folio),
			    nr_pages);

	return 0;
}

static inline bool is_valid_folio(struct folio *folio)
{
	if (!folio || folio_test_unevictable(folio) || !folio_mapped(folio) ||
		folio_is_zone_device(folio) || folio_likely_mapped_shared(folio))
		return false;

	return true;
}

enum kmmscand_migration_err {
	KMMSCAND_NULL_MM = 1,
	KMMSCAND_EXITING_MM,
	KMMSCAND_INVALID_FOLIO,
	KMMSCAND_NONLRU_FOLIO,
	KMMSCAND_INELIGIBLE_SRC_NODE,
	KMMSCAND_SAME_SRC_DEST_NODE,
	KMMSCAND_PTE_NOT_PRESENT,
	KMMSCAND_PMD_NOT_PRESENT,
	KMMSCAND_NO_PTE_OFFSET_MAP_LOCK,
	KMMSCAND_LRU_ISOLATION_ERR,
};

static int kmmscand_promote_folio(struct kmmscand_migrate_info *info, int destnid)
{
	unsigned long pfn;
	unsigned long address;
	struct page *page;
	struct folio *folio;
	int ret;
	struct mm_struct *mm;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	pmd_t pmde;
	int srcnid;

	if (info->mm == NULL)
		return KMMSCAND_NULL_MM;

	if (info->mm == READ_ONCE(kmmscand_cur_migrate_mm) &&
		READ_ONCE(kmmscand_migration_list_dirty)) {
		WARN_ON_ONCE(mm);
		return KMMSCAND_EXITING_MM;
	}

	mm = info->mm;
	folio = info->folio;

	/* Check again if the folio is really valid now */
	if (folio) {
		pfn = folio_pfn(folio);
		page = pfn_to_online_page(pfn);
	}

	if (!page || PageTail(page) || !is_valid_folio(folio))
		return KMMSCAND_INVALID_FOLIO;

	if (!folio_test_lru(folio))
		return KMMSCAND_NONLRU_FOLIO;

	folio_get(folio);

	srcnid = folio_nid(folio);

	/* Do not try to promote pages from regular nodes */
	if (!kmmscand_eligible_srcnid(srcnid)) {
		folio_put(folio);
		return KMMSCAND_INELIGIBLE_SRC_NODE;
	}

	/* Also happen when it is already migrated */
	if (srcnid == destnid) {
		folio_put(folio);
		return KMMSCAND_SAME_SRC_DEST_NODE;
	}
	address = info->address;
	pmd = pmd_off(mm, address);
	pmde = pmdp_get(pmd);

	if (!pmd_present(pmde)) {
		folio_put(folio);
		return KMMSCAND_PMD_NOT_PRESENT;
	}

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!pte) {
		folio_put(folio);
		WARN_ON_ONCE(!pte);
		return KMMSCAND_NO_PTE_OFFSET_MAP_LOCK;
	}

	ret = kmmscand_migrate_misplaced_folio_prepare(folio, NULL, destnid);
	if (ret) {
		folio_put(folio);
		pte_unmap_unlock(pte, ptl);
		return KMMSCAND_LRU_ISOLATION_ERR;
	}

	folio_put(folio);
	pte_unmap_unlock(pte, ptl);

	return  migrate_misplaced_folio(folio, destnid);
}

static bool folio_idle_clear_pte_refs_one(struct folio *folio,
					 struct vm_area_struct *vma,
					 unsigned long addr,
					 pte_t *ptep)
{
	bool referenced = false;
	struct mm_struct *mm = vma->vm_mm;
	pmd_t *pmd = pmd_off(mm, addr);

	if (ptep) {
		if (ptep_clear_young_notify(vma, addr, ptep))
			referenced = true;
	} else if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)) {
		if (!pmd_present(*pmd))
			WARN_ON_ONCE(1);
		if (pmdp_clear_young_notify(vma, addr, pmd))
			referenced = true;
	} else {
		WARN_ON_ONCE(1);
	}

	if (referenced) {
		folio_clear_idle(folio);
		folio_set_young(folio);
	}

	return true;
}

static void page_idle_clear_pte_refs(struct page *page, pte_t *pte, struct mm_walk *walk)
{
	bool need_lock;
	struct folio *folio =  page_folio(page);
	unsigned long address;

	if (!folio_mapped(folio) || !folio_raw_mapping(folio))
		return;

	need_lock = !folio_test_anon(folio) || folio_test_ksm(folio);
	if (need_lock && !folio_trylock(folio))
		return;
	address = vma_address(walk->vma, page_pgoff(folio, page), compound_nr(page));
	VM_BUG_ON_VMA(address == -EFAULT, walk->vma);
	folio_idle_clear_pte_refs_one(folio, walk->vma, address, pte);

	if (need_lock)
		folio_unlock(folio);
}

static int hot_vma_idle_pte_entry(pte_t *pte,
				 unsigned long addr,
				 unsigned long next,
				 struct mm_walk *walk)
{
	struct page *page;
	struct folio *folio;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct kmmscand_migrate_info *info;
	struct kmmscand_scanctrl *scanctrl = walk->private;
	int srcnid;

	scanctrl->address = addr;
	pte_t pteval = ptep_get(pte);

	if (!pte_present(pteval))
		return 0;

	if (pte_none(pteval))
		return 0;

	vma = walk->vma;
	mm = vma->vm_mm;

	page = pte_page(*pte);

	page_idle_clear_pte_refs(page, pte, walk);

	folio = page_folio(page);
	folio_get(folio);

	if (!is_valid_folio(folio)) {
		folio_put(folio);
		return 0;
	}
	srcnid = folio_nid(folio);


	if (!folio_test_lru(folio)) {
		folio_put(folio);
		return 0;
	}

	if (!folio_test_idle(folio) || folio_test_young(folio) ||
			mmu_notifier_test_young(mm, addr) ||
			folio_test_referenced(folio) || pte_young(pteval)) {

		/* Do not try to promote pages from regular nodes */
		if (!kmmscand_eligible_srcnid(srcnid)) {
			folio_put(folio);
			return 0;
		}
		info = kzalloc(sizeof(struct kmmscand_migrate_info), GFP_NOWAIT);
		if (info && scanctrl) {

			info->mm = mm;
			info->address = addr;
			info->folio = folio;

			/* No need of lock now */
			list_add_tail(&info->migrate_node, &scanctrl->scan_list);
		}
	}

	folio_set_idle(folio);
	folio_put(folio);
	return 0;
}

static const struct mm_walk_ops hot_vma_set_idle_ops = {
	.pte_entry = hot_vma_idle_pte_entry,
	.walk_lock = PGWALK_RDLOCK,
};

static void kmmscand_walk_page_vma(struct vm_area_struct *vma, struct kmmscand_scanctrl *scanctrl)
{
	if (!vma_migratable(vma) || !vma_policy_mof(vma) ||
	    is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
		return;
	}
	if (!vma->vm_mm ||
	    (vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ)))
		return;

	if (!vma_is_accessible(vma))
		return;

	walk_page_vma(vma, &hot_vma_set_idle_ops, scanctrl);
}

static inline int kmmscand_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
}

static void kmmscand_cleanup_migration_list(struct mm_struct *mm)
{
	struct kmmscand_migrate_info *info, *tmp;

	spin_lock(&kmmscand_migrate_lock);
	if (!list_empty(&kmmscand_migrate_list.migrate_head)) {
		if (mm == READ_ONCE(kmmscand_cur_migrate_mm)) {
			/* A folio in this mm is being migrated. wait */
			WRITE_ONCE(kmmscand_migration_list_dirty, true);
		}

		list_for_each_entry_safe(info, tmp, &kmmscand_migrate_list.migrate_head,
			migrate_node) {
			if (info && (info->mm == mm)) {
				info->mm = NULL;
				WRITE_ONCE(kmmscand_migration_list_dirty, true);
			}
		}
	}
	spin_unlock(&kmmscand_migrate_lock);
}

static void kmmscand_collect_mm_slot(struct kmmscand_mm_slot *mm_slot)
{
	struct mm_slot *slot = &mm_slot->slot;
	struct mm_struct *mm = slot->mm;

	lockdep_assert_held(&kmmscand_mm_lock);

	if (kmmscand_test_exit(mm)) {
		/* free mm_slot */
		hash_del(&slot->hash);
		list_del(&slot->mm_node);

		kmmscand_cleanup_migration_list(mm);

		mm_slot_free(kmmscand_slot_cache, mm_slot);
		mmdrop(mm);
	}
}

static void kmmscand_migrate_folio(void)
{
	int ret = 0, dest = -1;
	struct kmmscand_migrate_info *info, *tmp;

	spin_lock(&kmmscand_migrate_lock);

	if (!list_empty(&kmmscand_migrate_list.migrate_head)) {
		list_for_each_entry_safe(info, tmp, &kmmscand_migrate_list.migrate_head,
			migrate_node) {
			if (READ_ONCE(kmmscand_migration_list_dirty)) {
				kmmscand_migration_list_dirty = false;
				list_del(&info->migrate_node);
				/*
				 * Do not try to migrate this entry because mm might have
				 * vanished underneath.
				 */
				kfree(info);
				spin_unlock(&kmmscand_migrate_lock);
				goto dirty_list_handled;
			}

			list_del(&info->migrate_node);
			/* Note down the mm of folio entry we are migrating */
			WRITE_ONCE(kmmscand_cur_migrate_mm, info->mm);
			spin_unlock(&kmmscand_migrate_lock);

			if (info->mm) {
				dest = kmmscand_get_target_node(NULL);
				ret = kmmscand_promote_folio(info, dest);
			}

			kfree(info);

			spin_lock(&kmmscand_migrate_lock);
			/* Reset  mm  of folio entry we are migrating */
			WRITE_ONCE(kmmscand_cur_migrate_mm, NULL);
			spin_unlock(&kmmscand_migrate_lock);
dirty_list_handled:
			cond_resched();
			spin_lock(&kmmscand_migrate_lock);
		}
	}
	spin_unlock(&kmmscand_migrate_lock);
}

/*
 * This is the normal change percentage when old and new delta remain same.
 * i.e., either both positive or both zero.
 */
#define SCAN_PERIOD_TUNE_PERCENT	15

/* This is to change the scan_period aggressively when deltas are different */
#define SCAN_PERIOD_CHANGE_SCALE	3
/*
 * XXX: Hack to prevent unmigrated pages coming again and again while scanning.
 * Actual fix needs to identify the type of unmigrated pages OR consider migration
 * failures in next scan.
 */
#define KMMSCAND_IGNORE_SCAN_THR	256

#define SCAN_SIZE_CHANGE_SHIFT	1

/* Maintains stability of scan_period by decaying last time accessed pages */
#define SCAN_DECAY_SHIFT	4
/*
 * X : Number of useful pages in the last scan.
 * Y : Number of useful pages found in current scan.
 * Tuning scan_period:
 *	Initial scan_period is 2s.
 *	case 1: (X = 0, Y = 0)
 *		Increase scan_period by SCAN_PERIOD_TUNE_PERCENT.
 *	case 2: (X = 0, Y > 0)
 *		Decrease scan_period by (2 << SCAN_PERIOD_CHANGE_SCALE).
 *	case 3: (X > 0, Y = 0 )
 *		Increase scan_period by (2 << SCAN_PERIOD_CHANGE_SCALE).
 *	case 4: (X > 0, Y > 0)
 *		Decrease scan_period by SCAN_PERIOD_TUNE_PERCENT.
 * Tuning scan_size:
 * Initial scan_size is 4GB
 *	case 1: (X = 0, Y = 0)
 *		Decrease scan_size by (1 << SCAN_SIZE_CHANGE_SHIFT).
 *	case 2: (X = 0, Y > 0)
 *		scan_size = KMMSCAND_SCAN_SIZE_MAX
 *  case 3: (X > 0, Y = 0 )
 *		No change
 *  case 4: (X > 0, Y > 0)
 *		Increase scan_size by (1 << SCAN_SIZE_CHANGE_SHIFT).
 */
static inline void kmmscand_update_mmslot_info(struct kmmscand_mm_slot *mm_slot,
				unsigned long total)
{
	unsigned int scan_period;
	unsigned long now;
	unsigned long scan_size;
	unsigned long old_scan_delta;

	scan_size = mm_slot->scan_size;
	scan_period = mm_slot->scan_period;
	old_scan_delta = mm_slot->scan_delta;

	/* decay old value */
	total = (old_scan_delta >> SCAN_DECAY_SHIFT) + total;

	/* XXX: Hack to get rid of continuously failing/unmigrateable pages */
	if (total < KMMSCAND_IGNORE_SCAN_THR)
		total = 0;

	/*
	 * case 1: old_scan_delta and new delta are similar, (slow) TUNE_PERCENT used.
	 * case 2: old_scan_delta and new delta are different. (fast) CHANGE_SCALE used.
	 * TBD:
	 * 1. Further tune scan_period based on delta between last and current scan delta.
	 * 2. Optimize calculation
	 */
	if (!old_scan_delta && !total) {
		scan_period = (100 + SCAN_PERIOD_TUNE_PERCENT) * scan_period;
		scan_period /= 100;
		scan_size = scan_size >> SCAN_SIZE_CHANGE_SHIFT;
	} else if (old_scan_delta && total) {
		scan_period = (100 - SCAN_PERIOD_TUNE_PERCENT) * scan_period;
		scan_period /= 100;
		scan_size = scan_size << SCAN_SIZE_CHANGE_SHIFT;
	} else if (old_scan_delta && !total) {
		scan_period = scan_period << SCAN_PERIOD_CHANGE_SCALE;
	} else {
		scan_period = scan_period >> SCAN_PERIOD_CHANGE_SCALE;
		scan_size = KMMSCAND_SCAN_SIZE_MAX;
	}

	scan_period = clamp(scan_period, KMMSCAND_SCAN_PERIOD_MIN, KMMSCAND_SCAN_PERIOD_MAX);
	scan_size = clamp(scan_size, KMMSCAND_SCAN_SIZE_MIN, KMMSCAND_SCAN_SIZE_MAX);

	now = jiffies;
	mm_slot->next_scan = now + msecs_to_jiffies(scan_period);
	mm_slot->scan_period = scan_period;
	mm_slot->scan_size = scan_size;
	mm_slot->scan_delta = total;
}

static unsigned long kmmscand_scan_mm_slot(void)
{
	bool next_mm = false;
	bool update_mmslot_info = false;

	unsigned int mm_slot_scan_period;
	unsigned long now;
	unsigned long mm_slot_next_scan;
	unsigned long mm_slot_scan_size;
	unsigned long vma_scanned_size = 0;
	unsigned long address;
	unsigned long total = 0;

	struct mm_slot *slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma = NULL;
	struct kmmscand_mm_slot *mm_slot;

	/* Retrieve mm */
	spin_lock(&kmmscand_mm_lock);

	if (kmmscand_scan.mm_slot) {
		mm_slot = kmmscand_scan.mm_slot;
		slot = &mm_slot->slot;
		address = mm_slot->address;
	} else {
		slot = list_entry(kmmscand_scan.mm_head.next,
				     struct mm_slot, mm_node);
		mm_slot = mm_slot_entry(slot, struct kmmscand_mm_slot, slot);
		address = mm_slot->address;
		kmmscand_scan.mm_slot = mm_slot;
	}

	mm = slot->mm;
	mm_slot->is_scanned = true;
	mm_slot_next_scan = mm_slot->next_scan;
	mm_slot_scan_period = mm_slot->scan_period;
	mm_slot_scan_size = mm_slot->scan_size;
	spin_unlock(&kmmscand_mm_lock);

	if (unlikely(!mmap_read_trylock(mm)))
		goto outerloop_mmap_lock;

	if (unlikely(kmmscand_test_exit(mm))) {
		next_mm = true;
		goto outerloop;
	}

	now = jiffies;

	if (mm_slot_next_scan && time_before(now, mm_slot_next_scan))
		goto outerloop;

	VMA_ITERATOR(vmi, mm, address);

	for_each_vma(vmi, vma) {
		kmmscand_walk_page_vma(vma, &kmmscand_scanctrl);
		vma_scanned_size += vma->vm_end - vma->vm_start;

		if (vma_scanned_size >= kmmscand_scan_size) {
			next_mm = true;
			/* Add scanned folios to migration list */
			spin_lock(&kmmscand_migrate_lock);
			list_splice_tail_init(&kmmscand_scanctrl.scan_list,
						&kmmscand_migrate_list.migrate_head);
			spin_unlock(&kmmscand_migrate_lock);
			break;
		}
		spin_lock(&kmmscand_migrate_lock);
		list_splice_tail_init(&kmmscand_scanctrl.scan_list,
					&kmmscand_migrate_list.migrate_head);
		spin_unlock(&kmmscand_migrate_lock);
	}

	if (!vma)
		address = 0;
	else
		address = kmmscand_scanctrl.address + PAGE_SIZE;

	update_mmslot_info = true;

	if (update_mmslot_info) {
		mm_slot->address = address;
		kmmscand_update_mmslot_info(mm_slot, total);
	}

outerloop:
	/* exit_mmap will destroy ptes after this */
	mmap_read_unlock(mm);

outerloop_mmap_lock:
	spin_lock(&kmmscand_mm_lock);
	WARN_ON(kmmscand_scan.mm_slot != mm_slot);

	/*
	 * Release the current mm_slot if this mm is about to die, or
	 * if we scanned all vmas of this mm.
	 */
	if (unlikely(kmmscand_test_exit(mm)) || !vma || next_mm) {
		/*
		 * Make sure that if mm_users is reaching zero while
		 * kmmscand runs here, kmmscand_exit will find
		 * mm_slot not pointing to the exiting mm.
		 */
		if (slot->mm_node.next != &kmmscand_scan.mm_head) {
			slot = list_entry(slot->mm_node.next,
					struct mm_slot, mm_node);
			kmmscand_scan.mm_slot =
				mm_slot_entry(slot, struct kmmscand_mm_slot, slot);

		} else
			kmmscand_scan.mm_slot = NULL;

		if (kmmscand_test_exit(mm)) {
			kmmscand_collect_mm_slot(mm_slot);
			goto end;
		}
	}
	mm_slot->is_scanned = false;
end:
	spin_unlock(&kmmscand_mm_lock);
	return 0;
}

static void kmmscand_do_scan(void)
{
	unsigned long iter = 0, mms_to_scan;

	mms_to_scan = READ_ONCE(kmmscand_mms_to_scan);

	while (true) {
		cond_resched();

		if (unlikely(kthread_should_stop()) ||
			!READ_ONCE(kmmscand_scan_enabled))
			break;

		if (kmmscand_has_work())
			kmmscand_scan_mm_slot();

		iter++;
		if (iter >= mms_to_scan)
			break;
	}
}

static int kmmscand(void *none)
{
	for (;;) {
		if (unlikely(kthread_should_stop()))
			break;

		kmmscand_do_scan();

		while (!READ_ONCE(kmmscand_scan_enabled)) {
			cpu_relax();
			kmmscand_wait_work();
		}

		kmmscand_wait_work();
	}
	return 0;
}

static inline void kmmscand_destroy(void)
{
	kmem_cache_destroy(kmmscand_slot_cache);
}

void __kmmscand_enter(struct mm_struct *mm)
{
	struct kmmscand_mm_slot *kmmscand_slot;
	struct mm_slot *slot;
	unsigned long now;
	int wakeup;

	/* __kmmscand_exit() must not run from under us */
	VM_BUG_ON_MM(kmmscand_test_exit(mm), mm);

	kmmscand_slot = mm_slot_alloc(kmmscand_slot_cache);

	if (!kmmscand_slot)
		return;

	now = jiffies;
	kmmscand_slot->address = 0;
	kmmscand_slot->scan_period = kmmscand_mm_scan_period_ms;
	kmmscand_slot->scan_size = kmmscand_scan_size;
	kmmscand_slot->next_scan = now +
			msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
	kmmscand_slot->scan_delta = 0;

	slot = &kmmscand_slot->slot;

	spin_lock(&kmmscand_mm_lock);
	mm_slot_insert(kmmscand_slots_hash, mm, slot);

	wakeup = list_empty(&kmmscand_scan.mm_head);
	list_add_tail(&slot->mm_node, &kmmscand_scan.mm_head);
	spin_unlock(&kmmscand_mm_lock);

	mmgrab(mm);
	if (wakeup)
		wake_up_interruptible(&kmmscand_wait);
}

void __kmmscand_exit(struct mm_struct *mm)
{
	struct kmmscand_mm_slot *mm_slot;
	struct mm_slot *slot;
	int free = 0, serialize = 1;

	spin_lock(&kmmscand_mm_lock);
	slot = mm_slot_lookup(kmmscand_slots_hash, mm);
	mm_slot = mm_slot_entry(slot, struct kmmscand_mm_slot, slot);
	if (mm_slot && kmmscand_scan.mm_slot != mm_slot) {
		hash_del(&slot->hash);
		list_del(&slot->mm_node);
		free = 1;
	} else if (mm_slot && kmmscand_scan.mm_slot == mm_slot && !mm_slot->is_scanned) {
		hash_del(&slot->hash);
		list_del(&slot->mm_node);
		free = 1;
		/* TBD: Set the actual next slot */
		kmmscand_scan.mm_slot = NULL;
	} else if (mm_slot && kmmscand_scan.mm_slot == mm_slot && mm_slot->is_scanned) {
		serialize = 0;
	}

	spin_unlock(&kmmscand_mm_lock);

	if (serialize)
		kmmscand_cleanup_migration_list(mm);

	if (free) {
		mm_slot_free(kmmscand_slot_cache, mm_slot);
		mmdrop(mm);
	} else if (mm_slot) {
		mmap_write_lock(mm);
		mmap_write_unlock(mm);
	}
}

static int start_kmmscand(void)
{
	int err = 0;

	guard(mutex)(&kmmscand_mutex);

	/* Some one already succeeded in starting daemon */
	if (kmmscand_thread)
		goto end;

	kmmscand_thread = kthread_run(kmmscand, NULL, "kmmscand");
	if (IS_ERR(kmmscand_thread)) {
		pr_err("kmmscand: kthread_run(kmmscand) failed\n");
		err = PTR_ERR(kmmscand_thread);
		kmmscand_thread = NULL;
		goto end;
	} else {
		pr_info("kmmscand: Successfully started kmmscand");
	}

	if (!list_empty(&kmmscand_scan.mm_head))
		wake_up_interruptible(&kmmscand_wait);

end:
	return err;
}

static int stop_kmmscand(void)
{
	int err = 0;

	guard(mutex)(&kmmscand_mutex);

	if (kmmscand_thread) {
		kthread_stop(kmmscand_thread);
		kmmscand_thread = NULL;
	}

	return err;
}
static int kmmmigrated(void *arg)
{
	for (;;) {
		WRITE_ONCE(migrated_need_wakeup, false);
		if (unlikely(kthread_should_stop()))
			break;
		if (kmmmigrated_has_work())
			kmmscand_migrate_folio();
		msleep(20);
		kmmmigrated_wait_work();
	}
	return 0;
}

static int start_kmmmigrated(void)
{
	int err = 0;

	guard(mutex)(&kmmmigrated_mutex);

	/* Someone already succeeded in starting daemon */
	if (kmmmigrated_thread)
		goto end;

	kmmmigrated_thread = kthread_run(kmmmigrated, NULL, "kmmmigrated");
	if (IS_ERR(kmmmigrated_thread)) {
		pr_err("kmmmigrated: kthread_run(kmmmigrated)  failed\n");
		err = PTR_ERR(kmmmigrated_thread);
		kmmmigrated_thread = NULL;
		goto end;
	} else {
		pr_info("kmmmigrated: Successfully started kmmmigrated");
	}

	wake_up_interruptible(&kmmmigrated_wait);
end:
	return err;
}

static int stop_kmmmigrated(void)
{
	guard(mutex)(&kmmmigrated_mutex);
	kthread_stop(kmmmigrated_thread);
	return 0;
}

static void init_list(void)
{
	INIT_LIST_HEAD(&kmmscand_migrate_list.migrate_head);
	INIT_LIST_HEAD(&kmmscand_scanctrl.scan_list);
	spin_lock_init(&kmmscand_migrate_lock);
	init_waitqueue_head(&kmmscand_wait);
	init_waitqueue_head(&kmmmigrated_wait);
}

static int __init kmmscand_init(void)
{
	int err;

	kmmscand_slot_cache = KMEM_CACHE(kmmscand_mm_slot, 0);

	if (!kmmscand_slot_cache) {
		pr_err("kmmscand: kmem_cache error");
		return -ENOMEM;
	}

	init_list();
	err = start_kmmscand();
	if (err)
		goto err_kmmscand;

	err = start_kmmmigrated();
	if (err)
		goto err_kmmmigrated;

	return 0;

err_kmmmigrated:
	stop_kmmmigrated();

err_kmmscand:
	stop_kmmscand();
	kmmscand_destroy();

	return err;
}
subsys_initcall(kmmscand_init);
