// SPDX-License-Identifier: GPL-2.0-only
/*
 * hello_nested - Go from vEL2 to EL1 then back
 */
#include "kvm_util.h"
#include "nested.h"
#include "processor.h"
#include "test_util.h"
#include "ucall.h"

static void l2_guest_code(void)
{
	/* nothing */
}

static void guest_code(void)
{
	GUEST_ASSERT_EQ(get_current_el(), 2);
	GUEST_PRINTF("vEL2 entry\n");
	run_l2();
	GUEST_DONE();
}

int main(void)
{
	struct kvm_vcpu_init init;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	TEST_REQUIRE(kvm_check_cap(KVM_CAP_ARM_EL2));
	vm = vm_create(1);

	kvm_get_default_vcpu_target(vm, &init);
	init.features[0] |= BIT(KVM_ARM_VCPU_HAS_EL2);
	vcpu = aarch64_vcpu_add(vm, 0, &init, guest_code);
	kvm_arch_vm_finalize_vcpus(vm);

	prepare_l2_stack(vm, vcpu);
	prepare_hyp_state(vm, vcpu);
	prepare_eret_destination(vm, vcpu, l2_guest_code);
	prepare_nested_sync_handler(vm, vcpu);

	while (true) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
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
