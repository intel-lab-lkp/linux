// SPDX-License-Identifier: GPL-2.0
#include <linux/sizes.h>

#include <test_util.h>
#include <kvm_util.h>
#include <processor.h>
#include "sev.h"

/* Arbitrarily chosen values */
#define TEST_SIZE		(SZ_2M + PAGE_SIZE)
#define TEST_NPAGES		(TEST_SIZE / PAGE_SIZE)
#define TEST_SLOT		10
#define TEST_GPA		0x100000000ul
#define TEST_GVA		0x100000000ul

enum prefault_snp_test_type {
	/* Skip pre-faulting tests. */
	NO_PREFAULT_TYPE,
	/*
	 * Issue KVM_PRE_FAULT_MEMORY for GFNs mapping non-private memory
	 * before finalizing the initial guest contents (e.g. via
	 * KVM_SEV_SNP_LAUNCH_FINISH for SNP guests).
	 *
	 * This should result in failure since KVM explicitly disallows
	 * KVM_PRE_FAULT_MEMORY from being issued prior to finalizing the
	 * initial guest contents.
	 */
	PREFAULT_SHARED_BEFORE_FINALIZING,
	/*
	 * Issue KVM_PRE_FAULT_MEMORY for GFNs mapping private memory
	 * before finalizing the initial guest contents (e.g. via
	 * KVM_SEV_SNP_LAUNCH_FINISH for SNP guests).
	 *
	 * This should result in failure since KVM explicitly disallows
	 * KVM_PRE_FAULT_MEMORY from being issued prior to finalizing the
	 * initial guest contents.
	 */
	PREFAULT_PRIVATE_BEFORE_FINALIZING,
	/*
	 * Issue KVM_PRE_FAULT_MEMORY for GFNs mapping shared/private
	 * memory after finalizing the initial guest contents
	 * (e.g. via * KVM_SEV_SNP_LAUNCH_FINISH for SNP guests).
	 *
	 * This should succeed since pre-faulting is supported for both
	 * non-private/private memory once the guest contents are finalized.
	 */
	PREFAULT_PRIVATE_SHARED_AFTER_FINALIZING
};

enum falloc_snp_test_type {
	/* Skip alloc tests. */
	NO_ALLOC_TYPE,
	/*
	 * Allocate and/or deallocate a region of guest memfd before
	 * memory regions are updated to be protected and encrypted
	 *
	 * This should succeed since allocation and deallocation is
	 * supported before the memory is finalized.
	 */
	ALLOC_BEFORE_UPDATE,
	ALLOC_AFTER_UPDATE,
	DEALLOC_BEFORE_UPDATE,
	ALLOC_DEALLOC_BEFORE_UPDATE,
	/*
	 * Allocate and/or deallocate a region of guest memfd after
	 * memory regions are updated to be protected and encrypted
	 *
	 * This should fail since dealloc will nuke the pages that
	 * contain the initial code that the guest will run.
	 */
	DEALLOC_AFTER_UPDATE,
	ALLOC_DEALLOC_AFTER_UPDATE
};

static void guest_code_sev(void)
{
	int i;

	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);

	for (i = 0; i < TEST_NPAGES; i++) {
		uint64_t *src = (uint64_t *)(TEST_GVA + i * PAGE_SIZE);
		uint64_t val = *src;

		/* Validate the data stored in the pages */
		if ((i < TEST_NPAGES / 2 && val != i + 1) ||
		    (i >= TEST_NPAGES / 2 && val != 0)) {
			GUEST_FAIL("Inconsistent view of memory values in guest");
		}
	}

	if (rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ES_ENABLED) {
		wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
		__asm__ __volatile__("rep; vmmcall");
		GUEST_FAIL("This should be unreachable.");
	}

	GUEST_DONE();
}

static void __falloc_region(struct kvm_vm *vm, bool punch_hole)
{
	int ctr, ret, flags = FALLOC_FL_KEEP_SIZE;
	struct userspace_mem_region *region;

	hash_for_each(vm->regions.slot_hash, ctr, region, slot_node) {
		if (punch_hole)
			flags |= FALLOC_FL_PUNCH_HOLE;
		ret = fallocate(region->region.guest_memfd, flags, 0, PAGE_SIZE * TEST_NPAGES);
		TEST_ASSERT(!ret, "fallocate should succeed.");
	}
}

