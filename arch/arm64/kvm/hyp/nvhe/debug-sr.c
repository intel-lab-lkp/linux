// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#include <hyp/debug-sr.h>

#include <linux/compiler.h>
#include <linux/kvm_host.h>

#include <asm/debug-monitors.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>

static void __debug_save_spe(u64 *pmscr_el1)
{
	u64 reg;

	/* Clear pmscr in case of early return */
	*pmscr_el1 = 0;

	/*
	 * At this point, we know that this CPU implements
	 * SPE and is available to the host.
	 * Check if the host is actually using it ?
	 */
	reg = read_sysreg_s(SYS_PMBLIMITR_EL1);
	if (!(reg & BIT(PMBLIMITR_EL1_E_SHIFT)))
		return;

	/* Yes; save the control register and disable data generation */
	*pmscr_el1 = read_sysreg_s(SYS_PMSCR_EL1);
	write_sysreg_s(0, SYS_PMSCR_EL1);
	isb();

	/* Now drain all buffered data to memory */
	psb_csync();
}

static void __debug_restore_spe(u64 pmscr_el1)
{
	if (!pmscr_el1)
		return;

	/* The host page table is installed, but not yet synchronised */
	isb();

	/* Re-enable data generation */
	write_sysreg_s(pmscr_el1, SYS_PMSCR_EL1);
}

/*
 * Save TRFCR and disable trace completely if TRBE is being used, otherwise
 * apply required guest TRFCR value.
 */
static void __debug_save_trace(struct kvm_vcpu *vcpu)
{
	vcpu->arch.host_debug_state.host_trfcr_el1 = read_sysreg_s(SYS_TRFCR_EL1);

	/* Check if the TRBE is enabled */
	if (read_sysreg_s(SYS_TRBLIMITR_EL1) & TRBLIMITR_EL1_E) {
		/*
		 * Prohibit trace generation while we are in guest. Since access
		 * to TRFCR_EL1 is trapped, the guest can't modify the filtering
		 * set by the host.
		 */
		vcpu->arch.host_debug_state.guest_trfcr_el1 = 0;
		write_sysreg_s(0, SYS_TRFCR_EL1);
		isb();
		/* Drain the trace buffer to memory */
		tsb_csync();
	} else {
		/*
		 * Not using TRBE, so guest trace works. Apply the guest filters
		 * provided by the Coresight driver, if different.
		 */
		if (vcpu->arch.host_debug_state.guest_trfcr_el1 !=
		    vcpu->arch.host_debug_state.host_trfcr_el1)
			write_sysreg_s(vcpu->arch.host_debug_state.guest_trfcr_el1,
				       SYS_TRFCR_EL1);
	}
}

static void __debug_restore_trace(struct kvm_vcpu *vcpu)
{
	/* Restore trace filter controls */
	if (vcpu->arch.host_debug_state.host_trfcr_el1 !=
	    vcpu->arch.host_debug_state.guest_trfcr_el1)
		write_sysreg_s(vcpu->arch.host_debug_state.host_trfcr_el1, SYS_TRFCR_EL1);
}

void __debug_save_host_buffers_nvhe(struct kvm_vcpu *vcpu)
{
	/* Disable and flush SPE data generation */
	if (vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_SPE))
		__debug_save_spe(&vcpu->arch.host_debug_state.pmscr_el1);

	if (vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_TRFCR))
		__debug_save_trace(vcpu);
}

void __debug_switch_to_guest(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_guest_common(vcpu);
}

void __debug_restore_host_buffers_nvhe(struct kvm_vcpu *vcpu)
{
	if (vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_SPE))
		__debug_restore_spe(vcpu->arch.host_debug_state.pmscr_el1);
	if (vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_TRFCR))
		__debug_restore_trace(vcpu);
}

void __debug_switch_to_host(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_host_common(vcpu);
}

u64 __kvm_get_mdcr_el2(void)
{
	return read_sysreg(mdcr_el2);
}
