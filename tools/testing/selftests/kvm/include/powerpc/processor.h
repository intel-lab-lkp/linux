/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * powerpc processor specific defines
 */
#ifndef SELFTEST_KVM_PROCESSOR_H
#define SELFTEST_KVM_PROCESSOR_H

#include <linux/compiler.h>
#include "ppc_asm.h"

extern unsigned char __interrupts_start[];
extern unsigned char __interrupts_end[];

struct kvm_vm;
struct kvm_vcpu;
extern bool (*interrupt_handler)(struct kvm_vcpu *vcpu, unsigned trap);

struct ex_regs {
	uint64_t	gprs[32];
	uint64_t	nia;
	uint64_t	msr;
	uint64_t	cfar;
	uint64_t	lr;
	uint64_t	ctr;
	uint64_t	xer;
	uint32_t	cr;
	uint32_t	trap;
	uint64_t	vaddr; /* vaddr of this struct */
};

void vm_install_exception_handler(struct kvm_vm *vm, int vector,
			void (*handler)(struct ex_regs *));

static inline void cpu_relax(void)
{
	asm volatile("" ::: "memory");
}

#endif
