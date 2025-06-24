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
#include <linux/kscand.h>
#include <linux/memory-tiers.h>
#include <linux/mempolicy.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/cleanup.h>
#include <linux/minmax.h>
#include <trace/events/kmem.h>

#include <asm/pgalloc.h>
#include "internal.h"
#include "mm_slot.h"

static struct task_struct *kscand_thread __read_mostly;
static DEFINE_MUTEX(kscand_mutex);
extern unsigned int sysctl_numa_balancing_scan_delay;

/*
 * Total VMA size to cover during scan.
 * Min: 256MB default: 1GB max: 4GB
 */
#define KSCAND_SCAN_SIZE_MIN	(256 * 1024 * 1024UL)
#define KSCAND_SCAN_SIZE_MAX	(4 * 1024 * 1024 * 1024UL)
#define KSCAND_SCAN_SIZE	(1 * 1024 * 1024 * 1024UL)

static unsigned long kscand_scan_size __read_mostly = KSCAND_SCAN_SIZE;

/*
 * Scan period for each mm.
 * Min: 600ms default: 2sec Max: 5sec
 */
#define KSCAND_SCAN_PERIOD_MAX	5000U
#define KSCAND_SCAN_PERIOD_MIN	600U
#define KSCAND_SCAN_PERIOD		2000U

static unsigned int kscand_mm_scan_period_ms __read_mostly = KSCAND_SCAN_PERIOD;

/* How long to pause between two scan cycles */
static unsigned int kscand_scan_sleep_ms __read_mostly = 20;

/* Max number of mms to scan in one scan cycle */
#define KSCAND_MMS_TO_SCAN	(4 * 1024UL)
static unsigned long kscand_mms_to_scan __read_mostly = KSCAND_MMS_TO_SCAN;

bool kscand_scan_enabled = true;
static bool need_wakeup;
static bool migrated_need_wakeup;

/* How long to pause between two migration cycles */
static unsigned int kmigrate_sleep_ms __read_mostly = 20;

static struct task_struct *kmigrated_thread __read_mostly;
static DEFINE_MUTEX(kmigrated_mutex);
static DECLARE_WAIT_QUEUE_HEAD(kmigrated_wait);
static unsigned long kmigrated_sleep_expire;

/* mm of the migrating folio entry */
static struct mm_struct *kmigrated_cur_mm;

/* Migration list is manipulated underneath because of mm_exit */
static bool  kmigrated_clean_list;

static unsigned long kscand_sleep_expire;
#define KSCAND_DEFAULT_TARGET_NODE	(0)
static int kscand_target_node = KSCAND_DEFAULT_TARGET_NODE;

static DEFINE_SPINLOCK(kscand_mm_lock);
static DEFINE_SPINLOCK(kscand_migrate_lock);
static DECLARE_WAIT_QUEUE_HEAD(kscand_wait);

#define KSCAND_SLOT_HASH_BITS 10
static DEFINE_READ_MOSTLY_HASHTABLE(kscand_slots_hash, KSCAND_SLOT_HASH_BITS);

static struct kmem_cache *kscand_slot_cache __read_mostly;

#define KMIGRATED_SLOT_HASH_BITS 10
static DEFINE_READ_MOSTLY_HASHTABLE(kmigrated_slots_hash, KMIGRATED_SLOT_HASH_BITS);
static struct kmem_cache *kmigrated_slot_cache __read_mostly;

/* Per mm information collected to control VMA scanning */
struct kscand_mm_slot {
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
	int target_node;
};

/* Data structure to keep track of current mm under scan */
struct kscand_scan {
	struct list_head mm_head;
	struct kscand_mm_slot *mm_slot;
};

struct kscand_scan kscand_scan = {
	.mm_head = LIST_HEAD_INIT(kscand_scan.mm_head),
};

/* Per memory node information used to caclulate target_node for migration */
struct kscand_nodeinfo {
	unsigned long nr_scanned;
	unsigned long nr_accessed;
	int node;
	bool is_toptier;
};

/*
 * Data structure passed to control scanning and also collect
 * per memory node information
 */
