// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020, Google LLC.
 *
 * Tests for KVM paravirtual feature disablement
 */
#include <asm/kvm_para.h>
#include <linux/kvm_para.h>
#include <pthread.h>
#include <stdint.h>

#include "apic.h"
#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

struct msr_data {
	u32 idx;
	const char *name;
};

#define TEST_MSR(msr) { .idx = msr, .name = #msr }
#define UCALL_PR_MSR 0xdeadbeef
#define PR_MSR(msr) ucall(UCALL_PR_MSR, 1, msr)

/*
 * KVM paravirtual msrs to test. Expect a #GP if any of these msrs are read or
 * written, as the KVM_CPUID_FEATURES leaf is cleared.
 */
static struct msr_data msrs_to_test[] = {
	TEST_MSR(MSR_KVM_SYSTEM_TIME),
	TEST_MSR(MSR_KVM_SYSTEM_TIME_NEW),
	TEST_MSR(MSR_KVM_WALL_CLOCK),
	TEST_MSR(MSR_KVM_WALL_CLOCK_NEW),
	TEST_MSR(MSR_KVM_ASYNC_PF_EN),
	TEST_MSR(MSR_KVM_STEAL_TIME),
	TEST_MSR(MSR_KVM_PV_EOI_EN),
	TEST_MSR(MSR_KVM_POLL_CONTROL),
	TEST_MSR(MSR_KVM_ASYNC_PF_INT),
	TEST_MSR(MSR_KVM_ASYNC_PF_ACK),
};

static void test_msr(struct msr_data *msr)
{
	u64 ignored;
	u8 vector;

	PR_MSR(msr);

	vector = rdmsr_safe(msr->idx, &ignored);
	GUEST_ASSERT_EQ(vector, GP_VECTOR);

	vector = wrmsr_safe(msr->idx, 0);
	GUEST_ASSERT_EQ(vector, GP_VECTOR);
}

struct hcall_data {
	u64 nr;
	const char *name;
};

#define TEST_HCALL(hc) { .nr = hc, .name = #hc }
#define UCALL_PR_HCALL 0xdeadc0de
#define PR_HCALL(hc) ucall(UCALL_PR_HCALL, 1, hc)

/*
 * KVM hypercalls to test. Expect -KVM_ENOSYS when called, as the corresponding
 * features have been cleared in KVM_CPUID_FEATURES.
 */
static struct hcall_data hcalls_to_test[] = {
	TEST_HCALL(KVM_HC_KICK_CPU),
	TEST_HCALL(KVM_HC_SEND_IPI),
	TEST_HCALL(KVM_HC_SCHED_YIELD),
};

static void test_hcall(struct hcall_data *hc)
{
	u64 r;

	PR_HCALL(hc);
	r = kvm_hypercall(hc->nr, 0, 0, 0, 0);
	GUEST_ASSERT_EQ(r, -KVM_ENOSYS);
}

static void guest_main(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(msrs_to_test); i++) {
		test_msr(&msrs_to_test[i]);
	}

	for (i = 0; i < ARRAY_SIZE(hcalls_to_test); i++) {
		test_hcall(&hcalls_to_test[i]);
	}

	GUEST_DONE();
}

static void pr_msr(struct ucall *uc)
{
	struct msr_data *msr = (struct msr_data *)uc->args[0];

	pr_info("testing msr: %s (%#x)\n", msr->name, msr->idx);
}

static void pr_hcall(struct ucall *uc)
{
	struct hcall_data *hc = (struct hcall_data *)uc->args[0];

	pr_info("testing hcall: %s (%lu)\n", hc->name, hc->nr);
}

static void enter_guest(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	while (true) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_PR_MSR:
			pr_msr(&uc);
			break;
		case UCALL_PR_HCALL:
			pr_hcall(&uc);
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			return;
		case UCALL_DONE:
			return;
		}
	}
}

