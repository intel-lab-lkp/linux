// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM nested SVM PMU Host-Only/Guest-Only test
 *
 * Copyright (C) 2026, Google LLC.
 *
 * Test that KVM correctly virtualizes the AMD PMU Host-Only (bit 41) and
 * Guest-Only (bit 40) event selector bits across all SVM state
 * transitions.
 *
 * Programs 4 PMCs simultaneously with all combinations of Host-Only and
 * Guest-Only bits, then verifies correct counting behavior through:
 *   1. SVME=0: all counters count (Host-Only/Guest-Only bits ignored)
 *   2. Set SVME=1: Host-Only and neither/both count; Guest-Only stops
 *   3. VMRUN to L2: Guest-Only and neither/both count; Host-Only stops
 *   4. VMEXIT to L1: Host-Only and neither/both count; Guest-Only stops
 *   5. Clear SVME=0: all counters count (bits ignored again)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"
#include "pmu.h"

#define L2_GUEST_STACK_SIZE	255

#define EVENTSEL_RETIRED_INSNS	(ARCH_PERFMON_EVENTSEL_OS |	\
				 ARCH_PERFMON_EVENTSEL_USR |	\
				 ARCH_PERFMON_EVENTSEL_ENABLE |	\
				 AMD_ZEN_INSTRUCTIONS_RETIRED)

/* PMC configurations: index corresponds to Host-Only | Guest-Only bits */
#define PMC_NEITHER	0  /* Neither bit set */
#define PMC_GUESTONLY	1  /* Guest-Only bit set */
#define PMC_HOSTONLY	2  /* Host-Only bit set */
#define PMC_BOTH	3  /* Both bits set */
#define NR_PMCS		4

/* Bitmasks for which PMCs should be counting in each state */
#define COUNTS_ALL	(BIT(PMC_NEITHER) | BIT(PMC_GUESTONLY) | \
			 BIT(PMC_HOSTONLY) | BIT(PMC_BOTH))
#define COUNTS_L1	(BIT(PMC_NEITHER) | BIT(PMC_HOSTONLY) | BIT(PMC_BOTH))
#define COUNTS_L2	(BIT(PMC_NEITHER) | BIT(PMC_GUESTONLY) | BIT(PMC_BOTH))

#define LOOP_INSNS	1000

static __always_inline void run_instruction_loop(void)
{
	unsigned int i;

	for (i = 0; i < LOOP_INSNS; i++)
		__asm__ __volatile__("nop");
}

static __always_inline void read_counters(uint64_t *counts)
{
	int i;

	for (i = 0; i < NR_PMCS; i++)
		counts[i] = rdmsr(MSR_F15H_PERF_CTR + 2 * i);
}

static __always_inline void run_and_measure(uint64_t *deltas)
{
	uint64_t before[NR_PMCS], after[NR_PMCS];
	int i;

	read_counters(before);
	run_instruction_loop();
	read_counters(after);

	for (i = 0; i < NR_PMCS; i++)
		deltas[i] = after[i] - before[i];
}

static void assert_pmc_counts(uint64_t *deltas, unsigned int expected_counting)
{
	int i;

	for (i = 0; i < NR_PMCS; i++) {
		if (expected_counting & BIT(i))
			GUEST_ASSERT_NE(deltas[i], 0);
		else
			GUEST_ASSERT_EQ(deltas[i], 0);
	}
}

struct test_data {
	uint64_t l2_deltas[NR_PMCS];
	bool l2_done;
};

static struct test_data *test_data;

static void l2_guest_code(void)
{
	run_and_measure(test_data->l2_deltas);
	test_data->l2_done = true;
	vmmcall();
}

static void l1_guest_code(struct svm_test_data *svm, struct test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE] __stack_aligned__;
	struct vmcb *vmcb = svm->vmcb;
	uint64_t deltas[NR_PMCS];
	uint64_t eventsel;
	int i;

	test_data = data;

	/* Program 4 PMCs with all combinations of Host-Only/Guest-Only bits */
	for (i = 0; i < NR_PMCS; i++) {
		eventsel = EVENTSEL_RETIRED_INSNS;
		if (i & PMC_GUESTONLY)
			eventsel |= AMD64_EVENTSEL_GUESTONLY;
		if (i & PMC_HOSTONLY)
			eventsel |= AMD64_EVENTSEL_HOSTONLY;
		wrmsr(MSR_F15H_PERF_CTL + 2 * i, eventsel);
		wrmsr(MSR_F15H_PERF_CTR + 2 * i, 0);
	}

	/* Step 1: SVME=0 - Host-Only/Guest-Only bits ignored; all count */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVME);
	run_and_measure(deltas);
	assert_pmc_counts(deltas, COUNTS_ALL);

	/* Step 2: Set SVME=1 - In L1 "host mode"; Guest-Only stops */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
	run_and_measure(deltas);
	assert_pmc_counts(deltas, COUNTS_L1);

	/* Step 3: VMRUN to L2 - In "guest mode"; Host-Only stops */
	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);
	vmcb->control.intercept &= ~(1ULL << INTERCEPT_MSR_PROT);

	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	GUEST_ASSERT(data->l2_done);
	assert_pmc_counts(data->l2_deltas, COUNTS_L2);

	/* Step 4: After VMEXIT to L1 - Back in "host mode"; Guest-Only stops */
	run_and_measure(deltas);
	assert_pmc_counts(deltas, COUNTS_L1);

	/* Step 5: Clear SVME - Host-Only/Guest-Only bits ignored; all count */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVME);
	run_and_measure(deltas);
	assert_pmc_counts(deltas, COUNTS_ALL);

	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	vm_vaddr_t svm_gva, data_gva;
	struct test_data *data_hva;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));
	TEST_REQUIRE(kvm_is_pmu_enabled());
	TEST_REQUIRE(get_kvm_amd_param_bool("enable_mediated_pmu"));
	TEST_REQUIRE(host_cpu_is_amd && kvm_cpu_family() >= 0x17);

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);

	vcpu_alloc_svm(vm, &svm_gva);

	data_gva = vm_vaddr_alloc_page(vm);
	data_hva = addr_gva2hva(vm, data_gva);
	memset(data_hva, 0, sizeof(*data_hva));

	vcpu_args_set(vcpu, 2, svm_gva, data_gva);

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	case UCALL_DONE:
		break;
	default:
		TEST_FAIL("Unknown ucall %lu", uc.cmd);
	}

	kvm_vm_free(vm);
	return 0;
}
