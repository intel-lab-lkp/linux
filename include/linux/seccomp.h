/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SECCOMP_H
#define _LINUX_SECCOMP_H

#include <uapi/linux/seccomp.h>
#include <linux/seccomp_types.h>

#define SECCOMP_FILTER_FLAG_MASK	(SECCOMP_FILTER_FLAG_TSYNC | \
					 SECCOMP_FILTER_FLAG_LOG | \
					 SECCOMP_FILTER_FLAG_SPEC_ALLOW | \
					 SECCOMP_FILTER_FLAG_NEW_LISTENER | \
					 SECCOMP_FILTER_FLAG_TSYNC_ESRCH | \
					 SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV)

/* sizeof() the first published struct seccomp_notif_addfd */
#define SECCOMP_NOTIFY_ADDFD_SIZE_VER0 24
#define SECCOMP_NOTIFY_ADDFD_SIZE_LATEST SECCOMP_NOTIFY_ADDFD_SIZE_VER0

#ifdef CONFIG_SECCOMP

#include <linux/thread_info.h>
#include <linux/atomic.h>
#include <asm/seccomp.h>

extern int __secure_computing(void);

#ifdef CONFIG_HAVE_ARCH_SECCOMP_FILTER
static inline int secure_computing(void)
{
	if (unlikely(test_syscall_work(SECCOMP)))
		return  __secure_computing();
	return 0;
}
#else
extern void secure_computing_strict(int this_syscall);
#endif

extern long prctl_get_seccomp(void);
extern long prctl_set_seccomp(unsigned long, void __user *);

static inline int seccomp_mode(struct seccomp *s)
{
	return s->mode;
}

#else /* CONFIG_SECCOMP */

#include <linux/errno.h>

struct seccomp_data;

#ifdef CONFIG_HAVE_ARCH_SECCOMP_FILTER
static inline int secure_computing(void) { return 0; }
#else
static inline void secure_computing_strict(int this_syscall) { return; }
#endif
static inline int __secure_computing(void) { return 0; }

static inline long prctl_get_seccomp(void)
{
	return -EINVAL;
}

static inline long prctl_set_seccomp(unsigned long arg2, char __user *arg3)
{
	return -EINVAL;
}

static inline int seccomp_mode(struct seccomp *s)
{
	return SECCOMP_MODE_DISABLED;
}
#endif /* CONFIG_SECCOMP */

#ifdef CONFIG_SECCOMP_FILTER
extern void seccomp_filter_release(struct task_struct *tsk);
extern void get_seccomp_filter(struct task_struct *tsk);
extern void seccomp_clear_pinned_args(struct task_struct *tsk);

/**
 * seccomp_pin_lookup_current - find a live PIN_ARGS snapshot for current().
 * @user_addr:	the userspace address the syscall body is about to read.
 *
 * Called from syscall fetch points (getname_flags, copy_strings,
 * move_addr_to_kernel, import_ubuf). Returns a pinned-arg entry whose
 * @data / @size the caller may consume in place of re-reading user
 * memory, or NULL if there is no live snapshot, the current syscall
 * does not match the one captured at pin time, or no entry matches
 * @user_addr.
 *
 * Safe to call lockless: current owns its seccomp.pinned_args field
 * once the PIN_ARGS orchestrator has installed it via WRITE_ONCE.
 */
const struct seccomp_pinned_arg *seccomp_pin_lookup_current(u64 user_addr);

/**
 * seccomp_pin_kvec_for - return a stable kvec for the given pin entry.
 * @pin:	a pin returned by seccomp_pin_lookup_current(); must belong
 *		to the current task.
 *
 * The returned pointer references kvec storage that outlives the pin
 * (freed at syscall exit), suitable for iov_iter_kvec() callers whose
 * iov_iter consumes after the wrapping function returns.
 */
struct kvec;
const struct kvec *seccomp_pin_kvec_for(const struct seccomp_pinned_arg *pin);
#else  /* CONFIG_SECCOMP_FILTER */
static inline void seccomp_filter_release(struct task_struct *tsk)
{
	return;
}
static inline void get_seccomp_filter(struct task_struct *tsk)
{
	return;
}
static inline void seccomp_clear_pinned_args(struct task_struct *tsk) { }
static inline const struct seccomp_pinned_arg *
seccomp_pin_lookup_current(u64 user_addr) { return NULL; }
struct kvec;
static inline const struct kvec *
seccomp_pin_kvec_for(const struct seccomp_pinned_arg *pin) { return NULL; }
#endif /* CONFIG_SECCOMP_FILTER */

#if defined(CONFIG_SECCOMP_FILTER) && defined(CONFIG_CHECKPOINT_RESTORE)
extern long seccomp_get_filter(struct task_struct *task,
			       unsigned long filter_off, void __user *data);
extern long seccomp_get_metadata(struct task_struct *task,
				 unsigned long filter_off, void __user *data);
#else
static inline long seccomp_get_filter(struct task_struct *task,
				      unsigned long n, void __user *data)
{
	return -EINVAL;
}
static inline long seccomp_get_metadata(struct task_struct *task,
					unsigned long filter_off,
					void __user *data)
{
	return -EINVAL;
}
#endif /* CONFIG_SECCOMP_FILTER && CONFIG_CHECKPOINT_RESTORE */

#ifdef CONFIG_SECCOMP_CACHE_DEBUG
struct seq_file;
struct pid_namespace;
struct pid;

int proc_pid_seccomp_cache(struct seq_file *m, struct pid_namespace *ns,
			   struct pid *pid, struct task_struct *task);
#endif
#endif /* _LINUX_SECCOMP_H */
