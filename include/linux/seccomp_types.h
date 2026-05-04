/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SECCOMP_TYPES_H
#define _LINUX_SECCOMP_TYPES_H

#include <linux/types.h>

#ifdef CONFIG_SECCOMP

struct seccomp_filter;
struct seccomp_pinned_args;

#define SECCOMP_PIN_MAX_ARGS 6

/**
 * struct seccomp_pinned_arg - one kernel-owned snapshot of a user-pointer arg.
 * @user_addr:	the original userspace address (key for lookup at consumption).
 * @size:	bytes actually populated in @data.
 * @arg_idx:	syscall register slot 0..5.
 * @kind:	one of SECCOMP_PIN_*.
 * @data:	kvmalloc'd buffer holding the snapshotted bytes.
 *
 * Consumption sites (getname_flags, copy_strings, move_addr_to_kernel,
 * import_ubuf) inspect @data and @size after a successful
 * seccomp_pin_lookup_current(). For sites that need a stable kvec
 * pointer outliving the call (import_ubuf -> vfs_write iter),
 * seccomp_pin_kvec_for() returns a kvec stored alongside the pin
 * with matching lifetime.
 */
struct seccomp_pinned_arg {
	u64	user_addr;
	u32	size;
	u8	arg_idx;
	u8	kind;
	u16	_pad;
	void	*data;
};

/**
 * struct seccomp - the state of a seccomp'ed process
 *
 * @mode:  indicates one of the valid values above for controlled
 *         system calls available to a process.
 * @filter_count: number of seccomp filters
 * @filter: must always point to a valid seccomp-filter or NULL as it is
 *          accessed without locking during system call entry.
 *
 *          @filter must only be accessed from the context of current as there
 *          is no read locking.
 * @pinned_args: NULL except during a PIN_ARGS window. Owned by the trapped
 *          task itself; populated by SECCOMP_IOCTL_NOTIF_PIN_ARGS, consumed
 *          on CONTINUE_PINNED, freed at syscall exit, listener release, or
 *          task exit. See kernel/seccomp_pin.c.
 */
struct seccomp {
	int mode;
	atomic_t filter_count;
	struct seccomp_filter *filter;
	struct seccomp_pinned_args *pinned_args;
};

#else

struct seccomp { };
struct seccomp_filter { };

#endif

#endif /* _LINUX_SECCOMP_TYPES_H */
