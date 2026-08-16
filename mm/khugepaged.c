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

#define CREATE_TRACE_POINTS
#include <trace/events/huge_memory.h>

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

/* MADV_COLLAPSE was asked for explicitly, so it is not held to those. */
static void collapse_policy_forced(struct collapse_policy *p)
{
	p->max_ptes_none = HPAGE_PMD_NR;
	p->max_ptes_swap = HPAGE_PMD_NR;
	p->max_ptes_shared = HPAGE_PMD_NR;
	p->strict_sub_pmd = false;
	p->skip_lazyfree = false;
	p->require_referenced = false;
	p->install_pmd = true;
	p->writeback_dirty = true;
	p->gfp = GFP_TRANSHUGE;
	p->tva_type = TVA_FORCED_COLLAPSE;
}

/*
 * If mmap_lock temporarily dropped, revalidate vma
 * after taking the mmap_lock again.
 * Returns enum scan_result value.
 */

static enum scan_result hugepage_vma_revalidate(struct mm_struct *mm, unsigned long address,
		bool expect_anon, struct vm_area_struct **vmap,
		struct collapse_control *cc, unsigned int order)
{
	struct vm_area_struct *vma;
	enum tva_type type = cc->policy.tva_type;

	if (unlikely(collapse_test_exit_or_disable(mm)))
		return SCAN_ANY_PROCESS;

	*vmap = vma = find_vma(mm, address);
	if (!vma)
		return SCAN_VMA_NULL;

	/*
	 * We cannot collapse VMA regions that do not span the full PMD. This is
	 * due to the potential of the PMD being shared by another VMA leaving
	 * us vulnerable to a race condition. Always check the PMD order here to
	 * ensure its not shared by another VMA. We'd need to lock all VMAs in
	 * the PMD range to support this.
	 */
	if (!thp_vma_suitable_order(vma, address, PMD_ORDER))
		return SCAN_ADDRESS_RANGE;
	if (!thp_vma_allowable_orders(vma, vma->vm_flags, type, BIT(order)))
		return SCAN_VMA_CHECK;
	/*
	 * Anon VMA expected, the address may be unmapped then
	 * remapped to file after khugepaged reacquired the mmap_lock.
	 *
	 * thp_vma_allowable_orders() may return true for qualified file
	 * vmas.
	 */
	if (expect_anon && (!(*vmap)->anon_vma || !vma_is_anonymous(*vmap)))
		return SCAN_PAGE_ANON;
	return SCAN_SUCCEED;
}

static void count_collapse_event(unsigned int order, enum vm_event_item vm_event,
		enum mthp_stat_item mthp_event)
{
	if (is_pmd_order(order))
		count_vm_event(vm_event);
	count_mthp_stat(order, mthp_event);
}

static void collapse_control_init_scan(struct collapse_control *cc)
{
	memset(cc->node_load, 0, sizeof(cc->node_load));
	nodes_clear(cc->alloc_nmask);
	bitmap_zero(cc->eligible_ptes, MAX_PTRS_PER_PTE);
}

static enum scan_result alloc_charge_folio(struct folio **foliop, struct mm_struct *mm,
		struct collapse_control *cc, unsigned int order)
{
	gfp_t gfp = cc->policy.gfp;
	int node = collapse_find_target_node(cc);
	struct folio *folio;

	folio = __folio_alloc(gfp, order, node, &cc->alloc_nmask);
	if (!folio) {
		*foliop = NULL;
		count_collapse_event(order, THP_COLLAPSE_ALLOC_FAILED,
				     MTHP_STAT_COLLAPSE_ALLOC_FAILED);
		return SCAN_ALLOC_HUGE_PAGE_FAIL;
	}

	count_collapse_event(order, THP_COLLAPSE_ALLOC, MTHP_STAT_COLLAPSE_ALLOC);

	if (unlikely(mem_cgroup_charge(folio, mm, gfp))) {
		folio_put(folio);
		*foliop = NULL;
		return SCAN_CGROUP_CHARGE_FAIL;
	}

	if (is_pmd_order(order))
		count_memcg_folio_events(folio, THP_COLLAPSE_ALLOC, 1);

	*foliop = folio;
	return SCAN_SUCCEED;
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

/* folio must be locked, and mmap_lock must be held */
static enum scan_result set_huge_pmd(struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmdp, struct folio *folio, struct page *page)
{
	struct mm_struct *mm = vma->vm_mm;
	struct vm_fault vmf = {
		.vma = vma,
		.address = addr,
		.flags = 0,
	};
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;

	mmap_assert_locked(vma->vm_mm);

	if (!pmdp) {
		pgdp = pgd_offset(mm, addr);
		p4dp = p4d_alloc(mm, pgdp, addr);
		if (!p4dp)
			return SCAN_FAIL;
		pudp = pud_alloc(mm, p4dp, addr);
		if (!pudp)
			return SCAN_FAIL;
		pmdp = pmd_alloc(mm, pudp, addr);
		if (!pmdp)
			return SCAN_FAIL;
	}

	vmf.pmd = pmdp;
	if (do_set_pmd(&vmf, folio, page))
		return SCAN_FAIL;

