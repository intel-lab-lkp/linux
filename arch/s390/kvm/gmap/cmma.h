/* SPDX-License-Identifier: GPL-2.0 */

#ifndef ARCH_KVM_GMAP_CMMA_H
#define ARCH_KVM_GMAP_CMMA_H

#include "dat.h"

#define ESSA_GET_STATE              0
#define ESSA_SET_STABLE             1
#define ESSA_SET_UNUSED             2
#define ESSA_SET_VOLATILE           3
#define ESSA_SET_POT_VOLATILE       4
#define ESSA_SET_STABLE_RESIDENT    5
#define ESSA_SET_STABLE_IF_RESIDENT 6
#define ESSA_SET_STABLE_NODAT       7

int dat_perform_essa(union asce asce, gfn_t gfn, int orc, union essa_state *state, bool *dirty);
long dat_reset_cmma(union asce asce, gfn_t start_gfn);
int dat_peek_cmma(gfn_t start, union asce asce, unsigned int *count, u8 *values);
int dat_get_cmma(union asce asce, gfn_t *start, unsigned int *count, u8 *values, atomic64_t *rem);
int dat_set_cmma_bits(struct kvm_s390_mmu_cache *mc, union asce asce, gfn_t gfn,
		      unsigned long count, unsigned long mask, const uint8_t *bits);

void _gmap_set_cmma_all(struct gmap *gmap, bool dirty);

static inline void gmap_set_cmma_all_dirty(struct gmap *gmap)
{
	_gmap_set_cmma_all(gmap, true);
}

static inline void gmap_set_cmma_all_clean(struct gmap *gmap)
{
	_gmap_set_cmma_all(gmap, false);
}

#endif /* ARCH_KVM_GMAP_CMMA_H */
