/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Virtual swap space
 *
 * Copyright (C) 2026 Nhat Pham
 */
#ifndef _MM_VSWAP_H
#define _MM_VSWAP_H


#include <linux/swap.h>
#include "swap.h"

struct zswap_entry;

static inline bool swap_is_vswap(struct swap_info_struct *si)
{
	return si->flags & SWP_VSWAP;
}

/*
 * Backing type enum. The first three are stored in the vtable per slot;
 * the last two are return-only and synthesized by vswap_check_backing()
 * from swap_table state.
 */
enum vswap_backing_type {
	VSWAP_NONE	= 0,
	VSWAP_SWAPFILE	= 1,
	VSWAP_ZSWAP	= 2,
	VSWAP_ZERO,
	VSWAP_FOLIO,
};

#ifdef CONFIG_VSWAP

#include "swap_table.h"

static inline bool is_vswap_entry(swp_entry_t entry)
{
	return swap_is_vswap(__swap_entry_to_info(entry));
}

/*
 * Rmap cache-only helpers for physical cluster Pointer-tagged entries.
 * SWP_RMAP_CACHE_ONLY records, inline on the physical swap_table entry,
 * that the backing vswap entry has swap_count == 0 (swap-cache-only, so
 * reclaimable). The physical reclaim scanner reads it directly instead of
 * chasing the rmap into the vswap layer and paying the cluster-lookup
 * indirection.
 */

static inline void swap_rmap_mark_cache_only(struct swap_cluster_info *ci,
					     unsigned int off)
{
	atomic_long_t *table;

	table = rcu_dereference_check(ci->table, true);
	atomic_long_or(SWP_RMAP_CACHE_ONLY, &table[off]);
}

static inline void swap_rmap_clear_cache_only(struct swap_cluster_info *ci,
					      unsigned int off)
{
	atomic_long_t *table;

	table = rcu_dereference_check(ci->table, true);
	atomic_long_and(~SWP_RMAP_CACHE_ONLY, &table[off]);
}

static inline bool swap_rmap_is_cache_only(struct swap_cluster_info *ci,
					   unsigned int off)
{
	atomic_long_t *table;
	bool ret;

	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	rcu_read_lock();
	table = rcu_dereference(ci->table);
	ret = table && (atomic_long_read(&table[off]) & SWP_RMAP_CACHE_ONLY);
	rcu_read_unlock();
	return ret;
}

/*
 * Virtual table entry encoding for vswap clusters.
 *
 * Each entry in ci_dyn->virtual_table stores the backing type and
 * pointer for a virtual swap slot. Tag in low 3 bits, payload in
 * upper 61 bits.
 *
 *   NONE:   |----- 0000 ------|000|  - no separate backend pointer
 *   PHYS:   |-- (type:5,off:N)|001|  - on a physical swapfile (shifted)
 *   ZSWAP:  |--- zswap_entry* |010|  - compressed in zswap (tag in low bits)
 *
 * PHYS payloads are shifted left by 3. Pointer payloads (ZSWAP) are
 * stored directly with the tag OR'd into the low bits (kernel pointers
 * are >= 8-byte aligned, same approach as xarray).
 *
 * vtable[i] = NONE does not by itself mean "free". The swap_table entry
 * and the per-slot zero flag carry the rest of the state. The full
 * per-slot state table is:
 *
 *   vtable[i] | swap_table[i] | zero  | meaning
 *   ----------+---------------+-------+--------------------------------
 *   NONE      | NULL          | clear | truly free / unbacked
 *   NONE      | PFN           | clear | folio cached, no backing
 *   NONE      | shadow        | clear | folio evicted, no backing (bug)
 *   NONE      | *             | set   | zero-backed; cached if PFN set
 *   ZSWAP     | PFN           | clear | folio cached + zswap entry
 *   ZSWAP     | shadow / NULL | clear | evicted, only in zswap
 *   SWAPFILE  | PFN           | clear | folio cached + phys backing
 *   SWAPFILE  | shadow / NULL | clear | evicted, only on phys swap
 *
 * Zero-backed slots use the swap_table per-slot zero flag (same as
 * direct-mapped physical swap), since CONFIG_VSWAP requires 64BIT and
 * SWAP_TABLE_HAS_ZEROFLAG is always true on 64-bit. Cached folios are
 * read out of the swap_table PFN entry; there is no separate FOLIO
 * vtable type because the folio pointer would duplicate that PFN and
 * would go stale on folio migration / split.
 *
 * enum vswap_backing_type is declared above. VSWAP_ZERO and VSWAP_FOLIO
 * are return-only synthesized values from vswap_check_backing(); they are
 * never used as vtable tags.
 */