	folio_get(folio);
	return SCAN_SUCCEED;
}

static enum scan_result try_collapse_pte_mapped_thp(struct mm_struct *mm, unsigned long addr,
		bool install_pmd)
{
	enum scan_result result = SCAN_FAIL;
	int nr_mapped_ptes = 0;
	unsigned int nr_batch_ptes;
	struct mmu_notifier_range range;
	bool notified = false;
	unsigned long haddr = addr & HPAGE_PMD_MASK;
	unsigned long end = haddr + HPAGE_PMD_SIZE;
	struct vm_area_struct *vma = vma_lookup(mm, haddr);
	struct folio *folio;
	pte_t *start_pte, *pte;
	pmd_t *pmd, pgt_pmd;
	spinlock_t *pml = NULL, *ptl;
	int i;

	mmap_assert_locked(mm);

	/* First check VMA found, in case page tables are being torn down */
	if (!vma || !vma->vm_file ||
	    !range_in_vma(vma, haddr, haddr + HPAGE_PMD_SIZE))
		return SCAN_VMA_CHECK;

	/* Fast check before locking page if already PMD-mapped */
	result = find_pmd_or_thp_or_none(mm, haddr, &pmd);
	if (result == SCAN_PMD_MAPPED)
		return result;

	/*
	 * If we are here, we've succeeded in replacing all the native pages
	 * in the page cache with a single hugepage. If a mm were to fault-in
	 * this memory (mapped by a suitably aligned VMA), we'd get the hugepage
	 * and map it by a PMD, regardless of sysfs THP settings. As such, let's
	 * analogously elide sysfs THP settings here and force collapse.
	 */
	if (!thp_vma_allowable_order(vma, vma->vm_flags, TVA_FORCED_COLLAPSE, PMD_ORDER))
		return SCAN_VMA_CHECK;

	/*
	 * Keep pmd pgtable while the uffd bit is in use; see comment in
	 * retract_page_tables().
	 */
	if (userfaultfd_protected(vma))
		return SCAN_PTE_UFFD;

	folio = filemap_lock_folio(vma->vm_file->f_mapping,
			       linear_page_index(vma, haddr));
	if (IS_ERR(folio))
		return SCAN_PAGE_NULL;

	if (!is_pmd_order(folio_order(folio))) {
		result = SCAN_PAGE_COMPOUND;
		goto drop_folio;
	}

	result = find_pmd_or_thp_or_none(mm, haddr, &pmd);
	switch (result) {
	case SCAN_SUCCEED:
		break;
	case SCAN_NO_PTE_TABLE:
		/*
		 * All pte entries have been removed and pmd cleared.
		 * Skip all the pte checks and just update the pmd mapping.
		 */
		goto maybe_install_pmd;
	default:
		goto drop_folio;
	}

	result = SCAN_FAIL;
	start_pte = pte_offset_map_lock(mm, pmd, haddr, &ptl);
	if (!start_pte)		/* mmap_lock + page lock should prevent this */
		goto drop_folio;

	/* step 1: check all mapped PTEs are to the right huge page */
	for (i = 0, addr = haddr, pte = start_pte;
	     i < HPAGE_PMD_NR; i++, addr += PAGE_SIZE, pte++) {
		struct page *page;
		pte_t ptent = ptep_get(pte);

		/* empty pte, skip */
		if (pte_none(ptent))
			continue;

		/* page swapped out, abort */
		if (!pte_present(ptent)) {
			result = SCAN_PTE_NON_PRESENT;
			goto abort;
		}

		page = vm_normal_page(vma, addr, ptent);
		if (WARN_ON_ONCE(page && is_zone_device_page(page)))
			page = NULL;
		/*
		 * Note that uprobe, debugger, or MAP_PRIVATE may change the
		 * page table, but the new page will not be a subpage of hpage.
		 */
		if (folio_page(folio, i) != page)
			goto abort;
	}

	pte_unmap_unlock(start_pte, ptl);
	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm,
				haddr, haddr + HPAGE_PMD_SIZE);
	mmu_notifier_invalidate_range_start(&range);
	notified = true;

	/*
	 * pmd_lock covers a wider range than ptl, and (if split from mm's
	 * page_table_lock) ptl nests inside pml. The less time we hold pml,
	 * the better; but userfaultfd's mfill_atomic_pte() on a private VMA
	 * inserts a valid as-if-COWed PTE without even looking up page cache.
	 * So page lock of folio does not protect from it, so we must not drop
	 * ptl before pgt_pmd is removed, so uffd private needs pml taken now.
	 */
	if (userfaultfd_armed(vma) && !(vma->vm_flags & VM_SHARED))
		pml = pmd_lock(mm, pmd);

