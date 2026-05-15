/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal interfaces for SECCOMP_IOCTL_NOTIF_INJECT.
 *
 * The inject record allocation, validation, per-syscall injectors and
 * the dispatch entrypoint live in kernel/seccomp_inject.c to keep
 * kernel/seccomp.c focused on the notify state machine.
 */
#ifndef _KERNEL_SECCOMP_INJECT_H
#define _KERNEL_SECCOMP_INJECT_H

#include <linux/types.h>
#include <uapi/linux/seccomp.h>

struct seccomp_knotif;
struct seccomp_notif_inject;

/**
 * struct seccomp_inject_record - kernel-side per-knotif inject state.
 * @nr:		substitute syscall number, validated against the injectable
 *		whitelist.
 * @args:	substitute syscall arguments. For each i with the i'th bit
 *		set in @args_in_buf_mask, args[i] is a byte offset into
 *		@buf rather than the raw argument value.
 * @args_in_buf_mask: bitmask of pointer-shaped args backed by @buf.
 * @buf_size:	bytes valid in @buf.
 * @buf:	kernel-owned copy of the supervisor-supplied bytes; allocated
 *		at attach time, freed at consumption (or knotif teardown).
 *
 * Allocated by SECCOMP_IOCTL_NOTIF_INJECT, attached to the knotif under
 * filter->notify_lock, consumed by the trapped task on
 * SECCOMP_USER_NOTIF_FLAG_INJECTED, freed in the same path. Also freed
 * if the knotif is dropped without being injected (listener close,
 * task exit, supervisor changes its mind).
 */
struct seccomp_inject_record {
	u64	nr;
	u64	args[6];
	u32	args_in_buf_mask;
	u32	buf_size;
	void	*buf;
};

#ifdef CONFIG_SECCOMP_FILTER

/* Allocate, validate, and copy in @uinj. Caller takes ownership of *out. */
long seccomp_inject_record_build(const struct seccomp_notif_inject *uinj,
				 struct seccomp_inject_record **out);

/* Free a record built by seccomp_inject_record_build(). */
void seccomp_inject_record_free(struct seccomp_inject_record *rec);

/* Dispatch a built record into the matching kernel-mode syscall helper.
 * Runs in the trapped task's context (current is the trapped task).
 * Returns the helper's result, which becomes the syscall return value.
 */
long seccomp_inject_dispatch(const struct seccomp_inject_record *rec);

#else /* !CONFIG_SECCOMP_FILTER */

static inline void seccomp_inject_record_free(struct seccomp_inject_record *rec) { }

#endif /* CONFIG_SECCOMP_FILTER */

#endif /* _KERNEL_SECCOMP_INJECT_H */
