// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 - Google Inc
 * Author: Pierre-Clément Tosi <ptosi@google.com>
 */
#include <asm/rwonce.h>

#include <hyp/cfi.h>

void (*hyp_test_host_ctxt_cfi)(void);
void (*hyp_test_guest_ctxt_cfi)(void);

int __kvm_register_cfi_test_cb(void (*cb)(void), bool in_host_ctxt)
{
	if (in_host_ctxt)
		hyp_test_host_ctxt_cfi = cb;
	else
		hyp_test_guest_ctxt_cfi = cb;

	return 0;
}

void hyp_builtin_cfi_fault_target(int __always_unused unused)
{
}

void hyp_trigger_builtin_cfi_fault(void)
{
	/* Intentional UB cast & dereference, to trigger a kCFI fault. */
	void (*target)(void) = (void *)&hyp_builtin_cfi_fault_target;

	/*
	 * READ_ONCE() prevents this indirect call from being optimized out,
	 * forcing the compiler to generate the kCFI check before the branch.
	 */
	READ_ONCE(target)();
}
