// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM nested SVM PMU Host-Only/Guest-Only test
 *
 * Copyright (C) 2026, Google LLC.
 *
 * Test that KVM correctly virtualizes the AMD PMU Host-Only (bit 41) and
 * Guest-Only (bit 40) event selector bits across all SVM state transitions.
 *
 * For Guest-Only counters:
 *   1. SVME=0: counter counts (HG_ONLY bits ignored)
 *   2. Set SVME=1: counter stops (in host mode)
 *   3. VMRUN to L2: counter counts (in guest mode)
 *   4. VMEXIT to L1: counter stops (back to host mode)
 *   5. Clear SVME=0: counter counts (HG_ONLY bits ignored)
 *
 * For Host-Only counters:
 *   1. SVME=0: counter counts (HG_ONLY bits ignored)
 *   2. Set SVME=1: counter counts (in host mode)
 *   3. VMRUN to L2: counter stops (in guest mode)
 *   4. VMEXIT to L1: counter counts (back to host mode)
 *   5. Clear SVME=0: counter counts (HG_ONLY bits ignored)
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

#define L2_GUEST_STACK_SIZE	256

#define MSR_F15H_PERF_CTL0	0xc0010200
#define MSR_F15H_PERF_CTR0	0xc0010201

#define AMD64_EVENTSEL_GUESTONLY	BIT_ULL(40)
#define AMD64_EVENTSEL_HOSTONLY		BIT_ULL(41)

#define EVENTSEL_RETIRED_INSNS	(ARCH_PERFMON_EVENTSEL_OS |	\
				 ARCH_PERFMON_EVENTSEL_USR |	\
				 ARCH_PERFMON_EVENTSEL_ENABLE | \
				 AMD_ZEN_INSTRUCTIONS_RETIRED)

#define LOOP_INSNS	1000

static __always_inline void run_instruction_loop(void)
{
	unsigned int i;

	for (i = 0; i < LOOP_INSNS; i++)
		__asm__ __volatile__("nop");
}

static __always_inline uint64_t run_and_measure(void)
{
	uint64_t count_before, count_after;

	count_before = rdmsr(MSR_F15H_PERF_CTR0);
	run_instruction_loop();
	count_after = rdmsr(MSR_F15H_PERF_CTR0);

	return count_after - count_before;
}

struct hg_test_data {
	uint64_t l2_delta;
	bool l2_done;
};

static struct hg_test_data *hg_data;

static void l2_guest_code(void)
{
	hg_data->l2_delta = run_and_measure();
	hg_data->l2_done = true;
	vmmcall();
}

/*
 * Test Guest-Only counter across all relevant state transitions.
 */
static void l1_guest_code_guestonly(struct svm_test_data *svm,
				    struct hg_test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb = svm->vmcb;
	uint64_t eventsel, delta;

	hg_data = data;

	eventsel = EVENTSEL_RETIRED_INSNS | AMD64_EVENTSEL_GUESTONLY;
	wrmsr(MSR_F15H_PERF_CTL0, eventsel);
	wrmsr(MSR_F15H_PERF_CTR0, 0);

	/* Step 1: SVME=0; HG_ONLY ignored */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	/* Step 2: Set SVME=1; Guest-Only counter stops */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_EQ(delta, 0);

	/* Step 3: VMRUN to L2; Guest-Only counter counts */
	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);
	vmcb->control.intercept &= ~(1ULL << INTERCEPT_MSR_PROT);

	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	GUEST_ASSERT(data->l2_done);
	GUEST_ASSERT_NE(data->l2_delta, 0);

	/* Step 4: After VMEXIT to L1; Guest-Only counter stops */
	delta = run_and_measure();
	GUEST_ASSERT_EQ(delta, 0);

	/* Step 5: Clear SVME; HG_ONLY ignored */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	GUEST_DONE();
}

/*
 * Test Host-Only counter across all relevant state transitions.
 */
