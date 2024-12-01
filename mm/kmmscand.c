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
#include <linux/cleanup.h>
#include <linux/minmax.h>
#include <trace/events/kmem.h>

#include <asm/pgalloc.h>
#include "internal.h"
#include "mm_slot.h"

static struct task_struct *kmmscand_thread __read_mostly;
static DEFINE_MUTEX(kmmscand_mutex);

/*
 * Total VMA size to cover during scan.
 * Min: 512MB default: 4GB max: 16GB
 */
#define KMMSCAND_SCAN_SIZE_MIN	(512 * 1024 * 1024UL)
#define KMMSCAND_SCAN_SIZE_MAX	(16 * 1024 * 1024 * 1024UL)
#define KMMSCAND_SCAN_SIZE	(4 * 1024 * 1024 * 1024UL)

static unsigned long kmmscand_scan_size __read_mostly = KMMSCAND_SCAN_SIZE;

/*
 * Scan period for each mm.
 * Min: 400ms default: 2sec Max: 5sec
 */
#define KMMSCAND_SCAN_PERIOD_MAX	5000U
#define KMMSCAND_SCAN_PERIOD_MIN	400U
#define KMMSCAND_SCAN_PERIOD		2000U

static unsigned int kmmscand_mm_scan_period_ms __read_mostly = KMMSCAND_SCAN_PERIOD;

/* How long to pause between two scan and migration cycle */
static unsigned int kmmscand_scan_sleep_ms __read_mostly = 16;

/* Max number of mms to scan in one scan and migration cycle */
#define KMMSCAND_MMS_TO_SCAN	(4 * 1024UL)
static unsigned long kmmscand_mms_to_scan __read_mostly = KMMSCAND_MMS_TO_SCAN;

volatile bool kmmscand_scan_enabled = true;
static bool need_wakeup;

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
};

struct kmmscand_scan {
	struct list_head mm_head;
	struct kmmscand_mm_slot *mm_slot;
};

struct kmmscand_scan kmmscand_scan = {
	.mm_head = LIST_HEAD_INIT(kmmscand_scan.mm_head),
};

struct kmmscand_migrate_list {
	struct list_head migrate_head;
};

struct kmmscand_migrate_list kmmscand_migrate_list = {
	.migrate_head = LIST_HEAD_INIT(kmmscand_migrate_list.migrate_head),
};

struct kmmscand_migrate_info {
	struct list_head migrate_node;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct folio *folio;
	unsigned long address;
};

#ifdef CONFIG_SYSFS
static ssize_t scan_sleep_ms_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", kmmscand_scan_sleep_ms);
}

static ssize_t scan_sleep_ms_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	unsigned int msecs;
	int err;

	err = kstrtouint(buf, 10, &msecs);
	if (err)
		return -EINVAL;

	kmmscand_scan_sleep_ms = msecs;
	kmmscand_sleep_expire = 0;
	wake_up_interruptible(&kmmscand_wait);

	return count;
}
static struct kobj_attribute scan_sleep_ms_attr =
	__ATTR_RW(scan_sleep_ms);

static ssize_t mm_scan_period_ms_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", kmmscand_mm_scan_period_ms);
}

/* If a value less than MIN or greater than MAX asked for store value is clamped */
static ssize_t mm_scan_period_ms_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	unsigned int msecs, stored_msecs;
	int err;

	err = kstrtouint(buf, 10, &msecs);
	if (err)
		return -EINVAL;

	stored_msecs = clamp(msecs, KMMSCAND_SCAN_PERIOD_MIN, KMMSCAND_SCAN_PERIOD_MAX);

	kmmscand_mm_scan_period_ms = stored_msecs;
	kmmscand_sleep_expire = 0;
	wake_up_interruptible(&kmmscand_wait);

	return count;
}

static struct kobj_attribute mm_scan_period_ms_attr =
	__ATTR_RW(mm_scan_period_ms);

static ssize_t mms_to_scan_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%lu\n", kmmscand_mms_to_scan);
}

static ssize_t mms_to_scan_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return -EINVAL;

	kmmscand_mms_to_scan = val;
	kmmscand_sleep_expire = 0;
	wake_up_interruptible(&kmmscand_wait);

	return count;
}