static void gmemfd_alloc(struct kvm_vm *vm)
{
	__falloc_region(vm, false);
}

static void gmemfd_dealloc(struct kvm_vm *vm)
{
	__falloc_region(vm, true);
}

static void __pre_fault_memory(struct kvm_vcpu *vcpu, u64 gpa, u64 size,
			       u64 left, bool expect_fail)
{
	struct kvm_pre_fault_memory range = {
		.gpa = gpa,
		.size = size,
		.flags = 0,
	};
	int ret, save_errno;
	u64 prev;

	do {
		prev = range.size;
		ret = __vcpu_ioctl(vcpu, KVM_PRE_FAULT_MEMORY, &range);
		save_errno = errno;
		TEST_ASSERT((range.size < prev) ^ (ret < 0),
			    "%sexpecting range.size to change on %s",
			    ret < 0 ? "not " : "",
			    ret < 0 ? "failure" : "success");
	} while (ret >= 0 ? range.size : save_errno == EINTR);

	TEST_ASSERT(expect_fail ? !(range.size == left) : (range.size == left),
		    "[EXPECT %s] completed with %lld bytes left, expected %" PRId64,
		    expect_fail ? "FAIL" : "PASS",
		    range.size, left);

	if (left == 0) {
		TEST_ASSERT(expect_fail ? ret : !ret,
			    "[EXPECT %s] KVM_PRE_FAULT_MEMORY",
			    expect_fail ? "FAIL" : "PASS");
	} else {
		/*
		 * For shared memory, no memory slot causes RET_PF_EMULATE. It
		 * results in -ENOENT.
		 *
		 * For private memory, no memory slot is an error case returning
		 * -EFAULT. However, it is also possible that only the GPA
		 *  ranges backed by a slot are marked as private, in which case
		 *  the noslot range will also result in -ENOENT.
		 *
		 *  So allow both errors for now, but in the future it would be
		 *  good to distinguish between these cases to tighten up the
		 *  error-checking.
		 */
		TEST_ASSERT(expect_fail ? !ret :
			    (ret && (save_errno == EFAULT || save_errno == ENOENT)),
			    "[EXPECT %s] KVM_PRE_FAULT_MEMORY",
			    expect_fail ? "FAIL" : "PASS");
	}
}

static void pre_fault_memory(struct kvm_vcpu *vcpu, u64 gpa,
			     u64 size, u64 left)
{
	__pre_fault_memory(vcpu, gpa, size, left, false);
}

static void pre_fault_memory_negative(struct kvm_vcpu *vcpu, u64 gpa,
				      u64 size, u64 left)
{
	__pre_fault_memory(vcpu, gpa, size, left, true);
}

static void pre_fault_memory_snp(struct kvm_vcpu *vcpu, struct kvm_vm *vm,
				 bool private, enum prefault_snp_test_type p_type,
				 enum falloc_snp_test_type f_type)
{
	if (f_type == ALLOC_BEFORE_UPDATE ||
	    f_type == ALLOC_DEALLOC_BEFORE_UPDATE) {
		gmemfd_alloc(vm);
	}

	if (f_type == DEALLOC_BEFORE_UPDATE ||
	    f_type == ALLOC_DEALLOC_BEFORE_UPDATE) {
		gmemfd_dealloc(vm);
	}

	if (p_type == PREFAULT_SHARED_BEFORE_FINALIZING)
		pre_fault_memory_negative(vcpu, TEST_GPA, SZ_2M, 0);

	snp_vm_launch_start(vm, SNP_POLICY);

	if (f_type == ALLOC_BEFORE_UPDATE ||
	    f_type == ALLOC_DEALLOC_BEFORE_UPDATE) {
		gmemfd_alloc(vm);
	}

	if (f_type == DEALLOC_BEFORE_UPDATE ||
	    f_type == ALLOC_DEALLOC_BEFORE_UPDATE) {
		gmemfd_dealloc(vm);
	}

	if (p_type == PREFAULT_SHARED_BEFORE_FINALIZING)
		pre_fault_memory_negative(vcpu, TEST_GPA, SZ_2M, 0);

