/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_KWATCH_H
#define _MM_KWATCH_H

#include <linux/fprobe.h>
#include <linux/kprobes.h>
#include <linux/perf_event.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/atomic.h>

#define MAX_CONFIG_STR_LEN 512
#define MAX_DEREF_CHAIN 4

struct kwatch_watchpoint;

struct kwatch_tsk_ctx {
	struct task_struct *task;
	struct kwatch_watchpoint *wp;
	u16 depth;
	u32 epoch;
};

struct kwatch_watchpoint {
	struct perf_event *__percpu *event;
	call_single_data_t __percpu *csd_arm;
	call_single_data_t __percpu *csd_disarm;
	struct perf_event_attr attr;
	atomic_t in_use; // multi-consumer safe get/put
	struct list_head list; // for cpu online and offline

	struct task_struct *arm_tsk;
	atomic_t pending_ipis;
	atomic_t refcount;
	bool teardown;
};

enum kwatch_base_type {
	KWATCH_BASE_STACK,
	KWATCH_BASE_ABS_ADDR,
	KWATCH_BASE_GLOBAL_SYM,
	KWATCH_BASE_ARG1,
	KWATCH_BASE_ARG2,
	KWATCH_BASE_ARG3,
	KWATCH_BASE_ARG4,
	KWATCH_BASE_ARG5,
	KWATCH_BASE_ARG6,
};

struct kwatch_config {
	u16 max_watch;
	char func_name[KSYM_NAME_LEN];
	u16 func_offset;
	u16 depth;
	u16 duration;
	u16 watch_len;

	/* Unified Deref Engine State */
	enum kwatch_base_type base;
	char watch_expr[MAX_CONFIG_STR_LEN];
	unsigned long sym_addr;
	long offsets[MAX_DEREF_CHAIN];
	u8 offset_count;
	u16 max_concurrency;
};

int kwatch_hwbp_prealloc(u16 max_watch);
void kwatch_hwbp_free(void);
int kwatch_hwbp_get(struct kwatch_watchpoint **out_wp);
void kwatch_hwbp_arm(struct kwatch_watchpoint *wp, unsigned long addr, u16 len);
int kwatch_hwbp_put(struct kwatch_watchpoint *wp);

int kwatch_probe_start(struct kwatch_config *cfg);
void kwatch_probe_stop(void);
void kwatch_probe_mute(bool mute);
bool kwatch_probe_validate_hit(struct pt_regs *regs, struct task_struct *arm_tsk);
unsigned long kwatch_probe_nmi_rejected(void);
unsigned long kwatch_hwbp_arm_ipi_suppressed(void);

int kwatch_tsk_ctx_prealloc(u16 max_concurrency);
struct kwatch_tsk_ctx *kwatch_tsk_ctx_get(bool can_alloc);
void kwatch_tsk_ctx_put(void);
void kwatch_tsk_ctx_release(struct kwatch_tsk_ctx *ctx);
void kwatch_tsk_ctx_reset(struct kwatch_tsk_ctx *ctx, u32 new_epoch);
void kwatch_tsk_ctx_release_wps(void);
void kwatch_tsk_ctx_free(void);

void kwatch_global_anchor(unsigned long duration_sec);
int kwatch_anchor_start(u16 duration);
void kwatch_anchor_stop(void);
void kwatch_anchor_cancel_work(void);
bool kwatch_anchor_has_expired(void);
void kwatch_anchor_clear_expired(void);
void kwatch_auto_stop(void);

int kwatch_deref_resolve(const struct kwatch_config *cfg, struct pt_regs *regs,
			 unsigned long *out_addr, u16 *out_len);
int kwatch_deref_parse(struct kwatch_config *cfg, const char *watch_expr);

#endif /* _MM_KWATCH_H */