static struct kobj_attribute mms_to_scan_attr =
	__ATTR_RW(mms_to_scan);

static ssize_t scan_enabled_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", kmmscand_scan_enabled ? 1 : 0);
}

static ssize_t scan_enabled_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	unsigned int val;
	int err;

	err = kstrtouint(buf, 10, &val);
	if (err || val > 1)
		return -EINVAL;

	if (val) {
		kmmscand_scan_enabled = true;
		need_wakeup = true;
	} else
		kmmscand_scan_enabled = false;

	kmmscand_sleep_expire = 0;
	wake_up_interruptible(&kmmscand_wait);

	return count;
}

static struct kobj_attribute scan_enabled_attr =
	__ATTR_RW(scan_enabled);

static ssize_t target_node_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", kmmscand_target_node);
}

static ssize_t target_node_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	int err, node;

	err = kstrtoint(buf, 10, &node);
	if (err)
		return -EINVAL;

	kmmscand_sleep_expire = 0;
	if (!node_is_toptier(node))
		return -EINVAL;

	kmmscand_target_node = node;
	wake_up_interruptible(&kmmscand_wait);

	return count;
}
static struct kobj_attribute target_node_attr =
	__ATTR_RW(target_node);

static struct attribute *kmmscand_attr[] = {
	&scan_sleep_ms_attr.attr,
	&mm_scan_period_ms_attr.attr,
	&mms_to_scan_attr.attr,
	&scan_enabled_attr.attr,
	&target_node_attr.attr,
	NULL,
};

struct attribute_group kmmscand_attr_group = {
	.attrs = kmmscand_attr,
	.name = "kmmscand",
};
#endif

void count_kmmscand_mm_scans(void)
{
	count_vm_numa_event(KMMSCAND_MM_SCANS);
}
void count_kmmscand_vma_scans(void)
{
	count_vm_numa_event(KMMSCAND_VMA_SCANS);
}
void count_kmmscand_migadded(void)
{
	count_vm_numa_event(KMMSCAND_MIGADDED);
}
void count_kmmscand_migrated(void)
{
	count_vm_numa_event(KMMSCAND_MIGRATED);
}
void count_kmmscand_migrate_failed(void)
{
	count_vm_numa_event(KMMSCAND_MIGRATE_FAILED);
}
void count_kmmscand_slowtier(void)
{
	count_vm_numa_event(KMMSCAND_SLOWTIER);
}
void count_kmmscand_toptier(void)
{
	count_vm_numa_event(KMMSCAND_TOPTIER);
}
void count_kmmscand_idlepage(void)
{
	count_vm_numa_event(KMMSCAND_IDLEPAGE);
}

static int kmmscand_has_work(void)
{
	return !list_empty(&kmmscand_scan.mm_head);
}

static bool kmmscand_should_wakeup(void)
{
	bool wakeup =  kthread_should_stop() || need_wakeup ||
	       time_after_eq(jiffies, kmmscand_sleep_expire);
	if (need_wakeup)
		need_wakeup = false;

	return wakeup;
}

