// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/kvm_host.h>
#include <linux/pgalloc.h>
#include <asm/gmap_helpers.h>
#include "sk.h"
#include "gmap.h"

static void dat_update_ptep_sd(union pgste old, union pgste pgste, union pte *ptep)
{
	if (pgste.acc != old.acc || pgste.fp != old.fp || pgste.gr != old.gr || pgste.gc != old.gc)
		__atomic64_or(_PAGE_SD, &ptep->val);
}

int dat_get_storage_key(union asce asce, gfn_t gfn, union skey *skey)
{
	union crste *crstep;
	union pgste pgste;
	union pte *ptep;
	int rc;

	skey->skey = 0;
	rc = dat_entry_walk(NULL, gfn, asce, DAT_WALK_ANY, TABLE_TYPE_PAGE_TABLE, &crstep, &ptep);
	if (rc)
		return rc;

	if (!ptep) {
		union crste crste;

		crste = READ_ONCE(*crstep);
		if (!crste.h.fc || !crste.s.fc1.pr)
			return 0;
		skey->skey = page_get_storage_key(large_crste_to_phys(crste, gfn));
		return 0;
	}
	pgste = pgste_get_lock(ptep);
	if (ptep->h.i) {
		skey->acc = pgste.acc;
		skey->fp = pgste.fp;
	} else {
		skey->skey = page_get_storage_key(pte_origin(*ptep));
	}
	skey->r |= pgste.gr;
	skey->c |= pgste.gc;
	pgste_set_unlock(ptep, pgste);
	return 0;
}

int dat_set_storage_key(struct kvm_s390_mmu_cache *mc, union asce asce, gfn_t gfn,
			union skey skey, bool nq)
{
	union pgste pgste, old;
	union crste *crstep;
	union pte *ptep;
	int rc;

	rc = dat_entry_walk(mc, gfn, asce, DAT_WALK_LEAF_ALLOC, TABLE_TYPE_PAGE_TABLE,
			    &crstep, &ptep);
	if (rc)
		return rc;

	if (!ptep) {
		page_set_storage_key(large_crste_to_phys(*crstep, gfn), skey.skey, !nq);
		return 0;
	}

	old = pgste_get_lock(ptep);
	pgste = old;

	pgste.acc = skey.acc;
	pgste.fp = skey.fp;
	pgste.gc = skey.c;
	pgste.gr = skey.r;

	if (!ptep->h.i) {
		union skey old_skey;

		old_skey.skey = page_get_storage_key(pte_origin(*ptep));
		pgste.hc |= old_skey.c;
		pgste.hr |= old_skey.r;
		old_skey.c = old.gc;
		old_skey.r = old.gr;
		skey.r = 0;
		skey.c = 0;
		page_set_storage_key(pte_origin(*ptep), skey.skey, !nq);
	}

	dat_update_ptep_sd(old, pgste, ptep);
	pgste_set_unlock(ptep, pgste);
	return 0;
}

static bool page_cond_set_storage_key(phys_addr_t paddr, union skey skey, union skey *oldkey,
				      bool nq, bool mr, bool mc)
{
	oldkey->skey = page_get_storage_key(paddr);
	if (oldkey->acc == skey.acc && oldkey->fp == skey.fp &&
	    (oldkey->r == skey.r || mr) && (oldkey->c == skey.c || mc))
		return false;
	page_set_storage_key(paddr, skey.skey, !nq);
	return true;
}

int dat_cond_set_storage_key(struct kvm_s390_mmu_cache *mmc, union asce asce, gfn_t gfn,
			     union skey skey, union skey *oldkey, bool nq, bool mr, bool mc)
{
	union pgste pgste, old;
	union crste *crstep;
	union skey prev;
	union pte *ptep;
	int rc;

	rc = dat_entry_walk(mmc, gfn, asce, DAT_WALK_LEAF_ALLOC, TABLE_TYPE_PAGE_TABLE,
			    &crstep, &ptep);
	if (rc)
		return rc;

	if (!ptep)
		return page_cond_set_storage_key(large_crste_to_phys(*crstep, gfn), skey, oldkey,
						 nq, mr, mc);

	old = pgste_get_lock(ptep);
	pgste = old;

	rc = 1;
	pgste.acc = skey.acc;
	pgste.fp = skey.fp;
	pgste.gc = skey.c;
	pgste.gr = skey.r;

