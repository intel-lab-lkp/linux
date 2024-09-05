// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <math.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"
#include "linux/psp-sev.h"
#include "sev.h"


#define XFEATURE_MASK_X87_AVX (XFEATURE_MASK_FP | XFEATURE_MASK_SSE | XFEATURE_MASK_YMM)

static bool is_smt_active(void)
{
	FILE *f;

	f = fopen("/sys/devices/system/cpu/smt/active", "r");
	if (!f)
		return false;

	return fgetc(f) - '0';
}

static void guest_snp_code(void)
{
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ES_ENABLED);
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_SNP_ENABLED);

	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	__asm__ __volatile__("rep; vmmcall");
}

static void guest_sev_es_code(void)
{
	/* TODO: Check CPUID after GHCB-based hypercall support is added. */
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ES_ENABLED);

	/*
	 * TODO: Add GHCB and ucall support for SEV-ES guests.  For now, simply
	 * force "termination" to signal "done" via the GHCB MSR protocol.
	 */
	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	__asm__ __volatile__("rep; vmmcall");
}

static void guest_sev_code(void)
{
	GUEST_ASSERT(this_cpu_has(X86_FEATURE_SEV));
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);

	GUEST_DONE();
}

/* Stash state passed via VMSA before any compiled code runs.  */
extern void guest_code_xsave(void);
asm("guest_code_xsave:\n"
    "mov $-1, %eax\n"
    "mov $-1, %edx\n"
    "xsave (%rdi)\n"
    "jmp guest_sev_es_code");

static void compare_xsave(u8 *from_host, u8 *from_guest)
{
	int i;
	bool bad = false;
	for (i = 0; i < 4095; i++) {
		if (from_host[i] != from_guest[i]) {
			printf("mismatch at %02hhx | %02hhx %02hhx\n", i, from_host[i], from_guest[i]);
			bad = true;
		}
	}

	if (bad)
		abort();
}

static void test_sync_vmsa(uint32_t type, uint64_t policy)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	vm_vaddr_t gva;
	void *hva;

	double x87val = M_PI;
	struct kvm_xsave __attribute__((aligned(64))) xsave = { 0 };
	struct kvm_sregs sregs;
	struct kvm_xcrs xcrs = {
		.nr_xcrs = 1,
		.xcrs[0].xcr = 0,
		.xcrs[0].value = XFEATURE_MASK_X87_AVX,
	};

	TEST_ASSERT(type != KVM_X86_SEV_VM,
		    "sync_vmsa only supported for SEV-ES and SNP VM types");

	vm = vm_sev_create_with_one_vcpu(type, guest_code_xsave, &vcpu);
	gva = vm_vaddr_alloc_shared(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR,
				    MEM_REGION_TEST_DATA);
	hva = addr_gva2hva(vm, gva);

	vcpu_args_set(vcpu, 1, gva);

	vcpu_sregs_get(vcpu, &sregs);
	sregs.cr4 |= X86_CR4_OSFXSR | X86_CR4_OSXSAVE;
	vcpu_sregs_set(vcpu, &sregs);

	vcpu_xcrs_set(vcpu, &xcrs);
	asm("fninit\n"
	    "vpcmpeqb %%ymm4, %%ymm4, %%ymm4\n"
	    "fldl %3\n"
	    "xsave (%2)\n"
	    "fstp %%st\n"
	    : "=m"(xsave)
	    : "A"(XFEATURE_MASK_X87_AVX), "r"(&xsave), "m" (x87val)
	    : "ymm4", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
	vcpu_xsave_set(vcpu, &xsave);

	vm_sev_launch(vm, policy, NULL);

	/* This page is shared, so make it decrypted.  */
	memset(hva, 0, 4096);

	vcpu_run(vcpu);

	TEST_ASSERT(vcpu->run->exit_reason == KVM_EXIT_SYSTEM_EVENT,
		    "Wanted SYSTEM_EVENT, got %s",
		    exit_reason_str(vcpu->run->exit_reason));
	TEST_ASSERT_EQ(vcpu->run->system_event.type, KVM_SYSTEM_EVENT_SEV_TERM);
	TEST_ASSERT_EQ(vcpu->run->system_event.ndata, 1);
	TEST_ASSERT_EQ(vcpu->run->system_event.data[0], GHCB_MSR_TERM_REQ);

	compare_xsave((u8 *)&xsave, (u8 *)hva);

	kvm_vm_free(vm);
}

