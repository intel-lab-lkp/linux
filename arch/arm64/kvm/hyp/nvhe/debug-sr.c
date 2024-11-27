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
	*pmscr_el1 = read_sysreg_el1(SYS_PMSCR);
	write_sysreg_el1(0, SYS_PMSCR);
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
	write_sysreg_el1(pmscr_el1, SYS_PMSCR);
}

static void __trace_do_switch(u64 *saved_trfcr, u64 new_trfcr)
{
	*saved_trfcr = read_sysreg_el1(SYS_TRFCR);
	write_sysreg_el1(new_trfcr, SYS_TRFCR);

	/* No need to drain if going to an enabled state or from disabled state */
	if (new_trfcr || !*saved_trfcr)
		return;

	isb();
	tsb_csync();
}

static bool __trace_needs_switch(void)
{
	return host_data_test_flag(TRBE_ENABLED) ||
	       (is_protected_kvm_enabled() && host_data_test_flag(HAS_TRF));
}

static void __trace_switch_to_guest(void)
{
	/* Unsupported with TRBE so disable */
	if (host_data_test_flag(TRBE_ENABLED))
		*host_data_ptr(guest_trfcr_el1) = 0;

	__trace_do_switch(host_data_ptr(host_debug_state.trfcr_el1),
			  *host_data_ptr(guest_trfcr_el1));
}

static void __trace_switch_to_host(void)
{
	__trace_do_switch(host_data_ptr(guest_trfcr_el1),
			  *host_data_ptr(host_debug_state.trfcr_el1));
}

void __debug_save_host_buffers_nvhe(struct kvm_vcpu *vcpu)
{
	/* Disable and flush SPE data generation */
	if (host_data_test_flag(HAS_SPE))
		__debug_save_spe(host_data_ptr(host_debug_state.pmscr_el1));

	if (__trace_needs_switch())
		__trace_switch_to_guest();
}

void __debug_switch_to_guest(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_guest_common(vcpu);
}

void __debug_restore_host_buffers_nvhe(struct kvm_vcpu *vcpu)
{
	if (host_data_test_flag(HAS_SPE))
		__debug_restore_spe(*host_data_ptr(host_debug_state.pmscr_el1));
	if (__trace_needs_switch())
		__trace_switch_to_host();
}

void __debug_switch_to_host(struct kvm_vcpu *vcpu)
{
	__debug_switch_to_host_common(vcpu);
}