struct kscand_scanctrl {
	struct list_head scan_list;
	struct kscand_nodeinfo *nodeinfo[MAX_NUMNODES];
	unsigned long address;
	unsigned long nr_to_scan;
};

struct kscand_scanctrl kscand_scanctrl;

/* Per mm migration list */
struct kmigrated_mm_slot {
	/* Tracks mm that has non empty migration list */
	struct mm_slot mm_slot;
	/* Per mm lock used to synchronize migration list */
	spinlock_t migrate_lock;
	/* Head of per mm migration list */
	struct list_head migrate_head;
};

/* System wide list of mms that maintain migration list */
struct kmigrated_daemon {
	struct list_head mm_head;
	struct kmigrated_mm_slot *mm_slot;
};

struct kmigrated_daemon kmigrated_daemon = {
	.mm_head = LIST_HEAD_INIT(kmigrated_daemon.mm_head),
};

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

#ifdef CONFIG_SYSFS
static ssize_t scan_sleep_ms_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", kscand_scan_sleep_ms);
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

	kscand_scan_sleep_ms = msecs;
	kscand_sleep_expire = 0;
	wake_up_interruptible(&kscand_wait);

	return count;
}

static struct kobj_attribute scan_sleep_ms_attr =
	__ATTR_RW(scan_sleep_ms);

static ssize_t mm_scan_period_ms_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", kscand_mm_scan_period_ms);
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

	stored_msecs = clamp(msecs, KSCAND_SCAN_PERIOD_MIN, KSCAND_SCAN_PERIOD_MAX);

	kscand_mm_scan_period_ms = stored_msecs;
	kscand_sleep_expire = 0;
	wake_up_interruptible(&kscand_wait);

	return count;
}

static struct kobj_attribute mm_scan_period_ms_attr =
	__ATTR_RW(mm_scan_period_ms);

static ssize_t mms_to_scan_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%lu\n", kscand_mms_to_scan);
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

	kscand_mms_to_scan = val;
	kscand_sleep_expire = 0;
	wake_up_interruptible(&kscand_wait);

	return count;
}

static struct kobj_attribute mms_to_scan_attr =
	__ATTR_RW(mms_to_scan);

static ssize_t scan_enabled_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", kscand_scan_enabled ? 1 : 0);
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
		kscand_scan_enabled = true;
		need_wakeup = true;
	} else
		kscand_scan_enabled = false;

	kscand_sleep_expire = 0;
	wake_up_interruptible(&kscand_wait);

	return count;
}

static struct kobj_attribute scan_enabled_attr =
	__ATTR_RW(scan_enabled);

static ssize_t target_node_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%u\n", kscand_target_node);
}

static ssize_t target_node_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	int err, node;

	err = kstrtoint(buf, 10, &node);
	if (err)
		return -EINVAL;

	kscand_sleep_expire = 0;
	if (!node_is_toptier(node))
		return -EINVAL;

	kscand_target_node = node;
	wake_up_interruptible(&kscand_wait);

	return count;
}
static struct kobj_attribute target_node_attr =
	__ATTR_RW(target_node);

static struct attribute *kscand_attr[] = {
	&scan_sleep_ms_attr.attr,
	&mm_scan_period_ms_attr.attr,
	&mms_to_scan_attr.attr,
	&scan_enabled_attr.attr,
	&target_node_attr.attr,
	NULL,
};

struct attribute_group kscand_attr_group = {
	.attrs = kscand_attr,
	.name = "kscand",
};
#endif

static inline int kscand_has_work(void)
{
	return !list_empty(&kscand_scan.mm_head);
}

static inline int kmigrated_has_work(void)
{
	return !list_empty(&kmigrated_daemon.mm_head);
}

static inline bool kscand_should_wakeup(void)
{
	bool wakeup = kthread_should_stop() || need_wakeup ||
	       time_after_eq(jiffies, kscand_sleep_expire);

	need_wakeup = false;

	return wakeup;
}

