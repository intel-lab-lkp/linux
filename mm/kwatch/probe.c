// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/sched.h>

#include "kwatch.h"
#define TRAMPOLINE_CHECK_DEPTH 16
static DEFINE_PER_CPU(bool, kwatch_probe_cpu_muted);

struct kwatch_probe_ctx {
	struct kprobe kp;
	struct kretprobe rp;
	struct kprobe pin_kp;
	const struct kwatch_config *cfg;
	bool rp_via_int3;

	u32 epoch;
};

static struct kwatch_probe_ctx kwatch_probe_ctx;
static atomic_long_t kwatch_nmi_rejected;

unsigned long kwatch_probe_nmi_rejected(void)
{
	return atomic_long_read(&kwatch_nmi_rejected);
}

/*
 * True if the probed function itself runs in an NMI-like context.
 * int3-based kprobe delivery adds one NMI-like layer of its own;
 * delivery is pinned at registration so the subtraction stays exact.
 */
static bool kwatch_probed_ctx_in_nmi(bool via_int3)
{
	return (preempt_count() & NMI_MASK) > (via_int3 ? NMI_OFFSET : 0);
}

static void kwatch_pin_post_handler(struct kprobe *p, struct pt_regs *regs,
				    unsigned long flags)
{
	/* a post_handler pins the probepoint: no jump optimization */
}

bool kwatch_probe_validate_hit(struct pt_regs *regs,
			       struct task_struct *arm_tsk)
{
	struct kwatch_tsk_ctx *ctx = kwatch_tsk_ctx_get(false);
	const struct kwatch_config *cfg = kwatch_probe_ctx.cfg;

	if (unlikely(!ctx || !cfg))
		return true;

	if (arm_tsk != current || ctx->depth != cfg->depth + 1)
		return true;

	return false;
}

void kwatch_probe_mute(bool mute)
{
	__this_cpu_write(kwatch_probe_cpu_muted, mute);
}

static inline bool kwatch_probe_is_muted(void)
{
	return __this_cpu_read(kwatch_probe_cpu_muted);
}

enum kwatch_probe_position {
	KWATCH_PROBE_POSITION_ENTRY,
	KWATCH_PROBE_POSITION_ACTIVE,
	KWATCH_PROBE_POSITION_EXIT
};

static bool kwatch_tsk_ctx_check(enum kwatch_probe_position pos)
{
	struct kwatch_tsk_ctx *ctx = kwatch_tsk_ctx_get(true);
	u32 epoch;

	if (unlikely(!ctx))
		return false;

	/* Pairs with smp_store_release() in kwatch_probe_start/stop() */
	epoch = smp_load_acquire(&kwatch_probe_ctx.epoch);

	if (unlikely(ctx->epoch != epoch))
		kwatch_tsk_ctx_reset(ctx, epoch);

	if (unlikely(!epoch)) {
		/*
		 * No active session (not yet published, or already stopped):
		 * kwatch_tsk_ctx_get(true) above may have just claimed a slot
		 * for current. Release it here, otherwise an entry that lands
		 * in the register->epoch-publish window leaks the slot until
		 * the pool is freed.
		 */
		kwatch_tsk_ctx_release(ctx);
		return false;
	}

	switch (pos) {
	case KWATCH_PROBE_POSITION_ENTRY:
		ctx->depth++;
		return true;
	case KWATCH_PROBE_POSITION_ACTIVE:
		return true;
	case KWATCH_PROBE_POSITION_EXIT:
		if (unlikely(ctx->depth == 0)) {
			kwatch_tsk_ctx_put();
			return false;
		}

		ctx->depth--;
		if (ctx->depth == 0) {
			kwatch_tsk_ctx_put();
			return false;
		}
		return true;
	}
	return false;
}

static int kwatch_activate_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct kwatch_tsk_ctx *ctx = kwatch_tsk_ctx_get(false);
	unsigned long watch_addr;
	u16 watch_len;

	if (unlikely(!ctx))
		return 0;

	if (unlikely(kwatch_probe_is_muted()))
		return 0;

	if (unlikely(!kwatch_tsk_ctx_check(KWATCH_PROBE_POSITION_ACTIVE)))
		return 0;

	if (ctx->depth != kwatch_probe_ctx.cfg->depth + 1 || ctx->wp)
		return 0;

	if (kwatch_deref_resolve(kwatch_probe_ctx.cfg, regs, &watch_addr,
				 &watch_len))
		return 0;

	if (kwatch_hwbp_get(&ctx->wp))
		return 0;

	kwatch_hwbp_arm(ctx->wp, watch_addr, watch_len);
	return 0;
}

