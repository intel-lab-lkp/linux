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

static void __debug_save_trace(u64 *trfcr_el1)
{
	*trfcr_el1 = 0;

	/* Check if the TRBE is enabled */
	if (!(read_sysreg_s(SYS_TRBLIMITR_EL1) & TRBLIMITR_EL1_E))
		return;
	/*
	 * Prohibit trace generation while we are in guest.
	 * Since access to TRFCR_EL1 is trapped, the guest can't
	 * modify the filtering set by the host.
	 */
	*trfcr_el1 = read_sysreg_el1(SYS_TRFCR);
	write_sysreg_el1(0, SYS_TRFCR);
	isb();
	/* Drain the trace buffer to memory */
	tsb_csync();
}

static void __debug_restore_trace(u64 trfcr_el1)
{
	if (!trfcr_el1)
		return;

	/* Restore trace filter controls */
	write_sysreg_el1(trfcr_el1, SYS_TRFCR);
}

void __debug_save_host_buffers_nvhe(void)
{
	/* Disable and flush SPE data generation */
	if (__debug_spe_enabled())
		__debug_save_spe();

	/* Disable and flush Self-Hosted Trace generation */
	if (host_data_get_flag(HOST_FEAT_HAS_TRBE))
		__debug_save_trace(host_data_ptr(host_debug_state.trfcr_el1));
}

void __debug_switch_to_guest(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_guest_common(vcpu);
}

void __debug_restore_host_buffers_nvhe(struct kvm_vcpu *vcpu)
{
	__debug_restore_spe();
	if (host_data_get_flag(HOST_FEAT_HAS_TRBE))
		__debug_restore_trace(*host_data_ptr(host_debug_state.trfcr_el1));
}

void __debug_switch_to_host(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_host_common(vcpu);
}

u64 __kvm_get_mdcr_el2(void)
{
	return read_sysreg(mdcr_el2);
}
