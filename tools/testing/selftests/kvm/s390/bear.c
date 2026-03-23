// SPDX-License-Identifier: GPL-2.0-only
/*
 * LPSW(E|Y) bear tests.
 * LPSW and LPSWE do set the bear but LPSWEY doesn't.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "test_util.h"
#include "kvm_util.h"
#include "kselftest.h"
#include "ucall_common.h"
#include "facility.h"

static void guest_lpswey(void)
{
	extern void lpswey_dest_addr(void);
	u64 psw[2] = {0x0400000180000000ULL, (uintptr_t)lpswey_dest_addr};
	u64 bear;

	asm volatile (
		"	larl	%%r3,lpswey_addr\n"
		"lpswey_addr:\n"
		"	.insn	siy,0xeb0000000071,%[psw],0\n"
		"       nop\n"
		"       nop\n"
		".globl lpswey_dest_addr \n"
		"lpswey_dest_addr:\n"
		"       .insn   s,0xb2010000,%[bear]\n"
		"	lg	%%r4, %[bear]\n"
		"       nop\n"
		"       nop\n"
		: [bear] "=Q" (bear)
		: [psw] "T" (psw)
		: "cc", "r3", "r4"
		);
}

static void guest_lpswe(void)
{
	extern void lpswe_dest_addr(void);
	u64 psw[2] = {0x0400000180000000ULL, (uintptr_t)lpswe_dest_addr};
	u64 bear;

	asm volatile (
		"	larl	%%r3,lpswe_addr\n"
		"lpswe_addr:\n"
		"       lpswe    %[psw]\n"
		"       nop\n"
		"       nop\n"
		".globl lpswe_dest_addr\n"
		"lpswe_dest_addr:\n"
		"       .insn   s,0xb2010000,%[bear]\n"
		"	lg	%%r4, %[bear]\n"
		"       nop\n"
		"       nop\n"
		: [bear] "=Q" (bear)
		: [psw] "Q" (psw)
		: "cc", "r3", "r4"
		);
}

static void guest_lpsw(void)
{
	extern void lpsw_dest_addr(void);
	u64 psw_short = (0x0400000180000000ULL | BIT(63 - 12) |
			 (uintptr_t)lpsw_dest_addr);
	u64 bear;

	asm volatile (
		"	larl	%%r3,lpsw_addr\n"
		"lpsw_addr:\n"
		"       lpsw    %[psw]\n"
		"       nop\n"
		"       nop\n"
		".globl lpsw_dest_addr\n"
		"lpsw_dest_addr:\n"
		"       .insn   s,0xb2010000,%[bear]\n"
		"	lg	%%r4, %[bear]\n"
		"       nop\n"
		"       nop\n"
		: [bear] "=Q" (bear)
		: [psw] "Q" (psw_short)
		: "cc", "r3", "r4"
		);
}

/* A machine check forces KVM to emulate PSW loading */
static void inject_mcheck(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_irq irq = {};
	int irqs;

	irq.type = KVM_S390_MCHK;
	/* External damage mcheck */
	irq.u.mchk.cr14 = BIT(63 - 38);
	irq.u.mchk.mcic = BIT(58);
	irqs = __vcpu_ioctl(vcpu, KVM_S390_IRQ, &irq);
	TEST_ASSERT(irqs >= 0, "Error injecting MCHECK errno %d", errno);
}

static void test_lpswey(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_run *run;
	struct kvm_vm *vm;

	vm = vm_create_with_one_vcpu(&vcpu, guest_lpswey);
	inject_mcheck(vcpu);
	run = vcpu->run;
	vcpu_run(vcpu);
	ksft_test_result(run->s.regs.gprs[3] != run->s.regs.gprs[4],
			 "emulation: lpswey bear does not match\n");
	kvm_vm_free(vm);

	vm = vm_create_with_one_vcpu(&vcpu, guest_lpswey);
	run = vcpu->run;
	vcpu_run(vcpu);
	ksft_test_result(run->s.regs.gprs[3] &&
			 run->s.regs.gprs[3] != run->s.regs.gprs[4],
			 "interpretation: lpswey bear does not match\n");
	kvm_vm_free(vm);
}

static void test_lpswe(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_run *run;
	struct kvm_vm *vm;

	vm = vm_create_with_one_vcpu(&vcpu, guest_lpswe);
	inject_mcheck(vcpu);
	run = vcpu->run;
	vcpu_run(vcpu);
	ksft_test_result(run->s.regs.gprs[3] == run->s.regs.gprs[4],
			 "emulation: lpswe bear matches\n");
	kvm_vm_free(vm);

	vm = vm_create_with_one_vcpu(&vcpu, guest_lpsw);
	run = vcpu->run;
	vcpu_run(vcpu);
	ksft_test_result(run->s.regs.gprs[3] &&
			 run->s.regs.gprs[3] == run->s.regs.gprs[4],
			 "interpretation: lpswe bear matches\n");
	kvm_vm_free(vm);
}

static void test_lpsw(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_run *run;
	struct kvm_vm *vm;

	vm = vm_create_with_one_vcpu(&vcpu, guest_lpsw);
	inject_mcheck(vcpu);
	run = vcpu->run;
	vcpu_run(vcpu);
	ksft_test_result(run->s.regs.gprs[3] == run->s.regs.gprs[4],
			 "emulation: lpsw bear matches\n");
	kvm_vm_free(vm);

	vm = vm_create_with_one_vcpu(&vcpu, guest_lpsw);
	run = vcpu->run;
	vcpu_run(vcpu);
	ksft_test_result(run->s.regs.gprs[3] &&
			 run->s.regs.gprs[3] == run->s.regs.gprs[4],
			 "interpretation: lpsw bear matches\n");
	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(test_facility(193));

	ksft_print_header();
	ksft_set_plan(6);
	test_lpsw();
	test_lpswe();
	test_lpswey();
	ksft_finished();
}
