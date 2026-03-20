/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_SWAP_H
#define _MM_SWAP_H

#include <linux/atomic.h> /* for atomic_long_t */
struct mempolicy;
struct swap_iocb;

extern int page_cluster;

#ifdef CONFIG_THP_SWAP
#define SWAPFILE_CLUSTER	HPAGE_PMD_NR
#define swap_slot_order(order)	(order)
#else
#define SWAPFILE_CLUSTER	256
#define swap_slot_order(order)	0
#endif

extern struct swap_info_struct *swap_info[];

/*
 * We use this to track usage of a cluster. A cluster is a block of swap disk
 * space with SWAPFILE_CLUSTER pages long and naturally aligns in disk. All
 * free clusters are organized into a list. We fetch an entry from the list to
 * get a free cluster.
 *
 * The flags field determines if a cluster is free. This is
 * protected by cluster lock.
 */
struct swap_cluster_info {
	spinlock_t lock;	/*
				 * Protect swap_cluster_info fields
				 * other than list, and swap_info_struct->swap_map
				 * elements corresponding to the swap cluster.
				 */
	u16 count;
	u8 flags;
	u8 order;
	/*
	 * Reverse map, to look up the virtual swap slot backed by a given physical
	 * swap slot.
	 */
	atomic_long_t __rcu *table;
	struct list_head list;
};

/* All on-list cluster must have a non-zero flag. */
enum swap_cluster_flags {
	CLUSTER_FLAG_NONE = 0, /* For temporary off-list cluster */
	CLUSTER_FLAG_FREE,
	CLUSTER_FLAG_NONFULL,
	CLUSTER_FLAG_FRAG,
	/* Clusters with flags above are allocatable */
	CLUSTER_FLAG_USABLE = CLUSTER_FLAG_FRAG,
	CLUSTER_FLAG_FULL,
	CLUSTER_FLAG_DISCARD,
	CLUSTER_FLAG_MAX,
};

#ifdef CONFIG_SWAP
#include <linux/swapops.h> /* for swp_offset */
#include <linux/blk_types.h> /* for bio_end_io_t */

static inline unsigned int swp_cluster_offset(swp_slot_t slot)
{
	return swp_slot_offset(slot) % SWAPFILE_CLUSTER;
}

/*
 * Callers of all helpers below must ensure the entry, type, or offset is
 * valid, and protect the swap device with reference count or locks.
 */
static inline struct swap_info_struct *__swap_type_to_info(int type)
{
	struct swap_info_struct *si;

	si = READ_ONCE(swap_info[type]); /* rcu_dereference() */
	VM_WARN_ON_ONCE(percpu_ref_is_zero(&si->users)); /* race with swapoff */
	return si;
}

static inline struct swap_info_struct *__swap_slot_to_info(swp_slot_t slot)
{
	return __swap_type_to_info(swp_slot_type(slot));
}

static inline struct swap_cluster_info *__swap_offset_to_cluster(
		struct swap_info_struct *si, pgoff_t offset)
{
	VM_WARN_ON_ONCE(percpu_ref_is_zero(&si->users)); /* race with swapoff */
	VM_WARN_ON_ONCE(offset >= si->max);
	return &si->cluster_info[offset / SWAPFILE_CLUSTER];
}

static inline struct swap_cluster_info *__swap_slot_to_cluster(swp_slot_t slot)
{
	return __swap_offset_to_cluster(__swap_slot_to_info(slot),
					swp_slot_offset(slot));
}

static __always_inline struct swap_cluster_info *__swap_cluster_lock(
		struct swap_info_struct *si, unsigned long offset, bool irq)
{
	struct swap_cluster_info *ci = __swap_offset_to_cluster(si, offset);

