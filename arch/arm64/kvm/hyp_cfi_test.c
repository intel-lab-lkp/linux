// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 - Google Inc
 * Author: Pierre-Clément Tosi <ptosi@google.com>
 */
#include <asm/kvm_asm.h>
#include <asm/kvm_cfi.h>
#include <asm/kvm_host.h>
#include <asm/kvm_mmu.h>
#include <asm/virt.h>

#include <linux/export.h>
#include <linux/stddef.h>
#include <linux/types.h>

/* For calling directly into the VHE hypervisor; see <hyp/cfi.h>. */
int __kvm_register_cfi_test_cb(void (*)(void), bool);

static int kvm_register_nvhe_cfi_test_cb(void *cb, bool in_host_ctxt)
{
	extern void *kvm_nvhe_sym(hyp_test_host_ctxt_cfi);
	extern void *kvm_nvhe_sym(hyp_test_guest_ctxt_cfi);

	if (is_protected_kvm_enabled()) {
		phys_addr_t cb_phys = cb ? virt_to_phys(cb) : 0;

		/* Use HVC as only the hyp can modify its callback pointers. */
		return kvm_call_hyp_nvhe(__kvm_register_cfi_test_cb, cb_phys,
					 in_host_ctxt);
	}

	/*
	 * In non-protected nVHE, the pKVM HVC is not available but the
	 * hyp callback pointers can be accessed and modified directly.
	 */
	if (cb)
		cb = kern_hyp_va(kvm_ksym_ref(cb));

	if (in_host_ctxt)
		kvm_nvhe_sym(hyp_test_host_ctxt_cfi) = cb;
	else
		kvm_nvhe_sym(hyp_test_guest_ctxt_cfi) = cb;

	return 0;
}

static int kvm_register_cfi_test_cb(void (*vhe_cb)(void), void *nvhe_cb,
				    bool in_host_ctxt)
{
	if (!is_hyp_mode_available())
		return -ENXIO;

	if (is_hyp_nvhe())
		return kvm_register_nvhe_cfi_test_cb(nvhe_cb, in_host_ctxt);

	return __kvm_register_cfi_test_cb(vhe_cb, in_host_ctxt);
}

int kvm_cfi_test_register_host_ctxt_cb(void (*vhe_cb)(void), void *nvhe_cb)
{
	return kvm_register_cfi_test_cb(vhe_cb, nvhe_cb, true);
}
EXPORT_SYMBOL(kvm_cfi_test_register_host_ctxt_cb);

int kvm_cfi_test_register_guest_ctxt_cb(void (*vhe_cb)(void), void *nvhe_cb)
{
	return kvm_register_cfi_test_cb(vhe_cb, nvhe_cb, false);
}
EXPORT_SYMBOL(kvm_cfi_test_register_guest_ctxt_cb);

/* Hypervisor callbacks for the test module to register. */
EXPORT_SYMBOL(hyp_trigger_builtin_cfi_fault);
EXPORT_SYMBOL(kvm_nvhe_sym(hyp_trigger_builtin_cfi_fault));
EXPORT_SYMBOL(hyp_builtin_cfi_fault_target);
EXPORT_SYMBOL(kvm_nvhe_sym(hyp_builtin_cfi_fault_target));