	start_pte = pte_offset_map_rw_nolock(mm, pmd, haddr, &pgt_pmd, &ptl);
	if (!start_pte)		/* mmap_lock + page lock should prevent this */
		goto abort;
	if (!pml)
		spin_lock(ptl);
	else if (ptl != pml)
		spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);

	if (unlikely(!pmd_same(pgt_pmd, pmdp_get_lockless(pmd))))
		goto abort;

	/* step 2: clear page table and adjust rmap */
	for (i = 0, addr = haddr, pte = start_pte; i < HPAGE_PMD_NR;
	     i += nr_batch_ptes, addr += nr_batch_ptes * PAGE_SIZE,
	     pte += nr_batch_ptes) {
		unsigned int max_nr_batch_ptes = (end - addr) >> PAGE_SHIFT;
		struct page *page;
		pte_t ptent = ptep_get(pte);

		nr_batch_ptes = 1;

		if (pte_none(ptent))
			continue;
		/*
		 * We dropped ptl after the first scan, to do the mmu_notifier:
		 * page lock stops more PTEs of the folio being faulted in, but
		 * does not stop write faults COWing anon copies from existing
		 * PTEs; and does not stop those being swapped out or migrated.
		 */
		if (!pte_present(ptent)) {
			result = SCAN_PTE_NON_PRESENT;
			goto abort;
		}
		page = vm_normal_page(vma, addr, ptent);

		if (folio_page(folio, i) != page)
			goto abort;

		nr_batch_ptes = folio_pte_batch(folio, pte, ptent, max_nr_batch_ptes);

		/*
		 * Must clear entry, or a racing truncate may re-remove it.
		 * TLB flush can be left until pmdp_collapse_flush() does it.
		 * PTE dirty? Shmem page is already dirty; file is read-only.
		 */
		clear_ptes(mm, addr, pte, nr_batch_ptes);
		folio_remove_rmap_ptes(folio, page, nr_batch_ptes, vma);
		nr_mapped_ptes += nr_batch_ptes;
	}

	if (!pml)
		spin_unlock(ptl);

	/* step 3: set proper refcount and mm_counters. */
	if (nr_mapped_ptes) {
		folio_ref_sub(folio, nr_mapped_ptes);
		add_mm_counter(mm, mm_counter_file(folio), -nr_mapped_ptes);
	}

	/* step 4: remove empty page table */
	if (!pml) {
		pml = pmd_lock(mm, pmd);
		if (ptl != pml) {
			spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);
			if (unlikely(!pmd_same(pgt_pmd, pmdp_get_lockless(pmd)))) {
				flush_tlb_mm(mm);
				goto unlock;
			}
		}
	}
	pgt_pmd = pmdp_collapse_flush(vma, haddr, pmd);
	pmdp_get_lockless_sync();
	pte_unmap_unlock(start_pte, ptl);
	if (ptl != pml)
		spin_unlock(pml);

	mmu_notifier_invalidate_range_end(&range);

	mm_dec_nr_ptes(mm);
	page_table_check_pte_clear_range(mm, haddr, pgt_pmd);
	pte_free_defer(mm, pmd_pgtable(pgt_pmd));

maybe_install_pmd:
	/* step 5: install pmd entry */
	result = install_pmd
			? set_huge_pmd(vma, haddr, pmd, folio, &folio->page)
			: SCAN_SUCCEED;
	goto drop_folio;
abort:
	if (nr_mapped_ptes) {
		flush_tlb_mm(mm);
		folio_ref_sub(folio, nr_mapped_ptes);
		add_mm_counter(mm, mm_counter_file(folio), -nr_mapped_ptes);
	}
unlock:
	if (start_pte)
		pte_unmap_unlock(start_pte, ptl);
	if (pml && pml != ptl)
		spin_unlock(pml);
	if (notified)
		mmu_notifier_invalidate_range_end(&range);
drop_folio:
	folio_unlock(folio);
	folio_put(folio);
	return result;
}

/**
 * collapse_pte_mapped_thp - Try to collapse a pte-mapped THP for mm at
 * address haddr.
 *
 * @mm: process address space where collapse happens
 * @addr: THP collapse address
 * @install_pmd: If a huge PMD should be installed
 *
 * This function checks whether all the PTEs in the PMD are pointing to the
 * right THP. If so, retract the page table so the THP can refault in with
 * as pmd-mapped. Possibly install a huge PMD mapping the THP.
 */
void collapse_pte_mapped_thp(struct mm_struct *mm, unsigned long addr,
		bool install_pmd)
{
	try_collapse_pte_mapped_thp(mm, addr, install_pmd);
}

/* Can we retract page tables for this file-backed VMA? */
static bool file_backed_vma_is_retractable(struct vm_area_struct *vma)
{
	/*
	 * Check vma->anon_vma to exclude MAP_PRIVATE mappings that
	 * got written to. These VMAs are likely not worth removing
	 * page tables from, as PMD-mapping is likely to be split later.
	 */
	if (READ_ONCE(vma->anon_vma))
		return false;

	/*
	 * When a vma is registered with uffd-wp or RWP, we cannot recycle
	 * the page table because there may be pte markers installed.
	 * VM_UFFD_RWP ranges similarly rely on per-PTE uffd state
	 * and cannot be recycled to a shared PMD. Other vmas can still
	 * have the same file mapped hugely, but skip this one: it will
	 * always be mapped in small page size for these registrations.
	 */
	if (userfaultfd_protected(vma))
		return false;

	/*
	 * If the VMA contains guard regions then we can't collapse it.
	 *
	 * This is set atomically on guard marker installation under mmap/VMA
	 * read lock, and here we may not hold any VMA or mmap lock at all.
	 *
	 * This is therefore serialised on the PTE page table lock, which is
	 * obtained on guard region installation after the flag is set, so this
	 * check being performed under this lock excludes races.
	 */
	if (vma_test_atomic_flag(vma, VMA_MAYBE_GUARD_BIT))
		return false;

	return true;
}

