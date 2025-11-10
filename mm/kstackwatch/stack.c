// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/fprobe.h>
#include <linux/kprobes.h>
#include <linux/kstackwatch.h>
#include <linux/kstackwatch_types.h>
#include <linux/ktime.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/timex.h>

#define MAX_CANARY_SEARCH_STEPS 128
static struct kprobe entry_probe;
static struct fprobe exit_probe;

static bool probe_enable;
static u16 probe_generation;

#ifdef CONFIG_KSTACKWATCH_PROFILING
struct measure_data {
	u64 total_entry_with_watch_ns;
	u64 total_entry_with_watch_cycles;
	u64 total_entry_without_watch_ns;
	u64 total_entry_without_watch_cycles;
	u64 total_exit_with_watch_ns;
	u64 total_exit_with_watch_cycles;
	u64 total_exit_without_watch_ns;
	u64 total_exit_without_watch_cycles;
	u64 entry_with_watch_count;
	u64 entry_without_watch_count;
	u64 exit_with_watch_count;
	u64 exit_without_watch_count;
};

static DEFINE_PER_CPU(struct measure_data, measure_stats);

struct measure_ctx {
	u64 ns_start;
	u64 cycles_start;
};

static __always_inline void measure_start(struct measure_ctx *ctx)
{
	ctx->ns_start = ktime_get_ns();
	ctx->cycles_start = get_cycles();
}

static __always_inline void measure_end(struct measure_ctx *ctx, u64 *total_ns,
					u64 *total_cycles, u64 *count)
{
	u64 ns_end = ktime_get_ns();
	u64 c_end = get_cycles();

	*total_ns += ns_end - ctx->ns_start;
	*total_cycles += c_end - ctx->cycles_start;
	(*count)++;
}

static void show_measure_stats(void)
{
	int cpu;
	struct measure_data sum = {};

	for_each_possible_cpu(cpu) {
		struct measure_data *md = per_cpu_ptr(&measure_stats, cpu);

		sum.total_entry_with_watch_ns += md->total_entry_with_watch_ns;
		sum.total_entry_with_watch_cycles +=
			md->total_entry_with_watch_cycles;
		sum.total_entry_without_watch_ns +=
			md->total_entry_without_watch_ns;
		sum.total_entry_without_watch_cycles +=
			md->total_entry_without_watch_cycles;

		sum.total_exit_with_watch_ns += md->total_exit_with_watch_ns;
		sum.total_exit_with_watch_cycles +=
			md->total_exit_with_watch_cycles;
		sum.total_exit_without_watch_ns +=
			md->total_exit_without_watch_ns;
		sum.total_exit_without_watch_cycles +=
			md->total_exit_without_watch_cycles;

		sum.entry_with_watch_count += md->entry_with_watch_count;
		sum.entry_without_watch_count += md->entry_without_watch_count;
		sum.exit_with_watch_count += md->exit_with_watch_count;
		sum.exit_without_watch_count += md->exit_without_watch_count;
	}

#define AVG(ns, cnt) ((cnt) ? ((ns) / (cnt)) : 0ULL)

	pr_info("entry (with watch):    %llu ns, %llu cycles (%llu samples)\n",
		AVG(sum.total_entry_with_watch_ns, sum.entry_with_watch_count),
		AVG(sum.total_entry_with_watch_cycles,
		    sum.entry_with_watch_count),
		sum.entry_with_watch_count);

	pr_info("entry (without watch): %llu ns, %llu cycles (%llu samples)\n",
		AVG(sum.total_entry_without_watch_ns,
		    sum.entry_without_watch_count),
		AVG(sum.total_entry_without_watch_cycles,
		    sum.entry_without_watch_count),
		sum.entry_without_watch_count);

	pr_info("exit (with watch):     %llu ns, %llu cycles (%llu samples)\n",
		AVG(sum.total_exit_with_watch_ns, sum.exit_with_watch_count),
		AVG(sum.total_exit_with_watch_cycles,
		    sum.exit_with_watch_count),
		sum.exit_with_watch_count);

	pr_info("exit (without watch):  %llu ns, %llu cycles (%llu samples)\n",
		AVG(sum.total_exit_without_watch_ns,
		    sum.exit_without_watch_count),
		AVG(sum.total_exit_without_watch_cycles,
		    sum.exit_without_watch_count),
		sum.exit_without_watch_count);
}

static void reset_measure_stats(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct measure_data *md = per_cpu_ptr(&measure_stats, cpu);

		memset(md, 0, sizeof(*md));
	}

	pr_info("measure stats reset.\n");
}

#endif

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

static unsigned long ksw_find_stack_canary_addr(struct pt_regs *regs)
{
	unsigned long *stack_ptr, *stack_end, *stack_base;
	unsigned long expected_canary;
	unsigned int i;
#ifdef CONFIG_FRAME_POINTER
	unsigned long *fp = NULL;
#endif

	stack_ptr = (unsigned long *)kernel_stack_pointer(regs);
	stack_base = (unsigned long *)(current->stack);

	stack_end = (unsigned long *)((char *)current->stack + THREAD_SIZE);
#ifdef CONFIG_FRAME_POINTER
	/*
	 * Use the compiler-provided frame pointer.
	 * Limit the search to the current frame
	 * Works on any arch that keeps FP when CONFIG_FRAME_POINTER=y.
	 */
	fp = __builtin_frame_address(0);

	if (fp > stack_ptr && fp < stack_end)
		stack_end = fp;
#endif

#ifdef CONFIG_STACKPROTECTOR
	expected_canary = current->stack_canary;
#else
	pr_err("no canary without CONFIG_STACKPROTECTOR\n");
	return 0;
#endif

	if (stack_ptr < stack_base || stack_ptr >= stack_end) {
		pr_err("Stack pointer 0x%lx out of bounds [0x%lx, 0x%lx)\n",
		       (unsigned long)stack_ptr, (unsigned long)stack_base,
		       (unsigned long)stack_end);
		return 0;
	}

	for (i = 0; i < MAX_CANARY_SEARCH_STEPS; i++) {
		if (&stack_ptr[i] >= stack_end)
			break;

		if (stack_ptr[i] == expected_canary)
			return (unsigned long)&stack_ptr[i];
	}

	pr_err("canary not found in first %d steps\n", MAX_CANARY_SEARCH_STEPS);
	return 0;
}

