// SPDX-License-Identifier: GPL-2.0-only
/*
 *  svm_idlehalt_test
 *
 *  Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 *  For licencing details see kernel-base/COPYING
 *
 *  Author:
 *  Manali Shukla  <manali.shukla@amd.com>
 */
#include "kvm_util.h"
#include "svm_util.h"
#include "processor.h"
#include "test_util.h"
#include "apic.h"

#define VINTR_VECTOR     0x30
#define NUM_ITERATIONS 100000

/*
 * Incremented in the VINTR handler. Provides evidence to the sender that the
 * VINR is arrived at the destination.
 */
static volatile uint64_t vintr_rcvd;

void verify_apic_base_addr(void)
{
	uint64_t msr = rdmsr(MSR_IA32_APICBASE);
	uint64_t base = GET_APIC_BASE(msr);

	GUEST_ASSERT(base == APIC_DEFAULT_GPA);
}

/*
 * The halting guest code instruments the scenario where there is a V_INTR pending
 * event available while hlt instruction is executed. The HLT VM Exit doesn't
 * occur in above-mentioned scenario if the Idle HLT intercept feature is enabled.
 */

static void halter_guest_code(void)
{
	uint32_t icr_val;
	int i;

	verify_apic_base_addr();
	xapic_enable();

	icr_val = (APIC_DEST_SELF | APIC_INT_ASSERT | VINTR_VECTOR);

	for (i = 0; i < NUM_ITERATIONS; i++) {
		xapic_write_reg(APIC_ICR, icr_val);
		asm volatile("sti; hlt; cli");
	}
	GUEST_DONE();
}

static void guest_vintr_handler(struct ex_regs *regs)
{
	vintr_rcvd++;
	xapic_write_reg(APIC_EOI, 0x30);
}

int main(int argc, char *argv[])
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct ucall uc;
	uint64_t  halt_exits, vintr_exits;
	uint64_t *pvintr_rcvd;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));

	/* Check the extension for binary stats */
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_BINARY_STATS_FD));

	vm = vm_create_with_one_vcpu(&vcpu, halter_guest_code);

	vm_init_descriptor_tables(vm);
	vcpu_init_descriptor_tables(vcpu);
	vm_install_exception_handler(vm, VINTR_VECTOR, guest_vintr_handler);
	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	halt_exits = vcpu_get_stat(vcpu, "halt_exits");
	vintr_exits = vcpu_get_stat(vcpu, "irq_window_exits");
	pvintr_rcvd = (uint64_t *)addr_gva2hva(vm, (uint64_t)&vintr_rcvd);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		/* NOT REACHED */
	case UCALL_DONE:
		goto done;
	default:
		TEST_FAIL("Unknown ucall 0x%lx.", uc.cmd);
	}

done:
	TEST_ASSERT(halt_exits == 0,
		    "Test Failed:\n"
		    "Guest executed VINTR followed by halts: %d times\n"
		    "The guest exited due to halt: %ld times and number\n"
		    "of vintr exits: %ld and vintr got re-injected: %ld times\n",
		    NUM_ITERATIONS, halt_exits, vintr_exits, *pvintr_rcvd);

	fprintf(stderr,
		"Test Successful:\n"
		"Guest executed VINTR followed by halts: %d times\n"
		"The guest exited due to halt: %ld times and number\n"
		"of vintr exits: %ld and vintr got re-injected: %ld times\n",
		NUM_ITERATIONS, halt_exits, vintr_exits, *pvintr_rcvd);

	kvm_vm_free(vm);
	return 0;
}