static void retract_page_tables(struct address_space *mapping, pgoff_t pgoff)
{
	struct vm_area_struct *vma;

	i_mmap_lock_read(mapping);
	mapping_rmap_tree_foreach(vma, mapping, pgoff, pgoff) {
		struct mmu_notifier_range range;
		struct mm_struct *mm;
		unsigned long addr;
		pmd_t *pmd, pgt_pmd;
		spinlock_t *pml;
		spinlock_t *ptl;
		bool success = false;

		addr = vma->vm_start +
			((pgoff - vma_start_pgoff(vma)) << PAGE_SHIFT);
		if (addr & ~HPAGE_PMD_MASK ||
		    vma->vm_end < addr + HPAGE_PMD_SIZE)
			continue;

		mm = vma->vm_mm;
		if (find_pmd_or_thp_or_none(mm, addr, &pmd) != SCAN_SUCCEED)
			continue;

		if (collapse_test_exit(mm))
			continue;

		if (!file_backed_vma_is_retractable(vma))
			continue;

		/* PTEs were notified when unmapped; but now for the PMD? */
		mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm,
					addr, addr + HPAGE_PMD_SIZE);
		mmu_notifier_invalidate_range_start(&range);

		pml = pmd_lock(mm, pmd);
		/*
		 * The lock of new_folio is still held, we will be blocked in
		 * the page fault path, which prevents the pte entries from
		 * being set again. So even though the old empty PTE page may be
		 * concurrently freed and a new PTE page is filled into the pmd
		 * entry, it is still empty and can be removed.
		 *
		 * So here we only need to recheck if the state of pmd entry
		 * still meets our requirements, rather than checking pmd_same()
		 * like elsewhere.
		 */
		if (check_pmd_state(pmd) != SCAN_SUCCEED)
			goto drop_pml;
		ptl = pte_lockptr(mm, pmd);
		if (ptl != pml)
			spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);

		/*
		 * Huge page lock is still held, so normally the page table must
		 * remain empty; and we have already skipped anon_vma and
		 * userfaultfd_wp() vmas.  But since the mmap_lock is not held,
		 * it is still possible for a racing userfaultfd_ioctl() or
		 * madvise() to have inserted ptes or markers.  Now that we hold
		 * ptlock, repeating the retractable checks protects us from
		 * races against the prior checks.
		 */
		if (likely(file_backed_vma_is_retractable(vma))) {
			pgt_pmd = pmdp_collapse_flush(vma, addr, pmd);
			pmdp_get_lockless_sync();
			success = true;
		}

		if (ptl != pml)
			spin_unlock(ptl);
drop_pml:
		spin_unlock(pml);

		mmu_notifier_invalidate_range_end(&range);

		if (success) {
			mm_dec_nr_ptes(mm);
			page_table_check_pte_clear_range(mm, addr, pgt_pmd);
			pte_free_defer(mm, pmd_pgtable(pgt_pmd));
		}
	}
	i_mmap_unlock_read(mapping);
}

/**
 * collapse_file - collapse filemap/tmpfs/shmem pages into huge one.
 *
 * @mm: process address space where collapse happens
 * @addr: virtual collapse start address
 * @file: file that collapse on
 * @start: collapse start address
 * @cc: collapse context and scratchpad
 *
 * Basic scheme is simple, details are more complex:
 *  - allocate and lock a new huge page;
 *  - scan page cache, locking old pages
 *    + swap/gup in pages if necessary;
 *  - copy data to new page
 *  - handle shmem holes
 *    + re-validate that holes weren't filled by someone else
 *    + check for userfaultfd
 *  - finalize updates to the page cache;
 *  - if replacing succeeds:
 *    + unlock huge page;
 *    + free old pages;
 *  - if replacing failed;
 *    + unlock old pages
 *    + unlock and free huge page;
 */
static enum scan_result collapse_file(struct mm_struct *mm, unsigned long addr,
		struct file *file, pgoff_t start, struct collapse_control *cc)
{
	struct address_space *mapping = file->f_mapping;
	struct page *dst;
	struct folio *folio, *tmp, *new_folio;
	pgoff_t index = 0, end = start + HPAGE_PMD_NR;
	LIST_HEAD(pagelist);
	XA_STATE_ORDER(xas, &mapping->i_pages, start, HPAGE_PMD_ORDER);
	enum scan_result result = SCAN_SUCCEED;
	int nr_none = 0;
	bool is_shmem = shmem_file(file);

	/*
	 * MADV_COLLAPSE ignores shmem huge config, so do not check shmem
	 *
	 * TODO: once shmem always calls mapping_set_large_folios() on its
	 * mapping, the shmem check can be removed.
	 */
	VM_WARN_ON_ONCE(!is_shmem && !mapping_pmd_folio_support(mapping));
	VM_WARN_ON_ONCE(start & (HPAGE_PMD_NR - 1));

	result = alloc_charge_folio(&new_folio, mm, cc, HPAGE_PMD_ORDER);
	if (result != SCAN_SUCCEED)
		goto out;

	mapping_set_update(&xas, mapping);

	__folio_set_locked(new_folio);
	if (is_shmem)
		__folio_set_swapbacked(new_folio);
	new_folio->index = start;
	new_folio->mapping = mapping;