#define VTABLE_TAG_BITS		3
#define VTABLE_TAG_MASK		((1UL << VTABLE_TAG_BITS) - 1)

static inline enum vswap_backing_type vtable_type(unsigned long vt)
{
	return vt & VTABLE_TAG_MASK;
}

static inline unsigned long vtable_payload(unsigned long vt)
{
	return vt >> VTABLE_TAG_BITS;
}

static inline unsigned long vtable_mk(enum vswap_backing_type type,
				       unsigned long payload)
{
	return (payload << VTABLE_TAG_BITS) | type;
}

static inline unsigned long vtable_mk_none(void)
{
	return 0;
}

static inline unsigned long vtable_mk_phys(swp_entry_t entry)
{
	return vtable_mk(VSWAP_SWAPFILE, entry.val);
}

static inline swp_entry_t vtable_to_phys(unsigned long vt)
{
	swp_entry_t entry;

	VM_WARN_ON(vtable_type(vt) != VSWAP_SWAPFILE);
	entry.val = vtable_payload(vt);
	return entry;
}

static inline struct zswap_entry *vtable_to_zswap(unsigned long vt)
{
	VM_WARN_ON(vtable_type(vt) != VSWAP_ZSWAP);
	return (struct zswap_entry *)(vt & ~VTABLE_TAG_MASK);
}

/* Virtual table accessors */

static inline unsigned long __vtable_get(struct swap_cluster_info_dynamic *ci_dyn,
					 unsigned int off)
{
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	return atomic_long_read(&ci_dyn->virtual_table[off]);
}

static inline void __vtable_set(struct swap_cluster_info_dynamic *ci_dyn,
				unsigned int off, unsigned long vt)
{
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	atomic_long_set(&ci_dyn->virtual_table[off], vt);
}

/*
 * Lock a vswap cluster and return the dynamic info + slot offset.
 * Returns NULL if cluster not found.
 * Caller must spin_unlock(&ci_dyn->ci.lock) when done.
 */
static inline struct swap_cluster_info_dynamic *
vswap_lock_cluster(swp_entry_t entry, unsigned int *voff)
{
	struct swap_cluster_info *ci;
	struct swap_cluster_info_dynamic *ci_dyn;

	ci = __swap_entry_to_cluster(entry);
	if (!ci)
		return NULL;
	ci_dyn = container_of(ci, struct swap_cluster_info_dynamic, ci);
	*voff = swp_cluster_offset(entry);
	spin_lock(&ci->lock);
	return ci_dyn;
}

static inline swp_entry_t vswap_to_phys(swp_entry_t entry)
{
	struct swap_cluster_info_dynamic *ci_dyn;
	unsigned int voff;
	unsigned long vt;

	ci_dyn = vswap_lock_cluster(entry, &voff);
	if (!ci_dyn)
		return (swp_entry_t){};

	vt = __vtable_get(ci_dyn, voff);
	spin_unlock(&ci_dyn->ci.lock);

	if (vtable_type(vt) != VSWAP_SWAPFILE)
		return (swp_entry_t){};

	return vtable_to_phys(vt);
}