static void sev_guest_neg_status_assert(struct kvm_vm *vm, uint32_t type)
{
	struct kvm_sev_guest_status status;
	int ret;

	ret = __vm_sev_ioctl(vm, KVM_SEV_GUEST_STATUS, &status);
	TEST_ASSERT(ret, "KVM_SEV_GUEST_STATUS should fail, invalid VM Type.");
}

static void vm_sev_es_launch_neg(struct kvm_vm *vm, uint32_t type, uint64_t policy)
{
	int ret;

	/* Launch start with policy SEV_POLICY_NO_DBG (0x0) */
	ret = __sev_vm_launch_start(vm, 0);
	TEST_ASSERT(ret,
		    "KVM_SEV_LAUNCH_START should fail due to type (%d) - policy(0x0) mismatch",
		    type);

	ret = __sev_vm_launch_update(vm, policy);
	TEST_ASSERT(ret,
		    "KVM_SEV_LAUNCH_UPDATE should fail due to LAUNCH_START. type: %d policy: 0x%lx",
		    type, policy);
	sev_guest_neg_status_assert(vm, type);

	ret = __sev_vm_launch_measure(vm, alloca(256));
	TEST_ASSERT(ret,
		    "KVM_SEV_LAUNCH_UPDATE should fail due to LAUNCH_START. type: %d policy: 0x%lx",
		    type, policy);
	sev_guest_neg_status_assert(vm, type);

	ret = __sev_vm_launch_finish(vm);
	TEST_ASSERT(ret,
		    "KVM_SEV_LAUNCH_UPDATE should fail due to LAUNCH_START. type: %d policy: 0x%lx",
		    type, policy);
	sev_guest_neg_status_assert(vm, type);
}

/*
 * Test for SEV ioctl launch path
 * VMs of the type SEV and SEV-ES are created, however they are launched with
 * an empty policy to observe the effect on the control flow of launching a VM.
 *
 * SEV - Expected to pass through the path of launch start, update, measure,
 * and finish. vcpu_run expected to fail with error KVM_EXIT_IO.
 *
 * SEV-ES - Expected to fail the launch start as vm created with type
 * KVM_X86_DEFAULT_VM but policy passed to launch start is KVM_X86_SEV_ES_VM.
 * Post this, calls that pass the correct policy to update, measure, and finish
 * are also expected to fail cascading.
 */
static void test_sev_launch(void *guest_code, uint32_t type, uint64_t policy)
{
	struct kvm_vcpu *vcpu;
	int exp_exit_reason;
	struct kvm_vm *vm;
	struct ucall uc;

	vm = vm_sev_create_with_one_vcpu(type, guest_code, &vcpu);

	if (type == KVM_X86_SEV_VM) {
		sev_vm_launch(vm, 0);
		sev_vm_launch_measure(vm, alloca(256));
		sev_vm_launch_finish(vm);
	} else {
		vm_sev_es_launch_neg(vm, type, policy);
	}

	vcpu_run(vcpu);
	get_ucall(vcpu, &uc);
	if (type == KVM_X86_SEV_VM)
		exp_exit_reason = KVM_EXIT_IO;
	else
		exp_exit_reason = KVM_EXIT_FAIL_ENTRY;

	TEST_ASSERT(vcpu->run->exit_reason == exp_exit_reason,
		    "vcpu_run failed exit expected: %d, got: %d",
		    exp_exit_reason, vcpu->run->exit_reason);

	kvm_vm_free(vm);
}

static int __test_snp_launch_start(uint32_t type, uint64_t policy,
				   uint8_t flags, bool assert)
{
	struct kvm_vm *vm;
	int ret = 0;

	vm = vm_create_type(type, 1);
	ret = __snp_vm_launch_start(vm, policy, flags);
	if (assert)
		TEST_ASSERT_VM_VCPU_IOCTL(!ret, KVM_SEV_SNP_LAUNCH_START, ret, vm);
	kvm_vm_free(vm);

	return ret;
}