	/*
	 * Ensure we have slots for all the pages in the range.  This is
	 * almost certainly a no-op because most of the pages must be present
	 */
	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (!xas_error(&xas))
			break;
		xas_unlock_irq(&xas);
		if (!xas_nomem(&xas, GFP_KERNEL)) {
			result = SCAN_FAIL;
			goto rollback;
		}
	} while (1);

	for (index = start; index < end;) {
		xas_set(&xas, index);
		folio = xas_load(&xas);

		VM_BUG_ON(index != xas.xa_index);
		if (is_shmem) {
			if (!folio) {
				/*
				 * Stop if extent has been truncated or
				 * hole-punched, and is now completely
				 * empty.
				 */
				if (index == start) {
					if (!xas_next_entry(&xas, end - 1)) {
						result = SCAN_TRUNCATED;
						goto xa_locked;
					}
				}
				nr_none++;
				index++;
				continue;
			}

			if (xa_is_value(folio) || !folio_test_uptodate(folio)) {
				xas_unlock_irq(&xas);
				/* swap in or instantiate fallocated page */
				if (shmem_get_folio(mapping->host, index, 0,
						&folio, SGP_NOALLOC)) {
					result = SCAN_FAIL;
					goto xa_unlocked;
				}
				/* drain lru cache to help folio_isolate_lru() */
				lru_add_drain();
			} else if (folio_trylock(folio)) {
				folio_get(folio);
				xas_unlock_irq(&xas);
			} else {
				result = SCAN_PAGE_LOCK;
				goto xa_locked;
			}
		} else {	/* !is_shmem */
			if (!folio || xa_is_value(folio)) {
				xas_unlock_irq(&xas);
				page_cache_sync_readahead(mapping, &file->f_ra,
							  file, index,
							  end - index);
				/* drain lru cache to help folio_isolate_lru() */
				lru_add_drain();
				folio = filemap_lock_folio(mapping, index);
				if (IS_ERR(folio)) {
					result = SCAN_FAIL;
					goto xa_unlocked;
				}
			} else if (folio_test_dirty(folio)) {
				/*
				 * This page is dirty because it hasn't
				 * been flushed since first write.
				 *
				 * Trigger async flush for read-only files and
				 * hope the writeback is done when khugepaged
				 * revisits this page. Writable files can have
				 * their folios dirty at any time; blindly
				 * flushing them would cause undesirable
				 * system-wide writeback.
				 *
				 * This is a one-off situation. We are not
				 * forcing writeback in loop.
				 */
				xas_unlock_irq(&xas);
				if (!inode_is_open_for_write(mapping->host))
					filemap_flush(mapping);
				result = SCAN_PAGE_DIRTY_OR_WRITEBACK;
				goto xa_unlocked;
			} else if (folio_test_writeback(folio)) {
				xas_unlock_irq(&xas);
				result = SCAN_PAGE_DIRTY_OR_WRITEBACK;
				goto xa_unlocked;
			} else if (folio_trylock(folio)) {
				folio_get(folio);
				xas_unlock_irq(&xas);
			} else {
				result = SCAN_PAGE_LOCK;
				goto xa_locked;
			}
		}

		/*
		 * The folio must be locked, so we can drop the i_pages lock
		 * without racing with truncate.
		 */
		VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

		/* make sure the folio is up to date */
		if (unlikely(!folio_test_uptodate(folio))) {
			result = SCAN_FAIL;
			goto out_unlock;
		}

		/*
		 * If file was truncated then extended, or hole-punched, before
		 * we locked the first folio, then a THP might be there already.
		 * This will be discovered on the first iteration.
		 */
		if (is_pmd_order(folio_order(folio))) {
			result = SCAN_PTE_MAPPED_HUGEPAGE;
			goto out_unlock;
		}

		if (folio_mapping(folio) != mapping) {
			result = SCAN_TRUNCATED;
			goto out_unlock;
		}

		if (!is_shmem && (folio_test_dirty(folio) ||
				  folio_test_writeback(folio))) {
			/*
			 * khugepaged only works on clean file-backed folios,
			 * so this folio is dirty because it hasn't been flushed
			 * since first write.
			 */
			result = SCAN_PAGE_DIRTY_OR_WRITEBACK;
			goto out_unlock;
		}

		if (!folio_isolate_lru(folio)) {
			result = SCAN_DEL_PAGE_LRU;
			goto out_unlock;
		}

		if (!filemap_release_folio(folio, GFP_KERNEL)) {
			result = SCAN_PAGE_HAS_PRIVATE;
			folio_putback_lru(folio);
			goto out_unlock;
		}

		if (folio_mapped(folio))
			try_to_unmap(folio,
					TTU_IGNORE_MLOCK | TTU_BATCH_FLUSH);

		xas_lock_irq(&xas);

		VM_BUG_ON_FOLIO(folio != xa_load(xas.xa, index), folio);

		/*
		 * We control 2 + nr_pages references to the folio:
		 *  - we hold a pin on it;
		 *  - nr_pages reference from page cache;
		 *  - one from lru_isolate_folio;
		 * If those are the only references, then any new usage
		 * of the folio will have to fetch it from the page
		 * cache. That requires locking the folio to handle
		 * truncate, so any new usage will be blocked until we
		 * unlock folio after collapse/during rollback.
		 */
		if (folio_ref_count(folio) != 2 + folio_nr_pages(folio)) {
			result = SCAN_PAGE_COUNT;
			xas_unlock_irq(&xas);
			folio_putback_lru(folio);
			goto out_unlock;
		}

		/*
		 * At this point, the folio is locked and unmapped. If the PTE
		 * was dirty, try_to_unmap() has transferred the dirty bit to
		 * the folio and we must not collapse it into a clean
		 * file-backed folio.
		 *
		 * If the folio is clean here, no one can write it until we
		 * drop the folio lock. A write through a stale TLB entry came
		 * from a clean PTE and must fault because the PTE has been
		 * cleared; the fault path has to take the folio lock before
		 * installing a writable mapping. Buffered write paths also
		 * have to take the folio lock before modifying file contents
		 * without a mapping, typically via write_begin_get_folio().
		 */
		if (!is_shmem && folio_test_dirty(folio)) {
			result = SCAN_PAGE_DIRTY_OR_WRITEBACK;
			xas_unlock_irq(&xas);
			folio_putback_lru(folio);
			goto out_unlock;
		}

		/*
		 * Accumulate the folios that are being collapsed.
		 */
		list_add_tail(&folio->lru, &pagelist);
		index += folio_nr_pages(folio);
		continue;
out_unlock:
		folio_unlock(folio);
		folio_put(folio);
		goto xa_unlocked;
	}

xa_locked:
	xas_unlock_irq(&xas);
