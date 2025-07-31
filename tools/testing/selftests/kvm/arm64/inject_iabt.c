// SPDX-License-Identifier: GPL-2.0-only
/*
 * inject_iabt.c - Tests for injecting instruction aborts into guest.
 */

#include "processor.h"
#include "test_util.h"

static void expect_iabt_handler(struct ex_regs *regs)
{
	u64 esr = read_sysreg(esr_el1);

	GUEST_PRINTF("Handling Guest SEA\n");
	GUEST_PRINTF("  ESR_EL1=%#lx\n", esr);

	GUEST_ASSERT_EQ(ESR_ELx_EC(esr), ESR_ELx_EC_IABT_CUR);
	GUEST_ASSERT_EQ(esr & ESR_ELx_FSC_TYPE, ESR_ELx_FSC_EXTABT);

	GUEST_DONE();
}

static void guest_code(void)
{
	GUEST_FAIL("Guest should only run SEA handler");
}

static void vcpu_run_expect_done(struct kvm_vcpu *vcpu)
{
	struct ucall uc;
	bool guest_done = false;

	do {
		vcpu_run(vcpu);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_PRINTF:
			ksft_print_msg("From guest: %s", uc.buffer);
			break;
		case UCALL_DONE:
			ksft_print_msg("Guest done gracefully!\n");
			guest_done = true;
			break;
		default:
			TEST_FAIL("Unexpected ucall: %lu", uc.cmd);
		}
	} while (!guest_done);
}

static void vcpu_inject_ext_iabt(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_events events = {};

	events.exception.ext_iabt_pending = true;
	vcpu_events_set(vcpu, &events);
}

static void vcpu_inject_invalid_abt(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_events events = {};
	int r;

	events.exception.ext_iabt_pending = true;
	events.exception.ext_dabt_pending = true;

	ksft_print_msg("Injecting invalid external abort events\n");
	r = __vcpu_ioctl(vcpu, KVM_SET_VCPU_EVENTS, &events);
	TEST_ASSERT(r && errno == EINVAL,
		    KVM_IOCTL_ERROR(KVM_SET_VCPU_EVENTS, r));
}

static void test_inject_iabt(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	vm_init_descriptor_tables(vm);
	vcpu_init_descriptor_tables(vcpu);

	vm_install_sync_handler(vm, VECTOR_SYNC_CURRENT,
				ESR_ELx_EC_IABT_CUR, expect_iabt_handler);

	vcpu_inject_invalid_abt(vcpu);

	vcpu_inject_ext_iabt(vcpu);
	vcpu_run_expect_done(vcpu);

	kvm_vm_free(vm);
}

int main(void)
{
	test_inject_iabt();
	return 0;
}
