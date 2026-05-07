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

static void guest_sev_test_msr(u32 msr)
{
	u64 val = rdmsr(msr);

	wrmsr(msr, val);
	GUEST_ASSERT(val == rdmsr(msr));
}

#define guest_sev_test_reg(reg)			\
do {						\
	u64 val = get_##reg();			\
						\
	set_##reg(val);				\
	GUEST_ASSERT(val == get_##reg());	\
} while (0)

static void guest_sev_test_regs(void)
{
	guest_sev_test_msr(MSR_EFER);
	guest_sev_test_reg(cr0);
	guest_sev_test_reg(cr3);
	guest_sev_test_reg(cr4);
	guest_sev_test_reg(cr8);
}

#define XFEATURE_MASK_X87_AVX (XFEATURE_MASK_FP | XFEATURE_MASK_SSE | XFEATURE_MASK_YMM)

static void guest_snp_code(void)
{
	u64 sev_msr = rdmsr(MSR_AMD64_SEV);

	GUEST_ASSERT(sev_msr & MSR_AMD64_SEV_ENABLED);
	GUEST_ASSERT(sev_msr & MSR_AMD64_SEV_ES_ENABLED);
	GUEST_ASSERT(sev_msr & MSR_AMD64_SEV_SNP_ENABLED);

	guest_sev_test_regs();

	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	vmgexit();
}

static void guest_sev_es_code(void)
{
	/* TODO: Check CPUID after GHCB-based hypercall support is added. */
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ES_ENABLED);

	guest_sev_test_regs();

	/*
	 * TODO: Add GHCB and ucall support for SEV-ES guests.  For now, simply
	 * force "termination" to signal "done" via the GHCB MSR protocol.
	 */
	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	vmgexit();
}

static void guest_sev_code(void)
{
	GUEST_ASSERT(this_cpu_has(X86_FEATURE_SEV));
	GUEST_ASSERT(rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED);

	guest_sev_test_regs();

	GUEST_DONE();
}

static void xsave_all_registers(void *addr)
{
	__asm__ __volatile__(
		"mov $" __stringify(XFEATURE_MASK_X87_AVX) ", %eax\n"
		"xor %edx, %edx\n"
		"xsave (%0)"
		:
		: "r"(addr)
		: "eax", "edx", "memory"
	 );
}

static void guest_code_xsave(void *vmsa_gva)
{
	xsave_all_registers(vmsa_gva);
	guest_sev_es_code();
}

static void compare_xsave(u8 *from_host, u8 *from_guest)
{
	int i;
	bool bad = false;
	for (i = 0; i < 4095; i++) {
		if (from_host[i] != from_guest[i]) {
			printf("mismatch at %u | %02hhx %02hhx\n",
			       i, from_host[i], from_guest[i]);
			bad = true;
		}
	}

	if (bad)
		abort();
}

static void test_sync_vmsa(u32 type, u64 policy)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	gva_t gva;
	void *hva;

	double x87val = M_PI;
	struct kvm_xsave __attribute__((aligned(64))) xsave = { 0 };

	vm = vm_sev_create_with_one_vcpu(type, guest_code_xsave, &vcpu);
	gva = vm_alloc_shared(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR,
			      MEM_REGION_TEST_DATA);
	hva = addr_gva2hva(vm, gva);

	vcpu_args_set(vcpu, 1, gva);

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
	memset(hva, 0, PAGE_SIZE);

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

