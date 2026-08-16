// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
#include <linux/kthread.h>
#include <linux/khugepaged.h>
#include <linux/freezer.h>
#include <linux/mman.h>
#include <linux/hashtable.h>
#include <linux/userfaultfd_k.h>
#include <linux/page_idle.h>
#include <linux/page_table_check.h>
#include <linux/rcupdate_wait.h>
#include <linux/leafops.h>
#include <linux/shmem_fs.h>
#include <linux/dax.h>
#include <linux/ksm.h>
#include <linux/pgalloc.h>
#include <linux/backing-dev.h>
#include <linux/cleanup.h>

#include <asm/tlb.h>
#include "collapse.h"
#include "internal.h"
#include "page_alloc.h"
#include "mm_slot.h"

#include <trace/events/collapse.h>

static struct task_struct *khugepaged_thread __read_mostly;
static DEFINE_MUTEX(khugepaged_mutex);

/*
 * default scan 8*HPAGE_PMD_NR ptes, pte_mapped_hugepage, pmd_mapped,
 * no_pte_table or vmas every 10 second.
 */
static unsigned int khugepaged_pages_to_scan __read_mostly;
static unsigned int khugepaged_pages_collapsed;
static unsigned int khugepaged_full_scans;
static unsigned int khugepaged_scan_sleep_millisecs __read_mostly = 10000;
/* during fragmentation poll the hugepage allocator once every minute */
static unsigned int khugepaged_alloc_sleep_millisecs __read_mostly = 60000;
static unsigned long khugepaged_sleep_expire;
static DEFINE_SPINLOCK(khugepaged_mm_lock);
static DECLARE_WAIT_QUEUE_HEAD(khugepaged_wait);
/*
 * default collapse hugepages if there is at least one pte mapped like
 * it would have happened if the vma was large enough during page
 * fault.
 *
 * Note that these are only respected if collapse was initiated by khugepaged.
 */
unsigned int khugepaged_max_ptes_none __read_mostly;
static unsigned int khugepaged_max_ptes_swap __read_mostly;
static unsigned int khugepaged_max_ptes_shared __read_mostly;

#define MM_SLOTS_HASH_BITS 10
static DEFINE_READ_MOSTLY_HASHTABLE(mm_slots_hash, MM_SLOTS_HASH_BITS);

static struct kmem_cache *mm_slot_cache __ro_after_init;

/**
 * struct khugepaged_scan - cursor for scanning
 * @mm_head: the head of the mm list to scan
 * @mm_slot: the current mm_slot we are scanning
 * @address: the next address inside that to be scanned
 *
 * There is only the one khugepaged_scan instance of this cursor structure.
 */
struct khugepaged_scan {
	struct list_head mm_head;
	struct mm_slot *mm_slot;
	unsigned long address;
};

static struct khugepaged_scan khugepaged_scan = {
	.mm_head = LIST_HEAD_INIT(khugepaged_scan.mm_head),
};

#ifdef CONFIG_SYSFS
static ssize_t scan_sleep_millisecs_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", khugepaged_scan_sleep_millisecs);
}

static ssize_t __sleep_millisecs_store(const char *buf, size_t count,
				       unsigned int *millisecs)
{
	unsigned int msecs;
	int err;

	err = kstrtouint(buf, 10, &msecs);
	if (err)
		return -EINVAL;

	*millisecs = msecs;
	khugepaged_sleep_expire = 0;
	wake_up_interruptible(&khugepaged_wait);

	return count;
}

static ssize_t scan_sleep_millisecs_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	return __sleep_millisecs_store(buf, count, &khugepaged_scan_sleep_millisecs);
}
static struct kobj_attribute scan_sleep_millisecs_attr =
	__ATTR_RW(scan_sleep_millisecs);

static ssize_t alloc_sleep_millisecs_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	return sysfs_emit(buf, "%u\n", khugepaged_alloc_sleep_millisecs);
}

static ssize_t alloc_sleep_millisecs_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	return __sleep_millisecs_store(buf, count, &khugepaged_alloc_sleep_millisecs);
}
static struct kobj_attribute alloc_sleep_millisecs_attr =
	__ATTR_RW(alloc_sleep_millisecs);

