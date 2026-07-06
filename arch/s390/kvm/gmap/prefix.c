// SPDX-License-Identifier: GPL-2.0

#include <linux/kvm_host.h>
#include <linux/kvm_types.h>

#include "gmap.h"
#include "dat.h"
#include "prefix.h"
#include "../s390/s390.h"

bool _gmap_unmap_prefix(struct gmap *gmap, gfn_t gfn, gfn_t end, bool hint)
{
	struct kvm *kvm = gmap->kvm;
	struct kvm_vcpu *vcpu;
	gfn_t prefix_gfn;
	unsigned long i;

	if (is_shadow(gmap))
		return false;
	kvm_for_each_vcpu(i, vcpu, kvm) {
		/* Match against both prefix pages */
		prefix_gfn = gpa_to_gfn(kvm_s390_get_prefix(vcpu));
		if (prefix_gfn < end && gfn <= prefix_gfn + 1) {
			if (hint && kvm_s390_is_in_sie(vcpu))
				return false;
			VCPU_EVENT(vcpu, 2, "gmap notifier for %llx-%llx",
				   gfn_to_gpa(gfn), gfn_to_gpa(end));
			kvm_s390_sync_request(KVM_REQ_REFRESH_GUEST_PREFIX, vcpu);
		}
	}
	return true;
}

static long dat_set_pn_crste(union crste *crstep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	union crste newcrste, oldcrste;
	int *n = walk->priv;

	do {
		oldcrste = READ_ONCE(*crstep);
		if (!oldcrste.h.fc || oldcrste.h.i || oldcrste.h.p)
			return 0;
		if (oldcrste.s.fc1.prefix_notif)
			break;
		newcrste = oldcrste;
		newcrste.s.fc1.prefix_notif = 1;
	} while (!dat_crstep_xchg_atomic(crstep, oldcrste, newcrste, gfn, walk->asce));
	*n = 2;
	return 0;
}

static long dat_set_pn_pte(union pte *ptep, gfn_t gfn, gfn_t next, struct dat_walk *walk)
{
	int *n = walk->priv;
	union pgste pgste;

	pgste = pgste_get_lock(ptep);
	if (!ptep->h.i && !ptep->h.p) {
		pgste.prefix_notif = 1;
		*n += 1;
	}
	pgste_set_unlock(ptep, pgste);
	return 0;
}

int dat_set_prefix_notif_bit(union asce asce, gfn_t gfn)
{
	static const struct dat_walk_ops ops = {
		.pte_entry = dat_set_pn_pte,
		.pmd_entry = dat_set_pn_crste,
		.pud_entry = dat_set_pn_crste,
	};

	int n = 0;

	_dat_walk_gfn_range(gfn, gfn + 2, asce, &ops, DAT_WALK_IGN_HOLES, &n);
	if (n != 2)
		return -EAGAIN;
	return 0;
}
