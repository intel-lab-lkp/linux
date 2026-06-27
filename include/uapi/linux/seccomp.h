/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SECCOMP_H
#define _UAPI_LINUX_SECCOMP_H

#include <linux/compiler.h>
#include <linux/types.h>


/* Valid values for seccomp.mode and prctl(PR_SET_SECCOMP, <mode>) */
#define SECCOMP_MODE_DISABLED	0 /* seccomp is not in use. */
#define SECCOMP_MODE_STRICT	1 /* uses hard-coded filter. */
#define SECCOMP_MODE_FILTER	2 /* uses user-supplied filter. */

/* Valid operations for seccomp syscall. */
#define SECCOMP_SET_MODE_STRICT		0
#define SECCOMP_SET_MODE_FILTER		1
#define SECCOMP_GET_ACTION_AVAIL	2
#define SECCOMP_GET_NOTIF_SIZES		3

/* Valid flags for SECCOMP_SET_MODE_FILTER */
#define SECCOMP_FILTER_FLAG_TSYNC		(1UL << 0)
#define SECCOMP_FILTER_FLAG_LOG			(1UL << 1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW		(1UL << 2)
#define SECCOMP_FILTER_FLAG_NEW_LISTENER	(1UL << 3)
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH		(1UL << 4)
/* Received notifications wait in killable state (only respond to fatal signals) */
#define SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV	(1UL << 5)
/*
 * Declares that this listener's notifier may issue
 * SECCOMP_IOCTL_NOTIF_PIN_INSTALL / SECCOMP_IOCTL_NOTIF_SEND_REDIRECT. At most
 * one such filter may exist in a task's filter chain. Requires NEW_LISTENER.
 */
#define SECCOMP_FILTER_FLAG_REDIRECT		(1UL << 6)

/*
 * All BPF programs must return a 32-bit value.
 * The bottom 16-bits are for optional return data.
 * The upper 16-bits are ordered from least permissive values to most,
 * as a signed value (so 0x8000000 is negative).
 *
 * The ordering ensures that a min_t() over composed return values always
 * selects the least permissive choice.
 */
#define SECCOMP_RET_KILL_PROCESS 0x80000000U /* kill the process */
#define SECCOMP_RET_KILL_THREAD	 0x00000000U /* kill the thread */
#define SECCOMP_RET_KILL	 SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_TRAP	 0x00030000U /* disallow and force a SIGSYS */
#define SECCOMP_RET_ERRNO	 0x00050000U /* returns an errno */
#define SECCOMP_RET_USER_NOTIF	 0x7fc00000U /* notifies userspace */
#define SECCOMP_RET_TRACE	 0x7ff00000U /* pass to a tracer or disallow */
#define SECCOMP_RET_LOG		 0x7ffc0000U /* allow after logging */
#define SECCOMP_RET_ALLOW	 0x7fff0000U /* allow */

/* Masks for the return value sections. */
#define SECCOMP_RET_ACTION_FULL	0xffff0000U
#define SECCOMP_RET_ACTION	0x7fff0000U
#define SECCOMP_RET_DATA	0x0000ffffU

/**
 * struct seccomp_data - the format the BPF program executes over.
 * @nr: the system call number
 * @arch: indicates system call convention as an AUDIT_ARCH_* value
 *        as defined in <linux/audit.h>.
 * @instruction_pointer: at the time of the system call.
 * @args: up to 6 system call arguments always stored as 64-bit values
 *        regardless of the architecture.
 */
struct seccomp_data {
	int nr;
	__u32 arch;
	__u64 instruction_pointer;
	__u64 args[6];
};

struct seccomp_notif_sizes {
	__u16 seccomp_notif;
	__u16 seccomp_notif_resp;
	__u16 seccomp_data;
};

struct seccomp_notif {
	__u64 id;
	__u32 pid;
	__u32 flags;
	struct seccomp_data data;
};