void __vswap_release_backing(struct swap_cluster_info *ci,
			     unsigned int ci_start, unsigned int nr);

static inline void vswap_zswap_store(swp_entry_t entry,
				     struct zswap_entry *ze)
{
	struct swap_cluster_info_dynamic *ci_dyn;
	unsigned int voff;

	ci_dyn = vswap_lock_cluster(entry, &voff);
	if (!ci_dyn)
		return;
	__vswap_release_backing(&ci_dyn->ci, voff, 1);
	__vtable_set(ci_dyn, voff, (unsigned long)ze | VSWAP_ZSWAP);
	spin_unlock(&ci_dyn->ci.lock);
}

static inline struct zswap_entry *vswap_zswap_load(swp_entry_t entry)
{
	struct swap_cluster_info_dynamic *ci_dyn;
	unsigned int voff;
	unsigned long vt;

	ci_dyn = vswap_lock_cluster(entry, &voff);
	if (!ci_dyn)
		return NULL;
	vt = __vtable_get(ci_dyn, voff);
	spin_unlock(&ci_dyn->ci.lock);

	if (vtable_type(vt) != VSWAP_ZSWAP)
		return NULL;
	return vtable_to_zswap(vt);
}


void folio_release_vswap_backing(struct folio *folio);
void folio_release_non_phys_swap_backing(struct folio *folio);

/*
 * Walk nr vtable slots starting at voff in ci_dyn. Returns the prefix
 * length of slots sharing one effective backing type. For SWAPFILE,
 * the prefix is also restricted to contiguous offsets in the same
 * swapfile.
 *
 * Effective type per slot (zero flag takes precedence over PFN since
 * zero is a backend state and the cached folio is just an overlay):
 *   vtable=NONE + zero flag set       -> VSWAP_ZERO
 *   vtable=NONE + swap_table PFN tag  -> VSWAP_FOLIO
 *   vtable=NONE + neither             -> VSWAP_NONE
 *   vtable=SWAPFILE                   -> VSWAP_SWAPFILE
 *   vtable=ZSWAP                      -> VSWAP_ZSWAP
 *
 * *typep returns the effective type of slot 0. Caller holds
 * ci_dyn->ci.lock.
 */
static inline int __vswap_check_backing(struct swap_cluster_info_dynamic *ci_dyn,
					unsigned int voff, int nr,
					enum vswap_backing_type *typep)
{
	enum vswap_backing_type first_type = VSWAP_NONE;
	enum vswap_backing_type slot_type;
	swp_entry_t first_phys = {};
	unsigned long vt, swap_tb;
	int i;

	lockdep_assert_held(&ci_dyn->ci.lock);

	for (i = 0; i < nr; i++) {
		vt = __vtable_get(ci_dyn, voff + i);
		if (vtable_type(vt) == VSWAP_NONE) {
			swap_tb = __swap_table_get(&ci_dyn->ci, voff + i);
			if (__swap_table_test_zero(&ci_dyn->ci, voff + i))
				slot_type = VSWAP_ZERO;
			else if (swp_tb_is_folio(swap_tb))
				slot_type = VSWAP_FOLIO;
			else
				slot_type = VSWAP_NONE;
		} else {
			slot_type = vtable_type(vt);
		}

		if (!i) {
			first_type = slot_type;
			if (first_type == VSWAP_SWAPFILE)
				first_phys = vtable_to_phys(vt);
		} else if (slot_type != first_type) {
			break;
		} else if (first_type == VSWAP_SWAPFILE &&
			   vtable_to_phys(vt).val != first_phys.val + i) {
			break;
		}
	}

	if (typep)
		*typep = first_type;
	return i;
}

static inline int vswap_check_backing(swp_entry_t entry, int nr,
				      enum vswap_backing_type *typep)
{
	struct swap_cluster_info_dynamic *ci_dyn;
	unsigned int voff;
	int ret;

