/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 - Google Inc
 * Author: Pierre-Clément Tosi <ptosi@google.com>
 */

#ifndef __ARM64_KVM_CFI_H__
#define __ARM64_KVM_CFI_H__

#include <asm/kvm_asm.h>
#include <linux/errno.h>

#ifdef CONFIG_HYP_SUPPORTS_CFI_TEST

int kvm_cfi_test_register_host_ctxt_cb(void (*vhe_cb)(void), void *nvhe_cb);
int kvm_cfi_test_register_guest_ctxt_cb(void (*vhe_cb)(void), void *nvhe_cb);

#else

static inline int kvm_cfi_test_register_host_ctxt_cb(void (*cb)(void))
{
	return -EOPNOTSUPP;
}

static inline int kvm_cfi_test_register_guest_ctxt_cb(void (*cb)(void))
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_HYP_SUPPORTS_CFI_TEST */

/* Symbols which the host can register as hyp callbacks; see <hyp/cfi.h>. */
void hyp_trigger_builtin_cfi_fault(void);
DECLARE_KVM_NVHE_SYM(hyp_trigger_builtin_cfi_fault);
void hyp_builtin_cfi_fault_target(int unused);
DECLARE_KVM_NVHE_SYM(hyp_builtin_cfi_fault_target);

#endif /* __ARM64_KVM_CFI_H__ */