	if (private) {
		/*
		 * Make sure when pages are pre-faulted later after
		 * finalization they are treated the same as a private
		 * access by the guest so that the expected gmem
		 * backing pages are used.
		 */
		vm_mem_set_private(vm, TEST_GPA, TEST_SIZE);
		if (p_type == PREFAULT_PRIVATE_BEFORE_FINALIZING)
			pre_fault_memory_negative(vcpu, TEST_GPA, SZ_2M, 0);
	} else {
		if (p_type == PREFAULT_SHARED_BEFORE_FINALIZING)
			pre_fault_memory_negative(vcpu, TEST_GPA, SZ_2M, 0);
	}

	snp_vm_launch_update(vm);

	if (f_type == ALLOC_AFTER_UPDATE ||
	    f_type == ALLOC_DEALLOC_AFTER_UPDATE) {
		gmemfd_alloc(vm);
	}

	/*
	 * Hole-punch after SNP LAUNCH UPDATE is not expected to fail
	 * immediately, rather its affects are observed on vcpu_run()
	 * as the pages that contain the initial code is nuked.
	 */
	if (f_type == DEALLOC_AFTER_UPDATE ||
	    f_type == ALLOC_DEALLOC_AFTER_UPDATE) {
		gmemfd_dealloc(vm);
	}

	if (p_type == PREFAULT_SHARED_BEFORE_FINALIZING)
		pre_fault_memory_negative(vcpu, TEST_GPA, SZ_2M, 0);

	snp_vm_launch_finish(vm);

	if (f_type == ALLOC_AFTER_UPDATE ||
	    f_type == ALLOC_DEALLOC_AFTER_UPDATE) {
		gmemfd_alloc(vm);
	}

	if (f_type == DEALLOC_AFTER_UPDATE ||
	    f_type == ALLOC_DEALLOC_AFTER_UPDATE) {
		gmemfd_dealloc(vm);
	}

	/*
	 * After finalization, pre-faulting either private or shared
	 * ranges should work regardless of whether the pages were
	 * encrypted as part of setting up initial guest state.
	 */
	if (p_type == PREFAULT_PRIVATE_SHARED_AFTER_FINALIZING) {
		pre_fault_memory(vcpu, TEST_GPA, SZ_2M, 0);
		pre_fault_memory(vcpu, TEST_GPA + SZ_2M, PAGE_SIZE * 2, PAGE_SIZE);
		pre_fault_memory(vcpu, TEST_GPA + TEST_SIZE, PAGE_SIZE, PAGE_SIZE);
	}
}

static void pre_fault_memory_sev(unsigned long vm_type, struct kvm_vcpu *vcpu,
				 struct kvm_vm *vm)
{
	uint32_t policy = (vm_type == KVM_X86_SEV_ES_VM) ? SEV_POLICY_ES : 0;

	pre_fault_memory(vcpu, TEST_GPA, SZ_2M, 0);
	pre_fault_memory(vcpu, TEST_GPA + SZ_2M, PAGE_SIZE * 2, PAGE_SIZE);
	pre_fault_memory(vcpu, TEST_GPA + TEST_SIZE, PAGE_SIZE, PAGE_SIZE);

	sev_vm_launch(vm, policy);

	pre_fault_memory(vcpu, TEST_GPA, SZ_2M, 0);
	pre_fault_memory(vcpu, TEST_GPA + SZ_2M, PAGE_SIZE * 2, PAGE_SIZE);
	pre_fault_memory(vcpu, TEST_GPA + TEST_SIZE, PAGE_SIZE, PAGE_SIZE);

	sev_vm_launch_measure(vm, alloca(256));

	pre_fault_memory(vcpu, TEST_GPA, SZ_2M, 0);
	pre_fault_memory(vcpu, TEST_GPA + SZ_2M, PAGE_SIZE * 2, PAGE_SIZE);
	pre_fault_memory(vcpu, TEST_GPA + TEST_SIZE, PAGE_SIZE, PAGE_SIZE);

	sev_vm_launch_finish(vm);

	pre_fault_memory(vcpu, TEST_GPA, SZ_2M, 0);
	pre_fault_memory(vcpu, TEST_GPA + SZ_2M, PAGE_SIZE * 2, PAGE_SIZE);
	pre_fault_memory(vcpu, TEST_GPA + TEST_SIZE, PAGE_SIZE, PAGE_SIZE);
}