static void kmmscand_wait_work(void)
{
	if (kmmscand_has_work()) {
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
}

static bool kmmscand_eligible_srcnid(int nid)
{
	if (!node_is_toptier(nid))
		return true;
	return false;
}

/*
 * Do not know what info to pass in the future to make
 * decision on taget node. Keep it void * now.
 */
static int kmmscand_get_target_node(void *data)
{
	return kmmscand_target_node;
}

static int kmmscand_migrate_misplaced_folio_prepare(struct folio *folio,
		struct vm_area_struct *vma, int node)
{
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

	if (!folio_isolate_lru(folio))
		return -EAGAIN;

	return 0;
}

enum kmmscand_migration_err {
	KMMSCAND_NULL_MM = 1,
	KMMSCAND_INVALID_FOLIO,
	KMMSCAND_INVALID_VMA,
	KMMSCAND_INELIGIBLE_SRC_NODE,
	KMMSCAND_SAME_SRC_DEST_NODE,
	KMMSCAND_LRU_ISOLATION_ERR,
};

static int kmmscand_promote_folio(struct kmmscand_migrate_info *info)
{
	unsigned long pfn;
	struct page *page;
	struct folio *folio;
	struct vm_area_struct *vma;
	int ret;

	int srcnid, destnid;

	if (info->mm == NULL)
		return KMMSCAND_NULL_MM;

	folio = info->folio;

	/* Check again if the folio is really valid now */
	if (folio) {
		pfn = folio_pfn(folio);
		page = pfn_to_online_page(pfn);
	}

	if (!page || !folio || !folio_test_lru(folio) ||
		folio_is_zone_device(folio) || !folio_mapped(folio))
		return KMMSCAND_INVALID_FOLIO;

	vma = info->vma;

	/* XXX: Need to validate vma here?. vma_lookup() results in 2x regression */
	if (!vma)
		return KMMSCAND_INVALID_VMA;

	srcnid = folio_nid(folio);

	/* Do not try to promote pages from regular nodes */
	if (!kmmscand_eligible_srcnid(srcnid))
		return KMMSCAND_INELIGIBLE_SRC_NODE;

	destnid = kmmscand_get_target_node(NULL);

	if (srcnid == destnid)
		return KMMSCAND_SAME_SRC_DEST_NODE;

	folio_get(folio);
	ret = kmmscand_migrate_misplaced_folio_prepare(folio, vma, destnid);
	if (ret) {
		folio_put(folio);
		return KMMSCAND_LRU_ISOLATION_ERR;
	}
	folio_put(folio);

	return  migrate_misplaced_folio(folio, vma, destnid);
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
	VM_BUG_ON_VMA(address == -EFAULT, vma);
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
	struct kmmscand_migrate_list *migrate_list = walk->private;
	int srcnid;

	pte_t pteval = ptep_get(pte);

	if (pte_none(pteval))
		return 1;
	vma = walk->vma;
	mm = vma->vm_mm;
	page = pte_page(*pte);

	page_idle_clear_pte_refs(page, pte, walk);

	folio = page_folio(page);
	folio_get(folio);

	if (!folio || folio_is_zone_device(folio)) {
		folio_put(folio);
		return 1;
	}

	srcnid = folio_nid(folio);

	if (node_is_toptier(srcnid))
		count_kmmscand_toptier();

	if (!folio_test_idle(folio) || folio_test_young(folio) ||
			mmu_notifier_test_young(mm, addr) ||
			folio_test_referenced(folio) || pte_young(pteval)) {

		/* Do not try to promote pages from regular nodes */
		if (!kmmscand_eligible_srcnid(srcnid))
			goto end;

		info = kzalloc(sizeof(struct kmmscand_migrate_info), GFP_KERNEL);
		if (info && migrate_list) {

			count_kmmscand_slowtier();
			info->mm = mm;
			info->vma = vma;
			info->folio = folio;

			spin_lock(&kmmscand_migrate_lock);
			list_add_tail(&info->migrate_node, &migrate_list->migrate_head);
			spin_unlock(&kmmscand_migrate_lock);

			/*
			 * XXX: Should nr_accessed be per vma for finer control?
			 * XXX: We are increamenting atomic var under mmap_readlock
			 */
			atomic_long_inc(&mm->nr_accessed);
			count_kmmscand_migadded();
		}
	} else
		count_kmmscand_idlepage();
end:
	folio_set_idle(folio);
	folio_put(folio);
	return 0;
}

static const struct mm_walk_ops hot_vma_set_idle_ops = {
	.pte_entry = hot_vma_idle_pte_entry,
	.walk_lock = PGWALK_RDLOCK,
};

static void kmmscand_walk_page_vma(struct vm_area_struct *vma)
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

	walk_page_vma(vma, &hot_vma_set_idle_ops, &kmmscand_migrate_list);
}

static inline int kmmscand_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
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

		mm_slot_free(kmmscand_slot_cache, mm_slot);
		mmdrop(mm);
	}
}