xa_unlocked:

	/*
	 * If collapse is successful, flush must be done now before copying.
	 * If collapse is unsuccessful, does flush actually need to be done?
	 * Do it anyway, to clear the state.
	 */
	try_to_unmap_flush();

	if (result == SCAN_SUCCEED && nr_none &&
	    !shmem_charge(mapping->host, nr_none))
		result = SCAN_FAIL;
	if (result != SCAN_SUCCEED) {
		nr_none = 0;
		goto rollback;
	}

	/*
	 * The old folios are locked, so they won't change anymore.
	 */
	index = start;
	dst = folio_page(new_folio, 0);
	list_for_each_entry(folio, &pagelist, lru) {
		int i, nr_pages = folio_nr_pages(folio);

		while (index < folio->index) {
			clear_highpage(dst);
			index++;
			dst++;
		}

		for (i = 0; i < nr_pages; i++) {
			if (copy_mc_highpage(dst, folio_page(folio, i)) > 0) {
				result = SCAN_COPY_MC;
				goto rollback;
			}
			index++;
			dst++;
		}
	}
	while (index < end) {
		clear_highpage(dst);
		index++;
		dst++;
	}

	if (nr_none) {
		struct vm_area_struct *vma;
		int nr_none_check = 0;

		i_mmap_lock_read(mapping);
		xas_lock_irq(&xas);

		xas_set(&xas, start);
		for (index = start; index < end; index++) {
			if (!xas_next(&xas)) {
				xas_store(&xas, XA_RETRY_ENTRY);
				if (xas_error(&xas)) {
					result = SCAN_STORE_FAILED;
					goto immap_locked;
				}
				nr_none_check++;
			}
		}

		if (nr_none != nr_none_check) {
			result = SCAN_PAGE_FILLED;
			goto immap_locked;
		}

		/*
		 * If userspace observed a missing page in a VMA with
		 * a MODE_MISSING userfaultfd, then it might expect a
		 * UFFD_EVENT_PAGEFAULT for that page. If so, we need to
		 * roll back to avoid suppressing such an event. Since
		 * wp/minor userfaultfds don't give userspace any
		 * guarantees that the kernel doesn't fill a missing
		 * page with a zero page, so they don't matter here.
		 *
		 * Any userfaultfds registered after this point will
		 * not be able to observe any missing pages due to the
		 * previously inserted retry entries.
		 */
		mapping_rmap_tree_foreach(vma, mapping, start, end) {
			if (userfaultfd_missing(vma)) {
				result = SCAN_EXCEED_NONE_PTE;
				goto immap_locked;
			}
		}

immap_locked:
		i_mmap_unlock_read(mapping);
		if (result != SCAN_SUCCEED) {
			xas_set(&xas, start);
			for (index = start; index < end; index++) {
				if (xas_next(&xas) == XA_RETRY_ENTRY)
					xas_store(&xas, NULL);
			}

			xas_unlock_irq(&xas);
			goto rollback;
		}
	} else {
		xas_lock_irq(&xas);
	}

	if (is_shmem) {
		lruvec_stat_mod_folio(new_folio, NR_SHMEM, HPAGE_PMD_NR);
		lruvec_stat_mod_folio(new_folio, NR_SHMEM_THPS, HPAGE_PMD_NR);
	} else {
		lruvec_stat_mod_folio(new_folio, NR_FILE_THPS, HPAGE_PMD_NR);
	}
	lruvec_stat_mod_folio(new_folio, NR_FILE_PAGES, HPAGE_PMD_NR);

	/*
	 * Mark new_folio as uptodate before inserting it into the
	 * page cache so that it isn't mistaken for an fallocated but
	 * unwritten page.
	 */
	folio_mark_uptodate(new_folio);
	folio_ref_add(new_folio, HPAGE_PMD_NR - 1);

	if (is_shmem)
		folio_mark_dirty(new_folio);
	folio_add_lru(new_folio);

	/* Join all the small entries into a single multi-index entry. */
	xas_set_order(&xas, start, HPAGE_PMD_ORDER);
	xas_store(&xas, new_folio);
	WARN_ON_ONCE(xas_error(&xas));
	xas_unlock_irq(&xas);

	/*
	 * Remove pte page tables, so we can re-fault the page as huge.  A caller
	 * that wants the PMD mapped now is told to go and do that.
	 */
	retract_page_tables(mapping, start);
	if (cc->policy.install_pmd)
		result = SCAN_PTE_MAPPED_HUGEPAGE;
	folio_unlock(new_folio);

	/*
	 * The collapse has succeeded, so free the old folios.
	 */
	list_for_each_entry_safe(folio, tmp, &pagelist, lru) {
		list_del(&folio->lru);
		lruvec_stat_mod_folio(folio, NR_FILE_PAGES,
				      -folio_nr_pages(folio));
		if (is_shmem)
			lruvec_stat_mod_folio(folio, NR_SHMEM,
					      -folio_nr_pages(folio));
		folio->mapping = NULL;
		folio_clear_active(folio);
		folio_clear_unevictable(folio);
		folio_unlock(folio);
		folio_put_refs(folio, 2 + folio_nr_pages(folio));
	}

	goto out;

rollback:
	/* Something went wrong: roll back page cache changes */
	if (nr_none) {
		xas_lock_irq(&xas);
		mapping->nrpages -= nr_none;
		xas_unlock_irq(&xas);
		shmem_uncharge(mapping->host, nr_none);
	}

	list_for_each_entry_safe(folio, tmp, &pagelist, lru) {
		list_del(&folio->lru);
		folio_unlock(folio);
		folio_putback_lru(folio);
		folio_put(folio);
	}

	new_folio->mapping = NULL;

	folio_unlock(new_folio);
	folio_put(new_folio);