/*
 * Valid flags for struct seccomp_notif_resp
 *
 * Note, the SECCOMP_USER_NOTIF_FLAG_CONTINUE flag must be used with caution!
 * If set by the process supervising the syscalls of another process the
 * syscall will continue. This is problematic because of an inherent TOCTOU.
 * An attacker can exploit the time while the supervised process is waiting on
 * a response from the supervising process to rewrite syscall arguments which
 * are passed as pointers of the intercepted syscall.
 * It should be absolutely clear that this means that the seccomp notifier
 * _cannot_ be used to implement a security policy! It should only ever be used
 * in scenarios where a more privileged process supervises the syscalls of a
 * lesser privileged process to get around kernel-enforced security
 * restrictions when the privileged process deems this safe. In other words,
 * in order to continue a syscall the supervising process should be sure that
 * another security mechanism or the kernel itself will sufficiently block
 * syscalls if arguments are rewritten to something unsafe.
 *
 * Similar precautions should be applied when stacking SECCOMP_RET_USER_NOTIF
 * or SECCOMP_RET_TRACE. For SECCOMP_RET_USER_NOTIF filters acting on the
 * same syscall, the most recently added filter takes precedence. This means
 * that the new SECCOMP_RET_USER_NOTIF filter can override any
 * SECCOMP_IOCTL_NOTIF_SEND from earlier filters, essentially allowing all
 * such filtered syscalls to be executed by sending the response
 * SECCOMP_USER_NOTIF_FLAG_CONTINUE. Note that SECCOMP_RET_TRACE can equally
 * be overriden by SECCOMP_USER_NOTIF_FLAG_CONTINUE.
 */
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)

struct seccomp_notif_resp {
	__u64 id;
	__s64 val;
	__s32 error;
	__u32 flags;
};

#define SECCOMP_USER_NOTIF_FD_SYNC_WAKE_UP (1UL << 0)

/* valid flags for seccomp_notif_addfd */
#define SECCOMP_ADDFD_FLAG_SETFD	(1UL << 0) /* Specify remote fd */
#define SECCOMP_ADDFD_FLAG_SEND		(1UL << 1) /* Addfd and return it, atomically */

/**
 * struct seccomp_notif_addfd
 * @id: The ID of the seccomp notification
 * @flags: SECCOMP_ADDFD_FLAG_*
 * @srcfd: The local fd number
 * @newfd: Optional remote FD number if SETFD option is set, otherwise 0.
 * @newfd_flags: The O_* flags the remote FD should have applied
 */
struct seccomp_notif_addfd {
	__u64 id;
	__u32 flags;
	__u32 srcfd;
	__u32 newfd;
	__u32 newfd_flags;
};

/**
 * struct seccomp_notif_pin_install - have the kernel install a sealed
 * MAP_SHARED mapping of @memfd into the trapped task's mm at @target_addr,
 * which SECCOMP_IOCTL_NOTIF_SEND_REDIRECT can then use as a target for
 * substituted pointer arguments.
 *
 * The supervisor owns @memfd. The kernel installs the mapping into
 * the trapped task's address space without target-side cooperation
 * (the target need not mmap or mseal anything itself). The mapping
 * is marked VM_SEALED at install time, so the target and any
 * CLONE_VM peer cannot munmap, mremap, mprotect, or MAP_FIXED-stomp
 * it. The mapping is read-only. The supervisor retains access via its
 * own mapping of the same memfd in its own mm.
 *
 * @memfd must be write-sealed (F_SEAL_WRITE or F_SEAL_FUTURE_WRITE),
 * otherwise the ioctl fails with -EINVAL. This guarantees the pin's bytes
 * cannot be rewritten through any other reference to the same memfd (for
 * example one the target reopened via the supervisor's /proc/<pid>/fd),
 * not just through the read-only pin itself. F_SEAL_FUTURE_WRITE still
 * lets the supervisor update the bytes through its own pre-seal mapping.
 *
 * @offset lets one memfd back several disjoint read-only pins.
 *
 * @id: The ID of an active seccomp notification on this listener,
 *      identifying the trapped task whose mm receives the pin.
 * @flags: Reserved, must be 0.
 * @memfd: Supervisor-side fd for the backing memfd. Must be write-sealed.
 * @target_addr: Address in the trapped task's mm to install at. Must be
 *               page-aligned. If non-zero, MAP_FIXED semantics apply, no
 *               other mapping may exist in [@target_addr, @target_addr +
 *               @size). If zero, the kernel chooses a free area in the
 *               target mm. On success the actual mapped address is written
 *               back here.
 * @size: Size of the pin in bytes. Must be page-aligned.
 * @offset: Page-aligned byte offset into @memfd to map from. Zero maps
 *          from the start of the memfd.
 */
struct seccomp_notif_pin_install {
	__u64 id;
	__u32 flags;
	__u32 memfd;
	__u64 target_addr;
	__u64 size;
	__u64 offset;
};

#define SECCOMP_IOC_MAGIC		'!'
#define SECCOMP_IO(nr)			_IO(SECCOMP_IOC_MAGIC, nr)
#define SECCOMP_IOR(nr, type)		_IOR(SECCOMP_IOC_MAGIC, nr, type)
#define SECCOMP_IOW(nr, type)		_IOW(SECCOMP_IOC_MAGIC, nr, type)
#define SECCOMP_IOWR(nr, type)		_IOWR(SECCOMP_IOC_MAGIC, nr, type)

