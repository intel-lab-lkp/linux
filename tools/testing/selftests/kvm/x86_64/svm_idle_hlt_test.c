// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 */
#include <kvm_util.h>
#include <processor.h>
#include <test_util.h>
#include "svm_util.h"
#include "apic.h"

#define VINTR_VECTOR     0x30
#define NUM_ITERATIONS   1000

static bool irq_received;

/*
 * The guest code instruments the scenario where there is a V_INTR pending
 * event available while hlt instruction is executed. The HLT VM Exit doesn't
 * occur in above-mentioned scenario if Idle HLT intercept feature is enabled.
 */

static void guest_code(void)
{
	uint32_t icr_val;
	int i;

	xapic_enable();

	icr_val = (APIC_DEST_SELF | APIC_INT_ASSERT | VINTR_VECTOR);

	for (i = 0; i < NUM_ITERATIONS; i++) {
		cli();
		xapic_write_reg(APIC_ICR, icr_val);
		safe_halt();
		GUEST_ASSERT(READ_ONCE(irq_received));
		WRITE_ONCE(irq_received, false);
	}
	GUEST_DONE();
}

static void guest_vintr_handler(struct ex_regs *regs)
{
	WRITE_ONCE(irq_received, true);
	xapic_write_reg(APIC_EOI, 0x00);
}

int main(int argc, char *argv[])
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct ucall uc;
	uint64_t  halt_exits, vintr_exits;

	/* Check the extension for binary stats */
	TEST_REQUIRE(this_cpu_has(X86_FEATURE_IDLE_HLT));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_BINARY_STATS_FD));

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	vm_install_exception_handler(vm, VINTR_VECTOR, guest_vintr_handler);
	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	halt_exits = vcpu_get_stat(vcpu, HALT_EXITS);
	vintr_exits = vcpu_get_stat(vcpu, IRQ_WINDOW_EXITS);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		/* NOT REACHED */
	case UCALL_DONE:
		break;

	default:
		TEST_FAIL("Unknown ucall 0x%lx.", uc.cmd);
	}

	TEST_ASSERT_EQ(halt_exits, 0);
	pr_debug("Guest executed VINTR followed by halts: %d times.\n"
		 "The guest exited due to halt: %ld times and number\n"
		 "of vintr exits: %ld.\n",
		 NUM_ITERATIONS, halt_exits, vintr_exits);

	kvm_vm_free(vm);
	return 0;
}
