// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/kvm_host.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_ras.h>
#include <asm/system_misc.h>

/*
 * For synchrnous external instruction or data abort, not on translation
 * table walk or hardware update of translation table, is FAR_EL2 valid?
 */
static inline bool kvm_vcpu_sea_far_valid(const struct kvm_vcpu *vcpu)
{
	return !(vcpu->arch.fault.esr_el2 & ESR_ELx_FnV);
}

/*
 * Was this synchronous external abort a RAS notification?
 * Returns '0' for errors handled by some RAS subsystem, or -ENOENT.
 */
static int kvm_delegate_guest_sea(phys_addr_t addr, u64 esr)
{
	/* apei_claim_sea(NULL) expects to mask interrupts itself */
	lockdep_assert_irqs_enabled();
	return apei_claim_sea(NULL);
}

void kvm_handle_guest_sea(struct kvm_vcpu *vcpu)
{
	bool sigbus_on_sea;
	int idx;
	u64 vcpu_esr = kvm_vcpu_get_esr(vcpu);
	u8 fsc = kvm_vcpu_trap_get_fault(vcpu);
	phys_addr_t fault_ipa = kvm_vcpu_get_fault_ipa(vcpu);
	gfn_t gfn = fault_ipa >> PAGE_SHIFT;
	/* When FnV is set, send 0 as si_addr like what do_sea() does. */
	unsigned long hva = 0UL;

	/*
	 * For RAS the host kernel may handle this abort.
	 * There is no need to SIGBUS VMM, or pass the error into the guest.
	 */
	if (kvm_delegate_guest_sea(fault_ipa, vcpu_esr) == 0)
		return;

	sigbus_on_sea = test_bit(KVM_ARCH_FLAG_SIGBUS_ON_SEA,
				 &(vcpu->kvm->arch.flags));

	/*
	 * In addition to userspace opt-in, SIGBUS only makes sense if the
	 * abort is NOT about translation table walk and NOT about hardware
	 * update of translation table.
	 */
	sigbus_on_sea &= (fsc == ESR_ELx_FSC_EXTABT || fsc == ESR_ELx_FSC_SECC);

	/* Pass the error directly into the guest. */
	if (!sigbus_on_sea) {
		kvm_inject_vabt(vcpu);
		return;
	}

	if (kvm_vcpu_sea_far_valid(vcpu)) {
		idx = srcu_read_lock(&vcpu->kvm->srcu);
		hva = gfn_to_hva(vcpu->kvm, gfn);
		srcu_read_unlock(&vcpu->kvm->srcu, idx);
	}

	/*
	 * Send a SIGBUS BUS_OBJERR to vCPU thread (the userspace thread that
	 * runs KVM_RUN) or VMM, which aligns with what host kernel do_sea()
	 * does if apei_claim_sea() fails.
	 */
	arm64_notify_die("synchronous external abort",
			 current_pt_regs(), SIGBUS, BUS_OBJERR, hva, vcpu_esr);
}
