// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 - Google Inc
 * Author: Pierre-Clément Tosi <ptosi@google.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <asm/kvm_asm.h>
#include <asm/kvm_cfi.h>
#include <asm/rwonce.h>

#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/printk.h>

static int set_host_mode(const char *val, const struct kernel_param *kp);
static int set_guest_mode(const char *val, const struct kernel_param *kp);

#define M_DESC \
	"\n\t0: none" \
	"\n\t1: built-in caller & built-in callee" \
	"\n\t2: built-in caller & module callee" \
	"\n\t3: module caller & built-in callee" \
	"\n\t4: module caller & module callee"

static unsigned int host_mode;
module_param_call(host, set_host_mode, param_get_uint, &host_mode, 0644);
MODULE_PARM_DESC(host,
		 "Hypervisor kCFI fault test case in host context:" M_DESC);

static unsigned int guest_mode;
module_param_call(guest, set_guest_mode, param_get_uint, &guest_mode, 0644);
MODULE_PARM_DESC(guest,
		 "Hypervisor kCFI fault test case in guest context:" M_DESC);

static void trigger_module2module_cfi_fault(void);
static void trigger_module2builtin_cfi_fault(void);
static void hyp_cfi_module2module_test_target(int);
static void hyp_cfi_builtin2module_test_target(int);

static int set_param_mode(const char *val, const struct kernel_param *kp,
			 int (*register_cb)(void (*)(void)))
{
	unsigned int *mode = kp->arg;
	int err;

	err = param_set_uint(val, kp);
	if (err)
		return err;

	switch (*mode) {
	case 0:
		return register_cb(NULL);
	case 1:
		return register_cb(hyp_trigger_builtin_cfi_fault);
	case 2:
		return register_cb((void *)hyp_cfi_builtin2module_test_target);
	case 3:
		return register_cb(trigger_module2builtin_cfi_fault);
	case 4:
		return register_cb(trigger_module2module_cfi_fault);
	default:
		return -EINVAL;
	}
}

static int set_host_mode(const char *val, const struct kernel_param *kp)
{
	return set_param_mode(val, kp, kvm_cfi_test_register_host_ctxt_cb);
}

static int set_guest_mode(const char *val, const struct kernel_param *kp)
{
	return set_param_mode(val, kp, kvm_cfi_test_register_guest_ctxt_cb);
}

static void __exit exit_hyp_cfi_test(void)
{
	int err;

	err = kvm_cfi_test_register_host_ctxt_cb(NULL);
	if (err)
		pr_err("Failed to unregister host context trigger: %d\n", err);

	err = kvm_cfi_test_register_guest_ctxt_cb(NULL);
	if (err)
		pr_err("Failed to unregister guest context trigger: %d\n", err);
}
module_exit(exit_hyp_cfi_test);

static void trigger_module2builtin_cfi_fault(void)
{
	/* Intentional UB cast & dereference, to trigger a kCFI fault. */
	void (*target)(void) = (void *)&hyp_builtin_cfi_fault_target;

	/*
	 * READ_ONCE() prevents this indirect call from being optimized out,
	 * forcing the compiler to generate the kCFI check before the branch.
	 */
	READ_ONCE(target)();

	pr_err_ratelimited("%s: Survived a kCFI violation\n", __func__);
}

static void trigger_module2module_cfi_fault(void)
{
	/* Intentional UB cast & dereference, to trigger a kCFI fault. */
	void (*target)(void) = (void *)&hyp_cfi_module2module_test_target;

	/*
	 * READ_ONCE() prevents this indirect call from being optimized out,
	 * forcing the compiler to generate the kCFI check before the branch.
	 */
	READ_ONCE(target)();

	pr_err_ratelimited("%s: Survived a kCFI violation\n", __func__);
}

/* Use different functions, for clearer symbols in kCFI panic reports. */
static noinline
void hyp_cfi_module2module_test_target(int __always_unused unused)
{
}

static noinline
void hyp_cfi_builtin2module_test_target(int __always_unused unused)
{
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pierre-Clément Tosi <ptosi@google.com>");
MODULE_DESCRIPTION("KVM hypervisor kCFI test module");