/* Flags for seccomp notification fd ioctl. */
#define SECCOMP_IOCTL_NOTIF_RECV	SECCOMP_IOWR(0, struct seccomp_notif)
#define SECCOMP_IOCTL_NOTIF_SEND	SECCOMP_IOWR(1,	\
						struct seccomp_notif_resp)
#define SECCOMP_IOCTL_NOTIF_ID_VALID	SECCOMP_IOW(2, __u64)
/* On success, the return value is the remote process's added fd number */
#define SECCOMP_IOCTL_NOTIF_ADDFD	SECCOMP_IOW(3, \
						struct seccomp_notif_addfd)

#define SECCOMP_IOCTL_NOTIF_SET_FLAGS	SECCOMP_IOW(4, __u64)

/* Valid flags for struct seccomp_notif_resp_redirect. */
#define SECCOMP_REDIRECT_FLAG_CONTINUE (1UL << 0)

/*
 * Number of syscall argument registers a redirect response may
 * substitute (matches struct seccomp_data::args[]).
 */
#define SECCOMP_REDIRECT_ARGS 6

/**
 * struct seccomp_notif_resp_redirect - resume the trapped syscall with
 * substituted arg-register values, optionally pointing into previously
 * installed pinned-memfd regions.
 *
 * Like SECCOMP_USER_NOTIF_FLAG_CONTINUE the syscall actually runs, but the
 * kernel first rewrites the arg registers selected by @args_mask. Each
 * pointer substitution (@ptr_mask) is validated against the trapped task's
 * current address space: the whole access [args[i], args[i] + ptr_len[i])
 * must lie inside a single VM_SEALED, read-only mapping of @memfd. No per-pin
 * bookkeeping is kept; authorization is re-derived from the live mapping, so
 * a target that has exited or execve()d (its mapping gone) simply fails
 * validation. Original registers are saved and restored at syscall exit for
 * ABI compliance - except after a successful execve, whose new register file
 * is left untouched (the redirect still applies, as execve copies the
 * pathname from the immutable pin before the old mm is gone, closing that
 * TOCTOU too).
 *
 * @id: The ID of the seccomp notification this response consumes.
 * @flags: SECCOMP_REDIRECT_FLAG_*. CONTINUE must be set.
 * @args_mask: Bit i set means args[i] replaces the trapped task's
 *             corresponding arg register before the syscall runs.
 * @ptr_mask: Subset of @args_mask. Bit i set means args[i] is a pointer and
 *            the access [args[i], args[i] + ptr_len[i]) is validated to lie
 *            entirely inside a single VM_SEALED, read-only mapping of @memfd.
 *            Scalar replacements (in @args_mask but not @ptr_mask) are
 *            written verbatim.
 * @memfd: Supervisor-side fd for the backing memfd whose sealed mapping the
 *         pointer substitutions must fall within. Consulted only when
 *         @ptr_mask is non-zero.
 * @args: Replacement values for the arg registers.
 * @ptr_len: For each bit set in @ptr_mask, ptr_len[i] is the byte length of
 *           the access starting at args[i]; it must be non-zero and args[i] +
 *           ptr_len[i] must not overflow. For every i whose bit is clear in
 *           @ptr_mask it must be 0.
 */
struct seccomp_notif_resp_redirect {
	__u64 id;
	__u32 flags;
	__u32 args_mask;
	__u32 ptr_mask;
	__u32 memfd;
	__u64 args[SECCOMP_REDIRECT_ARGS];
	__u64 ptr_len[SECCOMP_REDIRECT_ARGS];
};

/*
 * Install a sealed memfd-backed pin in the trapped task's mm without
 * target-side cooperation. The supervisor owns the backing memfd;
 * the kernel installs the mapping and marks it VM_SEALED. The actual
 * mapped address is written back to @target_addr (relevant when it was
 * passed as 0 to let the kernel choose).
 */
#define SECCOMP_IOCTL_NOTIF_PIN_INSTALL	SECCOMP_IOWR(5, \
						struct seccomp_notif_pin_install)

/*
 * Resume the trapped syscall with substituted arg-register values
 * pointing into an installed pin. The kernel saves and restores the
 * original registers at syscall exit so the caller observes ABI-
 * correct register preservation.
 */
#define SECCOMP_IOCTL_NOTIF_SEND_REDIRECT	SECCOMP_IOW(6, \
						struct seccomp_notif_resp_redirect)

#endif /* _UAPI_LINUX_SECCOMP_H */