static ssize_t pages_to_scan_show(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  char *buf)
{
	return sysfs_emit(buf, "%u\n", khugepaged_pages_to_scan);
}
static ssize_t pages_to_scan_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned int pages;
	int err;

	err = kstrtouint(buf, 10, &pages);
	if (err || !pages)
		return -EINVAL;

	khugepaged_pages_to_scan = pages;

	return count;
}
static struct kobj_attribute pages_to_scan_attr =
	__ATTR_RW(pages_to_scan);

static ssize_t pages_collapsed_show(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *buf)
{
	return sysfs_emit(buf, "%u\n", khugepaged_pages_collapsed);
}
static struct kobj_attribute pages_collapsed_attr =
	__ATTR_RO(pages_collapsed);

static ssize_t full_scans_show(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       char *buf)
{
	return sysfs_emit(buf, "%u\n", khugepaged_full_scans);
}
static struct kobj_attribute full_scans_attr =
	__ATTR_RO(full_scans);

static ssize_t defrag_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	return single_hugepage_flag_show(kobj, attr, buf,
					 TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG);
}
static ssize_t defrag_store(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	return single_hugepage_flag_store(kobj, attr, buf, count,
				 TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG);
}
static struct kobj_attribute khugepaged_defrag_attr =
	__ATTR_RW(defrag);

/*
 * max_ptes_none controls if khugepaged should collapse hugepages over
 * any unmapped ptes in turn potentially increasing the memory
 * footprint of the vmas. When max_ptes_none is 0 khugepaged will not
 * reduce the available free memory in the system as it
 * runs. Increasing max_ptes_none will instead potentially reduce the
 * free memory in the system during the khugepaged scan.
 */
static ssize_t max_ptes_none_show(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  char *buf)
{
	return sysfs_emit(buf, "%u\n", khugepaged_max_ptes_none);
}
static ssize_t max_ptes_none_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int err;
	unsigned long max_ptes_none;

	err = kstrtoul(buf, 10, &max_ptes_none);
	if (err || max_ptes_none > COLLAPSE_MAX_PTES_LIMIT)
		return -EINVAL;

	khugepaged_max_ptes_none = max_ptes_none;

	return count;
}
static struct kobj_attribute khugepaged_max_ptes_none_attr =
	__ATTR_RW(max_ptes_none);

static ssize_t max_ptes_swap_show(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  char *buf)
{
	return sysfs_emit(buf, "%u\n", khugepaged_max_ptes_swap);
}

static ssize_t max_ptes_swap_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int err;
	unsigned long max_ptes_swap;

	err  = kstrtoul(buf, 10, &max_ptes_swap);
	if (err || max_ptes_swap > COLLAPSE_MAX_PTES_LIMIT)
		return -EINVAL;

	khugepaged_max_ptes_swap = max_ptes_swap;

	return count;
}

static struct kobj_attribute khugepaged_max_ptes_swap_attr =
	__ATTR_RW(max_ptes_swap);

static ssize_t max_ptes_shared_show(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *buf)
{
	return sysfs_emit(buf, "%u\n", khugepaged_max_ptes_shared);
}

static ssize_t max_ptes_shared_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int err;
	unsigned long max_ptes_shared;

	err  = kstrtoul(buf, 10, &max_ptes_shared);
	if (err || max_ptes_shared > COLLAPSE_MAX_PTES_LIMIT)
		return -EINVAL;

	khugepaged_max_ptes_shared = max_ptes_shared;

	return count;
}

static struct kobj_attribute khugepaged_max_ptes_shared_attr =
	__ATTR_RW(max_ptes_shared);

static struct attribute *khugepaged_attr[] = {
	&khugepaged_defrag_attr.attr,
	&khugepaged_max_ptes_none_attr.attr,
	&khugepaged_max_ptes_swap_attr.attr,
	&khugepaged_max_ptes_shared_attr.attr,
	&pages_to_scan_attr.attr,
	&pages_collapsed_attr.attr,
	&full_scans_attr.attr,
	&scan_sleep_millisecs_attr.attr,
	&alloc_sleep_millisecs_attr.attr,
	NULL,
};

struct attribute_group khugepaged_attr_group = {
	.attrs = khugepaged_attr,
	.name = "khugepaged",
};
#endif /* CONFIG_SYSFS */

