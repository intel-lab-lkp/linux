// SPDX-License-Identifier: GPL-2.0-only

#include <linux/acpi.h>
#include <linux/types.h>

#include <asm/acpi.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_ras.h>
#include <asm/system_misc.h>

bool kvm_vcpu_sea_far_valid(const struct kvm_vcpu *vcpu)
{
	/*
	 * FnV is valid only for Data/Instruction aborts and if DFSC/IFSC
	 * is ESR_ELx_FSC_EXTABT(0b010000).
	 */
	if (kvm_vcpu_trap_get_fault(vcpu) == ESR_ELx_FSC_EXTABT)
		return !(vcpu->arch.fault.esr_el2 & ESR_ELx_FnV);

	/* Other exception classes or aborts don't care about FnV field. */
	return true;
}

/*
 * Was this synchronous external abort a RAS notification?
 * Returns '0' for errors handled by some RAS subsystem, or -ENOENT.
 */
static int kvm_delegate_guest_sea(void)
{
	/* apei_claim_sea(NULL) expects to mask interrupts itself */
	lockdep_assert_irqs_enabled();
	return apei_claim_sea(NULL);
}

void kvm_handle_guest_sea(struct kvm_vcpu *vcpu)
{
	int idx;
	u64 vcpu_esr = kvm_vcpu_get_esr(vcpu);
	phys_addr_t fault_ipa = kvm_vcpu_get_fault_ipa(vcpu);
	gfn_t gfn = fault_ipa >> PAGE_SHIFT;
	unsigned long hva = 0UL;

	/*
	 * For RAS the host kernel may handle this abort.
	 * There is no need to SIGBUS VMM, or pass the error into the guest.
	 */
	if (kvm_delegate_guest_sea() == 0)
		return;

	if (kvm_vcpu_sea_far_valid(vcpu)) {
		idx = srcu_read_lock(&vcpu->kvm->srcu);
		hva = gfn_to_hva(vcpu->kvm, gfn);
		srcu_read_unlock(&vcpu->kvm->srcu, idx);
	}

	/*
	 * When FAR is not valid, or GFN to HVA translation failed, send 0
	 * as si_addr like what do_sea() does.
	 */
	if (kvm_is_error_hva(hva))
		hva = 0UL;

	arm64_notify_die("synchronous external abort",
			 current_pt_regs(), SIGBUS, BUS_OBJERR, hva, vcpu_esr);
}