static inline bool kmigrated_should_wakeup(void)
{
	bool wakeup = kthread_should_stop() || migrated_need_wakeup ||
	       time_after_eq(jiffies, kmigrated_sleep_expire);

	migrated_need_wakeup = false;

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

static void kmigrated_wait_work(void)
{
	const unsigned long migrate_sleep_jiffies =
		msecs_to_jiffies(kmigrate_sleep_ms);

	if (!migrate_sleep_jiffies)
		return;

	kmigrated_sleep_expire = jiffies + migrate_sleep_jiffies;
	wait_event_timeout(kmigrated_wait, kmigrated_should_wakeup(),
			migrate_sleep_jiffies);
}

static unsigned long get_slowtier_accesed(struct kscand_scanctrl *scanctrl)
{
	int node;
	unsigned long accessed = 0;

	for_each_node_state(node, N_MEMORY) {
		if (!node_is_toptier(node) && scanctrl->nodeinfo[node])
			accessed += scanctrl->nodeinfo[node]->nr_accessed;
	}
	return accessed;
}

static inline void set_nodeinfo_nr_accessed(struct kscand_nodeinfo *ni, unsigned long val)
{
	ni->nr_accessed = val;
}
static inline unsigned long get_nodeinfo_nr_scanned(struct kscand_nodeinfo *ni)
{
	return ni->nr_scanned;
}

static inline void set_nodeinfo_nr_scanned(struct kscand_nodeinfo *ni, unsigned long val)
{
	ni->nr_scanned = val;
}

static inline void reset_nodeinfo_nr_scanned(struct kscand_nodeinfo *ni)
{
	set_nodeinfo_nr_scanned(ni, 0);
}

static inline void reset_nodeinfo(struct kscand_nodeinfo *ni)
{
	set_nodeinfo_nr_scanned(ni, 0);
	set_nodeinfo_nr_accessed(ni, 0);
}

static void init_one_nodeinfo(struct kscand_nodeinfo *ni, int node)
{
	ni->nr_scanned = 0;
	ni->nr_accessed = 0;
	ni->node = node;
	ni->is_toptier = node_is_toptier(node) ? true : false;
}

static struct kscand_nodeinfo *alloc_one_nodeinfo(int node)
{
	struct kscand_nodeinfo *ni;

	ni = kzalloc(sizeof(*ni), GFP_KERNEL);

	if (!ni)
		return NULL;

	init_one_nodeinfo(ni, node);

	return ni;
}

/* TBD: Handle errors */
static void init_scanctrl(struct kscand_scanctrl *scanctrl)
{
	struct kscand_nodeinfo *ni;
	int node;

	for_each_node(node) {
		ni = alloc_one_nodeinfo(node);
		if (!ni)
			WARN_ON_ONCE(ni);
		scanctrl->nodeinfo[node] = ni;
	}
}

static void reset_scanctrl(struct kscand_scanctrl *scanctrl)
{
	int node;

	for_each_node_state(node, N_MEMORY)
		reset_nodeinfo(scanctrl->nodeinfo[node]);

	/* XXX: Not rellay required? */
	scanctrl->nr_to_scan = kscand_scan_size;
}

static void free_scanctrl(struct kscand_scanctrl *scanctrl)
{
	int node;

	for_each_node(node)
		kfree(scanctrl->nodeinfo[node]);
}

static int kscand_get_target_node(void *data)
{
	return kscand_target_node;
}

static int get_target_node(struct kscand_scanctrl *scanctrl)
{
	int node, target_node = NUMA_NO_NODE;
	unsigned long prev = 0;

	for_each_node(node) {
		if (node_is_toptier(node) && scanctrl->nodeinfo[node] &&
				get_nodeinfo_nr_scanned(scanctrl->nodeinfo[node]) > prev) {
			prev = get_nodeinfo_nr_scanned(scanctrl->nodeinfo[node]);
			target_node = node;
		}
	}
	if (target_node == NUMA_NO_NODE)
		target_node = kscand_get_target_node(NULL);

	return target_node;
}

extern bool migrate_balanced_pgdat(struct pglist_data *pgdat,
					unsigned long nr_migrate_pages);

/*XXX: Taken from migrate.c to avoid NUMAB mode=2 and NULL vma checks*/
static int kscand_migrate_misplaced_folio_prepare(struct folio *folio,
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
		 * See folio_maybe_mapped_shared() on possible imprecision
		 * when we cannot easily detect if a folio is shared.
		 */
		if (vma && (vma->vm_flags & VM_EXEC) &&
		    folio_maybe_mapped_shared(folio))
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
		folio_is_zone_device(folio) || folio_maybe_mapped_shared(folio))
		return false;

	return true;
}