static void test_snp_launch_start(uint32_t type, uint64_t policy)
{
	uint8_t i;
	int ret;

	/* Flags must be zero for success */
	__test_snp_launch_start(type, policy, 0, true);

	for (i = 1; i < 8; i++) {
		ret = __test_snp_launch_start(type, policy, BIT(i), false);
		TEST_ASSERT(ret && errno == EINVAL,
			    "KVM_SEV_SNP_LAUNCH_START should fail, invalid flag\n"
			    "(type: %d policy: 0x%lx, flag: 0x%lx)",
			    type, policy, BIT(i));
	}

	ret = __test_snp_launch_start(type, SNP_POLICY_SMT, 0, false);
	TEST_ASSERT(ret && errno == EINVAL,
		    "KVM_SEV_SNP_LAUNCH_START should fail, SNP_POLICY_RSVD_MBO policy bit not set\n"
		    "(type: %d policy: 0x%llx, flags: 0x0)",
		    type, SNP_POLICY_SMT);

	ret = __test_snp_launch_start(type, SNP_POLICY_RSVD_MBO, 0, false);
	if (unlikely(!is_smt_active())) {
		TEST_ASSERT(!ret,
			    "KVM_SEV_SNP_LAUNCH_START should succeed, SNP_POLICY_SMT not required on non-SMT systems\n"
			    "(type: %d policy: 0x%llx, flags: 0x0)",
			    type, SNP_POLICY_RSVD_MBO);
	} else {
		TEST_ASSERT(ret && errno == EINVAL,
			    "KVM_SEV_SNP_LAUNCH_START should fail, SNP_POLICY_SMT is not set on a SMT system\n"
			    "(type: %d policy: 0x%llx, flags: 0x0)",
			    type, SNP_POLICY_RSVD_MBO);
	}

	ret = __test_snp_launch_start(type, SNP_POLICY |
				      SNP_FW_VER_MAJOR(UINT8_MAX) |
				      SNP_FW_VER_MINOR(UINT8_MAX), 0, false);
	TEST_ASSERT(ret && errno == EIO,
		    "KVM_SEV_SNP_LAUNCH_START should fail, invalid version\n"
		    "expected: %d.%d got: %d.%d (type: %d policy: 0x%llx, flags: 0x0)",
		    SNP_FW_REQ_VER_MAJOR, SNP_FW_REQ_VER_MINOR,
		    UINT8_MAX, UINT8_MAX, type,
		    SNP_POLICY | SNP_FW_VER_MAJOR(UINT8_MAX) | SNP_FW_VER_MINOR(UINT8_MAX));
}

static void test_snp_launch_update(uint32_t type, uint64_t policy)
{
	struct kvm_vm *vm;
	int ret;

	for (int pgtype = 0; pgtype <= KVM_SEV_SNP_PAGE_TYPE_CPUID + 1; pgtype++) {
		vm = vm_create_type(type, 1);
		snp_vm_launch_start(vm, policy);
		ret = __snp_vm_launch_update(vm, pgtype);

		switch (pgtype) {
		case KVM_SEV_SNP_PAGE_TYPE_NORMAL:
		case KVM_SEV_SNP_PAGE_TYPE_ZERO:
		case KVM_SEV_SNP_PAGE_TYPE_UNMEASURED:
		case KVM_SEV_SNP_PAGE_TYPE_SECRETS:
			TEST_ASSERT(!ret,
				    "KVM_SEV_SNP_LAUNCH_UPDATE should succeed, invalid Page type %d",
				    pgtype);
			break;
		case KVM_SEV_SNP_PAGE_TYPE_CPUID:
			/*
			 * Expect failure if performed on random pages of
			 * guest memory rather than properly formatted CPUID Page
			 */
			TEST_ASSERT(ret && errno == EIO,
				    "KVM_SEV_SNP_LAUNCH_UPDATE should fail,\n"
				    "CPUID page type only valid for CPUID pages");
			break;
		default:
			TEST_ASSERT(ret && errno == EINVAL,
				    "KVM_SEV_SNP_LAUNCH_UPDATE should fail, invalid Page type");
		}

		kvm_vm_free(vm);
	}
}

void test_snp_launch_finish(uint32_t type, uint64_t policy)
{
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_type(type, 1);
	snp_vm_launch_start(vm, policy);
	snp_vm_launch_update(vm);
	/* Flags must be zero for success */
	snp_vm_launch_finish(vm);
	kvm_vm_free(vm);

	for (int i = 1; i < 16; i++) {
		vm = vm_create_type(type, 1);
		snp_vm_launch_start(vm, policy);
		snp_vm_launch_update(vm);
		ret = __snp_vm_launch_finish(vm, BIT(i));
		TEST_ASSERT(ret && errno == EINVAL,
			    "KVM_SEV_SNP_LAUNCH_FINISH should fail, invalid flag\n"
			    "(type: %d policy: 0x%lx, flag: 0x%lx)",
			    type, policy, BIT(i));
		kvm_vm_free(vm);
	}
}

static void test_snp_ioctl(void *guest_code, uint32_t type, uint64_t policy)
{
	test_snp_launch_start(type, policy);
	test_snp_launch_update(type, policy);
	test_snp_launch_finish(type, policy);
}