static void test_sev(void *guest_code, u32 type, u64 policy)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	vm = vm_sev_create_with_one_vcpu(type, guest_code, &vcpu);

	/* TODO: Validate the measurement is as expected. */
	vm_sev_launch(vm, policy, NULL);

	for (;;) {
		vcpu_run(vcpu);

		if (is_sev_es_vm(vm)) {
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
	set_idt(&idt);

	__asm__ __volatile__("ud2");
}

static void test_sev_shutdown(u32 type, u64 policy)
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

static void test_sev_smoke(void *guest, u32 type, u64 policy)
{
	const u64 xf_mask = XFEATURE_MASK_X87_AVX;

	if (type == KVM_X86_SNP_VM)
		test_sev(guest, type, policy | SNP_POLICY_DBG);
	else
		test_sev(guest, type, policy | SEV_POLICY_NO_DBG);
	test_sev(guest, type, policy);

	if (type == KVM_X86_SEV_VM)
		return;

	test_sev_shutdown(type, policy);

	if (kvm_has_cap(KVM_CAP_XCRS) &&
	    (xgetbv(0) & kvm_cpu_supported_xcr0() & xf_mask) == xf_mask) {
		test_sync_vmsa(type, policy);
		if (type == KVM_X86_SNP_VM)
			test_sync_vmsa(type, policy | SNP_POLICY_DBG);
		else
			test_sync_vmsa(type, policy | SEV_POLICY_NO_DBG);
	}
}

#define GHCB_MSR_REG_GPA_REQ		0x012
#define GHCB_MSR_REG_GPA_REQ_VAL(v)                \
	/* GHCBData[63:12] */                      \
	(((u64)((v) & GENMASK_ULL(51, 0)) << 12) | \
	 /* GHCBData[11:0] */			   \
	 GHCB_MSR_REG_GPA_REQ)

#define GHCB_MSR_REG_GPA_RESP		0x013
#define GHCB_MSR_REG_GPA_RESP_VAL(v)			\
	/* GHCBData[63:12] */				\
	(((u64)(v) & GENMASK_ULL(63, 12)) >> 12)

#define GHCB_DATA_LOW			12
#define GHCB_MSR_INFO_MASK		(BIT_ULL(GHCB_DATA_LOW) - 1)
#define GHCB_RESP_CODE(v) ((v) & GHCB_MSR_INFO_MASK)

/*
 * SNP Page State Change Operation
 *
 * GHCBData[55:52] - Page operation:
 *   0x0001	Page assignment, Private
 *   0x0002	Page assignment, Shared
 */
enum psc_op {
	SNP_PAGE_STATE_PRIVATE = 1,
	SNP_PAGE_STATE_SHARED,
};

#define GHCB_MSR_PSC_REQ		0x014
#define GHCB_MSR_PSC_REQ_GFN(gfn, op)			\
	/* GHCBData[55:52] */				\
	(((u64)((op) & 0xf) << 52) |			\
	/* GHCBData[51:12] */				\
	((u64)((gfn) & GENMASK_ULL(39, 0)) << 12) |	\
	/* GHCBData[11:0] */				\
	GHCB_MSR_PSC_REQ)

#define GHCB_MSR_PSC_RESP		0x015
#define GHCB_MSR_PSC_RESP_VAL(val)			\
	/* GHCBData[63:32] */				\
	(((u64)(val) & GENMASK_ULL(63, 32)) >> 32)

static u64 ghcb_gpa;
static void snp_register_ghcb(void)
{
	u64 ghcb_pfn = ghcb_gpa >> PAGE_SHIFT;
	u64 val;

	GUEST_ASSERT(ghcb_gpa);

	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_REG_GPA_REQ_VAL(ghcb_gpa >> PAGE_SHIFT));
	vmgexit();

	val = rdmsr(MSR_AMD64_SEV_ES_GHCB);
	GUEST_ASSERT_EQ(GHCB_RESP_CODE(val), GHCB_MSR_REG_GPA_RESP);
	GUEST_ASSERT_EQ(GHCB_MSR_REG_GPA_RESP_VAL(val), ghcb_pfn);
}

static void snp_page_state_change(u64 gpa, enum psc_op op)
{
	u64 val;

	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_PSC_REQ_GFN(gpa >> PAGE_SHIFT, op));
	vmgexit();

	val = rdmsr(MSR_AMD64_SEV_ES_GHCB);
	GUEST_ASSERT_EQ(GHCB_RESP_CODE(val), GHCB_MSR_PSC_RESP);
	GUEST_ASSERT_EQ(GHCB_MSR_PSC_RESP_VAL(val), 0);
}

#define RMP_PG_SIZE_4K			0
static inline void pvalidate(void *vaddr, bool validate)
{
	bool no_rmpupdate;
	int rc;

	/* "pvalidate" mnemonic support in binutils 2.36 and newer */
	asm volatile(".byte 0xF2, 0x0F, 0x01, 0xFF\n\t"
		     : "=@ccc"(no_rmpupdate), "=a"(rc)
		     : "a"(vaddr), "c"(RMP_PG_SIZE_4K), "d"(validate)
		     : "memory", "cc");

	GUEST_ASSERT(!no_rmpupdate);
	GUEST_ASSERT_EQ(rc, 0);
}

#define CONVERSION_TEST_VALUE_SHARED_1 0xab
#define CONVERSION_TEST_VALUE_SHARED_2 0xcd
#define CONVERSION_TEST_VALUE_PRIVATE 0xef
#define CONVERSION_TEST_VALUE_SHARED_3 0xbc
#define CONVERSION_TEST_VALUE_SHARED_4 0xde
static void guest_code_conversion(u8 *test_shared_gva, u8 *test_private_gva, u64 test_gpa)
{
	snp_register_ghcb();

	GUEST_ASSERT_EQ(READ_ONCE(*test_shared_gva), CONVERSION_TEST_VALUE_SHARED_1);
	WRITE_ONCE(*test_shared_gva, CONVERSION_TEST_VALUE_SHARED_2);

	snp_page_state_change(test_gpa, SNP_PAGE_STATE_PRIVATE);
	pvalidate(test_private_gva, true);

	WRITE_ONCE(*test_private_gva, CONVERSION_TEST_VALUE_PRIVATE);
	GUEST_ASSERT_EQ(READ_ONCE(*test_private_gva), CONVERSION_TEST_VALUE_PRIVATE);

	pvalidate(test_private_gva, false);
	snp_page_state_change(test_gpa, SNP_PAGE_STATE_SHARED);

	GUEST_ASSERT_EQ(READ_ONCE(*test_shared_gva), CONVERSION_TEST_VALUE_SHARED_3);
	WRITE_ONCE(*test_shared_gva, CONVERSION_TEST_VALUE_SHARED_4);

	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
	vmgexit();
}

