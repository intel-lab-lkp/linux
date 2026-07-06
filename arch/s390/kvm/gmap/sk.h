/* SPDX-License-Identifier: GPL-2.0 */

#ifndef ARCH_KVM_GMAP_SK_H
#define ARCH_KVM_GMAP_SK_H

#include "dat.h"

int dat_get_storage_key(union asce asce, gfn_t gfn, union skey *skey);
int dat_set_storage_key(struct kvm_s390_mmu_cache *mc, union asce asce, gfn_t gfn,
			union skey skey, bool nq);
int dat_cond_set_storage_key(struct kvm_s390_mmu_cache *mmc, union asce asce, gfn_t gfn,
			     union skey skey, union skey *oldkey, bool nq, bool mr, bool mc);
int dat_reset_reference_bit(union asce asce, gfn_t gfn);
long dat_reset_skeys(union asce asce, gfn_t start);

int gmap_enable_skeys(struct gmap *gmap);

#endif /* ARCH_KVM_GMAP_SK_H */