static void test_pre_fault_memory_sev(unsigned long vm_type, bool private,
				      enum prefault_snp_test_type p_type,
				      enum falloc_snp_test_type f_type)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	int i;

	vm = vm_sev_create_with_one_vcpu(vm_type, guest_code_sev, &vcpu);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    TEST_GPA, TEST_SLOT, TEST_NPAGES,
				    (vm_type == KVM_X86_SNP_VM) ? KVM_MEM_GUEST_MEMFD : 0);

	/*
	 * Make sure guest page table is in agreement with what pages will be
	 * initially encrypted by the ASP.
	 */
	if (private)
		vm_mem_set_protected(vm, TEST_SLOT, TEST_GPA, TEST_NPAGES);

	virt_map(vm, TEST_GVA, TEST_GPA, TEST_NPAGES);

	/*
	 * Populate the pages to compare data consistency in the guest
	 * Fill the first half with data and second half with zeros
	 */
	for (i = 0; i < TEST_NPAGES; i++) {
		uint64_t *hva = addr_gva2hva(vm, TEST_GVA + i * PAGE_SIZE);

		if (i < TEST_NPAGES / 2)
			*hva = i + 1;
		else
			*hva = 0;
	}

	if (vm_type == KVM_X86_SNP_VM)
		pre_fault_memory_snp(vcpu, vm, private, p_type, f_type);
	else
		pre_fault_memory_sev(vm_type, vcpu, vm);

	vcpu_run(vcpu);

	/* Expect SHUTDOWN when we falloc using PUNCH_HOLE after SNP_UPDATE */
	if (vm->type == KVM_X86_SNP_VM &&
	    (f_type == DEALLOC_AFTER_UPDATE ||
	    f_type == ALLOC_DEALLOC_AFTER_UPDATE)) {
		TEST_ASSERT(vcpu->run->exit_reason == KVM_EXIT_SHUTDOWN,
			    "Wanted SYSTEM_EVENT, got %s",
			    exit_reason_str(vcpu->run->exit_reason));
		goto out;
	}

	if (vm->type == KVM_X86_SEV_ES_VM || vm->type == KVM_X86_SNP_VM) {
		TEST_ASSERT(vcpu->run->exit_reason == KVM_EXIT_SYSTEM_EVENT,
			    "Wanted SYSTEM_EVENT, got %s",
			    exit_reason_str(vcpu->run->exit_reason));
		TEST_ASSERT_EQ(vcpu->run->system_event.type, KVM_SYSTEM_EVENT_SEV_TERM);
		TEST_ASSERT_EQ(vcpu->run->system_event.ndata, 1);
		TEST_ASSERT_EQ(vcpu->run->system_event.data[0], GHCB_MSR_TERM_REQ);
		goto out;
	}

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_DONE:
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
	default:
		TEST_FAIL("Unexpected exit: %s",
			  exit_reason_str(vcpu->run->exit_reason));
	}

out:
	kvm_vm_free(vm);
}

static void test_pre_fault_memory(unsigned long vm_type, bool private)
{
	int pt, ft;

	if (vm_type && !(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(vm_type))) {
		pr_info("Skipping tests for vm_type 0x%lx\n", vm_type);
		return;
	}

	switch (vm_type) {
	case KVM_X86_SEV_VM:
	case KVM_X86_SEV_ES_VM:
		test_pre_fault_memory_sev(vm_type, private, NO_PREFAULT_TYPE, NO_ALLOC_TYPE);
		break;
	case KVM_X86_SNP_VM:
		for (pt = 0; pt <= PREFAULT_PRIVATE_SHARED_AFTER_FINALIZING; pt++) {
			for (ft = 0; ft <= ALLOC_DEALLOC_AFTER_UPDATE; ft++)
				test_pre_fault_memory_sev(vm_type, private, pt, ft);
		}
		break;
	default:
		abort();
	}
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_PRE_FAULT_MEMORY));

	test_pre_fault_memory(KVM_X86_SEV_VM, false);
	test_pre_fault_memory(KVM_X86_SEV_VM, true);
	test_pre_fault_memory(KVM_X86_SEV_ES_VM, false);
	test_pre_fault_memory(KVM_X86_SEV_ES_VM, true);
	test_pre_fault_memory(KVM_X86_SNP_VM, false);
	test_pre_fault_memory(KVM_X86_SNP_VM, true);

	return 0;
}