static void test_conversion(u64 policy)
{
	gva_t test_private_gva;
	gva_t test_shared_gva;
	struct kvm_vcpu *vcpu;
	gva_t ghcb_gva;
	gpa_t test_gpa;
	struct kvm_vm *vm;
	void *ghcb_hva;
	void *test_hva;

	vm = vm_sev_create_with_one_vcpu(KVM_X86_SNP_VM, guest_code_conversion, &vcpu);

	ghcb_gva = vm_alloc_shared(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR,
				   MEM_REGION_TEST_DATA);
	ghcb_hva = addr_gva2hva(vm, ghcb_gva);
	ghcb_gpa = addr_gva2gpa(vm, ghcb_gva);
	sync_global_to_guest(vm, ghcb_gpa);

	test_shared_gva = vm_alloc_shared(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR,
					  MEM_REGION_TEST_DATA);
	test_hva = addr_gva2hva(vm, test_shared_gva);
	test_gpa = addr_gva2gpa(vm, test_shared_gva);

	test_private_gva = vm_unused_gva_gap(vm, PAGE_SIZE, KVM_UTIL_MIN_VADDR);
	___virt_pg_map(vm, &vm->mmu, test_private_gva, test_gpa, PG_SIZE_4K, true);

	vcpu_args_set(vcpu, 3, test_shared_gva, test_private_gva, test_gpa);

	vm_sev_launch(vm, policy, NULL);

	WRITE_ONCE(*(u8 *)test_hva, CONVERSION_TEST_VALUE_SHARED_1);

	fprintf(stderr, "ghcb_hva=%p ghcb_gpa=%lx ghcb_gva=%lx\n", ghcb_hva, ghcb_gpa, ghcb_gva);
	fprintf(stderr, "test_hva=%p test_gpa=%lx test_private_gva=%lx test_shared_gva=%lx\n", test_hva, test_gpa, test_private_gva, test_shared_gva);

	vcpu_run(vcpu);

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_HYPERCALL);
	TEST_ASSERT_EQ(vcpu->run->hypercall.nr, KVM_HC_MAP_GPA_RANGE);
	TEST_ASSERT_EQ(vcpu->run->hypercall.args[0], test_gpa);
	TEST_ASSERT_EQ(vcpu->run->hypercall.args[1], 1);
	TEST_ASSERT_EQ(vcpu->run->hypercall.args[2], KVM_MAP_GPA_RANGE_ENCRYPTED | KVM_MAP_GPA_RANGE_PAGE_SZ_4K);

	vm_mem_set_private(vm, test_gpa, PAGE_SIZE);

	vcpu_run(vcpu);

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_HYPERCALL);
	TEST_ASSERT_EQ(vcpu->run->hypercall.nr, KVM_HC_MAP_GPA_RANGE);
	TEST_ASSERT_EQ(vcpu->run->hypercall.args[0], test_gpa);
	TEST_ASSERT_EQ(vcpu->run->hypercall.args[1], 1);
	TEST_ASSERT_EQ(vcpu->run->hypercall.args[2], KVM_MAP_GPA_RANGE_DECRYPTED | KVM_MAP_GPA_RANGE_PAGE_SZ_4K);

	vm_mem_set_shared(vm, test_gpa, PAGE_SIZE);

	fprintf(stderr, "test_hva contents = %x\n", READ_ONCE(*(u8 *)test_hva));

	WRITE_ONCE(*(u8 *)test_hva, CONVERSION_TEST_VALUE_SHARED_3);
	TEST_ASSERT_EQ(*(u8 *)test_hva, CONVERSION_TEST_VALUE_SHARED_3);

	vcpu_run(vcpu);

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_SYSTEM_EVENT);
	TEST_ASSERT_EQ(vcpu->run->system_event.type, KVM_SYSTEM_EVENT_SEV_TERM);
	TEST_ASSERT_EQ(vcpu->run->system_event.ndata, 1);
	TEST_ASSERT_EQ(vcpu->run->system_event.data[0], GHCB_MSR_TERM_REQ);

	TEST_ASSERT_EQ(*(u8 *)test_hva, CONVERSION_TEST_VALUE_SHARED_4);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV));

	// test_sev_smoke(guest_sev_code, KVM_X86_SEV_VM, 0);

	// if (kvm_cpu_has(X86_FEATURE_SEV_ES))
	// 	test_sev_smoke(guest_sev_es_code, KVM_X86_SEV_ES_VM, SEV_POLICY_ES);

	if (kvm_cpu_has(X86_FEATURE_SEV_SNP)) {
		test_conversion(snp_default_policy());

		// test_sev_smoke(guest_snp_code, KVM_X86_SNP_VM, snp_default_policy());
	}

	return 0;
}
