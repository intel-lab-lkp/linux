// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"
#include "vmx.h"

#define NR_ITERATIONS 100
#define L2_GUEST_STACK_SIZE 64

struct buslock_test {
	unsigned char pad[PAGE_SIZE - 2];
	atomic_long_t val;
} __packed;

struct buslock_test test __aligned(PAGE_SIZE);

static __always_inline void buslock_atomic_add(int i, atomic_long_t *v)
{
	asm volatile(LOCK_PREFIX "addl %1,%0"
		     : "+m" (v->counter)
		     : "ir" (i) : "memory");
}

static void buslock_add(void)
{
	/*
	 * Increment a page unaligned variable atomically.
	 * This should generate a bus lock exit.
	 */
	for (int i = 0; i < NR_ITERATIONS; i++)
		buslock_atomic_add(2, &test.val);
}

static void l2_guest_code(void)
{
	buslock_add();
	GUEST_DONE();
}

static void l1_svm_code(struct svm_test_data *svm)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb = svm->vmcb;

	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_VMMCALL);
	GUEST_DONE();
}

static void l1_vmx_code(struct vmx_pages *vmx)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

	GUEST_ASSERT_EQ(prepare_for_vmx_operation(vmx), true);
	GUEST_ASSERT_EQ(load_vmcs(vmx), true);

	prepare_vmcs(vmx, NULL, &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	GUEST_ASSERT(!vmwrite(GUEST_RIP, (u64)l2_guest_code));
	GUEST_ASSERT(!vmlaunch());

	GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_VMCALL);
	GUEST_DONE();
}

static void guest_code(void *test_data)
{
	buslock_add();

	if (this_cpu_has(X86_FEATURE_SVM))
		l1_svm_code(test_data);
	else
		l1_vmx_code(test_data);
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_run *run;
	struct kvm_vm *vm;
	vm_vaddr_t nested_test_data_gva;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM) || kvm_cpu_has(X86_FEATURE_VMX));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_X86_BUS_LOCK_EXIT));

	vm = vm_create(1);
	vm_enable_cap(vm, KVM_CAP_X86_BUS_LOCK_EXIT, KVM_BUS_LOCK_DETECTION_EXIT);
	vcpu = vm_vcpu_add(vm, 0, guest_code);

	if (kvm_cpu_has(X86_FEATURE_SVM))
		vcpu_alloc_svm(vm, &nested_test_data_gva);
	else
		vcpu_alloc_vmx(vm, &nested_test_data_gva);

	vcpu_args_set(vcpu, 1, nested_test_data_gva);

	run = vcpu->run;

	for (;;) {
		struct ucall uc;

		vcpu_run(vcpu);

		if (run->exit_reason == KVM_EXIT_IO) {
			switch (get_ucall(vcpu, &uc)) {
			case UCALL_ABORT:
				REPORT_GUEST_ASSERT(uc);
				/* NOT REACHED */
			case UCALL_SYNC:
				continue;
			case UCALL_DONE:
				goto done;
			default:
				TEST_FAIL("Unknown ucall 0x%lx.", uc.cmd);
			}
		}

		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_X86_BUS_LOCK);
	}
done:
	kvm_vm_free(vm);
	return 0;
}
