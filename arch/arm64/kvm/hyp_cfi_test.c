// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 - Google Inc
 * Author: Pierre-Clément Tosi <ptosi@google.com>
 */
#include <asm/kvm_asm.h>
#include <asm/kvm_cfi.h>
#include <asm/kvm_host.h>
#include <asm/virt.h>

#include <linux/export.h>
#include <linux/stddef.h>
#include <linux/types.h>

/* For calling directly into the VHE hypervisor; see <hyp/cfi.h>. */
int __kvm_register_cfi_test_cb(void (*)(void), bool);

static int kvm_register_cfi_test_cb(void (*vhe_cb)(void), bool in_host_ctxt)
{
	if (!is_hyp_mode_available())
		return -ENXIO;

	if (is_hyp_nvhe())
		return -EOPNOTSUPP;

	return __kvm_register_cfi_test_cb(vhe_cb, in_host_ctxt);
}

int kvm_cfi_test_register_host_ctxt_cb(void (*cb)(void))
{
	return kvm_register_cfi_test_cb(cb, true);
}
EXPORT_SYMBOL(kvm_cfi_test_register_host_ctxt_cb);

int kvm_cfi_test_register_guest_ctxt_cb(void (*cb)(void))
{
	return kvm_register_cfi_test_cb(cb, false);
}
EXPORT_SYMBOL(kvm_cfi_test_register_guest_ctxt_cb);

/* Hypervisor callbacks for the test module to register. */
EXPORT_SYMBOL(hyp_trigger_builtin_cfi_fault);
EXPORT_SYMBOL(hyp_builtin_cfi_fault_target);
