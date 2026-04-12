// SPDX-License-Identifier: GPL-2.0-only
/*
 * hello_nested - Go from vEL2 to EL1 then back
 */

#include "nested.h"
#include "processor.h"
#include "test_util.h"
#include "ucall.h"

#define XLATE2GPA	(0xABCD)
#define L2STACKSZ	(0x100)

/*
 * TPIDR_EL2 is used to store vcpu id, so save and restore it.
 */
static vm_paddr_t ucall_translate_to_gpa(void *gva)
{
	vm_paddr_t gpa;
	u64 vcpu_id = read_sysreg(tpidr_el2);

	GUEST_SYNC2(XLATE2GPA, gva);

	/* get the result from userspace */
	gpa = read_sysreg(tpidr_el2);

	write_sysreg(vcpu_id, tpidr_el2);

	return gpa;
}

static void l2_guest_code(void)
{
	do_hvc();
}

static void guest_code(void)
{
	struct vcpu vcpu;
	struct hyp_data hyp_data;
	int ret;
	vm_paddr_t l2_pc, l2_stack_top;
	/* force 16-byte alignment for the stack pointer */
	u8 l2_stack[L2STACKSZ] __attribute__((aligned(16)));

	GUEST_ASSERT_EQ(get_current_el(), 2);
	GUEST_PRINTF("vEL2 entry\n");

	l2_pc = ucall_translate_to_gpa(l2_guest_code);
	l2_stack_top = ucall_translate_to_gpa(&l2_stack[L2STACKSZ]);

	init_vcpu(&vcpu, l2_pc, l2_stack_top);
	prepare_hyp();

	ret = run_l2(&vcpu, &hyp_data);
	GUEST_ASSERT_EQ(ret, ARM_EXCEPTION_TRAP);
	GUEST_DONE();
}

int main(void)
{
	struct kvm_vcpu_init init;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	vm_paddr_t gpa;

	TEST_REQUIRE(kvm_check_cap(KVM_CAP_ARM_EL2));
	vm = vm_create(1);

	kvm_get_default_vcpu_target(vm, &init);
	init.features[0] |= BIT(KVM_ARM_VCPU_HAS_EL2);
	vcpu = aarch64_vcpu_add(vm, 0, &init, guest_code);
	kvm_arch_vm_finalize_vcpus(vm);

	while (true) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			if (uc.args[0] == XLATE2GPA) {
				gpa = addr_gva2gpa(vm, (vm_vaddr_t)uc.args[1]);
				vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_TPIDR_EL2), gpa);
			}
			break;
		case UCALL_PRINTF:
			pr_info("%s", uc.buffer);
			break;
		case UCALL_DONE:
			pr_info("DONE!\n");
			goto end;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			fallthrough;
		default:
			TEST_FAIL("Unhandled ucall: %ld\n", uc.cmd);
		}
	}

end:
	kvm_vm_free(vm);
	return 0;
}