int hugepage_madvise(struct vm_area_struct *vma,
		     vm_flags_t *vm_flags, int advice)
{
	switch (advice) {
	case MADV_HUGEPAGE:
		*vm_flags &= ~VM_NOHUGEPAGE;
		*vm_flags |= VM_HUGEPAGE;
		/*
		 * If the vma become good for khugepaged to scan,
		 * register it here without waiting a page fault that
		 * may not happen any time soon.
		 */
		khugepaged_enter_vma(vma, *vm_flags);
		break;
	case MADV_NOHUGEPAGE:
		*vm_flags &= ~VM_HUGEPAGE;
		*vm_flags |= VM_NOHUGEPAGE;
		/*
		 * Setting VM_NOHUGEPAGE will prevent khugepaged from scanning
		 * this vma even if we leave the mm registered in khugepaged if
		 * it got registered before VM_NOHUGEPAGE was set.
		 */
		break;
	}

	return 0;
}

int __init khugepaged_init(void)
{
	mm_slot_cache = KMEM_CACHE(mm_slot, 0);
	if (!mm_slot_cache)
		return -ENOMEM;

	khugepaged_pages_to_scan = HPAGE_PMD_NR * 8;
	khugepaged_max_ptes_none = COLLAPSE_MAX_PTES_LIMIT;
	khugepaged_max_ptes_swap = HPAGE_PMD_NR / 8;
	khugepaged_max_ptes_shared = HPAGE_PMD_NR / 2;

	return 0;
}

void __init khugepaged_destroy(void)
{
	kmem_cache_destroy(mm_slot_cache);
}

static inline bool anon_hpage_enabled(void)
{
	if (READ_ONCE(huge_anon_orders_always))
		return true;
	if (READ_ONCE(huge_anon_orders_madvise))
		return true;
	if (READ_ONCE(huge_anon_orders_inherit) &&
	    hugepage_global_enabled())
		return true;
	return false;
}

static bool hugepage_enabled(void)
{
	/*
	 * We cover the anon, shmem and the file-backed case here; file-backed
	 * hugepages are determined by the global control.
	 * Anon hugepages are determined by its per-size mTHP control.
	 * Shmem pmd-sized hugepages are also determined by its pmd-size control,
	 * except when the global shmem_huge is set to SHMEM_HUGE_DENY.
	 */
	if (hugepage_global_enabled())
		return true;
	if (anon_hpage_enabled())
		return true;
	if (shmem_hpage_pmd_enabled())
		return true;
	return false;
}

void __khugepaged_enter(struct mm_struct *mm)
{
	struct mm_slot *slot;
	int wakeup;

	/* __khugepaged_exit() must not run from under us */
	VM_BUG_ON_MM(collapse_test_exit(mm), mm);

	slot = mm_slot_alloc(mm_slot_cache);
	if (!slot)
		return;

	if (unlikely(mm_flags_test_and_set(MMF_VM_HUGEPAGE, mm))) {
		mm_slot_free(mm_slot_cache, slot);
		return;
	}

	spin_lock(&khugepaged_mm_lock);
	mm_slot_insert(mm_slots_hash, mm, slot);
	/*
	 * Insert just behind the scanning cursor, to let the area settle
	 * down a little.
	 */
	wakeup = list_empty(&khugepaged_scan.mm_head);
	list_add_tail(&slot->mm_node, &khugepaged_scan.mm_head);
	spin_unlock(&khugepaged_mm_lock);

	mmgrab(mm);
	if (wakeup)
		wake_up_interruptible(&khugepaged_wait);
}

static bool collapse_possible(struct vm_area_struct *vma,
		vm_flags_t vm_flags, enum tva_type tva_flags)
{
	return collapse_possible_orders(vma, vm_flags, tva_flags);
}

void khugepaged_enter_vma(struct vm_area_struct *vma,
			  vm_flags_t vm_flags)
{
	if (!mm_flags_test(MMF_VM_HUGEPAGE, vma->vm_mm) && hugepage_enabled()
	    && collapse_possible(vma, vm_flags, TVA_KHUGEPAGED))
		__khugepaged_enter(vma->vm_mm);
}

void __khugepaged_exit(struct mm_struct *mm)
{
	struct mm_slot *slot;
	int free = 0;

	spin_lock(&khugepaged_mm_lock);
	slot = mm_slot_lookup(mm_slots_hash, mm);
	if (slot && khugepaged_scan.mm_slot != slot) {
		mm_slot_remove(slot);
		free = 1;
	}
	spin_unlock(&khugepaged_mm_lock);

	if (free) {
		mm_flags_clear(MMF_VM_HUGEPAGE, mm);
		mm_slot_free(mm_slot_cache, slot);
		mmdrop(mm);
	} else if (slot) {
		/*
		 * This is required to serialize against
		 * collapse_test_exit() (which is guaranteed to run
		 * under mmap_lock read mode). Stop here (after we return all
		 * pagetables will be destroyed) until khugepaged has finished
		 * working on the pagetables under the mmap_lock.
		 */
		mmap_write_lock(mm);
		mmap_write_unlock(mm);
	}
}