static void kmmscand_cleanup_migration_list(struct mm_struct *mm)
{
	struct kmmscand_migrate_info *info, *tmp;

start_again:
	spin_lock(&kmmscand_migrate_lock);
	if (!list_empty(&kmmscand_migrate_list.migrate_head)) {

		if (mm == READ_ONCE(kmmscand_cur_migrate_mm)) {
			/* A folio in this mm is being migrated. wait */
			WRITE_ONCE(kmmscand_migration_list_dirty, true);
			spin_unlock(&kmmscand_migrate_lock);
			goto start_again;
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

static void kmmscand_migrate_folio(void)
{
	int ret = 0;
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

			if (info->mm)
				ret = kmmscand_promote_folio(info);

			/* TBD: encode migrated count here, currently assume folio_nr_pages */
			if (!ret)
				count_kmmscand_migrated();
			else
				count_kmmscand_migrate_failed();

			kfree(info);

			spin_lock(&kmmscand_migrate_lock);
			/* Reset  mm  of folio entry we are migrating */
			WRITE_ONCE(kmmscand_cur_migrate_mm, NULL);
			spin_unlock(&kmmscand_migrate_lock);
dirty_list_handled:
			//cond_resched();
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
#define KMMSCAND_IGNORE_SCAN_THR	100

#define SCAN_SIZE_CHANGE_SCALE	1
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
 *		Decrease scan_size by (1 << SCAN_SIZE_CHANGE_SCALE).
 *	case 2: (X = 0, Y > 0)
 *		scan_size = KMMSCAND_SCAN_SIZE_MAX
 *  case 3: (X > 0, Y = 0 )
 *		No change
 *  case 4: (X > 0, Y > 0)
 *		Increase scan_size by (1 << SCAN_SIZE_CHANGE_SCALE).
 */
static inline void kmmscand_update_mmslot_info(struct kmmscand_mm_slot *mm_slot, unsigned long total)
{
	unsigned int scan_period;
	unsigned long now;
	unsigned long scan_size;
	unsigned long old_scan_delta;

	/* XXX: Hack to get rid of continuously failing/unmigrateable pages */
	if (total < KMMSCAND_IGNORE_SCAN_THR)
		total = 0;

	scan_period = mm_slot->scan_period;
	scan_size = mm_slot->scan_size;

	old_scan_delta = mm_slot->scan_delta;

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
		scan_size = scan_size >> SCAN_SIZE_CHANGE_SCALE;
	} else if (old_scan_delta && total) {
		scan_period = (100 - SCAN_PERIOD_TUNE_PERCENT) * scan_period;
		scan_period /= 100;
		scan_size = scan_size << SCAN_SIZE_CHANGE_SCALE;
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
	unsigned long scanned_size = 0;
	unsigned long address;
	unsigned long folio_nr_access_s, folio_nr_access_e, total = 0;

	struct mm_slot *slot;
	struct mm_struct *mm;
	struct vma_iterator vmi;
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

	mm_slot_next_scan = mm_slot->next_scan;
	mm_slot_scan_period = mm_slot->scan_period;
	mm_slot_scan_size = mm_slot->scan_size;
	mm = slot->mm;

	spin_unlock(&kmmscand_mm_lock);

	if (unlikely(!mmap_read_trylock(mm)))
		goto outerloop_mmap_lock;

	if (unlikely(kmmscand_test_exit(mm))) {
		next_mm = true;
		goto outerloop;
	}

	now = jiffies;
	/*
	 * Dont scan if :
	 * This is not a first scan AND
	 * Reaching here before designated next_scan time.
	 */
	if (mm_slot_next_scan && time_before(now, mm_slot_next_scan))
		goto outerloop;

	folio_nr_access_s = atomic_long_read(&mm->nr_accessed);

	vma_iter_init(&vmi, mm, address);

	for_each_vma(vmi, vma) {
		/* Count the scanned pages here to decide exit */
		kmmscand_walk_page_vma(vma);
		count_kmmscand_vma_scans();
		scanned_size += vma->vm_end - vma->vm_start;
		address = vma->vm_end;

		if (scanned_size >= mm_slot_scan_size) {
			folio_nr_access_e = atomic_long_read(&mm->nr_accessed);
			total = folio_nr_access_e - folio_nr_access_s;
			/* If we had got accessed pages, ignore the current scan_size threshold */
			if (total > KMMSCAND_IGNORE_SCAN_THR) {
				mm_slot_scan_size = KMMSCAND_SCAN_SIZE_MAX;
				continue;
			}
			next_mm = true;
			break;
		}
	}
	folio_nr_access_e = atomic_long_read(&mm->nr_accessed);
	total = folio_nr_access_e - folio_nr_access_s;

	if (!vma)
		address = 0;

	update_mmslot_info = true;

	count_kmmscand_mm_scans();

outerloop:
	/* exit_mmap will destroy ptes after this */
	mmap_read_unlock(mm);

outerloop_mmap_lock:
	spin_lock(&kmmscand_mm_lock);
	VM_BUG_ON(kmmscand_scan.mm_slot != mm_slot);


	if (update_mmslot_info) {
		mm_slot->address = address;
		kmmscand_update_mmslot_info(mm_slot, total);
	}

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

		if (kmmscand_test_exit(mm))
			kmmscand_collect_mm_slot(mm_slot);
	}

	spin_unlock(&kmmscand_mm_lock);
	return total;
}

static void kmmscand_do_scan(void)
{
	unsigned long iter = 0, mms_to_scan;

	mms_to_scan = READ_ONCE(kmmscand_mms_to_scan);

	while (true) {
		cond_resched();

		if (unlikely(kthread_should_stop()) || !READ_ONCE(kmmscand_scan_enabled))
			break;

		if (kmmscand_has_work())
			kmmscand_scan_mm_slot();

		kmmscand_migrate_folio();
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

#ifdef CONFIG_SYSFS
extern struct kobject *mm_kobj;
static int __init kmmscand_init_sysfs(struct kobject **kobj)
{
	int err;

	err = sysfs_create_group(*kobj, &kmmscand_attr_group);
	if (err) {
		pr_err("failed to register kmmscand group\n");
		goto err_kmmscand_attr;
	}

	return 0;

err_kmmscand_attr:
	sysfs_remove_group(*kobj, &kmmscand_attr_group);
	return err;
}

static void __init kmmscand_exit_sysfs(struct kobject *kobj)
{
		sysfs_remove_group(kobj, &kmmscand_attr_group);
}
#else
static inline int __init kmmscand_init_sysfs(struct kobject **kobj)
{
	return 0;
}
static inline void __init kmmscand_exit_sysfs(struct kobject *kobj)
{
}
#endif

static inline void kmmscand_destroy(void)
{
	kmem_cache_destroy(kmmscand_slot_cache);
	kmmscand_exit_sysfs(mm_kobj);
}

void __kmmscand_enter(struct mm_struct *mm)
{
	struct kmmscand_mm_slot *kmmscand_slot;
	struct mm_slot *slot;
	int wakeup;

	/* __kmmscand_exit() must not run from under us */
	VM_BUG_ON_MM(kmmscand_test_exit(mm), mm);

	kmmscand_slot = mm_slot_alloc(kmmscand_slot_cache);

	if (!kmmscand_slot)
		return;

	kmmscand_slot->address = 0;
	kmmscand_slot->scan_period = kmmscand_mm_scan_period_ms;
	kmmscand_slot->scan_size = kmmscand_scan_size;
	kmmscand_slot->next_scan = 0;
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
	int free = 0;

	spin_lock(&kmmscand_mm_lock);
	slot = mm_slot_lookup(kmmscand_slots_hash, mm);
	mm_slot = mm_slot_entry(slot, struct kmmscand_mm_slot, slot);
	if (mm_slot && kmmscand_scan.mm_slot != mm_slot) {
		hash_del(&slot->hash);
		list_del(&slot->mm_node);
		free = 1;
	}

	spin_unlock(&kmmscand_mm_lock);

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

static int __init kmmscand_init(void)
{
	int err;

	kmmscand_slot_cache = KMEM_CACHE(kmmscand_mm_slot, 0);

	if (!kmmscand_slot_cache) {
		pr_err("kmmscand: kmem_cache error");
		return -ENOMEM;
	}

	err = kmmscand_init_sysfs(&mm_kobj);

	if (err)
		goto err_init_sysfs;

	err = start_kmmscand();
	if (err)
		goto err_kmmscand;

	return 0;

err_kmmscand:
	stop_kmmscand();
err_init_sysfs:
	kmmscand_destroy();

	return err;
}
subsys_initcall(kmmscand_init);
