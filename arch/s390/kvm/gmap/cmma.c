// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/kvm_host.h>
#include "cmma.h"
#include "gmap.h"

int dat_perform_essa(union asce asce, gfn_t gfn, int orc, union essa_state *state, bool *dirty)
{
	union crste *crstep;
	union pgste pgste;
	union pte *ptep;
	int res = 0;

	if (dat_entry_walk(NULL, gfn, asce, 0, TABLE_TYPE_PAGE_TABLE, &crstep, &ptep)) {
		*state = (union essa_state) { .exception = 1 };
		return -1;
	}

	pgste = pgste_get_lock(ptep);

	*state = (union essa_state) {
		.content = (ptep->h.i << 1) + (ptep->h.i && pgste.zero),
		.nodat = pgste.nodat,
		.usage = pgste.usage,
		};

	switch (orc) {
	case ESSA_GET_STATE:
		res = -1;
		break;
	case ESSA_SET_STABLE:
		pgste.usage = PGSTE_GPS_USAGE_STABLE;
		pgste.nodat = 0;
		break;
	case ESSA_SET_UNUSED:
		pgste.usage = PGSTE_GPS_USAGE_UNUSED;
		if (ptep->h.i)
			res = 1;
		break;
	case ESSA_SET_VOLATILE:
		pgste.usage = PGSTE_GPS_USAGE_VOLATILE;
		if (ptep->h.i)
			res = 1;
		break;
	case ESSA_SET_POT_VOLATILE:
		if (!ptep->h.i) {
			pgste.usage = PGSTE_GPS_USAGE_POT_VOLATILE;
		} else if (pgste.zero) {
			pgste.usage = PGSTE_GPS_USAGE_VOLATILE;
		} else if (!pgste.gc) {
			pgste.usage = PGSTE_GPS_USAGE_VOLATILE;
			res = 1;
		}
		break;
	case ESSA_SET_STABLE_RESIDENT:
		pgste.usage = PGSTE_GPS_USAGE_STABLE;
		/*
		 * Since the resident state can go away any time after this
		 * call, we will not make this page resident. We can revisit
		 * this decision if a guest will ever start using this.
		 */
		break;
	case ESSA_SET_STABLE_IF_RESIDENT:
		if (!ptep->h.i)
			pgste.usage = PGSTE_GPS_USAGE_STABLE;
		break;
	case ESSA_SET_STABLE_NODAT:
		pgste.usage = PGSTE_GPS_USAGE_STABLE;
		pgste.nodat = 1;
		break;
	default:
		WARN_ONCE(1, "Invalid ORC!");
		res = -1;
		break;
	}
	/* If we are discarding a page, set it to logical zero. */
	pgste.zero = res == 1;
	if (orc > 0) {
		*dirty = !pgste.cmma_d;
		pgste.cmma_d = 1;
	}

	pgste_set_unlock(ptep, pgste);

	return res;
}

static long dat_reset_cmma_pte(union pte *ptep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	union pgste pgste;

	pgste = pgste_get_lock(ptep);
	pgste.usage = 0;
	pgste.nodat = 0;
	pgste.cmma_d = 0;
	pgste_set_unlock(ptep, pgste);
	if (need_resched())
		return next;
	return 0;
}

long dat_reset_cmma(union asce asce, gfn_t start)
{
	const struct dat_walk_ops dat_reset_cmma_ops = {
		.pte_entry = dat_reset_cmma_pte,
	};

	return _dat_walk_gfn_range(start, asce_end(asce), asce, &dat_reset_cmma_ops,
				   DAT_WALK_IGN_HOLES, NULL);
}

struct dat_get_cmma_state {
	gfn_t start;
	gfn_t end;
	unsigned int count;
	u8 *values;
	atomic64_t *remaining;
};

static long __dat_peek_cmma_pte(union pte *ptep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	struct dat_get_cmma_state *state = walk->priv;
	union pgste pgste;

	pgste = pgste_get_lock(ptep);
	state->values[gfn - walk->start] = pgste.usage | (pgste.nodat << 6);
	pgste_set_unlock(ptep, pgste);
	state->end = next;

	return 0;
}

static long __dat_peek_cmma_crste(union crste *crstep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	struct dat_get_cmma_state *state = walk->priv;

	if (crstep->h.i)
		state->end = min(walk->end, next);
	return 0;
}

int dat_peek_cmma(gfn_t start, union asce asce, unsigned int *count, u8 *values)
{
	const struct dat_walk_ops ops = {
		.pte_entry = __dat_peek_cmma_pte,
		.pmd_entry = __dat_peek_cmma_crste,
		.pud_entry = __dat_peek_cmma_crste,
		.p4d_entry = __dat_peek_cmma_crste,
		.pgd_entry = __dat_peek_cmma_crste,
	};
	struct dat_get_cmma_state state = { .values = values, };
	int rc;

	rc = _dat_walk_gfn_range(start, start + *count, asce, &ops, DAT_WALK_DEFAULT, &state);
	*count = state.end >= start ? state.end - start : 0;
	/* Return success if at least one value was saved, otherwise an error. */
	return (rc == -EFAULT && *count > 0) ? 0 : rc;
}

