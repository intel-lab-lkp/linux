// SPDX-License-Identifier: GPL-2.0

#include <linux/kvm_types.h>
#include <linux/kvm_host.h>

#ifdef KVM_S390_ARM64
#include "arm.h"
#else
#include "s390.h"
#endif
#include "gmap.h"
#include "dat.h"
#include "faultin.h"

/*
 * Get (and clear) the dirty memory log for a memory slot.
 */
int gmap_get_dirty_log(struct kvm *kvm, struct kvm_dirty_log *log)
{
	int r;
	unsigned long n;
	struct kvm_memory_slot *memslot;
	int is_dirty;

	if (kvm_is_ucontrol(kvm))
		return -EINVAL;

	mutex_lock(&kvm->slots_lock);

	r = -EINVAL;
	if (log->slot >= KVM_USER_MEM_SLOTS)
		goto out;

	r = kvm_get_dirty_log(kvm, log, &is_dirty, &memslot);
	if (r)
		goto out;

	/* Clear the dirty log */
	if (is_dirty) {
		n = kvm_dirty_bitmap_bytes(memslot);
		memset(memslot->dirty_bitmap, 0, n);
	}
	r = 0;
out:
	mutex_unlock(&kvm->slots_lock);
	return r;
}

int gmap_prepare_memory_region(struct kvm *kvm,
			       const struct kvm_memory_slot *old,
			       struct kvm_memory_slot *new,
			       enum kvm_mr_change change)
{
	if (kvm_is_ucontrol(kvm) && new && new->id < KVM_USER_MEM_SLOTS)
		return -EINVAL;

	/* When we are protected, we should not change the memory slots */
	if (kvm_s390_pv_get_handle(kvm))
		return -EINVAL;

	if (change != KVM_MR_DELETE && change != KVM_MR_FLAGS_ONLY) {
		/*
		 * A few sanity checks. The memory in userland is ok to be
		 * fragmented into various different vmas. It is okay to mmap()
		 * and munmap() stuff in this slot after doing this call at any
		 * time.
		 */
		if (new->userspace_addr & ~PAGE_MASK)
			return -EINVAL;
		if ((new->base_gfn + new->npages) * PAGE_SIZE > kvm->arch.mem_limit)
			return -EINVAL;
		if (!asce_contains_gfn(kvm->arch.gmap->asce, new->base_gfn + new->npages - 1))
			return -EINVAL;
	}

	if (!kvm_s390_is_migration_mode(kvm))
		return 0;

	/*
	 * Turn off migration mode when:
	 * - userspace creates a new memslot with dirty logging off,
	 * - userspace modifies an existing memslot (MOVE or FLAGS_ONLY) and
	 *   dirty logging is turned off.
	 * Migration mode expects dirty page logging being enabled to store
	 * its dirty bitmap.
	 */
	if (change != KVM_MR_DELETE &&
	    !(new->flags & KVM_MEM_LOG_DIRTY_PAGES))
		WARN(kvm_s390_vm_stop_migration(kvm),
		     "Failed to stop migration mode");

	return 0;
}

void gmap_commit_memory_region(struct kvm *kvm,
			       struct kvm_memory_slot *old,
			       const struct kvm_memory_slot *new,
			       enum kvm_mr_change change)
{
	struct kvm_s390_mmu_cache *mc = NULL;
	int rc = 0;

	if (change == KVM_MR_FLAGS_ONLY)
		return;

	mc = kvm_s390_new_mmu_cache();
	if (!mc) {
		rc = -ENOMEM;
		goto out;
	}

	scoped_guard(write_lock, &kvm->mmu_lock) {
		switch (change) {
		case KVM_MR_DELETE:
			rc = dat_delete_slot(mc, kvm->arch.gmap->asce, old->base_gfn, old->npages);
			break;
		case KVM_MR_MOVE:
			rc = dat_delete_slot(mc, kvm->arch.gmap->asce, old->base_gfn, old->npages);
			if (rc)
				break;
			fallthrough;
		case KVM_MR_CREATE:
			rc = dat_create_slot(mc, kvm->arch.gmap->asce, new->base_gfn, new->npages);
			break;
		case KVM_MR_FLAGS_ONLY:
			break;
		default:
			WARN(1, "Unknown KVM MR CHANGE: %d\n", change);
		}
	}
out:
	if (rc)
		pr_warn("failed to commit memory region\n");
	kvm_s390_free_mmu_cache(mc);
}

/**
 * gmap_test_age_gfn() - test young
 * @kvm: the kvm instance
 * @range: the range of guest addresses whose young status needs to be cleared
 *
 * Context: called by KVM common code without holding the kvm mmu lock
 * Return: true if any page in the given range is young, otherwise 0.
 */
bool gmap_test_age_gfn(struct kvm *kvm, struct kvm_gfn_range *range)
{
	scoped_guard(read_lock, &kvm->mmu_lock)
		return dat_test_age_gfn(kvm->arch.gmap->asce, range->start, range->end);
}

/**
 * gmap_pre_fault_memory() - pre-fault and link gmap dat tables
 * @vcpu: the vcpu that shall appear to have generated the fault-in.
 * @range: the range that needs to be faulted in.
 *
 * The first page of the given range is faulted in and the corresponding gmap
 * page tables are created, as if the given vCPU had performed a read
 * operation.
 * If the range starts outside any memslots, an error is returned. An error is
 * also returned for UCONTROL VMs, which should instead use the
 * KVM_S390_VCPU_FAULT ioctl.
 *
 * Return:
 * * %-ENOENT if the range lies outside of a memslot.
 * * %-EINVAL in case of invalid state (for example if the VM is UCONTROL).
 * * %-EIO if errors happen while faulting-in the page (will trigger a warning
 *   in the caller).
 * * other error codes < 0 in case of other errors.
 * * otherwise a number > 0 of bytes that have been faulted in successfully.
 */
long gmap_pre_fault_memory(struct kvm_vcpu *vcpu, struct kvm_pre_fault_memory *range)
{
	struct guest_fault f = { .gfn = gpa_to_gfn(range->gpa), };
	gpa_t end;
	int rc;

	if (kvm_is_ucontrol(vcpu->kvm))
		return -EINVAL;

	rc = kvm_s390_faultin_gfn(vcpu, NULL, &f);
	if (rc == PGM_ADDRESSING)
		return -ENOENT;
	if (rc > 0)
		return -EIO;
	if (rc < 0)
		return rc;

	if (f.ptep)
		return PAGE_SIZE;

	end = ALIGN(range->gpa + PAGE_SIZE, f.crste_region3 ? _REGION3_SIZE : HPAGE_SIZE);
	return min(range->size, end - range->gpa);
}