static void khugepaged_alloc_sleep(void)
{
	DEFINE_WAIT(wait);

	add_wait_queue(&khugepaged_wait, &wait);
	__set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
	schedule_timeout(msecs_to_jiffies(khugepaged_alloc_sleep_millisecs));
	remove_wait_queue(&khugepaged_wait, &wait);
}

static struct collapse_control khugepaged_collapse_control;

#define khugepaged_defrag()					\
	(transparent_hugepage_flags &				\
	 (1<<TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG))

/* Defrag for khugepaged will enter direct reclaim/compaction if necessary */
static inline gfp_t alloc_hugepage_khugepaged_gfpmask(void)
{
	return khugepaged_defrag() ? GFP_TRANSHUGE : GFP_TRANSHUGE_LIGHT;
}

/* khugepaged collapses on its own initiative, so it obeys its own settings. */
static void collapse_policy_khugepaged(struct collapse_policy *p)
{
	p->max_ptes_none = READ_ONCE(khugepaged_max_ptes_none);
	p->max_ptes_swap = READ_ONCE(khugepaged_max_ptes_swap);
	p->max_ptes_shared = READ_ONCE(khugepaged_max_ptes_shared);
	p->strict_sub_pmd = true;
	p->skip_lazyfree = true;
	p->require_referenced = true;
	p->install_pmd = false;
	p->writeback_dirty = false;
	p->gfp = alloc_hugepage_khugepaged_gfpmask();
	p->tva_type = TVA_KHUGEPAGED;
}

static void collect_mm_slot(struct mm_slot *slot)
{
	struct mm_struct *mm = slot->mm;

	lockdep_assert_held(&khugepaged_mm_lock);

	if (collapse_test_exit(mm)) {
		/* free mm_slot */
		mm_slot_remove(slot);

		/*
		 * Not strictly needed because the mm exited already.
		 *
		 * mm_flags_clear(MMF_VM_HUGEPAGE, mm);
		 */

		/* khugepaged_mm_lock actually not necessary for the below */
		mm_slot_free(mm_slot_cache, slot);
		mmdrop(mm);
	}
}