	ci_dyn = vswap_lock_cluster(entry, &voff);
	if (!ci_dyn) {
		if (typep)
			*typep = VSWAP_NONE;
		return 0;
	}
	ret = __vswap_check_backing(ci_dyn, voff, nr, typep);
	spin_unlock(&ci_dyn->ci.lock);
	return ret;
}

static inline bool folio_phys_swap_backed(struct folio *folio)
{
	swp_entry_t entry = folio->swap;
	int nr = folio_nr_pages(folio);
	enum vswap_backing_type type;

	return !is_vswap_entry(entry) ||
	       (vswap_check_backing(entry, nr, &type) == nr &&
		type == VSWAP_SWAPFILE);
}

static inline int vswap_cluster_alloc_vtable(struct swap_cluster_info_dynamic *ci_dyn)
{
	ci_dyn->virtual_table = kcalloc(SWAPFILE_CLUSTER,
					sizeof(*ci_dyn->virtual_table),
					GFP_ATOMIC);
	return ci_dyn->virtual_table ? 0 : -ENOMEM;
}

static inline void vswap_cluster_free_vtable(struct swap_cluster_info *ci)
{
	struct swap_cluster_info_dynamic *ci_dyn;

	ci_dyn = container_of(ci, struct swap_cluster_info_dynamic, ci);
	kfree(ci_dyn->virtual_table);
	ci_dyn->virtual_table = NULL;
}

#else /* !CONFIG_VSWAP */

static inline bool is_vswap_entry(swp_entry_t entry)
{
	return false;
}

static inline swp_entry_t vswap_to_phys(swp_entry_t entry)
{
	return (swp_entry_t){};
}

static inline bool folio_phys_swap_backed(struct folio *folio)
{
	return true;
}

static inline bool swap_rmap_is_cache_only(struct swap_cluster_info *ci,
					   unsigned int off)
{
	return false;
}

static inline void __vswap_release_backing(struct swap_cluster_info *ci,
					   unsigned int ci_start,
					   unsigned int nr) {}

static inline void vswap_zswap_store(swp_entry_t entry,
				     struct zswap_entry *ze) {}

static inline struct zswap_entry *vswap_zswap_load(swp_entry_t entry)
{
	return NULL;
}

static inline void folio_release_vswap_backing(struct folio *folio) {}
static inline void folio_release_non_phys_swap_backing(struct folio *folio) {}

struct swap_cluster_info_dynamic;
static inline int __vswap_check_backing(struct swap_cluster_info_dynamic *ci_dyn,
					unsigned int voff, int nr,
					enum vswap_backing_type *typep)
{
	return 0;
}

static inline int vswap_cluster_alloc_vtable(struct swap_cluster_info_dynamic *ci_dyn)
{
	return 0;
}

static inline void vswap_cluster_free_vtable(struct swap_cluster_info *ci) {}

#endif /* CONFIG_VSWAP */

/*
 * Test a per-backend swap flag (SWP_SYNCHRONOUS_IO, SWP_STABLE_WRITES, ...)
 * for @entry. For a vswap entry the property belongs to the current
 * physical backing rather than vswap_si itself; resolve to the backing
 * and test there. Returns false for zswap/zero/unbacked vswap entries
 * as they don't have a backing bdev.
 */
static inline bool swap_entry_backend_has_flag(struct swap_info_struct *si,
					       swp_entry_t entry,
					       unsigned long flag)
{
	struct swap_info_struct *phys_si;
	swp_entry_t phys;
	bool has_flag;

	if (!swap_is_vswap(si))
		return data_race(si->flags & flag);

	phys = vswap_to_phys(entry);
	if (!phys.val)
		return false;

	phys_si = get_swap_device(phys);
	if (!phys_si)
		return false;

	has_flag = data_race(phys_si->flags & flag);
	put_swap_device(phys_si);
	return has_flag;
}

#endif /* _MM_VSWAP_H */
