/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kernel.h>
#include <asm/syscall.h>
#include <uapi/linux/sched/types.h>

/*
 * Dummy values if not available
 */
#ifndef __NR_sched_setscheduler
#define __NR_sched_setscheduler -1
#endif
#ifndef __NR_sched_setattr
#define __NR_sched_setattr -2
#endif

extern struct rv_monitor rv_deadline;

/*
 * If both have dummy values, the syscalls are not supported and we don't even
 * need to register the handler.
 */
static inline bool should_skip_syscall_handle(void)
{
	return __NR_sched_setattr < 0 && __NR_sched_setscheduler < 0;
}

static inline int get_server_id(void)
{
	/*
	 * Use negative numbers for the server.
	 * Currently only one fair server per CPU, may change in the future.
	 */
	return -__smp_processor_id();
}

static inline int extract_params(struct pt_regs *regs, long id, struct task_struct **p)
{
	size_t size = offsetof(struct sched_attr, sched_nice);
	struct sched_attr __user *uattr, attr;
	int new_policy = -1, ret;
	unsigned long args[6];
	pid_t pid;

	switch (id) {
		case __NR_sched_setscheduler:
			syscall_get_arguments(current, regs, args);
			pid = args[0];
			new_policy = args[1];
			break;
		case __NR_sched_setattr:
			syscall_get_arguments(current, regs, args);
			pid = args[0];
			uattr = (void *)args[1];
			/*
			 * Just copy up to sched_flags, we are not interested after that
			 */
			ret = copy_struct_from_user(&attr, size, uattr, size);
			if (ret)
				return ret;
			if (attr.sched_flags & SCHED_FLAG_KEEP_POLICY)
				return -EINVAL;
			new_policy = attr.sched_policy;
			break;
		default:
			return -EINVAL;
	}
	if (!pid)
		*p = current;
	else {
		/*
		 * Required for find_task_by_vpid, make sure the caller doesn't
		 * need to get_task_struct().
		 */
		guard(rcu)();
		*p = find_task_by_vpid(pid);
		if (unlikely(!p))
			return -EINVAL;
	}

	return new_policy;
}