	if (!ptep->h.i) {
		rc = page_cond_set_storage_key(pte_origin(*ptep), skey, &prev, nq, mr, mc);
		pgste.hc |= prev.c;
		pgste.hr |= prev.r;
		prev.c |= old.gc;
		prev.r |= old.gr;
	} else {
		prev.acc = old.acc;
		prev.fp = old.fp;
		prev.c = old.gc;
		prev.r = old.gr;
	}
	if (oldkey)
		*oldkey = prev;

	dat_update_ptep_sd(old, pgste, ptep);
	pgste_set_unlock(ptep, pgste);
	return rc;
}

int dat_reset_reference_bit(union asce asce, gfn_t gfn)
{
	union pgste pgste, old;
	union crste *crstep;
	union pte *ptep;
	int rc;

	rc = dat_entry_walk(NULL, gfn, asce, DAT_WALK_ANY, TABLE_TYPE_PAGE_TABLE, &crstep, &ptep);
	if (rc)
		return rc;

	if (!ptep) {
		union crste crste = READ_ONCE(*crstep);

		if (!crste.h.fc || !crste.s.fc1.pr)
			return 0;
		return page_reset_referenced(large_crste_to_phys(*crstep, gfn));
	}
	old = pgste_get_lock(ptep);
	pgste = old;

	if (!ptep->h.i) {
		rc = page_reset_referenced(pte_origin(*ptep));
		pgste.hr = rc >> 1;
	}
	rc |= (pgste.gr << 1) | pgste.gc;
	pgste.gr = 0;

	dat_update_ptep_sd(old, pgste, ptep);
	pgste_set_unlock(ptep, pgste);
	return rc;
}

static long dat_reset_skeys_pte(union pte *ptep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	union pgste pgste;

	pgste = pgste_get_lock(ptep);
	pgste.acc = 0;
	pgste.fp = 0;
	pgste.gr = 0;
	pgste.gc = 0;
	if (ptep->s.pr)
		page_set_storage_key(pte_origin(*ptep), PAGE_DEFAULT_KEY, 1);
	pgste_set_unlock(ptep, pgste);

	if (need_resched())
		return next;
	return 0;
}

static long dat_reset_skeys_crste(union crste *crstep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	phys_addr_t addr, end, origin = crste_origin_large(*crstep);

	if (!crstep->h.fc || !crstep->s.fc1.pr)
		return 0;

	addr = ((max(gfn, walk->start) - gfn) << PAGE_SHIFT) + origin;
	end = ((min(next, walk->end) - gfn) << PAGE_SHIFT) + origin;
	while (ALIGN(addr + 1, _SEGMENT_SIZE) <= end)
		addr = sske_frame(addr, PAGE_DEFAULT_KEY);
	for ( ; addr < end; addr += PAGE_SIZE)
		page_set_storage_key(addr, PAGE_DEFAULT_KEY, 1);

	if (need_resched())
		return next;
	return 0;
}

long dat_reset_skeys(union asce asce, gfn_t start)
{
	const struct dat_walk_ops ops = {
		.pte_entry = dat_reset_skeys_pte,
		.pmd_entry = dat_reset_skeys_crste,
		.pud_entry = dat_reset_skeys_crste,
	};

	return _dat_walk_gfn_range(start, asce_end(asce), asce, &ops, DAT_WALK_IGN_HOLES, NULL);
}

static int _gmap_enable_skeys(struct gmap *gmap)
{
	gfn_t start = 0;
	int rc;

	if (uses_skeys(gmap))
		return 0;

	set_bit(GMAP_FLAG_USES_SKEYS, &gmap->flags);
	rc = gmap_helper_disable_cow_sharing();
	if (rc) {
		clear_bit(GMAP_FLAG_USES_SKEYS, &gmap->flags);
		return rc;
	}

	do {
		scoped_guard(write_lock, &gmap->kvm->mmu_lock)
			start = dat_reset_skeys(gmap->asce, start);
		cond_resched();
	} while (start);
	return 0;
}

int gmap_enable_skeys(struct gmap *gmap)
{
	int rc;

	mmap_write_lock(gmap->kvm->mm);
	rc = _gmap_enable_skeys(gmap);
	mmap_write_unlock(gmap->kvm->mm);
	return rc;
}
