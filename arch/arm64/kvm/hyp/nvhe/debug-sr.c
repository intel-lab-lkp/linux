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
 * Save TRFCR and disable trace completely if TRBE is being used. Return true
 * if trace was disabled.
 */
static bool __debug_save_trace(u64 *trfcr_el1)
{
	*trfcr_el1 = 0;

	/* Check if the TRBE is enabled */
	if (!(read_sysreg_s(SYS_TRBLIMITR_EL1) & TRBLIMITR_EL1_E))
		return false;
	/*
	 * Prohibit trace generation while we are in guest.
	 * Since access to TRFCR_EL1 is trapped, the guest can't
	 * modify the filtering set by the host.
	 */
	*trfcr_el1 = read_sysreg_s(SYS_TRFCR_EL1);
	write_sysreg_s(0, SYS_TRFCR_EL1);
	isb();
	/* Drain the trace buffer to memory */
	tsb_csync();

	return true;
}

static void __debug_restore_trace(u64 trfcr_el1)
{
	if (!trfcr_el1)
		return;

	/* Restore trace filter controls */
	write_sysreg_s(trfcr_el1, SYS_TRFCR_EL1);
}

#if IS_ENABLED(CONFIG_PERF_EVENTS)
static inline void __debug_save_trfcr(struct kvm_vcpu *vcpu)
{
	u64 trfcr;
	struct kvm_etm_event etm_event = vcpu->arch.host_debug_state.etm_event;

	/* No change if neither are excluded */
	if (!etm_event.exclude_guest && !etm_event.exclude_host) {
		/* Zeroing prevents restoring a stale value */
		vcpu->arch.host_debug_state.trfcr_el1 = 0;
		return;
	}

	trfcr = read_sysreg_s(SYS_TRFCR_EL1);
	vcpu->arch.host_debug_state.trfcr_el1 = trfcr;

	if (etm_event.exclude_guest) {
		trfcr &= ~(TRFCR_ELx_ExTRE | TRFCR_ELx_E0TRE);
	} else {
		/*
		 * If host was excluded then EL0 and ELx tracing bits will
		 * already be cleared so they need to be set now for the guest.
		 */
		trfcr |= etm_event.exclude_kernel ? 0 : TRFCR_ELx_ExTRE;
		trfcr |= etm_event.exclude_user ? 0 : TRFCR_ELx_E0TRE;
	}
	write_sysreg_s(trfcr, SYS_TRFCR_EL1);
}
#else
static inline void __debug_save_trfcr(struct kvm_vcpu *vcpu) {}
#endif

void __debug_save_host_buffers_nvhe(struct kvm_vcpu *vcpu)
{
	bool trc_disabled = false;

	/* Disable and flush SPE data generation */
	if (vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_SPE))
		__debug_save_spe(&vcpu->arch.host_debug_state.pmscr_el1);
	/* Disable and flush Self-Hosted Trace generation */
	if (vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_TRBE))
		trc_disabled = __debug_save_trace(&vcpu->arch.host_debug_state.trfcr_el1);

	/*
	 * As long as trace wasn't completely disabled due to use of TRBE,
	 * TRFCR can be saved and the exclude_guest rules applied.
	 */
	if (!trc_disabled && vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_TRFCR))
		__debug_save_trfcr(vcpu);
}

void __debug_switch_to_guest(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_guest_common(vcpu);
}

void __debug_restore_host_buffers_nvhe(struct kvm_vcpu *vcpu)
{
	if (vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_SPE))
		__debug_restore_spe(vcpu->arch.host_debug_state.pmscr_el1);
	if (vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_TRBE) ||
	    vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_TRFCR))
		__debug_restore_trace(vcpu->arch.host_debug_state.trfcr_el1);
}

void __debug_switch_to_host(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_host_common(vcpu);
}

u64 __kvm_get_mdcr_el2(void)
{
	return read_sysreg(mdcr_el2);
}