out:
	VM_BUG_ON(!list_empty(&pagelist));
	trace_mm_khugepaged_collapse_file(mm, new_folio, index, addr, is_shmem, file, HPAGE_PMD_NR, result);
	return result;
}

static enum scan_result collapse_scan_file(struct mm_struct *mm,
		unsigned long addr, struct file *file, pgoff_t start,
		struct collapse_control *cc)
{
	const unsigned int max_ptes_none = collapse_max_ptes_none(cc, NULL, HPAGE_PMD_ORDER);
	const unsigned int max_ptes_swap = collapse_max_ptes_swap(cc, HPAGE_PMD_ORDER);
	struct folio *folio = NULL;
	struct address_space *mapping = file->f_mapping;
	XA_STATE(xas, &mapping->i_pages, start);
	int present, swap;
	int node = NUMA_NO_NODE;
	enum scan_result result = SCAN_SUCCEED;

	present = 0;
	swap = 0;
	collapse_control_init_scan(cc);
	rcu_read_lock();
	xas_for_each(&xas, folio, start + HPAGE_PMD_NR - 1) {
		if (xas_retry(&xas, folio))
			continue;

		if (xa_is_value(folio)) {
			swap += 1 << xas_get_order(&xas);
			if (swap > max_ptes_swap) {
				result = SCAN_EXCEED_SWAP_PTE;
				count_vm_event(THP_SCAN_EXCEED_SWAP_PTE);
				break;
			}
			continue;
		}

		if (!folio_try_get(folio)) {
			xas_reset(&xas);
			continue;
		}

		if (unlikely(folio != xas_reload(&xas))) {
			folio_put(folio);
			xas_reset(&xas);
			continue;
		}

		if (is_pmd_order(folio_order(folio))) {
			result = SCAN_PTE_MAPPED_HUGEPAGE;
			/*
			 * PMD-sized THP implies that we can only try
			 * retracting the PTE table.
			 */
			folio_put(folio);
			break;
		}

		node = folio_nid(folio);
		if (collapse_scan_abort(node, cc)) {
			result = SCAN_SCAN_ABORT;
			folio_put(folio);
			break;
		}
		cc->node_load[node]++;

		if (!folio_test_lru(folio)) {
			result = SCAN_PAGE_LRU;
			folio_put(folio);
			break;
		}

		if (folio_expected_ref_count(folio) + 1 != folio_ref_count(folio)) {
			result = SCAN_PAGE_COUNT;
			folio_put(folio);
			break;
		}

		/*
		 * We probably should check if the folio is referenced
		 * here, but nobody would transfer pte_young() to
		 * folio_test_referenced() for us.  And rmap walk here
		 * is just too costly...
		 */

		present += folio_nr_pages(folio);
		folio_put(folio);

		if (need_resched()) {
			xas_pause(&xas);
			cond_resched_rcu();
		}
	}
	rcu_read_unlock();
	if (result == SCAN_PTE_MAPPED_HUGEPAGE)
		cc->progress++;
	else
		cc->progress += HPAGE_PMD_NR;

	if (result == SCAN_SUCCEED) {
		if (present < HPAGE_PMD_NR - max_ptes_none) {
			result = SCAN_EXCEED_NONE_PTE;
			count_vm_event(THP_SCAN_EXCEED_NONE_PTE);
		} else {
			result = collapse_file(mm, addr, file, start, cc);
		}
	}

	trace_mm_khugepaged_scan_file(mm, folio, file, present, swap, result);
	return result;
}

/*
 * Try to collapse a single PMD starting at a PMD aligned addr, and return
 * the results.
 */
