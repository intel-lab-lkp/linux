/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_EVER_MAPPED_H
#define __KVM_X86_EVER_MAPPED_H

#include <linux/kvm_host.h>

/*
 * Track if pages have ever been mapped at an internal granularity of 2 MB.
 */
#define KVM_EVER_MAPPED_SHIFT 21

static inline void kvm_ever_mapped_set_range(struct kvm *kvm, gfn_t gfn,
					     u64 npages)
{
	const unsigned int shift = KVM_EVER_MAPPED_SHIFT - PAGE_SHIFT;
	unsigned long start = gfn >> shift;
	unsigned long end = (gfn + npages + (1UL << shift) - 1) >> shift;
	unsigned long i;

	end = min(end, kvm->arch.ever_mapped_max_gpa >> KVM_EVER_MAPPED_SHIFT);
	for (i = start; i < end; i++) {
		/*
		 * re-mapping zapped pages and breaking up of huge pages can
		 * trigger a number of sets on already set bits, so test first
		 * before atomic-set. Since bits can never be unset again, this
		 * is safe against races.
		 */
		if (!test_bit(i, kvm->arch.ever_mapped_bitmap))
			set_bit(i, kvm->arch.ever_mapped_bitmap);
	}
}

#endif /* __KVM_X86_EVER_MAPPED_H */