static void l1_guest_code_hostonly(struct svm_test_data *svm,
				   struct hg_test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb = svm->vmcb;
	uint64_t eventsel, delta;

	hg_data = data;

	eventsel = EVENTSEL_RETIRED_INSNS | AMD64_EVENTSEL_HOSTONLY;
	wrmsr(MSR_F15H_PERF_CTL0, eventsel);
	wrmsr(MSR_F15H_PERF_CTR0, 0);


	/* Step 1: SVME=0; HG_ONLY ignored */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	/* Step 2: Set SVME=1; Host-Only counter still counts */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	/* Step 3: VMRUN to L2; Host-Only counter stops */
	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);
	vmcb->control.intercept &= ~(1ULL << INTERCEPT_MSR_PROT);

	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	GUEST_ASSERT(data->l2_done);
	GUEST_ASSERT_EQ(data->l2_delta, 0);

	/* Step 4: After VMEXIT to L1; Host-Only counter counts */
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	/* Step 5: Clear SVME; HG_ONLY ignored */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	GUEST_DONE();
}

/*
 * Test that both bits set is the same as neither bit set (always counts).
 */
static void l1_guest_code_both_bits(struct svm_test_data *svm,
				    struct hg_test_data *data)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb = svm->vmcb;
	uint64_t eventsel, delta;

	hg_data = data;

	eventsel = EVENTSEL_RETIRED_INSNS |
		AMD64_EVENTSEL_HOSTONLY | AMD64_EVENTSEL_GUESTONLY;
	wrmsr(MSR_F15H_PERF_CTL0, eventsel);
	wrmsr(MSR_F15H_PERF_CTR0, 0);

	/* Step 1: SVME=0 */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	/* Step 2: Set SVME=1 */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	/* Step 3: VMRUN to L2 */
	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);
	vmcb->control.intercept &= ~(1ULL << INTERCEPT_MSR_PROT);

	run_guest(vmcb, svm->vmcb_gpa);

	GUEST_ASSERT_EQ(vmcb->control.exit_code, SVM_EXIT_VMMCALL);
	GUEST_ASSERT(data->l2_done);
	GUEST_ASSERT_NE(data->l2_delta, 0);

	/* Step 4: After VMEXIT to L1 */
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	/* Step 5: Clear SVME */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVME);
	delta = run_and_measure();
	GUEST_ASSERT_NE(delta, 0);

	GUEST_DONE();
}

static void l1_guest_code(struct svm_test_data *svm, struct hg_test_data *data,
			  int test_num)
{
	switch (test_num) {
	case 0:
		l1_guest_code_guestonly(svm, data);
		break;
	case 1:
		l1_guest_code_hostonly(svm, data);
		break;
	case 2:
		l1_guest_code_both_bits(svm, data);
		break;
	}
}

static void run_test(int test_number, const char *test_name)
{
	struct hg_test_data *data_hva;
	vm_vaddr_t svm_gva, data_gva;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	pr_info("Testing: %s\n", test_name);

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);

	vcpu_alloc_svm(vm, &svm_gva);

	data_gva = vm_vaddr_alloc_page(vm);
	data_hva = addr_gva2hva(vm, data_gva);
	memset(data_hva, 0, sizeof(*data_hva));

	vcpu_args_set(vcpu, 3, svm_gva, data_gva, test_number);

	for (;;) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			/* NOT REACHED */
		case UCALL_DONE:
			pr_info("  PASSED\n");
			kvm_vm_free(vm);
			return;
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
		}
	}
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));
	TEST_REQUIRE(kvm_is_pmu_enabled());
	TEST_REQUIRE(get_kvm_amd_param_bool("enable_mediated_pmu"));

	run_test(0, "Guest-Only counter across all transitions");
	run_test(1, "Host-Only counter across all transitions");
	run_test(2, "Both HG_ONLY bits set (always count)");

	return 0;
}