static void test_pv_unhalt(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct kvm_cpuid_entry2 *ent;
	u32 kvm_sig_old;
	int r;

	if (!(kvm_check_cap(KVM_CAP_X86_DISABLE_EXITS) & KVM_X86_DISABLE_EXITS_HLT))
		return;

	pr_info("testing KVM_FEATURE_PV_UNHALT\n");

	/* KVM_PV_UNHALT test */
	vm = vm_create_with_one_vcpu(&vcpu, guest_main);
	vcpu_set_cpuid_feature(vcpu, X86_FEATURE_KVM_PV_UNHALT);

	TEST_ASSERT(vcpu_cpuid_has(vcpu, X86_FEATURE_KVM_PV_UNHALT),
		    "Enabling X86_FEATURE_KVM_PV_UNHALT had no effect");

	/* Verify KVM disallows disabling exits after vCPU creation. */
	r = __vm_enable_cap(vm, KVM_CAP_X86_DISABLE_EXITS, KVM_X86_DISABLE_EXITS_HLT);
	TEST_ASSERT(r && errno == EINVAL,
		    "Disabling exits after vCPU creation didn't fail as expected");

	kvm_vm_free(vm);

	/* Verify that KVM clear PV_UNHALT from guest CPUID. */
	vm = vm_create(1);
	vm_enable_cap(vm, KVM_CAP_X86_DISABLE_EXITS, KVM_X86_DISABLE_EXITS_HLT);

	vcpu = vm_vcpu_add(vm, 0, NULL);
	TEST_ASSERT(!vcpu_cpuid_has(vcpu, X86_FEATURE_KVM_PV_UNHALT),
		    "vCPU created with PV_UNHALT set by default");

	vcpu_set_cpuid_feature(vcpu, X86_FEATURE_KVM_PV_UNHALT);
	TEST_ASSERT(!vcpu_cpuid_has(vcpu, X86_FEATURE_KVM_PV_UNHALT),
		    "PV_UNHALT set in guest CPUID when HLT-exiting is disabled");

	/*
	 * Clobber the KVM PV signature and verify KVM does NOT clear PV_UNHALT
	 * when KVM PV is not present, and DOES clear PV_UNHALT when switching
	 * back to the correct signature..
	 */
	ent = vcpu_get_cpuid_entry(vcpu, KVM_CPUID_SIGNATURE);
	kvm_sig_old = ent->ebx;
	ent->ebx = 0xdeadbeef;
	vcpu_set_cpuid(vcpu);

	vcpu_set_cpuid_feature(vcpu, X86_FEATURE_KVM_PV_UNHALT);
	TEST_ASSERT(vcpu_cpuid_has(vcpu, X86_FEATURE_KVM_PV_UNHALT),
		    "PV_UNHALT cleared when using bogus KVM PV signature");

	ent = vcpu_get_cpuid_entry(vcpu, KVM_CPUID_SIGNATURE);
	ent->ebx = kvm_sig_old;
	vcpu_set_cpuid(vcpu);

	TEST_ASSERT(!vcpu_cpuid_has(vcpu, X86_FEATURE_KVM_PV_UNHALT),
		    "PV_UNHALT set in guest CPUID when HLT-exiting is disabled");

	kvm_vm_free(vm);
}

static void pv_unhalt_halter_guest_code(void)
{
	/*
	 * Interrupts are disabled, so nothing except the KVM_HC_KICK_CPU from
	 * the other vCPU can end the halt, i.e. reaching GUEST_DONE() proves
	 * the kick was delivered.
	 */
	asm volatile("cli; hlt");

	GUEST_DONE();
}

static void pv_unhalt_kicker_guest_code(u32 halter_apic_id)
{
	GUEST_ASSERT_EQ(kvm_hypercall(KVM_HC_KICK_CPU, halter_apic_id, 0, 0, 0), 0);
	GUEST_DONE();
}

static void run_guest_to_done(struct kvm_vcpu *vcpu)
{
	struct ucall uc;
	u64 cmd;

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	cmd = get_ucall(vcpu, &uc);
	if (cmd == UCALL_ABORT)
		REPORT_GUEST_ASSERT(uc);
	TEST_ASSERT_EQ(cmd, UCALL_DONE);
}

static void *pv_unhalt_halter_thread(void *vcpu)
{
	run_guest_to_done(vcpu);
	return NULL;
}

static u32 vcpu_get_apic_id(struct kvm_vcpu *vcpu)
{
	struct kvm_lapic_state lapic;

	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &lapic);
	return GET_APIC_ID_FIELD(*(u32 *)&lapic.regs[APIC_ID]);
}

static void test_pv_unhalt_kick(void)
{
	struct kvm_vcpu *halter, *kicker;
	struct timespec start;
	struct kvm_vm *vm;
	pthread_t thread;
	int r;

	pr_info("testing KVM_HC_KICK_CPU\n");

	vm = vm_create_with_one_vcpu(&halter, pv_unhalt_halter_guest_code);
	kicker = vm_vcpu_add(vm, 1, pv_unhalt_kicker_guest_code);

	/*
	 * Enforce the PV CPUID so that KVM services the hypercall because
	 * PV_UNHALT is advertised to the kicker, and not because enforcement
	 * is off.  KVM advertises PV_UNHALT by default while HLT-exiting is
	 * enabled; set it explicitly so that the test keeps testing the
	 * feature if that ever changes.
	 */
	vcpu_enable_cap(kicker, KVM_CAP_ENFORCE_PV_FEATURE_CPUID, 1);
	vcpu_set_cpuid_feature(kicker, X86_FEATURE_KVM_PV_UNHALT);
	vcpu_args_set(kicker, 1, vcpu_get_apic_id(halter));

	r = pthread_create(&thread, NULL, pv_unhalt_halter_thread, halter);
	TEST_ASSERT(!r, "pthread_create halter failed, errno=%d", errno);

	/* Kick only once the halter has taken its HLT exit. */
	clock_gettime(CLOCK_MONOTONIC, &start);
	while (!vcpu_get_stat(halter, halt_exits)) {
		TEST_ASSERT(timespec_elapsed(start).tv_sec < 10,
			    "vCPU never halted");
		usleep(100);
	}

	run_guest_to_done(kicker);

	/* Nothing except the kick can get the halter to GUEST_DONE(). */
	r = pthread_join(thread, NULL);
	TEST_ASSERT(!r, "pthread_join halter failed, errno=%d", errno);

	kvm_vm_free(vm);
}

int main(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_ENFORCE_PV_FEATURE_CPUID));

	vm = vm_create_with_one_vcpu(&vcpu, guest_main);

	vcpu_enable_cap(vcpu, KVM_CAP_ENFORCE_PV_FEATURE_CPUID, 1);

	vcpu_clear_cpuid_entry(vcpu, KVM_CPUID_FEATURES);

	enter_guest(vcpu);
	kvm_vm_free(vm);

	test_pv_unhalt();
	test_pv_unhalt_kick();
}
