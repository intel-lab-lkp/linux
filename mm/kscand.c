// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/rmap.h>
#include <linux/pagewalk.h>
#include <linux/page_ext.h>
#include <linux/page_idle.h>
#include <linux/page_table_check.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
#include <linux/kthread.h>
#include <linux/kscand.h>
#include <linux/memory-tiers.h>
#include <linux/mempolicy.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/cleanup.h>

#include <asm/pgalloc.h>
#include "internal.h"
#include "mm_slot.h"

static struct task_struct *kscand_thread __read_mostly;
static DEFINE_MUTEX(kscand_mutex);
/*
 * Total VMA size to cover during scan.
 */
#define KSCAND_SCAN_SIZE	(1 * 1024 * 1024 * 1024UL)
static unsigned long kscand_scan_size __read_mostly = KSCAND_SCAN_SIZE;

/* How long to pause between two scan cycles */
static unsigned int kscand_scan_sleep_ms __read_mostly = 20;

/* Max number of mms to scan in one scan cycle */
#define KSCAND_MMS_TO_SCAN	(4 * 1024UL)
static unsigned long kscand_mms_to_scan __read_mostly = KSCAND_MMS_TO_SCAN;

bool kscand_scan_enabled = true;
static bool need_wakeup;

static unsigned long kscand_sleep_expire;

static DEFINE_SPINLOCK(kscand_mm_lock);
static DECLARE_WAIT_QUEUE_HEAD(kscand_wait);

#define KSCAND_SLOT_HASH_BITS 10
static DEFINE_READ_MOSTLY_HASHTABLE(kscand_slots_hash, KSCAND_SLOT_HASH_BITS);

static struct kmem_cache *kscand_slot_cache __read_mostly;

/* Per mm information collected to control VMA scanning */
struct kscand_mm_slot {
	struct mm_slot slot;
	long address;
	bool is_scanned;
};

/* Data structure to keep track of current mm under scan */
struct kscand_scan {
	struct list_head mm_head;
	struct kscand_mm_slot *mm_slot;
};

struct kscand_scan kscand_scan = {
	.mm_head = LIST_HEAD_INIT(kscand_scan.mm_head),
};

/*
 * Data structure passed to control scanning and also collect
 * per memory node information
 */
struct kscand_scanctrl {
	struct list_head scan_list;
	unsigned long address;
};

struct kscand_scanctrl kscand_scanctrl;
/* Per folio information used for migration */
struct kscand_migrate_info {
	struct list_head migrate_node;
	struct folio *folio;
	unsigned long address;
};

static bool kscand_eligible_srcnid(int nid)
{
	/* Only promotion case is considered */
	return  !node_is_toptier(nid);
}

static inline int kscand_has_work(void)
{
	return !list_empty(&kscand_scan.mm_head);
}

static inline bool kscand_should_wakeup(void)
{
	bool wakeup = kthread_should_stop() || need_wakeup ||
	       time_after_eq(jiffies, kscand_sleep_expire);

	need_wakeup = false;

	return wakeup;
}

static void kscand_wait_work(void)
{
	const unsigned long scan_sleep_jiffies =
		msecs_to_jiffies(kscand_scan_sleep_ms);

	if (!scan_sleep_jiffies)
		return;

	kscand_sleep_expire = jiffies + scan_sleep_jiffies;

	/* Allows kthread to pause scanning */
	wait_event_timeout(kscand_wait, kscand_should_wakeup(),
			scan_sleep_jiffies);
}

