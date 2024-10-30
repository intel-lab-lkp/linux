// SPDX-License-Identifier: GPL-2.0-only
/*
 * vmware_hypercall_test
 *
 * Copyright (c) 2024 Broadcom. All Rights Reserved. The term
 * “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
 *
 * Based on:
 *    xen_vmcall_test.c
 *
 *    Copyright © 2020 Amazon.com, Inc. or its affiliates.
 *
 * VMware hypercall testing
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#define ARGVALUE(x) (0xdeadbeef5a5a0000UL + (x))
#define RETVALUE(x) (0xcafef00dfbfbffffUL + (x))

static void guest_code(void)
{
	unsigned long rax = ARGVALUE(1);
	unsigned long rbx = ARGVALUE(2);
	unsigned long rcx = ARGVALUE(3);
	unsigned long rdx = ARGVALUE(4);
	unsigned long rsi = ARGVALUE(5);
	unsigned long rdi = ARGVALUE(6);
	register unsigned long rbp __asm__("rbp") = ARGVALUE(7);

	__asm__ __volatile__("vmcall" :
			     "=a"(rax),  "=b"(rbx), "=c"(rcx), "=d"(rdx),
			     "=S"(rsi), "=D"(rdi) :
			     "a"(rax), "b"(rbx), "c"(rcx), "d"(rdx),
			     "S"(rsi), "D"(rdi), "r"(rbp));
	GUEST_ASSERT_EQ(rax, RETVALUE(1));
	GUEST_ASSERT_EQ(rbx, RETVALUE(2));
	GUEST_ASSERT_EQ(rcx, RETVALUE(3));
	GUEST_ASSERT_EQ(rdx, RETVALUE(4));
	GUEST_ASSERT_EQ(rdi, RETVALUE(5));
	GUEST_ASSERT_EQ(rsi, RETVALUE(6));
	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	if (!kvm_check_cap(KVM_CAP_X86_VMWARE_HYPERCALL)) {
		print_skip("KVM_CAP_X86_VMWARE_HYPERCALL not available");
		exit(KSFT_SKIP);
	}

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	vm_enable_cap(vm, KVM_CAP_X86_VMWARE_HYPERCALL, 1);

	for (;;) {
		struct kvm_run *run = vcpu->run;
		struct ucall uc;

		vcpu_run(vcpu);

		if (run->exit_reason == KVM_EXIT_HYPERCALL) {
			struct kvm_regs regs;

			TEST_ASSERT_EQ(run->hypercall.ret, 0);
			TEST_ASSERT_EQ(run->hypercall.longmode, 1);
			TEST_ASSERT_EQ(run->hypercall.nr, ARGVALUE(1));
			TEST_ASSERT_EQ(run->hypercall.args[0], ARGVALUE(2));
			TEST_ASSERT_EQ(run->hypercall.args[1], ARGVALUE(3));
			TEST_ASSERT_EQ(run->hypercall.args[2], ARGVALUE(4));
			TEST_ASSERT_EQ(run->hypercall.args[3], ARGVALUE(5));
			TEST_ASSERT_EQ(run->hypercall.args[4], ARGVALUE(6));
			TEST_ASSERT_EQ(run->hypercall.args[5], ARGVALUE(7));

			run->hypercall.ret = RETVALUE(1);
			vcpu_regs_get(vcpu, &regs);
			regs.rbx = RETVALUE(2);
			regs.rcx = RETVALUE(3);
			regs.rdx = RETVALUE(4);
			regs.rdi = RETVALUE(5);
			regs.rsi = RETVALUE(6);
			vcpu_regs_set(vcpu, &regs);
			continue;
		}

		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

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
