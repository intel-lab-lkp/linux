// SPDX-License-Identifier: GPL-2.0-only
/*
 * svm_buslock_test
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 * SVM testing: Buslock exit
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "svm_util.h"

#define NO_ITERATIONS 100
#define __cacheline_aligned __aligned(128)

struct buslock_test {
	unsigned char pad[126];
	atomic_long_t val;
} __packed;

struct buslock_test test __cacheline_aligned;

static __always_inline void buslock_atomic_add(int i, atomic_long_t *v)
{
	asm volatile(LOCK_PREFIX "addl %1,%0"
		     : "+m" (v->counter)
		     : "ir" (i) : "memory");
}

static void buslock_add(void)
{
	/*
	 * Increment a cache unaligned variable atomically.
	 * This should generate a bus lock exit.
	 */
	for (int i = 0; i < NO_ITERATIONS; i++)
		buslock_atomic_add(2, &test.val);
}

static void l2_guest_code(void)
{
	buslock_add();
	GUEST_DONE();
}

static void l1_guest_code(struct svm_test_data *svm)
{
	#define L2_GUEST_STACK_SIZE 64
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	struct vmcb *vmcb = svm->vmcb;

	generic_svm_setup(svm, l2_guest_code,
			  &l2_guest_stack[L2_GUEST_STACK_SIZE]);
	run_guest(vmcb, svm->vmcb_gpa);
	GUEST_ASSERT(vmcb->control.exit_code == SVM_EXIT_VMMCALL);
	GUEST_DONE();
}

static void guest_code(struct svm_test_data *svm)
{
	buslock_add();

	if (this_cpu_has(X86_FEATURE_SVM))
		l1_guest_code(svm);
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_run *run;
	struct kvm_vm *vm;
	vm_vaddr_t svm_gva;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SVM));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_X86_BUS_LOCK_EXIT));

	vm = vm_create(1);
	vm_enable_cap(vm, KVM_CAP_X86_BUS_LOCK_EXIT, KVM_BUS_LOCK_DETECTION_EXIT);
	vcpu = vm_vcpu_add(vm, 0, guest_code);

	vcpu_alloc_svm(vm, &svm_gva);
	vcpu_args_set(vcpu, 1, svm_gva);

	run = vcpu->run;

	for (;;) {
		struct ucall uc;

		vcpu_run(vcpu);

		if (run->exit_reason == KVM_EXIT_X86_BUS_LOCK) {
			run->flags &= ~KVM_RUN_X86_BUS_LOCK;
			run->exit_reason = 0;
			continue;
		}

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			/* NOT REACHED */
		case UCALL_SYNC:
			break;
		case UCALL_DONE:
			goto done;
		default:
			TEST_FAIL("Unknown ucall 0x%lx.", uc.cmd);
		}
	}
done:
	kvm_vm_free(vm);
	return 0;
}