static inline bool is_valid_folio(struct folio *folio)
{
	if (!folio || folio_test_unevictable(folio) || !folio_mapped(folio) ||
		folio_is_zone_device(folio) || folio_maybe_mapped_shared(folio))
		return false;

	return true;
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
	struct kscand_migrate_info *info;
	struct kscand_scanctrl *scanctrl = walk->private;
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

		if (!kscand_eligible_srcnid(srcnid)) {
			folio_put(folio);
			return 0;
		}
		/* XXX: Leaking memory. TBD: consume info */

		info = kzalloc(sizeof(struct kscand_migrate_info), GFP_NOWAIT);
		if (info && scanctrl) {
			info->address = addr;
			info->folio = folio;
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

static void kscand_walk_page_vma(struct vm_area_struct *vma, struct kscand_scanctrl *scanctrl)
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

static inline int kscand_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
}

static void kscand_collect_mm_slot(struct kscand_mm_slot *mm_slot)
{
	struct mm_slot *slot = &mm_slot->slot;
	struct mm_struct *mm = slot->mm;

	lockdep_assert_held(&kscand_mm_lock);

	if (kscand_test_exit(mm)) {
		hash_del(&slot->hash);
		list_del(&slot->mm_node);

		mm_slot_free(kscand_slot_cache, mm_slot);
		mmdrop(mm);
	}
}

static unsigned long kscand_scan_mm_slot(void)
{
	bool next_mm = false;
	bool update_mmslot_info = false;

	unsigned long vma_scanned_size = 0;
	unsigned long address;

	struct mm_slot *slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma = NULL;
	struct kscand_mm_slot *mm_slot;


	spin_lock(&kscand_mm_lock);

	if (kscand_scan.mm_slot) {
		mm_slot = kscand_scan.mm_slot;
		slot = &mm_slot->slot;
		address = mm_slot->address;
	} else {
		slot = list_entry(kscand_scan.mm_head.next,
				     struct mm_slot, mm_node);
		mm_slot = mm_slot_entry(slot, struct kscand_mm_slot, slot);
		address = mm_slot->address;
		kscand_scan.mm_slot = mm_slot;
	}

	mm = slot->mm;
	mm_slot->is_scanned = true;
	spin_unlock(&kscand_mm_lock);

	if (unlikely(!mmap_read_trylock(mm)))
		goto outerloop_mmap_lock;

	if (unlikely(kscand_test_exit(mm))) {
		next_mm = true;
		goto outerloop;
	}

	VMA_ITERATOR(vmi, mm, address);

	for_each_vma(vmi, vma) {
		kscand_walk_page_vma(vma, &kscand_scanctrl);
		vma_scanned_size += vma->vm_end - vma->vm_start;

		if (vma_scanned_size >= kscand_scan_size) {
			next_mm = true;
			/* TBD: Add scanned folios to migration list */
			break;
		}
	}

	if (!vma)
		address = 0;
	else
		address = kscand_scanctrl.address + PAGE_SIZE;

	update_mmslot_info = true;

	if (update_mmslot_info)
		mm_slot->address = address;

outerloop:
	/* exit_mmap will destroy ptes after this */
	mmap_read_unlock(mm);

outerloop_mmap_lock:
	spin_lock(&kscand_mm_lock);
	WARN_ON(kscand_scan.mm_slot != mm_slot);

	/*
	 * Release the current mm_slot if this mm is about to die, or
	 * if we scanned all vmas of this mm.
	 */
	if (unlikely(kscand_test_exit(mm)) || !vma || next_mm) {
		/*
		 * Make sure that if mm_users is reaching zero while
		 * kscand runs here, kscand_exit will find
		 * mm_slot not pointing to the exiting mm.
		 */
		if (slot->mm_node.next != &kscand_scan.mm_head) {
			slot = list_entry(slot->mm_node.next,
					struct mm_slot, mm_node);
			kscand_scan.mm_slot =
				mm_slot_entry(slot, struct kscand_mm_slot, slot);

		} else
			kscand_scan.mm_slot = NULL;

		if (kscand_test_exit(mm)) {
			kscand_collect_mm_slot(mm_slot);
			goto end;
		}
	}
	mm_slot->is_scanned = false;
end:
	spin_unlock(&kscand_mm_lock);
	return 0;
}

static void kscand_do_scan(void)
{
	unsigned long iter = 0, mms_to_scan;

	mms_to_scan = READ_ONCE(kscand_mms_to_scan);

	while (true) {
		if (unlikely(kthread_should_stop()) ||
			!READ_ONCE(kscand_scan_enabled))
			break;

		if (kscand_has_work())
			kscand_scan_mm_slot();

		iter++;

		if (iter >= mms_to_scan)
			break;
		cond_resched();
	}
}

static int kscand(void *none)
{
	while (true) {
		if (unlikely(kthread_should_stop()))
			break;

		while (!READ_ONCE(kscand_scan_enabled)) {
			cpu_relax();
			kscand_wait_work();
		}

		kscand_do_scan();

		kscand_wait_work();
	}
	return 0;
}

static inline void kscand_destroy(void)
{
	kmem_cache_destroy(kscand_slot_cache);
}

void __kscand_enter(struct mm_struct *mm)
{
	struct kscand_mm_slot *kscand_slot;
	struct mm_slot *slot;
	int wakeup;

	/* __kscand_exit() must not run from under us */
	VM_BUG_ON_MM(kscand_test_exit(mm), mm);

	kscand_slot = mm_slot_alloc(kscand_slot_cache);

	if (!kscand_slot)
		return;

	kscand_slot->address = 0;
	slot = &kscand_slot->slot;

	spin_lock(&kscand_mm_lock);
	mm_slot_insert(kscand_slots_hash, mm, slot);

	wakeup = list_empty(&kscand_scan.mm_head);
	list_add_tail(&slot->mm_node, &kscand_scan.mm_head);
	spin_unlock(&kscand_mm_lock);

	mmgrab(mm);
	if (wakeup)
		wake_up_interruptible(&kscand_wait);
}

void __kscand_exit(struct mm_struct *mm)
{
	struct kscand_mm_slot *mm_slot;
	struct mm_slot *slot;
	int free = 0;

	spin_lock(&kscand_mm_lock);
	slot = mm_slot_lookup(kscand_slots_hash, mm);
	mm_slot = mm_slot_entry(slot, struct kscand_mm_slot, slot);
	if (mm_slot && kscand_scan.mm_slot != mm_slot) {
		hash_del(&slot->hash);
		list_del(&slot->mm_node);
		free = 1;
	} else if (mm_slot && kscand_scan.mm_slot == mm_slot && !mm_slot->is_scanned) {
		hash_del(&slot->hash);
		list_del(&slot->mm_node);
		free = 1;
		/* TBD: Set the actual next slot */
		kscand_scan.mm_slot = NULL;
	}

	spin_unlock(&kscand_mm_lock);

	if (free) {
		mm_slot_free(kscand_slot_cache, mm_slot);
		mmdrop(mm);
	} else if (mm_slot) {
		mmap_write_lock(mm);
		mmap_write_unlock(mm);
	}
}

static int start_kscand(void)
{
	struct task_struct *kthread;

	guard(mutex)(&kscand_mutex);

	if (kscand_thread)
		return 0;

	kthread = kthread_run(kscand, NULL, "kscand");
	if (IS_ERR(kscand_thread)) {
		pr_err("kscand: kthread_run(kscand) failed\n");
		return PTR_ERR(kthread);
	}

	kscand_thread = kthread;
	pr_info("kscand: Successfully started kscand");

	if (!list_empty(&kscand_scan.mm_head))
		wake_up_interruptible(&kscand_wait);

	return 0;
}

static int stop_kscand(void)
{
	guard(mutex)(&kscand_mutex);

	if (kscand_thread) {
		kthread_stop(kscand_thread);
		kscand_thread = NULL;
	}

	return 0;
}

static inline void init_list(void)
{
	INIT_LIST_HEAD(&kscand_scanctrl.scan_list);
	init_waitqueue_head(&kscand_wait);
}

static int __init kscand_init(void)
{
	int err;

	kscand_slot_cache = KMEM_CACHE(kscand_mm_slot, 0);

	if (!kscand_slot_cache) {
		pr_err("kscand: kmem_cache error");
		return -ENOMEM;
	}

	init_list();
	err = start_kscand();
	if (err)
		goto err_kscand;

	return 0;

err_kscand:
	stop_kscand();
	kscand_destroy();

	return err;
}
subsys_initcall(kscand_init);