static int ksw_stack_validate_addr(unsigned long addr, size_t size)
{
	unsigned long stack_start, stack_end;

	if (!addr || !size)
		return -EINVAL;

	stack_start = (unsigned long)current->stack;
	stack_end = stack_start + THREAD_SIZE;

	if (addr < stack_start || (addr + size) > stack_end)
		return -ERANGE;

	return 0;
}

static int ksw_stack_prepare_watch(struct pt_regs *regs,
				   const struct ksw_config *config,
				   ulong *watch_addr, u16 *watch_len)
{
	ulong addr;
	u16 len;

	if (ksw_get_config()->auto_canary) {
		addr = ksw_find_stack_canary_addr(regs);
		if (!addr)
			return -EINVAL;
		len = sizeof(ulong);
	} else {
		addr = kernel_stack_pointer(regs) + ksw_get_config()->sp_offset;
		len = ksw_get_config()->watch_len;
		if (!len)
			len = sizeof(ulong);
	}

	if (ksw_stack_validate_addr(addr, len)) {
		pr_err("invalid stack addr:0x%lx len :%u\n", addr, len);
		return -EINVAL;
	}

	*watch_addr = addr;
	*watch_len = len;
	return 0;
}

static void ksw_stack_entry_handler(struct kprobe *p, struct pt_regs *regs,
				    unsigned long flags)
{
	struct ksw_ctx *ctx = &current->ksw_ctx;
	ulong stack_pointer, watch_addr;
	u16 watch_len;
	int ret;
#ifdef CONFIG_KSTACKWATCH_PROFILING
	struct measure_ctx m;
	struct measure_data *md = this_cpu_ptr(&measure_stats);
	bool watched = false;

	measure_start(&m);
#endif

	stack_pointer = kernel_stack_pointer(regs);

	if (ctx->wp && ctx->sp == stack_pointer)
		goto out;

	if (!ksw_stack_check_ctx(true))
		goto out;

	ret = ksw_watch_get(&ctx->wp);
	if (ret)
		goto out;

	ret = ksw_stack_prepare_watch(regs, ksw_get_config(), &watch_addr,
				      &watch_len);
	if (ret) {
		ksw_watch_off(ctx->wp);
		ctx->wp = NULL;
		pr_err("failed to prepare watch target: %d\n", ret);
		goto out;
	}

	ret = ksw_watch_on(ctx->wp, watch_addr, watch_len);
	if (ret) {
		pr_err("failed to watch on depth:%d addr:0x%lx len:%u %d\n",
		       ksw_get_config()->depth, watch_addr, watch_len, ret);
		goto out;
	}

	ctx->sp = stack_pointer;
#ifdef CONFIG_KSTACKWATCH_PROFILING
	watched = true;
#endif

out:
#ifdef CONFIG_KSTACKWATCH_PROFILING
	if (watched)
		measure_end(&m, &md->total_entry_with_watch_ns,
			    &md->total_entry_with_watch_cycles,
			    &md->entry_with_watch_count);
	else
		measure_end(&m, &md->total_entry_without_watch_ns,
			    &md->total_entry_without_watch_cycles,
			    &md->entry_without_watch_count);
#endif
}

static void ksw_stack_exit_handler(struct fprobe *fp, unsigned long ip,
				   unsigned long ret_ip,
				   struct ftrace_regs *regs, void *data)
{
	struct ksw_ctx *ctx = &current->ksw_ctx;
#ifdef CONFIG_KSTACKWATCH_PROFILING
	struct measure_ctx m;
	struct measure_data *md = this_cpu_ptr(&measure_stats);
	bool watched = false;

	measure_start(&m);
#endif
	if (!ksw_stack_check_ctx(false))
		goto out;

	if (ctx->wp) {
		ksw_watch_off(ctx->wp);
		ctx->wp = NULL;
		ctx->sp = 0;
#ifdef CONFIG_KSTACKWATCH_PROFILING
		watched = true;
#endif
	}

out:
#ifdef CONFIG_KSTACKWATCH_PROFILING
	if (watched)
		measure_end(&m, &md->total_exit_with_watch_ns,
			    &md->total_exit_with_watch_cycles,
			    &md->exit_with_watch_count);
	else
		measure_end(&m, &md->total_exit_without_watch_ns,
			    &md->total_exit_without_watch_cycles,
			    &md->exit_without_watch_count);
#endif
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
#ifdef CONFIG_KSTACKWATCH_PROFILING
	reset_measure_stats();
#endif
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
#ifdef CONFIG_KSTACKWATCH_PROFILING
	show_measure_stats();
#endif
}