static void collapse_scan_mm_slot(unsigned int progress_max,
		enum scan_result *result, struct collapse_control *cc)
	__releases(&khugepaged_mm_lock)
	__acquires(&khugepaged_mm_lock)
{
	struct vma_iterator vmi;
	struct mm_slot *slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned int progress_prev = cc->progress;

	lockdep_assert_held(&khugepaged_mm_lock);
	*result = SCAN_FAIL;

	if (khugepaged_scan.mm_slot) {
		slot = khugepaged_scan.mm_slot;
	} else {
		slot = list_first_entry(&khugepaged_scan.mm_head,
				     struct mm_slot, mm_node);
		khugepaged_scan.address = 0;
		khugepaged_scan.mm_slot = slot;
	}
	spin_unlock(&khugepaged_mm_lock);

	mm = slot->mm;
	/*
	 * Don't wait for semaphore (to avoid long wait times).  Just move to
	 * the next mm on the list.
	 */
	vma = NULL;
	if (unlikely(!mmap_read_trylock(mm)))
		goto breakouterloop_mmap_lock;

	cc->progress++;
	if (unlikely(collapse_test_exit_or_disable(mm)))
		goto breakouterloop;

	vma_iter_init(&vmi, mm, khugepaged_scan.address);
	for_each_vma(vmi, vma) {
		unsigned long hstart, hend, window;
		unsigned long orders;

		cond_resched();
		if (unlikely(collapse_test_exit_or_disable(mm))) {
			cc->progress++;
			break;
		}
		orders = collapse_possible_orders(vma, vma->vm_flags,
						  TVA_KHUGEPAGED);
		if (!orders) {
			cc->progress++;
			continue;
		}

		/*
		 * Coverage is rooted at windows of the largest order the VMA
		 * allows: below the PMD order that reaches VMAs a whole table
		 * would not fit in, and parts of a VMA that a whole table would
		 * leave out.
		 */
		window = PAGE_SIZE << __fls(orders);
		hstart = ALIGN(vma->vm_start, window);
		hend = ALIGN_DOWN(vma->vm_end, window);
		if (khugepaged_scan.address > hend) {
			cc->progress++;
			continue;
		}
		if (khugepaged_scan.address < hstart)
			khugepaged_scan.address = hstart;

		while (khugepaged_scan.address < hend) {
			unsigned long pmd_addr, range_end, start;

			/* One table's worth at most, and never past the VMA */
			pmd_addr = khugepaged_scan.address & HPAGE_PMD_MASK;
			range_end = min(hend, pmd_addr + HPAGE_PMD_SIZE);

			cond_resched();
			if (unlikely(collapse_test_exit_or_disable(mm)))
				goto breakouterloop;

			VM_WARN_ON_ONCE(khugepaged_scan.address < hstart);

			start = khugepaged_scan.address;
			/* move to next address */
			khugepaged_scan.address = range_end;

			/* If nothing to collapse, the lock is still ours */
			if (!collapse_scan_pmd(vma, start, range_end, cc, orders)) {
				*result = cc->scan_refusal;
				if (cc->progress >= progress_max)
					goto breakouterloop;
				continue;
			}

			/* collapse_run_pmd() takes its own locks, so give this up */
			mmap_read_unlock(mm);
			*result = collapse_run_pmd(mm, start, range_end, cc);
			if (*result == SCAN_SUCCEED)
				++khugepaged_pages_collapsed;
			goto breakouterloop_mmap_lock;
		}
	}
breakouterloop:
	mmap_read_unlock(mm); /* exit_mmap will destroy ptes after this */
breakouterloop_mmap_lock:

	spin_lock(&khugepaged_mm_lock);
	VM_BUG_ON(khugepaged_scan.mm_slot != slot);
	/*
	 * Release the current mm_slot if this mm is about to die, or
	 * if we scanned all vmas of this mm, or THP got disabled.
	 */
	if (collapse_test_exit_or_disable(mm) || !vma) {
		/*
		 * Make sure that if mm_users is reaching zero while
		 * khugepaged runs here, khugepaged_exit will find
		 * mm_slot not pointing to the exiting mm.
		 */
		if (!list_is_last(&slot->mm_node, &khugepaged_scan.mm_head)) {
			khugepaged_scan.mm_slot = list_next_entry(slot, mm_node);
			khugepaged_scan.address = 0;
		} else {
			khugepaged_scan.mm_slot = NULL;
			khugepaged_full_scans++;
		}

		collect_mm_slot(slot);
	}

	trace_mm_khugepaged_scan(mm, cc->progress - progress_prev,
				 khugepaged_scan.mm_slot == NULL);
}

static int khugepaged_has_work(void)
{
	return !list_empty(&khugepaged_scan.mm_head) && hugepage_enabled();
}

static int khugepaged_wait_event(void)
{
	return !list_empty(&khugepaged_scan.mm_head) ||
		kthread_should_stop();
}

static void khugepaged_do_scan(struct collapse_control *cc)
{
	const unsigned int progress_max = READ_ONCE(khugepaged_pages_to_scan);
	unsigned int pass_through_head = 0;
	bool wait = true;
	enum scan_result result = SCAN_SUCCEED;

	lru_add_drain_all();

	/* One policy for the whole pass, so every table is judged the same */
	collapse_policy_khugepaged(&cc->policy);

	cc->progress = 0;
	while (true) {
		cond_resched();

		if (unlikely(kthread_should_stop()))
			break;

		spin_lock(&khugepaged_mm_lock);
		if (!khugepaged_scan.mm_slot)
			pass_through_head++;
		if (khugepaged_has_work() &&
		    pass_through_head < 2)
			collapse_scan_mm_slot(progress_max, &result, cc);
		else
			cc->progress = progress_max;
		spin_unlock(&khugepaged_mm_lock);

		if (cc->progress >= progress_max)
			break;

		if (result == SCAN_ALLOC_HUGE_PAGE_FAIL) {
			/*
			 * If fail to allocate the first time, try to sleep for
			 * a while.  When hit again, cancel the scan.
			 */
			if (!wait)
				break;
			wait = false;
			khugepaged_alloc_sleep();
		}
	}
}

static bool khugepaged_should_wakeup(void)
{
	return kthread_should_stop() ||
	       time_after_eq(jiffies, khugepaged_sleep_expire);
}