static int kwatch_lifecycle_entry(struct kretprobe_instance *ri,
				  struct pt_regs *regs)
{
	/*
	 * Single policy point: the target function's context is judged once
	 * here. A rejected invocation never increments depth, so the offset
	 * kprobe path inherits the verdict through the depth check.
	 */
	if (unlikely(kwatch_probed_ctx_in_nmi(kwatch_probe_ctx.rp_via_int3))) {
		atomic_long_inc(&kwatch_nmi_rejected);
		return 1; /* NMI context is unsupported: no window, no return hook */
	}

	if (!kwatch_tsk_ctx_check(KWATCH_PROBE_POSITION_ENTRY))
		return 0;

	if (kwatch_probe_ctx.cfg->func_offset == 0)
		kwatch_activate_handler(NULL, regs);

	return 0;
}

static int kwatch_lifecycle_exit(struct kretprobe_instance *ri,
				 struct pt_regs *regs)
{
	struct kwatch_tsk_ctx *ctx = kwatch_tsk_ctx_get(false);

	if (unlikely(!ctx))
		return 0;

	if (!kwatch_tsk_ctx_check(KWATCH_PROBE_POSITION_EXIT))
		return 0;

	if (ctx->depth == kwatch_probe_ctx.cfg->depth) {
		struct kwatch_watchpoint *wp = xchg(&ctx->wp, NULL);

		if (wp)
			kwatch_hwbp_put(wp);
	}

	return 0;
}

int kwatch_probe_start(struct kwatch_config *cfg)
{
	static u32 next_epoch;
	u32 current_epoch;
	int ret;

	/*
	 * Lockless check to prevent concurrent starts. Strictly serialized
	 * by the control plane mutex, but serves as a sanity check.
	 */
	if (smp_load_acquire(&kwatch_probe_ctx.epoch) != 0)
		return -EBUSY;

	memset(&kwatch_probe_ctx, 0, sizeof(kwatch_probe_ctx));
	kwatch_probe_ctx.cfg = cfg;

	/* Session-scoped, like arm_ipi_suppressed in kwatch_hwbp_prealloc() */
	atomic_long_set(&kwatch_nmi_rejected, 0);

	/*
	 * Pin the entry probepoint before the kretprobe registers, so its
	 * delivery (int3 vs ftrace) can never change under jump optimization.
	 * register_kretprobe() clears kp.post_handler, hence the companion.
	 */
	kwatch_probe_ctx.pin_kp.symbol_name = cfg->func_name;
	kwatch_probe_ctx.pin_kp.post_handler = kwatch_pin_post_handler;
	ret = register_kprobe(&kwatch_probe_ctx.pin_kp);
	if (ret < 0)
		return ret;

	kwatch_probe_ctx.rp.entry_handler = kwatch_lifecycle_entry;
	kwatch_probe_ctx.rp.handler = kwatch_lifecycle_exit;
	kwatch_probe_ctx.rp.kp.symbol_name = cfg->func_name;

	ret = register_kretprobe(&kwatch_probe_ctx.rp);
	if (ret < 0) {
		unregister_kprobe(&kwatch_probe_ctx.pin_kp);
		return ret;
	}
	kwatch_probe_ctx.rp_via_int3 = !kprobe_ftrace(&kwatch_probe_ctx.rp.kp);

	if (cfg->func_offset) {
		kwatch_probe_ctx.kp.symbol_name = cfg->func_name;
		kwatch_probe_ctx.kp.offset = cfg->func_offset;
		kwatch_probe_ctx.kp.pre_handler = kwatch_activate_handler;

		ret = register_kprobe(&kwatch_probe_ctx.kp);
		if (ret) {
			unregister_kretprobe(&kwatch_probe_ctx.rp);
			unregister_kprobe(&kwatch_probe_ctx.pin_kp);
			return ret;
		}
	}

	current_epoch = ++next_epoch;
	if (unlikely(!current_epoch))
		current_epoch = ++next_epoch;

	/* Pairs with smp_load_acquire() in kwatch_tsk_ctx_check() */
	smp_store_release(&kwatch_probe_ctx.epoch, current_epoch);

	return 0;
}

void kwatch_probe_stop(void)
{
	if (!kwatch_probe_ctx.epoch)
		return;

	/* Pairs with smp_load_acquire() in kwatch_tsk_ctx_check() */
	smp_store_release(&kwatch_probe_ctx.epoch, 0);

	if (kwatch_probe_ctx.cfg->func_offset > 0)
		unregister_kprobe(&kwatch_probe_ctx.kp);

	unregister_kretprobe(&kwatch_probe_ctx.rp);
	unregister_kprobe(&kwatch_probe_ctx.pin_kp);
}
