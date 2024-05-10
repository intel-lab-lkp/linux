/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 - Google Inc
 * Author: Pierre-Clément Tosi <ptosi@google.com>
 */

#ifndef __ARM64_KVM_HYP_CFI_H__
#define __ARM64_KVM_HYP_CFI_H__

#include <asm/bug.h>
#include <asm/errno.h>

#include <linux/compiler.h>

#ifdef CONFIG_HYP_SUPPORTS_CFI_TEST

int __kvm_register_cfi_test_cb(void (*cb)(void), bool in_host_ctxt);

extern void (*hyp_test_host_ctxt_cfi)(void);
extern void (*hyp_test_guest_ctxt_cfi)(void);

/* Hypervisor callbacks for the host to register. */
void hyp_trigger_builtin_cfi_fault(void);
void hyp_builtin_cfi_fault_target(int unused);

#else

static inline
int __kvm_register_cfi_test_cb(void (*cb)(void), bool in_host_ctxt)
{
	return -EOPNOTSUPP;
}

#define hyp_test_host_ctxt_cfi ((void(*)(void))(NULL))
#define hyp_test_guest_ctxt_cfi ((void(*)(void))(NULL))

static inline void hyp_trigger_builtin_cfi_fault(void)
{
}

static inline void hyp_builtin_cfi_fault_target(int __always_unused unused)
{
}

#endif /* CONFIG_HYP_SUPPORTS_CFI_TEST */

#endif /* __ARM64_KVM_HYP_CFI_H__ */
