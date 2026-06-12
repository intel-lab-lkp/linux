// SPDX-License-Identifier: GPL-2.0-only
/*
 * nv_pre_fault_memory_test - Test KVM_PRE_FAULT_MEMORY on a vCPU whose
 * last-run context is nested.
 *
 * The guest starts at vEL2, mirrors its EL2 translation regime into the
 * real EL1 registers, drops HCR_EL2.TGE and ERETs to vEL1, then exits to
 * userspace from vEL1 so that the vCPU's last-run context selects a
 * shadow stage-2 MMU. Userspace then enables an empty nested stage-2
 * before prefaulting. Prefaulting must target the canonical stage-2,
 * regardless of the vCPU's nested state.
 */
#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall.h"

#include <asm/sysreg.h>
#include <linux/sizes.h>

#define TEST_MEM_SLOT		10
#define NESTED_S2_ROOT_SLOT	11
#define TEST_MEM_SIZE		SZ_2M
#define TEST_MEM_GPA		SZ_1G
#define NESTED_S2_ROOT_GPA	(TEST_MEM_GPA + TEST_MEM_SIZE)

struct nested_s2_state {
	u64 hcr_el2;
	u64 vttbr_el2;
};

static void guest_el1_code(void)
{
	u64 offset;

	GUEST_ASSERT_EQ(get_current_el(), 1);

	/* Exit to userspace with the vEL1 (nested) context live. */
	GUEST_SYNC(1);

	/*
	 * Touch the prefaulted range. vstage-2 is disabled, so the shadow
	 * stage-2 is a 1:1 view of the canonical IPA space.
	 */
	for (offset = 0; offset < TEST_MEM_SIZE; offset += SZ_4K)
		READ_ONCE(*(u64 *)(TEST_MEM_GPA + offset));

	GUEST_DONE();
}

static void guest_code(void)
{
	u64 sp;

	GUEST_ASSERT_EQ(get_current_el(), 2);

	/*
	 * Mirror the EL2 translation regime into the real EL1 registers so
	 * that vEL1 runs on the test's stage-1 page tables. With E2H=1, the
	 * _EL1 accessors read the EL2 registers, and the _EL12 accessors
	 * write the real EL1 registers.
	 */
	write_sysreg_s(read_sysreg(sctlr_el1), SYS_SCTLR_EL12);
	write_sysreg_s(read_sysreg(tcr_el1), SYS_TCR_EL12);
	write_sysreg_s(read_sysreg(ttbr0_el1), SYS_TTBR0_EL12);
	write_sysreg_s(read_sysreg(mair_el1), SYS_MAIR_EL12);
	write_sysreg_s(read_sysreg(cpacr_el1), SYS_CPACR_EL12);

	/* Run vEL1 on the same stack. */
	asm volatile("mov %0, sp" : "=r"(sp));
	write_sysreg(sp, sp_el1);

	/*
	 * Drop TGE so that vEL1 is a nested context rather than host EL0.
	 * KVM backs it with a shadow stage-2 MMU even though vstage-2 is
	 * disabled (HCR_EL2.VM=0).
	 */
	write_sysreg(read_sysreg(hcr_el2) & ~HCR_EL2_TGE, hcr_el2);
	isb();

	write_sysreg(PSR_MODE_EL1h | PSR_F_BIT | PSR_I_BIT | PSR_A_BIT |
		     PSR_D_BIT, spsr_el2);
	write_sysreg((u64)guest_el1_code, elr_el2);
	asm volatile("eret");

	GUEST_ASSERT(false);
}

static void pre_fault(struct kvm_vcpu *vcpu, u64 gpa, u64 size)
{
	struct kvm_pre_fault_memory range = {
		.gpa = gpa,
		.size = size,
	};
	int ret;

	do {
		ret = __vcpu_ioctl(vcpu, KVM_PRE_FAULT_MEMORY, &range);
	} while (ret < 0 && errno == EINTR);

	TEST_ASSERT(!ret, "KVM_PRE_FAULT_MEMORY failed, ret: %d errno: %d",
		    ret, errno);
	TEST_ASSERT_EQ(range.size, 0);
}

static struct nested_s2_state enable_empty_nested_s2(struct kvm_vcpu *vcpu)
{
	struct nested_s2_state state = {
		.hcr_el2 = vcpu_get_reg(vcpu, KVM_ARM64_SYS_REG(SYS_HCR_EL2)),
		.vttbr_el2 = vcpu_get_reg(vcpu,
					   KVM_ARM64_SYS_REG(SYS_VTTBR_EL2)),
	};

	TEST_ASSERT(!(state.hcr_el2 & HCR_EL2_TGE),
		    "vCPU should be in nested/vEL1 context");

	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_VTTBR_EL2),
		     NESTED_S2_ROOT_GPA);
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_HCR_EL2),
		     state.hcr_el2 | HCR_EL2_VM);

	return state;
}

static void restore_nested_s2(struct kvm_vcpu *vcpu,
			      struct nested_s2_state *state)
{
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_HCR_EL2), state->hcr_el2);
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_VTTBR_EL2),
		     state->vttbr_el2);
}

int main(void)
{
	struct nested_s2_state s2;
	struct kvm_vcpu_init init;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	u64 npages;

	TEST_REQUIRE(kvm_check_cap(KVM_CAP_ARM_EL2));
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_PRE_FAULT_MEMORY));

	vm = vm_create(1);

	kvm_get_default_vcpu_target(vm, &init);
	init.features[0] |= BIT(KVM_ARM_VCPU_HAS_EL2);
	vcpu = aarch64_vcpu_add(vm, 0, &init, guest_code);
	kvm_arch_vm_finalize_vcpus(vm);

	npages = TEST_MEM_SIZE / vm->page_size;
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, TEST_MEM_GPA,
				    TEST_MEM_SLOT, npages, 0);
	virt_map(vm, TEST_MEM_GPA, TEST_MEM_GPA, npages);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    NESTED_S2_ROOT_GPA, NESTED_S2_ROOT_SLOT,
				    1, 0);

	/* Run the guest until it has ERET'd from vEL2 to vEL1. */
	vcpu_run(vcpu);
	switch (get_ucall(vcpu, &uc)) {
	case UCALL_SYNC:
		TEST_ASSERT_EQ(uc.args[1], 1);
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	default:
		TEST_FAIL("Unhandled ucall: %ld", uc.cmd);
	}

	/*
	 * The vCPU's last-run context is vEL1, backed by a shadow stage-2
	 * MMU. Enable nested stage-2 with an empty root so that the ioctl
	 * fails if it tries to interpret the userspace GPA as an L2 IPA.
	 * Prefault in two halves so that the second ioctl exercises a
	 * repeated shadow-MMU attach and canonical stage-2 swap.
	 */
	s2 = enable_empty_nested_s2(vcpu);
	pre_fault(vcpu, TEST_MEM_GPA, TEST_MEM_SIZE / 2);
	pre_fault(vcpu, TEST_MEM_GPA + TEST_MEM_SIZE / 2, TEST_MEM_SIZE / 2);
	restore_nested_s2(vcpu, &s2);

	/* Resume at vEL1 and touch the prefaulted range. */
	vcpu_run(vcpu);
	switch (get_ucall(vcpu, &uc)) {
	case UCALL_DONE:
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	default:
		TEST_FAIL("Unhandled ucall: %ld", uc.cmd);
	}

	kvm_vm_free(vm);
	return 0;
}
