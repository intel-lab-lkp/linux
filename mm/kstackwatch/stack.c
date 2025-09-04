// SPDX-License-Identifier: GPL-2.0

#include <linux/fprobe.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#include "kstackwatch.h"

struct ksw_config *probe_config;

/* prepare watch_addr and watch_len for watch */
static int ksw_stack_prepare_watch(struct pt_regs *regs,
				   struct ksw_config *config, u64 *watch_addr,
				   u64 *watch_len)
{
	/* TODO: implement logic */
	*watch_addr = 0;
	*watch_len = 0;
	return 0;
}

static struct kprobe entry_probe;
static struct fprobe exit_probe_fprobe;

static void ksw_stack_entry_handler(struct kprobe *p, struct pt_regs *regs,
				    unsigned long flags)
{
	int ret;
	u64 watch_addr;
	u64 watch_len;

	ret = ksw_stack_prepare_watch(regs, probe_config, &watch_addr,
				      &watch_len);
	if (ret) {
		pr_err("KSW: failed to prepare watch target: %d\n", ret);
		return;
	}

	ret = ksw_watch_on(watch_addr, watch_len);
	if (ret) {
		pr_err("KSW: failed to watch on addr:0x%llx len:%llx %d\n",
		       watch_addr, watch_len, ret);
		return;
	}
}

static void ksw_stack_exit_handler(struct fprobe *fp, unsigned long ip,
				   unsigned long ret_ip,
				   struct ftrace_regs *regs, void *data)
{
	ksw_watch_off();
}

int ksw_stack_init(struct ksw_config *config)
{
	int ret;
	char *symbuf = NULL;

	/* Setup entry probe */
	memset(&entry_probe, 0, sizeof(entry_probe));
	entry_probe.symbol_name = config->function;
	entry_probe.offset = config->ip_offset;
	entry_probe.post_handler = ksw_stack_entry_handler;
	probe_config = config;
	ret = register_kprobe(&entry_probe);
	if (ret < 0) {
		pr_err("KSW: Failed to register kprobe ret %d\n", ret);
		return ret;
	}

	/* Setup exit probe */
	memset(&exit_probe_fprobe, 0, sizeof(exit_probe_fprobe));
	exit_probe_fprobe.exit_handler = ksw_stack_exit_handler;
	symbuf = probe_config->function;

	ret = register_fprobe_syms(&exit_probe_fprobe, (const char **)&symbuf,
				   1);
	if (ret < 0) {
		pr_err("KSW: register_fprobe_syms fail %d\n", ret);
		unregister_kprobe(&entry_probe);
		return ret;
	}

	return 0;
}

void ksw_stack_exit(void)
{
	unregister_fprobe(&exit_probe_fprobe);
	unregister_kprobe(&entry_probe);
}
