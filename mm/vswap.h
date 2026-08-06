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

/*
 * VSWAP_ZERO and VSWAP_FOLIO are return-only values synthesized from
 * swap_table state; the rest are stored in the vtable per slot.
 */
enum vswap_backing_type {
	VSWAP_NONE	= 0,
	VSWAP_ZSWAP	= 1,
	VSWAP_ZERO,
	VSWAP_FOLIO,
};

#ifdef CONFIG_VSWAP

#include "swap_table.h"

extern struct swap_info_struct *vswap_si;

static inline bool is_vswap_entry(swp_entry_t entry)
{
	return swap_is_vswap(__swap_entry_to_info(entry));
}

bool vswap_is_enabled(void);

/*
 * Virtual table entry encoding for vswap clusters.
 *
 * Each entry in ci_dyn->virtual_table stores the backing type and
 * pointer for a virtual swap slot. Tag in low 3 bits, payload in
 * upper 61 bits.
 *
 *   NONE:   |----- 0000 ------|000|  - no separate backend pointer
 *   ZSWAP:  |--- zswap_entry* |001|  - compressed in zswap (tag in low bits)
 *
 * Pointer payloads (ZSWAP) are stored directly with the tag OR'd into the
 * low bits (kernel pointers are >= 8-byte aligned, same approach as xarray).
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
 *
 * Locking: a slot's vtable entry (the vswap entry's backend) is only
 * stable while the caller owns and holds the lock on that entry's swap
 * cache folio. The cluster lock (ci_dyn->ci.lock) only makes an individual
 * vtable read atomic, and by itself does not give the caller the right to
 * change the backend. A backend read without the folio lock is
 * best-effort and must be re-validated under the folio lock before
 * being acted on.
 *
 * Zero-backed slots use the swap_table per-slot zero flag (same as
 * direct-mapped physical swap), since CONFIG_VSWAP requires 64BIT and
 * SWAP_TABLE_HAS_ZEROFLAG is always true on 64-bit. Cached folios are
 * read out of the swap_table PFN entry; there is no separate FOLIO
 * vtable type because the folio pointer would duplicate that PFN and
 * would go stale on folio migration / split.
 */

#define VTABLE_TAG_BITS		3
#define VTABLE_TAG_MASK		((1UL << VTABLE_TAG_BITS) - 1)

static inline enum vswap_backing_type vtable_type(unsigned long vt)
{
	return vt & VTABLE_TAG_MASK;
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

/**
 * vswap_lock_cluster - look up and lock the vswap cluster for an entry
 * @entry: the virtual swap entry
 * @voff: out param, receives @entry's slot offset within the cluster
 *
 * Return: the locked vswap cluster, or NULL if no cluster is found for @entry.
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

void __vswap_release_backing(struct swap_cluster_info *ci,
			     unsigned int ci_start, unsigned int nr);

/**
 * vswap_zswap_store - record a zswap entry as the backing for a vswap entry.
 * @entry: the vswap entry
 * @ze: the zswap entry now holding @entry's compressed data
 *
 * Releases @entry's previous backing, and sets the zswap entry @ze as the new
 * backing.
 *
 * Context: takes and drops the vswap cluster lock internally.
 */
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

/**
 * vswap_zswap_load - return the zswap entry backing a vswap entry
 * @entry: the virtual swap entry
 *
 * Context: takes and drops the vswap cluster lock internally.
 * Return: the backing zswap entry, or NULL if @entry is not zswap-backed.
 */
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

static inline bool vswap_is_enabled(void) { return false; }

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

static inline int vswap_cluster_alloc_vtable(struct swap_cluster_info_dynamic *ci_dyn)
{
	return 0;
}

static inline void vswap_cluster_free_vtable(struct swap_cluster_info *ci) {}

#endif /* CONFIG_VSWAP */

#endif /* _MM_VSWAP_H */
