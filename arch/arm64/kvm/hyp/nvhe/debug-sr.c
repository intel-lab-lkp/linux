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

static bool __debug_spe_enabled(void)
{
	/*
	 * Check if the host is actually using SPE. In pKVM read the state,
	 * otherwise just trust that the host told us it was being used.
	 */
	if (unlikely(is_protected_kvm_enabled()))
		return host_data_get_flag(HOST_FEAT_HAS_SPE) &&
		       (read_sysreg_s(SYS_PMBLIMITR_EL1) & PMBLIMITR_EL1_E);
	else
		return host_data_get_flag(HOST_STATE_SPE_EN);
}

static void __debug_save_spe(void)
{
	/* Save the control register and disable data generation */
	*host_data_ptr(host_debug_state.pmscr_el1) = read_sysreg_el1(SYS_PMSCR);
	write_sysreg_el1(0, SYS_PMSCR);
	isb();

	/* Now drain all buffered data to memory */
	psb_csync();
}

static void __debug_restore_spe(void)
{
	u64 pmscr_el1 = *host_data_ptr(host_debug_state.pmscr_el1);

	/*
	 * PMSCR was set to 0 to disable so if it's already 0, no restore is
	 * necessary.
	 */
	if (!pmscr_el1)
		return;

	/* The host page table is installed, but not yet synchronised */
	isb();

	/* Re-enable data generation */
	write_sysreg_el1(pmscr_el1, SYS_PMSCR);

	/*
	 * Disable future restores until a non zero value is saved again. Since
	 * this is called unconditionally on exit, future register writes are
	 * skipped until they are needed again.
	 */
	*host_data_ptr(host_debug_state.pmscr_el1) = 0;
}

static bool __debug_should_save_trace(void)
{
	/* pKVM reads the state for itself rather than trusting the host */
	if (unlikely(is_protected_kvm_enabled())) {
		/* Always disable any trace regardless of TRBE */
		if (read_sysreg_el1(SYS_TRFCR) &
		    (TRFCR_ELx_E0TRE | TRFCR_ELx_ExTRE))
			return true;

		/*
		 * Trace could already be disabled but TRBE buffer
		 * might still need to be drained if it was in use.
		 */
		if (host_data_get_flag(HOST_FEAT_HAS_TRBE))
			return read_sysreg_s(SYS_TRBLIMITR_EL1) &
			       TRBLIMITR_EL1_E;
	}

	return host_data_get_flag(HOST_STATE_TRBE_EN);
}

static void __debug_save_trace(void)
{
	/*
	 * Prohibit trace generation while we are in guest.
	 * Since access to TRFCR_EL1 is trapped, the guest can't
	 * modify the filtering set by the host.
	 */
	*host_data_ptr(host_debug_state.trfcr_el1) = read_sysreg_el1(SYS_TRFCR);
	write_sysreg_el1(0, SYS_TRFCR);
	isb();
	/* Drain the trace buffer to memory */
	tsb_csync();

	host_data_set_flag(HOST_STATE_RESTORE_TRFCR);
}

static void __debug_swap_trace(void)
{
	u64 trfcr = read_sysreg_el1(SYS_TRFCR);

	write_sysreg_el1(*host_data_ptr(host_debug_state.trfcr_el1), SYS_TRFCR);
	*host_data_ptr(host_debug_state.trfcr_el1) = trfcr;
	host_data_set_flag(HOST_STATE_RESTORE_TRFCR);
}

static void __debug_restore_trace(void)
{
	u64 trfcr_el1;

	if (!host_data_get_flag(HOST_STATE_RESTORE_TRFCR))
		return;

	/* Restore trace filter controls */
	trfcr_el1 = *host_data_ptr(host_debug_state.trfcr_el1);
	*host_data_ptr(host_debug_state.trfcr_el1) = read_sysreg_el1(SYS_TRFCR);
	write_sysreg_el1(trfcr_el1, SYS_TRFCR);
	host_data_clear_flag(HOST_STATE_RESTORE_TRFCR);
}

void __debug_save_host_buffers_nvhe(void)
{
	/* Disable and flush SPE data generation */
	if (__debug_spe_enabled())
		__debug_save_spe();

	/* Any trace filtering requires TRFCR register */
	if (!host_data_get_flag(HOST_FEAT_HAS_TRF))
		return;

	/*
	 * Disable and flush Self-Hosted Trace generation for pKVM and TRBE,
	 * or swap if host requires different guest filters.
	 */
	if (__debug_should_save_trace())
		__debug_save_trace();
	else if (host_data_get_flag(HOST_STATE_SWAP_TRFCR))
		__debug_swap_trace();
}

void __debug_switch_to_guest(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_guest_common(vcpu);
}

void __debug_restore_host_buffers_nvhe(struct kvm_vcpu *vcpu)
{
	__debug_restore_spe();
	__debug_restore_trace();
}

void __debug_switch_to_host(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_host_common(vcpu);
}

u64 __kvm_get_mdcr_el2(void)
{
	return read_sysreg(mdcr_el2);
}
