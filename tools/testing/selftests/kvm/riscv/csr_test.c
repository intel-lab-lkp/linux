// SPDX-License-Identifier: GPL-2.0-only
/*
 * csr_test.c - Tests the csr functionality.
 */
#include "kvm_util.h"
#include "ucall_common.h"

#define CSR_TEST 0x240
#define FP 0x00006000

/*
 * Use the fcsr as a U-mode accesible csr and compare against the custom 'test'
 * hypervisor csr (currently using vsscratch)
 */
#define test_csr(write, initial, mode)								\
static void test_##write(void)									\
{												\
	unsigned long hypervisor_result, reference_result, old_hypervisor;			\
	unsigned long mask = 0x15;								\
	asm volatile (										\
		"csrs	sstatus, %[enable_fp]\n"						\
		"csrw	fcsr, %[init]\n"							\
		#write"	zero, fcsr, %[mask]\n"							\
		"csrr	%[ref_res], fcsr\n"							\
		: [ref_res] "=&r" (reference_result)						\
		: [enable_fp] "r" (FP), [mask] #mode(mask), [init] "r" (initial)		\
		: "memory"									\
	);											\
	asm volatile (										\
		"csrw	%[test_csr], %[init]\n"							\
		#write"	%[old], %[test_csr], %[mask]\n"						\
		"csrr	%[hyp_res], %[test_csr]\n"						\
		: [hyp_res] "=&r" (hypervisor_result), [old] "=&r" (old_hypervisor)		\
		: [test_csr] "i"(CSR_TEST), [mask] #mode(mask), [init] "r" (initial)		\
		: "memory"									\
	);											\
	/* Check that writing works */								\
	GUEST_ASSERT_EQ(reference_result, hypervisor_result);					\
	/* Check that reading works */								\
	GUEST_ASSERT_EQ(old_hypervisor, initial);						\
	GUEST_DONE();										\
}

test_csr(csrrw, 0x0, r)
test_csr(csrrs, 0x0, r)
test_csr(csrrc, 0x15, r)
test_csr(csrrwi, 0x0, i)
test_csr(csrrsi, 0x0, i)
test_csr(csrrci, 0x15, i)

static void run(void *guest_code, char *instruction)
{
	struct ucall uc;
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	kvm_create_device(vm, KVM_DEV_TYPE_TEST);

	vcpu_run(vcpu);

	TEST_ASSERT(get_ucall(vcpu, &uc) == UCALL_DONE,
		    "CSR instruction '%s' failed: '%s'", instruction,
		    uc.buffer);

	kvm_vm_free(vm);
}

static void check_test_csr_guest(void)
{
	unsigned long scause, stvec;

	asm volatile(
		"la	%[stvec], 1f\n"
		"csrw	stvec, %[stvec]\n"
		"csrwi	%[test_csr], 0x0\n"
		"1:\n"
		"csrr	%[scause], scause\n"
		: [scause] "=&r" (scause), [stvec] "=&r" (stvec)
		: [test_csr] "i" (CSR_TEST)
	);

	/* An illegal instruction will be generated if CONFIG_RISCV_KVM_TEST_CSR is not enabled. */
	if (scause == 2)
		GUEST_FAIL("CONFIG_RISCV_KVM_TEST_CSR not enabled.\n");
	GUEST_DONE();
}

static int check_test_csr(void)
{
	struct ucall uc;
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int success;

	vm = vm_create_with_one_vcpu(&vcpu, check_test_csr_guest);

	/* Skip if CONFIG_KVM_MMIO_TEST not enabled */
	if (!kvm_create_device(vm, KVM_DEV_TYPE_TEST))
		exit(KSFT_SKIP);

	vcpu_run(vcpu);

	success = get_ucall(vcpu, &uc) == UCALL_DONE;

	kvm_vm_free(vm);

	return success;
}

int main(void)
{
	/* Skip if CONFIG_RISCV_KVM_TEST_CSR not enabled */
	if (!check_test_csr())
		exit(KSFT_SKIP);

	run(test_csrrw, "csrrw");
	run(test_csrrs, "csrrs");
	run(test_csrrc, "csrrc");
	run(test_csrrwi, "csrrwi");
	run(test_csrrsi, "csrrsi");
	run(test_csrrci, "csrrci");

	return 0;
}
