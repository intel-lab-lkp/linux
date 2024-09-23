// SPDX-License-Identifier: GPL-2.0
/*
 * This test covers error processing when doing weird things with MMIO addresses,
 * i.e. jumping into MMIO range or specifying it as IDT / GDT descriptor base.
 */
#include <stdio.h>
#include "kvm_util.h"
#include "processor.h"
#include <stdint.h>

#define MMIO_ADDR 0xDEADBEE000UL
/* This address is not canonical, so any reference will result in #GP */
#define GP_ADDR 0xDEADBEEFDEADBEEFULL

enum test_desc_type {
	TEST_DESC_IDT,
	TEST_DESC_GDT,
};

static const struct desc_ptr faulty_desc = {
	.address = MMIO_ADDR,
	.size = 0xFFF,
};

static void faulty_desc_guest_code(enum test_desc_type dtype)
{
	if (dtype == TEST_DESC_IDT)
		__asm__ __volatile__("lidt %0"::"m"(faulty_desc));
	else
		__asm__ __volatile__("lgdt %0"::"m"(faulty_desc));

	/* Generate a #GP */
	*((uint8_t *)GP_ADDR) = 0x1;

	/* We should never reach this point */
	GUEST_ASSERT(0);
}

/*
 * This test tries to point the IDT / GDT descriptor to an MMIO range.
 * This action should cause a triple fault in guest, as it happens when
 * your descriptors are messed up on the actual hardware.
 */
static void test_faulty_desc(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int i;

	enum test_desc_type dtype_tests[] = { TEST_DESC_IDT, TEST_DESC_GDT };

	for (i = 0; i < ARRAY_SIZE(dtype_tests); i++) {
		vm = vm_create_with_one_vcpu(&vcpu, faulty_desc_guest_code);
		vcpu_args_set(vcpu, 1, dtype_tests[i]);
		virt_map(vm, MMIO_ADDR, MMIO_ADDR, 1);

		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_SHUTDOWN);
		kvm_vm_free(vm);
	}
}

static void jump_to_mmio_guest_code(bool write_first)
{
	void (*f)(void) = (void *)(MMIO_ADDR);

	if (write_first) {
		/*
		 * We get different vmexit codes when accessing the MMIO address for the second
		 * time with VMX. For the first time it is an EPT violation, for the second -
		 * EPT misconfig. We need to check that we get #UD in both cases.
		 */
		*((char *)MMIO_ADDR) = 0x1;
	}

	f();

	/* We should never reach this point */
	GUEST_ASSERT(0);
}

static void guest_ud_handler(struct ex_regs *regs)
{
	GUEST_DONE();
}

/*
 * This test tries to jump to an MMIO address. As fetching the instructions
 * from MMIO is not supported by KVM and doesn't make any practical sense,
 * KVM should handle it gracefully and inject #UD into guest.
 */
static void test_jump_to_mmio(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct kvm_run *run;
	struct ucall uc;
	int i;

	bool test_cases_write_first[] = { false, true };

	for (i = 0; i < ARRAY_SIZE(test_cases_write_first); i++) {
		vm = vm_create_with_one_vcpu(&vcpu, jump_to_mmio_guest_code);
		virt_map(vm, MMIO_ADDR, MMIO_ADDR, 1);
		vcpu_args_set(vcpu, 1, test_cases_write_first[i]);
		vm_install_exception_handler(vm, UD_VECTOR, guest_ud_handler);

		run = vcpu->run;

		vcpu_run(vcpu);
		if (test_cases_write_first[i] && run->exit_reason == KVM_EXIT_MMIO) {
			/* Process first MMIO access if required */
			vcpu_run(vcpu);
		}

		/* If #UD was injected correctly, our #UD handler will issue UCALL_DONE */
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, UCALL_EXIT_REASON);
		TEST_ASSERT(get_ucall(vcpu, &uc) == UCALL_DONE,
			    "Guest should have gone into #UD handler when jumping to MMIO address, however it didn't happen");
		kvm_vm_free(vm);
	}
}

static void faulty_idte_guest_code(void)
{
	/*
	 * We are triggering #GP here, and as it's IDT entry points to an MMIO range,
	 * we should get an #UD as instruction fetching from MMIO address is prohibited
	 */
	*((uint8_t *)GP_ADDR) = 0x1;

	/* We should never reach this point */
	GUEST_ASSERT(0);
}

/*
 * When IDT entry points to an MMIO address, it should be handled as a jump to MMIO address
 * and should cause #UD in the guest, as fetches from MMIO are not supported. It should not
 * cause a triple fault in such a case, so we don't expect KVM_EXIT_SHUTDOWN here.
 */
static void test_faulty_idte(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct ucall uc;

	vm = vm_create_with_one_vcpu(&vcpu, faulty_idte_guest_code);
	virt_map(vm, MMIO_ADDR, MMIO_ADDR, 1);

	/* GP vector points to MMIO range, jumping to it will trigger an #UD */
	vm_install_exception_handler(vm, GP_VECTOR, (void *)MMIO_ADDR);
	vm_install_exception_handler(vm, UD_VECTOR, guest_ud_handler);

	vcpu_run(vcpu);
	/* If we reach #UD handler it will issue UCALL_DONE */
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, UCALL_EXIT_REASON);
	TEST_ASSERT(get_ucall(vcpu, &uc) == UCALL_DONE,
		    "Guest should have gone into #UD handler when jumping to MMIO address, however it didn't happen");
	kvm_vm_free(vm);
}

static void faulty_ud_idte_guest_code(void)
{
	asm("ud2");

	/* We should never reach this point */
	GUEST_ASSERT(0);
}

/*
 * This test checks that we won't hang in the infinite loop if the #UD handler
 * also causes #UD (as it points to an MMIO address). In this situation, we will
 * run out of stack eventually, which will cause a triple fault
 */
static void test_faulty_ud_handler(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;

	vm = vm_create_with_one_vcpu(&vcpu, faulty_ud_idte_guest_code);
	virt_map(vm, MMIO_ADDR, MMIO_ADDR, 1);

	vm_install_exception_handler(vm, UD_VECTOR, (void *)MMIO_ADDR);

	vcpu_run(vcpu);
	/* #UD caused when jumping to #UD handler should overflow stack causing a triple fault */
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_SHUTDOWN);
	kvm_vm_free(vm);
}

int main(void)
{
	test_faulty_desc();
	test_jump_to_mmio();
	test_faulty_idte();
	test_faulty_ud_handler();

	return EXIT_SUCCESS;
}
