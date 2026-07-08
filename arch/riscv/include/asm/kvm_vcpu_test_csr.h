/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __KVM_VCPU_RISCV_TEST_CSR_H
#define __KVM_VCPU_RISCV_TEST_CSR_H

#include <asm/kvm_vcpu_insn.h>

#define KVM_RISCV_VCPU_TEST_CSR_FUNCS \
	{.base = CSR_VSSCRATCH,	.count = 1,	.func = kvm_riscv_vcpu_test_csr },

int kvm_riscv_vcpu_test_csr(struct kvm_vcpu *vcpu, unsigned int csr_num,
			    unsigned long *val, unsigned long new_val,
			    unsigned long wr_mask);

#endif /* !__KVM_VCPU_RISCV_TEST_CSR_H */
