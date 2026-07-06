// SPDX-License-Identifier: GPL-2.0
/*
 * kprobe_folio_stress.c - kprobe stress test for page fault handling
 *
 * This module installs a kprobe on folio_wait_bit_common() and is
 * intended to be used together with fault_stress.c to stress kprobe
 * handling around fault paths.
 *
 * Primary use case: reproduce arm64 kprobe stability issues related to
 * XOL slot execution and fault handling during single-stepping.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <linux/kallsyms.h>


static atomic64_t kp_hit_count = ATOMIC64_INIT(0);

static int folio_wait_bit_common_handler(
			struct kprobe *p, struct pt_regs *regs)
{
	unsigned long hit;

	hit = atomic64_inc_return(&kp_hit_count);

	pr_info("kp_folio: hit=%lu comm=%s tgid=%d tid=%d\n",
			    hit, current->comm, current->tgid, current->pid);

	return 0;
}

static struct kprobe kp_folio_probe = {
	.symbol_name = "folio_wait_bit_common",
	.pre_handler = folio_wait_bit_common_handler,
};

static int __init kprobe_folio_init(void)
{
	int ret;

	ret = register_kprobe(&kp_folio_probe);
	if (ret < 0) {
		pr_err("kp_folio: register_kprobe failed, ret=%d\n", ret);
		return ret;
	}

	pr_info("kp_folio: kprobe registered at %pS, addr=%px\n",
		kp_folio_probe.addr, kp_folio_probe.addr);

	return 0;
}

static void __exit kprobe_folio_exit(void)
{
	unregister_kprobe(&kp_folio_probe);
	pr_info("kp_folio: kprobe unregistered, total hits=%lld\n",
		atomic64_read(&kp_hit_count));
}

module_init(kprobe_folio_init);
module_exit(kprobe_folio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pu Hu <hupu@transsion.com>");
MODULE_DESCRIPTION("kprobe stress test for folio_wait_bit_common");
