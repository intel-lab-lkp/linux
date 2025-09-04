// SPDX-License-Identifier: GPL-2.0

#include <linux/fprobe.h>
#include <linux/interrupt.h>
#include <linux/kprobes.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stackprotector.h>

#include "kstackwatch.h"

struct ksw_config *probe_config;

/* Find canary address in current stack frame */
static unsigned long ksw_stack_find_canary(struct pt_regs *regs)
{
	unsigned long *stack_ptr, *stack_end;
	unsigned long expected_canary;
	unsigned int i;

	stack_ptr = (unsigned long *)kernel_stack_pointer(regs);
	stack_end =
		(unsigned long *)current->stack + THREAD_SIZE / sizeof(long);
	expected_canary = current->stack_canary;

	for (i = 0; i < MAX_FRAME_SEARCH && &stack_ptr[i] < stack_end; i++) {
		if (stack_ptr[i] == expected_canary) {
			pr_info("KSW: canary found i:%d 0x%px\n", i,
				&stack_ptr[i]);
			return (unsigned long)&stack_ptr[i];
		}
	}

	return 0;
}

/* Resolve stack offset to actual address */
static unsigned long ksw_stack_resolve_offset(struct pt_regs *regs,
					      s64 local_var_offset)
{
	unsigned long stack_base;
	unsigned long target_addr;

	if (!regs)
		return 0;

	/* Use stack pointer as base for offset calculation */
	stack_base = kernel_stack_pointer(regs);
	target_addr = stack_base + local_var_offset;

	pr_debug("KSW: stack resolve offset target: 0x%lx\n", target_addr);

	return target_addr;
}

/* Validate that address is within current stack bounds */
static int ksw_stack_validate_addr(unsigned long addr, size_t size)
{
	unsigned long stack_start, stack_end;

	if (!addr || !size)
		return -EINVAL;

	stack_start = (unsigned long)current->stack;
	stack_end = stack_start + THREAD_SIZE;

	if (addr < stack_start || (addr + size) > stack_end) {
		pr_warn("KSW: address 0x%lx (size %zu) outside stack bounds [0x%lx-0x%lx]\n",
			addr, size, stack_start, stack_end);
		return -ERANGE;
	}

	return 0;
}

/* prepare watch_addr and watch_len for watch */
static int ksw_stack_prepare_watch(struct pt_regs *regs,
				   struct ksw_config *config, u64 *watch_addr,
				   u64 *watch_len)
{
	u64 addr;
	u64 len;

	/* Resolve addresses for all active watches */
	switch (config->type) {
	case WATCH_CANARY:
		addr = ksw_stack_find_canary(regs);
		len = 8;
		break;

	case WATCH_LOCAL_VAR:
		addr = ksw_stack_resolve_offset(regs, config->local_var_offset);
		if (!addr) {
			pr_err("KSW: invalid stack var offset %u\n",
			       config->local_var_offset);
			return -EINVAL;
		}
		if (ksw_stack_validate_addr(addr, config->local_var_len)) {
			pr_err("KSW: invalid stack var len %u\n",
			       config->local_var_len);
		}
		len = config->local_var_len;
		break;

	default:
		pr_warn("KSW: Unknown watch type %d\n", config->type);
		return -EINVAL;
	}

	*watch_addr = addr;
	*watch_len = len;
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
