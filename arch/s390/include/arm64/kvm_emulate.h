/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __S390_ARM64_KVM_EMULATE_H__
#define __S390_ARM64_KVM_EMULATE_H__

#include <asm/fault.h>
#include <asm/ptrace.h>
#include <linux/kvm_host.h>

#include <arm64/kvm_arm.h>
#include <arm64/sysreg.h>

static __always_inline unsigned long *vcpu_pc(const struct kvm_vcpu *vcpu)
{
	return (unsigned long *)&vcpu->arch.sae_block.pc;
}

static __always_inline unsigned long *vcpu_cpsr(const struct kvm_vcpu *vcpu)
{
	return (unsigned long *)&vcpu->arch.sae_block.pstate;
}

static __always_inline unsigned long *vcpu_sp_el0(const struct kvm_vcpu *vcpu)
{
	return (unsigned long *)&vcpu->arch.sae_block.sp_el0;
}

static __always_inline bool vcpu_mode_is_32bit(const struct kvm_vcpu *vcpu)
{
	return false;
}

static __always_inline u64 kvm_vcpu_get_esr(const struct kvm_vcpu *vcpu)
{
	return vcpu->arch.sae_block.hai.esr_elz;
}

static __always_inline unsigned long kvm_vcpu_get_hfar(const struct kvm_vcpu *vcpu)
{
	return vcpu->arch.sae_block.hai.far_elz;
}

static __always_inline phys_addr_t kvm_vcpu_get_fault_ipa(const struct kvm_vcpu *vcpu)
{
	return vcpu->arch.sae_block.hai.teid.addr * PAGE_SIZE;
}

static inline u16 kvm_vcpu_fault_pic(const struct kvm_vcpu *vcpu)
{
	return vcpu->arch.sae_block.hai.pic & PGM_INT_CODE_MASK;
}

static __always_inline
bool kvm_vcpu_trap_is_permission_fault(const struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_fault_pic(vcpu) == PGM_PROTECTION;
}

static __always_inline bool kvm_condition_valid(const struct kvm_vcpu *vcpu)
{
	return true;
}

static __always_inline bool vcpu_el1_is_32bit(struct kvm_vcpu *vcpu)
{
	return false;
}

static inline void vcpu_reset_hcr(struct kvm_vcpu *vcpu)
{
	vcpu->arch.hcr_elz = HCR_E2H | HCR_RW | HCR_AMO | HCR_IMO | HCR_FMO |
			     HCR_PTW;
	/* traps */
	vcpu->arch.hcr_elz |= HCR_TSC | HCR_TID1 | HCR_TID2 | HCR_TID3 |
			      HCR_TID4 | HCR_TID5 | HCR_TIDCP;
}

static inline unsigned long vcpu_get_vsesr(struct kvm_vcpu *vcpu)
{
	WARN(true, "not implemented, just feat RAS");

	return 0L;
}

static inline void vcpu_set_vsesr(struct kvm_vcpu *vcpu, u64 vsesr)
{
	WARN(true, "not implemented, just feat RAS");
}

static inline bool vcpu_el2_tge_is_set(const struct kvm_vcpu *vcpu)
{
	return false;
}

static inline bool kvm_vcpu_is_be(struct kvm_vcpu *vcpu)
{
	return false;
}

static inline int kvm_vcpu_abt_gltl(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.sae_block.hai.gltl;
}

static inline bool is_hyp_ctxt(const struct kvm_vcpu *vcpu)
{
	return false;
}

static inline bool is_nested_ctxt(struct kvm_vcpu *vcpu)
{
	return false;
}

static inline bool vcpu_mode_priv(const struct kvm_vcpu *vcpu)
{
	u32 mode = *vcpu_cpsr(vcpu) & PSR_MODE_MASK;

	return mode != PSR_MODE_EL0t;
}

#define SPSR_SS BIT(21)

static inline void kvm_skip_instr(struct kvm_vcpu *vcpu)
{
	*vcpu_pc(vcpu) += 4;
	*vcpu_cpsr(vcpu) &= ~PSR_BTYPE_MASK;

	/* advance the singlestep state machine */
	*vcpu_cpsr(vcpu) &= ~SPSR_SS;
}

#include <arm64/kvm_emulate-part.h>

#endif /* __S390_ARM64_KVM_EMULATE_H__ */