	/*
	 * Nothing modifies swap cache in an IRQ context. All access to
	 * swap cache is wrapped by swap_cache_* helpers, and swap cache
	 * writeback is handled outside of IRQs. Swapin or swapout never
	 * occurs in IRQ, and neither does in-place split or replace.
	 *
	 * Besides, modifying swap cache requires synchronization with
	 * swap_map, which was never IRQ safe.
	 */
	VM_WARN_ON_ONCE(!in_task());
	VM_WARN_ON_ONCE(percpu_ref_is_zero(&si->users)); /* race with swapoff */
	if (irq)
		spin_lock_irq(&ci->lock);
	else
		spin_lock(&ci->lock);
	return ci;
}

/**
 * swap_cluster_lock - Lock and return the swap cluster of given offset.
 * @si: swap device the cluster belongs to.
 * @offset: the swap slot offset, pointing to a valid slot.
 *
 * Context: The caller must ensure the offset is in the valid range and
 * protect the swap device with reference count or locks.
 */
static inline struct swap_cluster_info *swap_cluster_lock(
		struct swap_info_struct *si, unsigned long offset)
{
	return __swap_cluster_lock(si, offset, false);
}

static inline struct swap_cluster_info *__swap_cluster_get_and_lock(
		const struct folio *folio, bool irq)
{
	swp_slot_t slot = swp_entry_to_swp_slot(folio->swap);

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	return __swap_cluster_lock(__swap_slot_to_info(slot),
				   swp_slot_offset(slot), irq);
}

/*
 * swap_cluster_get_and_lock - Locks the cluster that holds a folio's entries.
 * @folio: The folio.
 *
 * This locks and returns the swap cluster that contains a folio's swap
 * entries. The swap entries of a folio are always in one single cluster.
 * The folio has to be locked so its swap entries won't change and the
 * cluster won't be freed.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache.
 * Return: Pointer to the swap cluster.
 */
static inline struct swap_cluster_info *swap_cluster_get_and_lock(
		const struct folio *folio)
{
	return __swap_cluster_get_and_lock(folio, false);
}

/*
 * swap_cluster_get_and_lock_irq - Locks the cluster that holds a folio's entries.
 * @folio: The folio.
 *
 * Same as swap_cluster_get_and_lock but also disable IRQ.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache.
 * Return: Pointer to the swap cluster.
 */
static inline struct swap_cluster_info *swap_cluster_get_and_lock_irq(
		const struct folio *folio)
{
	return __swap_cluster_get_and_lock(folio, true);
}

static inline void swap_cluster_unlock(struct swap_cluster_info *ci)
{
	spin_unlock(&ci->lock);
}

static inline void swap_cluster_unlock_irq(struct swap_cluster_info *ci)
{
	spin_unlock_irq(&ci->lock);
}

/* linux/mm/page_io.c */
int sio_pool_init(void);
struct swap_iocb;
void swap_read_folio(struct folio *folio, struct swap_iocb **plug);
void __swap_read_unplug(struct swap_iocb *plug);
static inline void swap_read_unplug(struct swap_iocb *plug)
{
	if (unlikely(plug))
		__swap_read_unplug(plug);
}
void swap_write_unplug(struct swap_iocb *sio);
int swap_writeout(struct folio *folio, struct swap_iocb **swap_plug);
void __swap_writepage(struct folio *folio, struct swap_iocb **swap_plug);

/* linux/mm/swap_state.c */
extern struct address_space swap_space __read_mostly;

/* linux/mm/vswap.c */
void swap_cache_lock_irq(swp_entry_t entry);
void swap_cache_unlock_irq(swp_entry_t entry);
void swap_cache_lock(swp_entry_t entry);
void swap_cache_unlock(swp_entry_t entry);
void vswap_rmap_set(struct swap_cluster_info *ci, swp_slot_t slot,
			   unsigned long vswap, int nr);

static inline struct address_space *swap_address_space(swp_entry_t entry)
{
	return &swap_space;
}

/* Return the swap device position of the swap slot. */
static inline loff_t swap_slot_dev_pos(swp_slot_t slot)
{
	return ((loff_t)swp_slot_offset(slot)) << PAGE_SHIFT;
}

