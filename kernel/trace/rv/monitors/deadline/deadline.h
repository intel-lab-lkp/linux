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

static inline struct task_struct *dl_task_of(struct sched_dl_entity *dl_se)
{
	if (WARN_ONCE(dl_se->dl_server, "Call this only on a DL task!"))
		return NULL;
	return container_of(dl_se, struct task_struct, dl);
}

#ifdef CONFIG_RT_MUTEXES
static inline struct sched_dl_entity *pi_of(struct sched_dl_entity *dl_se)
{
	return dl_se->pi_se;
}

static inline bool is_dl_boosted(struct sched_dl_entity *dl_se)
{
	return pi_of(dl_se) != dl_se;
}
#else /* !CONFIG_RT_MUTEXES: */
static inline struct sched_dl_entity *pi_of(struct sched_dl_entity *dl_se)
{
	return dl_se;
}

static inline bool is_dl_boosted(struct sched_dl_entity *dl_se)
{
	return false;
}
#endif /* !CONFIG_RT_MUTEXES */

static inline bool dl_is_implicit(struct sched_dl_entity *dl_se)
{
	return dl_se->dl_deadline == dl_se->dl_period;
}

/*
 * If both have dummy values, the syscalls are not supported and we don't even
 * need to register the handler.
 */
static inline bool should_skip_syscall_handle(void)
{
	return __NR_sched_setattr < 0 && __NR_sched_setscheduler < 0;
}

/*
 * Use negative numbers for the server.
 * Currently only one fair server per CPU, may change in the future.
 */
#define fair_server_id(cpu) (-cpu)
/*
 * Get a unique id used for dl entities
 *
 * The cpu is not required for tasks as the pid is used there, if this function
 * is called on a dl_se that for sure corresponds to a task, DL_TASK can be
 * used in place of cpu.
 * We need the cpu for servers as it is provided in the tracepoint and we
 * cannot easily retrieve it from the dl_se (requires the struct rq definition).
 */
static inline int get_entity_id(struct sched_dl_entity *dl_se, int cpu)
{
	if (dl_se->dl_server)
		return fair_server_id(cpu);
	return dl_task_of(dl_se)->pid;
}

/* Expand id and target as arguments for da functions */
#define EXPAND_ID(dl_se, cpu) get_entity_id(dl_se, cpu), dl_se

/* Use this as the cpu in EXPAND_ID in case the dl_se is surely from a task */
#define DL_TASK -1

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

/* Helper functions requiring DA/HA utilities*/
#ifdef RV_MON_TYPE

/*
 * get_fair_server - get the fair server associated to a task
 *
 * If the task is a boosted task, the server is available in the task_struct,
 * otherwise grab the dl entity saved for the CPU where the task is enqueued.
 * This function assumes the task is enqueued somewhere.
 */
static inline struct sched_dl_entity *get_fair_server(struct task_struct *tsk)
{
	if (tsk->dl_server)
		return tsk->dl_server;
	return da_get_target_by_id(fair_server_id(task_cpu(tsk)));
}

/*
 * Initialise monitors for all tasks and pre-allocate the storage for servers.
 * This is necessary since we don't have access to the servers here and
 * allocation can cause deadlocks from their tracepoints. We can only fill
 * pre-initialised storage from there.
 */
static inline int init_storage(void)
{
	struct task_struct *g, *p;
	int cpu;

	for_each_possible_cpu(cpu) {
		if (!da_create_empty_storage(fair_server_id(cpu), GFP_KERNEL))
			goto fail;
	}

	read_lock(&tasklist_lock);
	for_each_process_thread(g, p) {
		if (p->policy == SCHED_DEADLINE) {
			if (!da_create_storage(EXPAND_ID(&p->dl, DL_TASK), NULL,
					       GFP_KERNEL)) {
				read_unlock(&tasklist_lock);
				goto fail;
			}
		}
	}
	read_unlock(&tasklist_lock);
	return 0;

fail:
	da_monitor_destroy();
	return -ENOMEM;
}

static void handle_newtask(void *data, struct task_struct *task, unsigned long flags)
{
	/* Might be superfluous as tasks are not started with this policy.. */
	if (task->policy == SCHED_DEADLINE)
		da_create_storage(EXPAND_ID(&task->dl, DL_TASK), NULL, GFP_NOWAIT);
}

static void handle_exit(void *data, struct task_struct *p, bool group_dead)
{
	if (p->policy == SCHED_DEADLINE)
		da_destroy_storage(get_entity_id(&p->dl, DL_TASK));
}

#endif