static enum scan_result collapse_single_pmd(unsigned long addr,
		unsigned long end, struct vm_area_struct *vma,
		bool *lock_dropped, struct collapse_control *cc)
{
	struct mm_struct *mm = vma->vm_mm;
	bool triggered_wb = false;
	enum scan_result result;
	struct file *file;
	pgoff_t pgoff;

	mmap_assert_locked(mm);

	if (vma_is_anonymous(vma)) {
		result = collapse_scan_anon_pmd(vma, addr, end, cc);
		if (!cc->select_orders)
			goto end;

		/* collapse_anon_pmd() takes mmap_lock itself, where it needs it */
		mmap_read_unlock(mm);
		*lock_dropped = true;

		result = collapse_anon_pmd(mm, addr, end, cc);
		goto end;
	}

	file = get_file(vma->vm_file);
	pgoff = linear_page_index(vma, addr);

	mmap_read_unlock(mm);
	*lock_dropped = true;
retry:
	result = collapse_scan_file(mm, addr, file, pgoff, cc);

	/* Dirty pages are worth a writeback and one more try, if asked for */
	if (cc->policy.writeback_dirty && result == SCAN_PAGE_DIRTY_OR_WRITEBACK &&
	    !triggered_wb && mapping_can_writeback(file->f_mapping)) {
		const loff_t lstart = (loff_t)pgoff << PAGE_SHIFT;
		const loff_t lend = lstart + HPAGE_PMD_SIZE - 1;

		filemap_write_and_wait_range(file->f_mapping, lstart, lend);
		triggered_wb = true;
		goto retry;
	}
	fput(file);

	if (result == SCAN_PTE_MAPPED_HUGEPAGE) {
		mmap_read_lock(mm);
		if (collapse_test_exit_or_disable(mm))
			result = SCAN_ANY_PROCESS;
		else
			result = try_collapse_pte_mapped_thp(mm, addr,
							cc->policy.install_pmd);
		if (result == SCAN_PMD_MAPPED)
			result = SCAN_SUCCEED;
		mmap_read_unlock(mm);
	}
end:
	return result;
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
			unsigned long pmd_addr, range_end;
			bool lock_dropped = false;

			/* One table's worth at most, and never past the VMA */
			pmd_addr = khugepaged_scan.address & HPAGE_PMD_MASK;
			range_end = min(hend, pmd_addr + HPAGE_PMD_SIZE);

			cond_resched();
			if (unlikely(collapse_test_exit_or_disable(mm)))
				goto breakouterloop;

			VM_WARN_ON_ONCE(khugepaged_scan.address < hstart);

			*result = collapse_single_pmd(khugepaged_scan.address,
						      range_end, vma,
						      &lock_dropped, cc);
			if (*result == SCAN_SUCCEED)
				++khugepaged_pages_collapsed;
			/* move to next address */
			khugepaged_scan.address = range_end;
			if (lock_dropped)
				/*
				 * We released mmap_lock so break loop.  Note
				 * that we drop mmap_lock before all hugepage
				 * allocations, so if allocation fails, we are
				 * guaranteed to break here and report the
				 * correct result back to caller.
				 */
				goto breakouterloop_mmap_lock;
			if (cc->progress >= progress_max)
				goto breakouterloop;
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

static int madvise_collapse_errno(enum scan_result r)
{
	/*
	 * MADV_COLLAPSE breaks from existing madvise(2) conventions to provide
	 * actionable feedback to caller, so they may take an appropriate
	 * fallback measure depending on the nature of the failure.
	 */
	switch (r) {
	case SCAN_ALLOC_HUGE_PAGE_FAIL:
		return -ENOMEM;
	case SCAN_CGROUP_CHARGE_FAIL:
	case SCAN_EXCEED_NONE_PTE:
		return -EBUSY;
	/* Resource temporary unavailable - trying again might succeed */
	case SCAN_PAGE_COUNT:
	case SCAN_PAGE_LOCK:
	case SCAN_PAGE_LRU:
	case SCAN_DEL_PAGE_LRU:
	case SCAN_PAGE_FILLED:
	case SCAN_PAGE_HAS_PRIVATE:
	case SCAN_PAGE_DIRTY_OR_WRITEBACK:
		return -EAGAIN;
	/*
	 * Other: Trying again likely not to succeed / error intrinsic to
	 * specified memory range. khugepaged likely won't be able to collapse
	 * either.
	 */
	default:
		return -EINVAL;
	}
}

int madvise_collapse(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end, bool *lock_dropped)
{
	struct collapse_control *cc;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long hstart, hend, addr;
	enum scan_result last_fail = SCAN_FAIL;
	int thps = 0;
	bool mmap_unlocked = false;
	int err;

	BUG_ON(vma->vm_start > start);
	BUG_ON(vma->vm_end < end);

	if (!collapse_possible(vma, vma->vm_flags, TVA_FORCED_COLLAPSE))
		return -EINVAL;

	hstart = ALIGN(start, HPAGE_PMD_SIZE);
	hend = ALIGN_DOWN(end, HPAGE_PMD_SIZE);

	if (hstart >= hend)
		return 0;

	cc = kmalloc_obj(*cc);
	if (!cc)
		return -ENOMEM;
	collapse_policy_forced(&cc->policy);
	cc->progress = 0;
	err = collapse_control_init(cc);
	if (err) {
		kfree(cc);
		return err;
	}

	mmgrab(mm);
	lru_add_drain_all();

	for (addr = hstart; addr < hend; addr += HPAGE_PMD_SIZE) {
		enum scan_result result = SCAN_FAIL;

		if (mmap_unlocked) {
			cond_resched();
			mmap_read_lock(mm);
			mmap_unlocked = false;
			*lock_dropped = true;
			result = hugepage_vma_revalidate(mm, addr, false, &vma,
							 cc, HPAGE_PMD_ORDER);
			if (result != SCAN_SUCCEED) {
				last_fail = result;
				goto out_nolock;
			}

			hend = min(hend, vma->vm_end & HPAGE_PMD_MASK);
		}

		result = collapse_single_pmd(addr, addr + HPAGE_PMD_SIZE, vma,
					     &mmap_unlocked, cc);

		switch (result) {
		case SCAN_SUCCEED:
		case SCAN_PMD_MAPPED:
			++thps;
			break;
		/* Whitelisted set of results where continuing OK */
		case SCAN_NO_PTE_TABLE:
		case SCAN_PTE_NON_PRESENT:
		case SCAN_PTE_UFFD:
		case SCAN_LACK_REFERENCED_PAGE:
		case SCAN_PAGE_NULL:
		case SCAN_PAGE_COUNT:
		case SCAN_PAGE_LOCK:
		case SCAN_PAGE_COMPOUND:
		case SCAN_PAGE_LRU:
		case SCAN_DEL_PAGE_LRU:
			last_fail = result;
			break;
		default:
			last_fail = result;
			/* Other error, exit */
			goto out_maybelock;
		}
	}

out_maybelock:
	/* Caller expects us to hold mmap_lock on return */
	if (mmap_unlocked) {
		*lock_dropped = true;
		mmap_read_lock(mm);
	}
out_nolock:
	mmap_assert_locked(mm);
	mmdrop(mm);
	collapse_control_release(cc);
	kfree(cc);

	return thps == ((hend - hstart) >> HPAGE_PMD_SHIFT) ? 0
			: madvise_collapse_errno(last_fail);
}