/**
 * folio_matches_swap_entry - Check if a folio matches a given swap entry.
 * @folio: The folio.
 * @entry: The swap entry to check against.
 *
 * Context: The caller should have the folio locked to ensure it's stable
 * and nothing will move it in or out of the swap cache.
 * Return: true or false.
 */
static inline bool folio_matches_swap_entry(const struct folio *folio,
					    swp_entry_t entry)
{
	swp_entry_t folio_entry = folio->swap;
	long nr_pages = folio_nr_pages(folio);

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	if (!folio_test_swapcache(folio))
		return false;
	VM_WARN_ON_ONCE_FOLIO(!IS_ALIGNED(folio_entry.val, nr_pages), folio);
	return folio_entry.val == round_down(entry.val, nr_pages);
}

/**
 * folio_matches_swap_slot - Check if a folio matches both the virtual
 *                           swap entry and its backing physical swap slot.
 * @folio: The folio.
 * @entry: The virtual swap entry to check against.
 * @slot: The physical swap slot to check against.
 *
 * Context: The caller should have the folio locked to ensure it's stable
 * and nothing will move it in or out of the swap cache.
 * Return: true if both checks pass, false otherwise.
 */
static inline bool folio_matches_swap_slot(const struct folio *folio,
					   swp_entry_t entry,
					   swp_slot_t slot)
{
	if (!folio_matches_swap_entry(folio, entry))
		return false;

	/*
	 * Confirm the virtual swap entry is still backed by the same
	 * physical swap slot.
	 */
	return slot.val == swp_entry_to_swp_slot(entry).val;
}

/*
 * All swap cache helpers below require the caller to ensure the swap entries
 * used are valid and stablize the device by any of the following ways:
 * - Hold a reference by get_swap_device(): this ensures a single entry is
 *   valid and increases the swap device's refcount.
 * - Locking a folio in the swap cache: this ensures the folio's swap entries
 *   are valid and pinned, also implies reference to the device.
 * - Locking anything referencing the swap entry: e.g. PTL that protects
 *   swap entries in the page table, similar to locking swap cache folio.
 * - See the comment of get_swap_device() for more complex usage.
 */
struct folio *swap_cache_get_folio(swp_entry_t entry);
void *swap_cache_get_shadow(swp_entry_t entry);
void swap_cache_add_folio(struct folio *folio, swp_entry_t entry,
			  void **shadow);
void swap_cache_del_folio(struct folio *folio);
/* Below helpers require the caller to lock the swap cache. */
void __swap_cache_del_folio(struct folio *folio, swp_entry_t entry, void *shadow);
void __swap_cache_replace_folio(struct folio *old, struct folio *new);

void show_swap_cache_info(void);
void swapcache_clear(swp_entry_t entry, int nr);
struct folio *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_area_struct *vma, unsigned long addr,
		struct swap_iocb **plug);
struct folio *__read_swap_cache_async(swp_entry_t entry, gfp_t gfp_flags,
		struct mempolicy *mpol, pgoff_t ilx, bool *new_page_allocated,
		bool skip_if_exists);
struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t flag,
		struct mempolicy *mpol, pgoff_t ilx);
struct folio *swapin_readahead(swp_entry_t entry, gfp_t flag,
		struct vm_fault *vmf);
void swap_update_readahead(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long addr);

static inline unsigned int folio_swap_flags(struct folio *folio)
{
	swp_slot_t swp_slot = swp_entry_to_swp_slot(folio->swap);

	return __swap_slot_to_info(swp_slot)->flags;
}

/*
 * Return the count of contiguous swap entries that share the same
 * zeromap status as the starting entry. If is_zeromap is not NULL,
 * it will return the zeromap status of the starting entry.
 */