static void test_sev_ioctl(void *guest_code, uint32_t type, uint64_t policy)
{
	test_sev_launch(guest_code, type, policy);
}

static void test_sev(void *guest_code, uint32_t type, uint64_t policy)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	if (type == KVM_X86_SNP_VM)
		test_snp_ioctl(guest_code, type, policy);
	else
		test_sev_ioctl(guest_code, type, policy);

	vm = vm_sev_create_with_one_vcpu(type, guest_code, &vcpu);

	/* TODO: Validate the measurement is as expected. */
	vm_sev_launch(vm, policy, NULL);

	for (;;) {
		vcpu_run(vcpu);

		if (vm->type == KVM_X86_SEV_ES_VM || vm->type == KVM_X86_SNP_VM) {
			TEST_ASSERT(vcpu->run->exit_reason == KVM_EXIT_SYSTEM_EVENT,
				    "Wanted SYSTEM_EVENT, got %s",
				    exit_reason_str(vcpu->run->exit_reason));
			TEST_ASSERT_EQ(vcpu->run->system_event.type, KVM_SYSTEM_EVENT_SEV_TERM);
			TEST_ASSERT_EQ(vcpu->run->system_event.ndata, 1);
			TEST_ASSERT_EQ(vcpu->run->system_event.data[0], GHCB_MSR_TERM_REQ);
			break;
		}

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			continue;
		case UCALL_DONE:
			return;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		default:
			TEST_FAIL("Unexpected exit: %s",
				  exit_reason_str(vcpu->run->exit_reason));
		}
	}

	kvm_vm_free(vm);
}

static void guest_shutdown_code(void)
{
	struct desc_ptr idt;

	/* Clobber the IDT so that #UD is guaranteed to trigger SHUTDOWN. */
	memset(&idt, 0, sizeof(idt));
	__asm__ __volatile__("lidt %0" :: "m"(idt));

	__asm__ __volatile__("ud2");
}

static void test_sev_shutdown(uint32_t type, uint64_t policy)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	vm = vm_sev_create_with_one_vcpu(type, guest_shutdown_code, &vcpu);

	vm_sev_launch(vm, policy, NULL);

	vcpu_run(vcpu);
	TEST_ASSERT(vcpu->run->exit_reason == KVM_EXIT_SHUTDOWN,
		    "Wanted SHUTDOWN, got %s",
		    exit_reason_str(vcpu->run->exit_reason));

	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV));

	test_sev(guest_sev_code, KVM_X86_SEV_VM, SEV_POLICY_NO_DBG);
	test_sev(guest_sev_code, KVM_X86_SEV_VM, 0);

	if (kvm_cpu_has(X86_FEATURE_SEV_ES)) {
		test_sev(guest_sev_es_code, KVM_X86_SEV_ES_VM, SEV_POLICY_ES | SEV_POLICY_NO_DBG);
		test_sev(guest_sev_es_code, KVM_X86_SEV_ES_VM, SEV_POLICY_ES);

		test_sev_shutdown(KVM_X86_SEV_ES_VM, SEV_POLICY_ES);

		if (kvm_has_cap(KVM_CAP_XCRS) &&
		    (xgetbv(0) & XFEATURE_MASK_X87_AVX) == XFEATURE_MASK_X87_AVX) {
			test_sync_vmsa(KVM_X86_SEV_ES_VM, SEV_POLICY_ES);
			test_sync_vmsa(KVM_X86_SEV_ES_VM, SEV_POLICY_ES | SEV_POLICY_NO_DBG);
		}
	}

	if (kvm_cpu_has(X86_FEATURE_SNP) && is_kvm_snp_supported()) {
		unsigned long snp_policy = SNP_POLICY;

		if (unlikely(!is_smt_active()))
			snp_policy &= ~SNP_POLICY_SMT;

		test_sev(guest_snp_code, KVM_X86_SNP_VM, snp_policy);
		/* Test minimum firmware level */
		test_sev(guest_snp_code, KVM_X86_SNP_VM,
			 snp_policy |
			 SNP_FW_VER_MAJOR(SNP_FW_REQ_VER_MAJOR) |
			 SNP_FW_VER_MINOR(SNP_FW_REQ_VER_MINOR));

		test_sev_shutdown(KVM_X86_SNP_VM, snp_policy);

		if (kvm_has_cap(KVM_CAP_XCRS) &&
		    (xgetbv(0) & XFEATURE_MASK_X87_AVX) == XFEATURE_MASK_X87_AVX) {
			test_sync_vmsa(KVM_X86_SNP_VM, snp_policy);
		}
	}

	return 0;
}