static long __dat_get_cmma_pte(union pte *ptep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	struct dat_get_cmma_state *state = walk->priv;
	union pgste pgste;

	if (state->start != -1) {
		if ((gfn - state->end) > KVM_S390_MAX_BIT_DISTANCE)
			return 1;
		if (gfn - state->start >= state->count)
			return 1;
	}

	if (!READ_ONCE(*pgste_of(ptep)).cmma_d)
		return 0;

	pgste = pgste_get_lock(ptep);
	if (pgste.cmma_d) {
		if (state->start == -1)
			state->start = gfn;
		pgste.cmma_d = 0;
		atomic64_dec(state->remaining);
		state->values[gfn - state->start] = pgste.usage | pgste.nodat << 6;
		state->end = next;
	}
	pgste_set_unlock(ptep, pgste);
	return 0;
}

int dat_get_cmma(union asce asce, gfn_t *start, unsigned int *count, u8 *values, atomic64_t *rem)
{
	const struct dat_walk_ops ops = { .pte_entry = __dat_get_cmma_pte, };
	struct dat_get_cmma_state state = {
		.remaining = rem,
		.values = values,
		.count = *count,
		.start = -1,
	};

	_dat_walk_gfn_range(*start, asce_end(asce), asce, &ops, DAT_WALK_IGN_HOLES, &state);
	/* If no dirty pages were found, wrap around and continue searching */
	if (*start && state.start == -1)
		_dat_walk_gfn_range(0, *start, asce, &ops, DAT_WALK_IGN_HOLES, &state);

	if (state.start == -1) {
		*count = 0;
	} else {
		*count = state.end - state.start;
		*start = state.start;
	}

	return 0;
}

struct dat_set_cmma_state {
	unsigned long mask;
	const u8 *bits;
};

static long __dat_set_cmma_pte(union pte *ptep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	struct dat_set_cmma_state *state = walk->priv;
	union pgste pgste, tmp;

	tmp.val = (state->bits[gfn - walk->start] << 24) & state->mask;

	pgste = pgste_get_lock(ptep);
	pgste.usage = tmp.usage;
	pgste.nodat = tmp.nodat;
	pgste_set_unlock(ptep, pgste);

	return 0;
}

/**
 * dat_set_cmma_bits() - Set CMMA bits for a range of guest pages.
 * @mc: Cache used for allocations.
 * @asce: The ASCE of the guest.
 * @gfn: The guest frame of the fist page whose CMMA bits are to set.
 * @count: How many pages need to be processed.
 * @mask: Which PGSTE bits should be set.
 * @bits: Points to an array with the CMMA attributes.
 *
 * This function sets the CMMA attributes for the given pages. If the input
 * buffer has zero length, no action is taken, otherwise the attributes are
 * set and the mm->context.uses_cmm flag is set.
 *
 * Each byte in @bits contains new values for bits 32-39 of the PGSTE.
 * Currently, only the fields NT and US are applied.
 *
 * Return: %0 in case of success, a negative error value otherwise.
 */
int dat_set_cmma_bits(struct kvm_s390_mmu_cache *mc, union asce asce, gfn_t gfn,
		      unsigned long count, unsigned long mask, const uint8_t *bits)
{
	const struct dat_walk_ops ops = { .pte_entry = __dat_set_cmma_pte, };
	struct dat_set_cmma_state state = { .mask = mask, .bits = bits, };
	union crste *crstep;
	union pte *ptep;
	gfn_t cur;
	int rc;

	for (cur = ALIGN_DOWN(gfn, _PAGE_ENTRIES); cur < gfn + count; cur += _PAGE_ENTRIES) {
		rc = dat_entry_walk(mc, cur, asce, DAT_WALK_ALLOC, TABLE_TYPE_PAGE_TABLE,
				    &crstep, &ptep);
		if (rc)
			return rc;
	}
	return _dat_walk_gfn_range(gfn, gfn + count, asce, &ops, DAT_WALK_IGN_HOLES, &state);
}

static long __set_cmma_clean_pte(union pte *ptep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	union pgste pgste;

	pgste = pgste_get_lock(ptep);
	pgste.cmma_d = 0;
	pgste_set_unlock(ptep, pgste);

	if (need_resched())
		return next;
	return 0;
}

static long __set_cmma_dirty_pte(union pte *ptep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	union pgste pgste;

	pgste = pgste_get_lock(ptep);
	if (!pgste.cmma_d)
		atomic64_inc(walk->priv);
	pgste.cmma_d = 1;
	pgste_set_unlock(ptep, pgste);

	if (need_resched())
		return next;
	return 0;
}

void _gmap_set_cmma_all(struct gmap *gmap, bool dirty)
{
	const struct dat_walk_ops ops = {
		.pte_entry = dirty ? __set_cmma_dirty_pte : __set_cmma_clean_pte,
	};
	gfn_t gfn = 0;

	do {
		scoped_guard(read_lock, &gmap->kvm->mmu_lock)
			gfn = _dat_walk_gfn_range(gfn, asce_end(gmap->asce), gmap->asce, &ops,
						  DAT_WALK_IGN_HOLES,
						  &gmap->kvm->arch.cmma_dirty_pages);
		cond_resched();
	} while (gfn);
}