static inline int swap_zeromap_batch(swp_entry_t entry, int max_nr,
		bool *is_zeromap)
{
	swp_slot_t slot = swp_entry_to_swp_slot(entry);
	struct swap_info_struct *sis = __swap_slot_to_info(slot);
	unsigned long start = swp_slot_offset(slot);
	unsigned long end = start + max_nr;
	bool first_bit;

	first_bit = test_bit(start, sis->zeromap);
	if (is_zeromap)
		*is_zeromap = first_bit;

	if (max_nr <= 1)
		return max_nr;
	if (first_bit)
		return find_next_zero_bit(sis->zeromap, end, start) - start;
	else
		return find_next_bit(sis->zeromap, end, start) - start;
}

int non_swapcache_batch(swp_entry_t entry, int max_nr);

#else /* CONFIG_SWAP */
struct swap_iocb;
static inline struct swap_cluster_info *swap_cluster_lock(
	struct swap_info_struct *si, unsigned long offset)
{
	return NULL;
}

static inline struct swap_cluster_info *swap_cluster_get_and_lock(
		struct folio *folio)
{
	return NULL;
}

static inline struct swap_cluster_info *swap_cluster_get_and_lock_irq(
		struct folio *folio)
{
	return NULL;
}

static inline void swap_cluster_unlock(struct swap_cluster_info *ci)
{
}

static inline void swap_cluster_unlock_irq(struct swap_cluster_info *ci)
{
}

static inline struct swap_info_struct *__swap_slot_to_info(swp_slot_t slot)
{
	return NULL;
}

static inline void swap_read_folio(struct folio *folio, struct swap_iocb **plug)
{
}
static inline void swap_write_unplug(struct swap_iocb *sio)
{
}

static inline struct address_space *swap_address_space(swp_entry_t entry)
{
	return NULL;
}

static inline bool folio_matches_swap_entry(const struct folio *folio, swp_entry_t entry)
{
	return false;
}

static inline bool folio_matches_swap_slot(const struct folio *folio,
					   swp_entry_t entry,
					   swp_slot_t slot)
{
	return false;
}

static inline void show_swap_cache_info(void)
{
}

static inline struct folio *swap_cluster_readahead(swp_entry_t entry,
			gfp_t gfp_mask, struct mempolicy *mpol, pgoff_t ilx)
{
	return NULL;
}

static inline struct folio *swapin_readahead(swp_entry_t swp, gfp_t gfp_mask,
			struct vm_fault *vmf)
{
	return NULL;
}

static inline void swap_update_readahead(struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr)
{
}

static inline int swap_writeout(struct folio *folio,
		struct swap_iocb **swap_plug)
{
	return 0;
}

static inline void swapcache_clear(swp_entry_t entry, int nr)
{
}

static inline struct folio *swap_cache_get_folio(swp_entry_t entry)
{
	return NULL;
}

static inline void *swap_cache_get_shadow(swp_entry_t entry)
{
	return NULL;
}

static inline void swap_cache_add_folio(struct folio *folio, swp_entry_t entry,
					void **shadow)
{
}

static inline void swap_cache_del_folio(struct folio *folio)
{
}

static inline void __swap_cache_del_folio(struct folio *folio, swp_entry_t entry, void *shadow)
{
}

static inline void __swap_cache_replace_folio(struct folio *old, struct folio *new)
{
}

static inline void swap_cache_lock_irq(swp_entry_t entry)
{
}

static inline void swap_cache_unlock_irq(swp_entry_t entry)
{
}

static inline void swap_cache_lock(swp_entry_t entry)
{
}

static inline void swap_cache_unlock(swp_entry_t entry)
{
}

static inline unsigned int folio_swap_flags(struct folio *folio)
{
	return 0;
}

static inline int swap_zeromap_batch(swp_entry_t entry, int max_nr,
		bool *has_zeromap)
{
	return 0;
}

static inline int non_swapcache_batch(swp_entry_t entry, int max_nr)
{
	return 0;
}
#endif /* CONFIG_SWAP */
#endif /* _MM_SWAP_H */
