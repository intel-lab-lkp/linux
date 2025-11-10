// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/fprobe.h>
#include <linux/kprobes.h>
#include <linux/kstackwatch.h>
#include <linux/kstackwatch_types.h>
#include <linux/printk.h>

static struct kprobe entry_probe;
static struct fprobe exit_probe;

static bool probe_enable;
static u16 probe_generation;

static void ksw_reset_ctx(void)
{
	struct ksw_ctx *ctx = &current->ksw_ctx;

	if (ctx->wp)
		ksw_watch_off(ctx->wp);

	ctx->wp = NULL;
	ctx->sp = 0;
	ctx->depth = 0;
	ctx->generation = READ_ONCE(probe_generation);
}

static bool ksw_stack_check_ctx(bool entry)
{
	struct ksw_ctx *ctx = &current->ksw_ctx;
	u16 cur_enable = READ_ONCE(probe_enable);
	u16 cur_generation = READ_ONCE(probe_generation);
	u16 cur_depth, target_depth = ksw_get_config()->depth;

	if (!cur_enable) {
		ksw_reset_ctx();
		return false;
	}

	if (ctx->generation != cur_generation)
		ksw_reset_ctx();

	if (!entry && !ctx->depth) {
		ksw_reset_ctx();
		return false;
	}

	if (entry)
		cur_depth = ctx->depth++;
	else
		cur_depth = --ctx->depth;

	if (cur_depth == target_depth)
		return true;
	else
		return false;
}

static int ksw_stack_prepare_watch(struct pt_regs *regs,
				   const struct ksw_config *config,
				   ulong *watch_addr, u16 *watch_len)
{
	/* implement logic will be added in following patches */
	*watch_addr = 0;
	*watch_len = 0;
	return 0;
}

static void ksw_stack_entry_handler(struct kprobe *p, struct pt_regs *regs,
				    unsigned long flags)
{
	struct ksw_ctx *ctx = &current->ksw_ctx;
	ulong stack_pointer;
	ulong watch_addr;
	u16 watch_len;
	int ret;

	stack_pointer = kernel_stack_pointer(regs);

	/*
	 * triggered more than once, may be in a loop
	 */
	if (ctx->wp && ctx->sp == stack_pointer)
		return;

	if (!ksw_stack_check_ctx(true))
		return;

	ret = ksw_watch_get(&ctx->wp);
	if (ret)
		return;

	ret = ksw_stack_prepare_watch(regs, ksw_get_config(), &watch_addr,
				      &watch_len);
	if (ret) {
		ksw_watch_off(ctx->wp);
		ctx->wp = NULL;
		pr_err("failed to prepare watch target: %d\n", ret);
		return;
	}

	ret = ksw_watch_on(ctx->wp, watch_addr, watch_len);
	if (ret) {
		pr_err("failed to watch on depth:%d addr:0x%lx len:%u %d\n",
		       ksw_get_config()->depth, watch_addr, watch_len, ret);
		return;
	}

	ctx->sp = stack_pointer;
}

static void ksw_stack_exit_handler(struct fprobe *fp, unsigned long ip,
				   unsigned long ret_ip,
				   struct ftrace_regs *regs, void *data)
{
	struct ksw_ctx *ctx = &current->ksw_ctx;

	if (!ksw_stack_check_ctx(false))
		return;

	if (ctx->wp) {
		ksw_watch_off(ctx->wp);
		ctx->wp = NULL;
		ctx->sp = 0;
	}
}

int ksw_stack_init(void)
{
	int ret;
	char *symbuf = NULL;

	memset(&entry_probe, 0, sizeof(entry_probe));
	entry_probe.symbol_name = ksw_get_config()->func_name;
	entry_probe.offset = ksw_get_config()->func_offset;
	entry_probe.post_handler = ksw_stack_entry_handler;
	ret = register_kprobe(&entry_probe);
	if (ret) {
		pr_err("failed to register kprobe ret %d\n", ret);
		return ret;
	}

	memset(&exit_probe, 0, sizeof(exit_probe));
	exit_probe.exit_handler = ksw_stack_exit_handler;
	symbuf = (char *)ksw_get_config()->func_name;

	ret = register_fprobe_syms(&exit_probe, (const char **)&symbuf, 1);
	if (ret < 0) {
		pr_err("failed to register fprobe ret %d\n", ret);
		unregister_kprobe(&entry_probe);
		return ret;
	}

	WRITE_ONCE(probe_generation, READ_ONCE(probe_generation) + 1);
	WRITE_ONCE(probe_enable, true);

	return 0;
}

void ksw_stack_exit(void)
{
	WRITE_ONCE(probe_enable, false);
	WRITE_ONCE(probe_generation, READ_ONCE(probe_generation) + 1);
	unregister_fprobe(&exit_probe);
	unregister_kprobe(&entry_probe);
}