enum kscand_migration_err {
	KSCAND_NULL_MM = 1,
	KSCAND_EXITING_MM,
	KSCAND_INVALID_FOLIO,
	KSCAND_NONLRU_FOLIO,
	KSCAND_INELIGIBLE_SRC_NODE,
	KSCAND_SAME_SRC_DEST_NODE,
	KSCAND_PTE_NOT_PRESENT,
	KSCAND_PMD_NOT_PRESENT,
	KSCAND_NO_PTE_OFFSET_MAP_LOCK,
	KSCAND_NOT_HOT_PAGE,
	KSCAND_LRU_ISOLATION_ERR,
};

static bool is_hot_page(struct folio *folio)
{
#ifdef CONFIG_LRU_GEN
	struct lruvec *lruvec;
	int gen = folio_lru_gen(folio);

	lruvec = folio_lruvec(folio);
	return lru_gen_is_active(lruvec, gen);
#else
	return folio_test_active(folio);
#endif
}

static int kmigrated_promote_folio(struct kscand_migrate_info *info,
					struct mm_struct *mm,
					int destnid)
{
	unsigned long pfn;
	unsigned long address;
	struct page *page;
	struct folio *folio;
	int ret;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	pmd_t pmde;
	int srcnid;

	if (mm == NULL)
		return KSCAND_NULL_MM;

	if (mm == READ_ONCE(kmigrated_cur_mm) &&
		READ_ONCE(kmigrated_clean_list)) {
		WARN_ON_ONCE(mm);
		return KSCAND_EXITING_MM;
	}

	folio = info->folio;

	/* Check again if the folio is really valid now */
	if (folio) {
		pfn = folio_pfn(folio);
		page = pfn_to_online_page(pfn);
	}

	if (!page || PageTail(page) || !is_valid_folio(folio))
		return KSCAND_INVALID_FOLIO;

	if (!folio_test_lru(folio))
		return KSCAND_NONLRU_FOLIO;

	if (!is_hot_page(folio))
		return KSCAND_NOT_HOT_PAGE;

	folio_get(folio);

	srcnid = folio_nid(folio);

	/* Do not try to promote pages from regular nodes */
	if (!kscand_eligible_srcnid(srcnid)) {
		folio_put(folio);
		return KSCAND_INELIGIBLE_SRC_NODE;
	}

	/* Also happen when it is already migrated */
	if (srcnid == destnid) {
		folio_put(folio);
		return KSCAND_SAME_SRC_DEST_NODE;
	}
	address = info->address;
	pmd = pmd_off(mm, address);
	pmde = pmdp_get(pmd);

	if (!pmd_present(pmde)) {
		folio_put(folio);
		return KSCAND_PMD_NOT_PRESENT;
	}

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!pte) {
		folio_put(folio);
		WARN_ON_ONCE(!pte);
		return KSCAND_NO_PTE_OFFSET_MAP_LOCK;
	}

	ret = kscand_migrate_misplaced_folio_prepare(folio, NULL, destnid);
	if (ret) {
		folio_put(folio);
		pte_unmap_unlock(pte, ptl);
		return KSCAND_LRU_ISOLATION_ERR;
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

	scanctrl->nodeinfo[srcnid]->nr_scanned++;
	if (scanctrl->nr_to_scan)
		scanctrl->nr_to_scan--;

	if (!scanctrl->nr_to_scan) {
		folio_put(folio);
		return 1;
	}

	if (!folio_test_lru(folio)) {
		folio_put(folio);
		return 0;
	}

	if (!folio_test_idle(folio) || folio_test_young(folio) ||
			mmu_notifier_test_young(mm, addr) ||
			folio_test_referenced(folio) || pte_young(pteval)) {

		scanctrl->nodeinfo[srcnid]->nr_accessed++;

		if (!kscand_eligible_srcnid(srcnid)) {
			folio_put(folio);
			return 0;
		}

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

struct destroy_list_work {
	struct list_head migrate_head;
	struct work_struct dwork;
};

static void kmigrated_destroy_list_fn(struct work_struct *work)
{
	struct destroy_list_work *dlw;
	struct kscand_migrate_info *info, *tmp;

	dlw = container_of(work, struct destroy_list_work, dwork);

	if (!list_empty(&dlw->migrate_head)) {
		list_for_each_entry_safe(info, tmp, &dlw->migrate_head,	migrate_node) {
			list_del(&info->migrate_node);
			kfree(info);
		}
	}

	kfree(dlw);
}

static void kmigrated_destroy_list(struct list_head *list_head)
{
	struct destroy_list_work *destroy_list_work;


	destroy_list_work = kmalloc(sizeof(*destroy_list_work), GFP_KERNEL);
	if (!destroy_list_work)
		return;

	INIT_LIST_HEAD(&destroy_list_work->migrate_head);
	list_splice_tail_init(list_head, &destroy_list_work->migrate_head);
	INIT_WORK(&destroy_list_work->dwork, kmigrated_destroy_list_fn);
	schedule_work(&destroy_list_work->dwork);
}

static struct kmigrated_mm_slot *kmigrated_get_mm_slot(struct mm_struct *mm, bool alloc)
{
	struct kmigrated_mm_slot *mm_slot = NULL;
	struct mm_slot *slot;

	guard(spinlock)(&kscand_migrate_lock);

	slot = mm_slot_lookup(kmigrated_slots_hash, mm);
	mm_slot = mm_slot_entry(slot, struct kmigrated_mm_slot, mm_slot);

	if (!mm_slot && alloc) {
		mm_slot = mm_slot_alloc(kmigrated_slot_cache);
		if (!mm_slot) {
			spin_unlock(&kscand_migrate_lock);
			return NULL;
		}

		slot = &mm_slot->mm_slot;
		INIT_LIST_HEAD(&mm_slot->migrate_head);
		spin_lock_init(&mm_slot->migrate_lock);
		mm_slot_insert(kmigrated_slots_hash, mm, slot);
		list_add_tail(&slot->mm_node, &kmigrated_daemon.mm_head);
	}

	return mm_slot;
}

static void kscand_cleanup_migration_list(struct mm_struct *mm)
{
	struct kmigrated_mm_slot *mm_slot;
	struct mm_slot *slot;

	mm_slot = kmigrated_get_mm_slot(mm, false);

	slot = &mm_slot->mm_slot;

	if (mm_slot && slot && slot->mm == mm) {
		spin_lock(&mm_slot->migrate_lock);

		if (!list_empty(&mm_slot->migrate_head)) {
			if (mm == READ_ONCE(kmigrated_cur_mm)) {
				/* A folio in this mm is being migrated. wait */
				WRITE_ONCE(kmigrated_clean_list, true);
			}

			kmigrated_destroy_list(&mm_slot->migrate_head);
			spin_unlock(&mm_slot->migrate_lock);
retry:
			if (!spin_trylock(&mm_slot->migrate_lock)) {
				cpu_relax();
				goto retry;
			}

			if (mm == READ_ONCE(kmigrated_cur_mm)) {
				spin_unlock(&mm_slot->migrate_lock);
				goto retry;
			}
		}
		/* Reset migrated mm_slot if it was pointing to us */
		if (kmigrated_daemon.mm_slot == mm_slot)
			kmigrated_daemon.mm_slot = NULL;

		hash_del(&slot->hash);
		list_del(&slot->mm_node);
		mm_slot_free(kmigrated_slot_cache, mm_slot);

		WRITE_ONCE(kmigrated_clean_list, false);

		spin_unlock(&mm_slot->migrate_lock);
		}
}

static void kscand_collect_mm_slot(struct kscand_mm_slot *mm_slot)
{
	struct mm_slot *slot = &mm_slot->slot;
	struct mm_struct *mm = slot->mm;

	lockdep_assert_held(&kscand_mm_lock);

	if (kscand_test_exit(mm)) {
		hash_del(&slot->hash);
		list_del(&slot->mm_node);

		kscand_cleanup_migration_list(mm);

		mm_slot_free(kscand_slot_cache, mm_slot);
		mmdrop(mm);
	}
}

static void kmigrated_migrate_mm(struct kmigrated_mm_slot *mm_slot)
{
	int ret = 0, dest = -1;
	struct mm_slot *slot;
	struct mm_struct *mm;
	struct kscand_migrate_info *info, *tmp;

	spin_lock(&mm_slot->migrate_lock);

	slot = &mm_slot->mm_slot;
	mm = slot->mm;

	if (!list_empty(&mm_slot->migrate_head)) {
		list_for_each_entry_safe(info, tmp, &mm_slot->migrate_head,
				migrate_node) {
			if (READ_ONCE(kmigrated_clean_list))
				goto clean_list_handled;

			list_del(&info->migrate_node);

			spin_unlock(&mm_slot->migrate_lock);

			if (!mmap_read_trylock(mm)) {
				dest = kscand_get_target_node(NULL);
			} else {
				dest = READ_ONCE(mm->target_node);
				mmap_read_unlock(mm);
			}

			ret = kmigrated_promote_folio(info, mm, dest);

			kfree(info);

			cond_resched();
			spin_lock(&mm_slot->migrate_lock);
		}
	}
clean_list_handled:
	/* Reset  mm  of folio entry we are migrating */
	WRITE_ONCE(kmigrated_cur_mm, NULL);
	spin_unlock(&mm_slot->migrate_lock);
}

static void kmigrated_migrate_folio(void)
{
	/* for each mm do migrate */
	struct kmigrated_mm_slot *kmigrated_mm_slot = NULL;
	struct mm_slot *slot;

	if (!list_empty(&kmigrated_daemon.mm_head)) {

		scoped_guard (spinlock, &kscand_migrate_lock) {
			if (kmigrated_daemon.mm_slot) {
				kmigrated_mm_slot = kmigrated_daemon.mm_slot;
			} else {
				slot = list_entry(kmigrated_daemon.mm_head.next,
						struct mm_slot, mm_node);

				kmigrated_mm_slot = mm_slot_entry(slot,
						struct kmigrated_mm_slot, mm_slot);
				kmigrated_daemon.mm_slot = kmigrated_mm_slot;
			}
			WRITE_ONCE(kmigrated_cur_mm, kmigrated_mm_slot->mm_slot.mm);
		}

		if (kmigrated_mm_slot)
			kmigrated_migrate_mm(kmigrated_mm_slot);
	}
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
#define KSCAND_IGNORE_SCAN_THR	256

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
 *		scan_size = KSCAND_SCAN_SIZE_MAX
 *  case 3: (X > 0, Y = 0 )
 *		No change
 *  case 4: (X > 0, Y > 0)
 *		Increase scan_size by (1 << SCAN_SIZE_CHANGE_SHIFT).
 */
static inline void kscand_update_mmslot_info(struct kscand_mm_slot *mm_slot,
				unsigned long total, int target_node)
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
	if (total < KSCAND_IGNORE_SCAN_THR)
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
		scan_size = KSCAND_SCAN_SIZE_MAX;
	}

	scan_period = clamp(scan_period, KSCAND_SCAN_PERIOD_MIN, KSCAND_SCAN_PERIOD_MAX);
	scan_size = clamp(scan_size, KSCAND_SCAN_SIZE_MIN, KSCAND_SCAN_SIZE_MAX);

	now = jiffies;
	mm_slot->next_scan = now + msecs_to_jiffies(scan_period);
	mm_slot->scan_period = scan_period;
	mm_slot->scan_size = scan_size;
	mm_slot->scan_delta = total;
	mm_slot->target_node = target_node;
}

static unsigned long kscand_scan_mm_slot(void)
{
	bool next_mm = false;
	bool update_mmslot_info = false;

	unsigned int mm_slot_scan_period;
	int target_node, mm_slot_target_node, mm_target_node;
	unsigned long now;
	unsigned long mm_slot_next_scan;
	unsigned long mm_slot_scan_size;
	unsigned long vma_scanned_size = 0;
	unsigned long address;
	unsigned long total = 0;

	struct mm_slot *slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma = NULL;
	struct kscand_mm_slot *mm_slot;

	struct kmigrated_mm_slot *kmigrated_mm_slot = NULL;

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
	mm_slot_next_scan = mm_slot->next_scan;
	mm_slot_scan_period = mm_slot->scan_period;
	mm_slot_scan_size = mm_slot->scan_size;
	mm_slot_target_node = mm_slot->target_node;
	spin_unlock(&kscand_mm_lock);

	if (unlikely(!mmap_read_trylock(mm)))
		goto outerloop_mmap_lock;

	if (unlikely(kscand_test_exit(mm))) {
		next_mm = true;
		goto outerloop;
	}

	mm_target_node = READ_ONCE(mm->target_node);
	if (mm_target_node != mm_slot_target_node)
		WRITE_ONCE(mm->target_node, mm_slot_target_node);
	now = jiffies;

	if (mm_slot_next_scan && time_before(now, mm_slot_next_scan))
		goto outerloop;

	VMA_ITERATOR(vmi, mm, address);

	/* Either Scan 25% of scan_size or cover vma size of scan_size */
	kscand_scanctrl.nr_to_scan =	mm_slot_scan_size >> PAGE_SHIFT;
	/* Reduce actual amount of pages scanned */
	kscand_scanctrl.nr_to_scan =	mm_slot_scan_size >> 1;

	/* XXX: skip scanning to avoid duplicates until all migrations done? */
	kmigrated_mm_slot = kmigrated_get_mm_slot(mm, false);

	for_each_vma(vmi, vma) {
		kscand_walk_page_vma(vma, &kscand_scanctrl);
		vma_scanned_size += vma->vm_end - vma->vm_start;

		if (vma_scanned_size >= mm_slot_scan_size ||
					!kscand_scanctrl.nr_to_scan) {
			next_mm = true;

			if (!list_empty(&kscand_scanctrl.scan_list)) {
				if (!kmigrated_mm_slot)
					kmigrated_mm_slot = kmigrated_get_mm_slot(mm, true);
				/* Add scanned folios to migration list */
				spin_lock(&kmigrated_mm_slot->migrate_lock);

				list_splice_tail_init(&kscand_scanctrl.scan_list,
						&kmigrated_mm_slot->migrate_head);
				spin_unlock(&kmigrated_mm_slot->migrate_lock);
				break;
			}
		}
		if (!list_empty(&kscand_scanctrl.scan_list)) {
			if (!kmigrated_mm_slot)
				kmigrated_mm_slot = kmigrated_get_mm_slot(mm, true);
			spin_lock(&kmigrated_mm_slot->migrate_lock);
			list_splice_tail_init(&kscand_scanctrl.scan_list,
					&kmigrated_mm_slot->migrate_head);
			spin_unlock(&kmigrated_mm_slot->migrate_lock);
		}
	}

	if (!vma)
		address = 0;
	else
		address = kscand_scanctrl.address + PAGE_SIZE;

	update_mmslot_info = true;

	total = get_slowtier_accesed(&kscand_scanctrl);
	target_node = get_target_node(&kscand_scanctrl);

	mm_target_node = READ_ONCE(mm->target_node);

	/* XXX: Do we need write lock? */
	if (mm_target_node != target_node)
		WRITE_ONCE(mm->target_node, target_node);
	reset_scanctrl(&kscand_scanctrl);

	if (update_mmslot_info) {
		mm_slot->address = address;
		kscand_update_mmslot_info(mm_slot, total, target_node);
	}

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

#ifdef CONFIG_SYSFS
extern struct kobject *mm_kobj;
static int __init kscand_init_sysfs(struct kobject **kobj)
{
	int err;

	err = sysfs_create_group(*kobj, &kscand_attr_group);
	if (err) {
		pr_err("failed to register kscand group\n");
		goto err_kscand_attr;
	}

	return 0;

err_kscand_attr:
	sysfs_remove_group(*kobj, &kscand_attr_group);
	return err;
}

static void __init kscand_exit_sysfs(struct kobject *kobj)
{
		sysfs_remove_group(kobj, &kscand_attr_group);
}
#else
static inline int __init kscand_init_sysfs(struct kobject **kobj)
{
	return 0;
}
static inline void __init kscand_exit_sysfs(struct kobject *kobj)
{
}
#endif

static inline void kscand_destroy(void)
{
	kmem_cache_destroy(kscand_slot_cache);
	/* XXX: move below to kmigrated thread */
	kmem_cache_destroy(kmigrated_slot_cache);
	kscand_exit_sysfs(mm_kobj);
}

void __kscand_enter(struct mm_struct *mm)
{
	struct kscand_mm_slot *kscand_slot;
	struct mm_slot *slot;
	unsigned long now;
	int wakeup;

	/* __kscand_exit() must not run from under us */
	VM_BUG_ON_MM(kscand_test_exit(mm), mm);

	kscand_slot = mm_slot_alloc(kscand_slot_cache);

	if (!kscand_slot)
		return;

	now = jiffies;
	kscand_slot->address = 0;
	kscand_slot->scan_period = kscand_mm_scan_period_ms;
	kscand_slot->scan_size = kscand_scan_size;
	kscand_slot->next_scan = now +
			msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
	kscand_slot->scan_delta = 0;

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
	int free = 0, serialize = 1;

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
	} else if (mm_slot && kscand_scan.mm_slot == mm_slot && mm_slot->is_scanned) {
		serialize = 0;
	}

	spin_unlock(&kscand_mm_lock);

	if (serialize)
		kscand_cleanup_migration_list(mm);

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
	free_scanctrl(&kscand_scanctrl);

	return 0;
}

static int kmigrated(void *arg)
{
	while (true) {
		WRITE_ONCE(migrated_need_wakeup, false);
		if (unlikely(kthread_should_stop()))
			break;
		if (kmigrated_has_work())
			kmigrated_migrate_folio();
		msleep(20);
		kmigrated_wait_work();
	}
	return 0;
}

static int start_kmigrated(void)
{
	struct task_struct *kthread;

	guard(mutex)(&kmigrated_mutex);

	/* Someone already succeeded in starting daemon */
	if (kmigrated_thread)
		return 0;

	kthread = kthread_run(kmigrated, NULL, "kmigrated");
	if (IS_ERR(kmigrated_thread)) {
		pr_err("kmigrated: kthread_run(kmigrated)  failed\n");
		return PTR_ERR(kthread);
	}

	kmigrated_thread = kthread;
	pr_info("kmigrated: Successfully started kmigrated");

	wake_up_interruptible(&kmigrated_wait);

	return 0;
}

static int stop_kmigrated(void)
{
	guard(mutex)(&kmigrated_mutex);
	kthread_stop(kmigrated_thread);
	return 0;
}

static inline void init_list(void)
{
	INIT_LIST_HEAD(&kscand_scanctrl.scan_list);
	spin_lock_init(&kscand_migrate_lock);
	init_waitqueue_head(&kscand_wait);
	init_waitqueue_head(&kmigrated_wait);
	init_scanctrl(&kscand_scanctrl);
}

static int __init kscand_init(void)
{
	int err;

	kscand_slot_cache = KMEM_CACHE(kscand_mm_slot, 0);

	if (!kscand_slot_cache) {
		pr_err("kscand: kmem_cache error");
		return -ENOMEM;
	}

	kmigrated_slot_cache = KMEM_CACHE(kscand_mm_slot, 0);

	if (!kmigrated_slot_cache) {
		pr_err("kmigrated: kmem_cache error");
		return -ENOMEM;
	}

	err = kscand_init_sysfs(&mm_kobj);
	if (err)
		goto err_init_sysfs;

	init_list();
	err = start_kscand();
	if (err)
		goto err_kscand;

	err = start_kmigrated();
	if (err)
		goto err_kmigrated;

	return 0;

err_kmigrated:
	stop_kmigrated();

err_kscand:
	stop_kscand();
err_init_sysfs:
	kscand_destroy();

	return err;
}
subsys_initcall(kscand_init);