static void khugepaged_wait_work(void)
{
	if (khugepaged_has_work()) {
		const unsigned long scan_sleep_jiffies =
			msecs_to_jiffies(khugepaged_scan_sleep_millisecs);

		if (!scan_sleep_jiffies)
			return;

		khugepaged_sleep_expire = jiffies + scan_sleep_jiffies;
		wait_event_freezable_timeout(khugepaged_wait,
					     khugepaged_should_wakeup(),
					     scan_sleep_jiffies);
		return;
	}

	if (hugepage_enabled())
		wait_event_freezable(khugepaged_wait, khugepaged_wait_event());
}

static int khugepaged(void *none)
{
	struct mm_slot *slot;

	set_freezable();
	set_user_nice(current, MAX_NICE);

	while (!kthread_should_stop()) {
		khugepaged_do_scan(&khugepaged_collapse_control);
		khugepaged_wait_work();
	}

	spin_lock(&khugepaged_mm_lock);
	slot = khugepaged_scan.mm_slot;
	khugepaged_scan.mm_slot = NULL;
	if (slot)
		collect_mm_slot(slot);
	spin_unlock(&khugepaged_mm_lock);
	return 0;
}

void set_recommended_min_free_kbytes(void)
{
	struct zone *zone;
	int nr_zones = 0;
	unsigned long recommended_min;

	if (!hugepage_enabled()) {
		calculate_min_free_kbytes();
		goto update_wmarks;
	}

	for_each_populated_zone(zone) {
		/*
		 * We don't need to worry about fragmentation of
		 * ZONE_MOVABLE since it only has movable pages.
		 */
		if (zone_idx(zone) > gfp_zone(GFP_USER))
			continue;

		nr_zones++;
	}

	/* Ensure 2 pageblocks are free to assist fragmentation avoidance */
	recommended_min = pageblock_nr_pages * nr_zones * 2;

	/*
	 * Make sure that on average at least two pageblocks are almost free
	 * of another type, one for a migratetype to fall back to and a
	 * second to avoid subsequent fallbacks of other types There are 3
	 * MIGRATE_TYPES we care about.
	 */
	recommended_min += pageblock_nr_pages * nr_zones *
			   MIGRATE_PCPTYPES * MIGRATE_PCPTYPES;

	/* don't ever allow to reserve more than 5% of the lowmem */
	recommended_min = min(recommended_min,
			      (unsigned long) nr_free_buffer_pages() / 20);
	recommended_min <<= (PAGE_SHIFT-10);

	if (recommended_min > min_free_kbytes) {
		if (user_min_free_kbytes >= 0)
			pr_info_ratelimited("raising min_free_kbytes from %d to %lu to help transparent hugepage allocations\n",
					    min_free_kbytes, recommended_min);

		min_free_kbytes = recommended_min;
	}

update_wmarks:
	setup_per_zone_wmarks();
}

int start_stop_khugepaged(void)
{
	guard(mutex)(&khugepaged_mutex);
	if (hugepage_enabled()) {
		if (!khugepaged_thread) {
			struct task_struct *new_thread;
			int err;

			/*
			 * The engine collapses out of its candidate array, so
			 * take it before starting the thread that needs it: a
			 * failure surfaces here rather than in the daemon.
			 */
			err = collapse_control_init(&khugepaged_collapse_control);
			if (err)
				return err;

			new_thread = kthread_run(khugepaged, NULL, "khugepaged");
			if (IS_ERR(new_thread)) {
				pr_err("khugepaged: kthread_run(khugepaged) failed\n");
				collapse_control_release(&khugepaged_collapse_control);
				return PTR_ERR(new_thread);
			}

			khugepaged_thread = new_thread;
		}

		if (!list_empty(&khugepaged_scan.mm_head))
			wake_up_interruptible(&khugepaged_wait);
	} else if (khugepaged_thread) {
		kthread_stop(khugepaged_thread);
		khugepaged_thread = NULL;
		collapse_control_release(&khugepaged_collapse_control);
	}
	set_recommended_min_free_kbytes();
	return 0;
}

void khugepaged_min_free_kbytes_update(void)
{
	guard(mutex)(&khugepaged_mutex);
	if (hugepage_enabled() && khugepaged_thread)
		set_recommended_min_free_kbytes();
}

bool current_is_khugepaged(void)
{
	return kthread_func(current) == khugepaged;
}
