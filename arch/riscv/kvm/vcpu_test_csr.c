// SPDX-License-Identifier: GPL-2.0

#include <linux/kvm_host.h>
#include <asm/kvm_vcpu_insn.h>
#include <asm/kvm_vcpu_test_csr.h>

#define vcpu_to_test_csr(vcpu) (&(vcpu)->arch.test_csr)

int kvm_riscv_vcpu_test_csr(struct kvm_vcpu *vcpu, unsigned int csr_num,
			    unsigned long *val, unsigned long new_val,
			    unsigned long wr_mask)
{
	struct kvm_test_csr *test_csr = vcpu_to_test_csr(vcpu);

	*val = test_csr->val;

	if (wr_mask)
		test_csr->val = (test_csr->val & ~wr_mask) | (new_val & wr_mask);

	return KVM_INSN_CONTINUE_NEXT_SEPC;
}
