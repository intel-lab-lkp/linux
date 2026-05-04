/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal interfaces for SECCOMP_IOCTL_NOTIF_PIN_ARGS.
 *
 * The pin lifecycle and walker live in kernel/seccomp_pin.c to keep
 * kernel/seccomp.c focused on the existing notify machinery.
 */
#ifndef _KERNEL_SECCOMP_PIN_H
#define _KERNEL_SECCOMP_PIN_H

#include <linux/types.h>
#include <uapi/linux/seccomp.h>

#include <linux/seccomp_types.h>	/* struct seccomp_pinned_arg, SECCOMP_PIN_MAX_ARGS */
#include <linux/uio.h>			/* struct kvec */
#include <linux/task_work.h>		/* struct callback_head */

struct task_struct;
struct seccomp_filter;
struct seccomp_knotif;
struct seccomp_notif_pin_args;

/*
 * Maximum cumulative bytes a single PIN_ARGS request may snapshot on
 * behalf of one notification. Defensive bound only — typical pins are
 * a few KiB (one PATH_MAX path; argv up to MAX_ARG_STRLEN). Hardcoded
 * rather than a sysctl: there is no legitimate use case for runtime
 * tuning. Smaller is always reachable via desc->max_bytes; larger
 * indicates a policy bug.
 */
#define SECCOMP_PIN_MAX_TOTAL_BYTES	(1UL << 20)	/* 1 MiB */

/**
 * struct seccomp_pinned_args - the per-task pin record.
 * @notif_id:	id of the outstanding notification this pin belongs to.
 * @syscall_nr:	syscall number captured at pin time; consumption checks this
 *              against current to skip pinned data on a mismatched syscall
 *              (e.g. one issued from a signal handler during restart).
 * @nr_args:	number of populated entries in @args.
 * @live:	false during the pin-decision window, set to true on
 *              CONTINUE_PINNED so consumption hooks know to use the snapshot.
 * @args:	per-slot pinned data; only the first @nr_args entries are valid.
 */
struct seccomp_pinned_args {
	u64	notif_id;
	int	syscall_nr;
	u8	nr_args;
	bool	live;
	bool	clear_queued;	/* clear_work has been task_work_add()'d */
	struct callback_head clear_work;
	struct seccomp_pinned_arg args[SECCOMP_PIN_MAX_ARGS];
	/*
	 * Per-arg stable kvec storage. Populated by the walker for kinds
	 * whose consumption hooks build an iov_iter (currently FIXED ->
	 * import_ubuf). The kvec must outlive the iter; this struct lives
	 * until syscall exit, which is after the iter is fully consumed.
	 */
	struct kvec arg_kvecs[SECCOMP_PIN_MAX_ARGS];
};

#ifdef CONFIG_SECCOMP_FILTER

struct seccomp_pinned_args *seccomp_alloc_pinned_args(u8 nr_args);
void seccomp_free_pinned_args(struct seccomp_pinned_args *kpa);
void seccomp_clear_pinned_args(struct task_struct *task);

/*
 * Queue a one-shot task_work that will clear @task's pinned_args when
 * @task next returns to userspace, i.e. after the trapped-and-resumed
 * syscall body has completed. Called from NOTIF_SEND on CONTINUE_PINNED.
 */
int seccomp_pin_queue_clear(struct task_struct *task);

/**
 * seccomp_pin_args_walk - per-arg snapshot phase (no seccomp locks).
 * @task:	the trapped child whose mm we're reading; caller must hold a
 *		reference (via get_task_struct).
 * @kargs:	in/out ioctl payload; the walker reads .nr_args / .args[i] inputs
 *		and writes back .args[i] outputs (actual_size, truncated, etc.).
 * @args:	syscall register args (knotif->data->args).
 * @syscall_nr:	syscall number captured at notif time.
 * @user_buf:	the supervisor's bulk byte buffer (user pointer).
 * @user_buf_size: capacity of @user_buf.
 * @out:	on success, *@out is a freshly-allocated kpa with the snapshot;
 *		caller takes ownership and must seccomp_free_pinned_args() if
 *		the attach step fails.
 *
 * Return: 0 on success, negative errno on failure.
 *
 * Phase B of PIN_ARGS: this runs without seccomp locks held. Phase A (notif
 * validation) and Phase C (attach) live in kernel/seccomp.c.
 */
long seccomp_pin_args_walk(struct task_struct *task,
			   struct seccomp_notif_pin_args *kargs,
			   const u64 *args, int syscall_nr,
			   void __user *user_buf, u32 user_buf_size,
			   struct seccomp_pinned_args **out);

/* seccomp_pin_lookup_current() lives in include/linux/seccomp.h; it is
 * called from consumption sites outside kernel/seccomp/ (fs/, net/, lib/).
 */

#else

static inline void seccomp_clear_pinned_args(struct task_struct *task) { }

#endif /* CONFIG_SECCOMP_FILTER */

#endif /* _KERNEL_SECCOMP_PIN_H */
