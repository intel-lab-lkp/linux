// SPDX-License-Identifier: GPL-2.0
/*
 * This file is generated, do not edit.
 */
#include <linux/rv.h>
#include <rv/instrumentation.h>
#include <trace/events/task.h>
#include <trace/events/sched.h>

#include "ba.h"

static_assert(NUM_ATOM <= RV_MAX_LTL_ATOM);

enum buchi_state {
	INIT,
	S3,
	DEAD,
};

int rv_rtapp_block_task_slot = RV_PER_TASK_MONITOR_INIT;

static void init_monitor(struct task_struct *task)
{
	struct ltl_monitor *mon = rv_rtapp_block_get_monitor(task);

	for (int i = 0; i < NUM_ATOM; ++i)
		mon->atoms[i] = LTL_UNDETERMINED;
	mon->state = INIT;
}

static void handle_task_newtask(void *data, struct task_struct *task, unsigned long flags)
{
	struct ltl_monitor *mon = rv_rtapp_block_get_monitor(task);

	init_monitor(task);

	rv_rtapp_block_atoms_init(task, mon);
	rv_rtapp_block_atoms_fetch(task, mon);
}

int rv_rtapp_block_init(size_t data_size)
{
	struct task_struct *g, *p;
	int ret, cpu;

	if (WARN_ON(data_size > RV_MAX_DATA_SIZE))
		return -EINVAL;

	ret = rv_get_task_monitor_slot();
	if (ret < 0)
		return ret;

	rv_rtapp_block_task_slot = ret;

	rv_attach_trace_probe("rtapp_block", task_newtask, handle_task_newtask);

	read_lock(&tasklist_lock);

	for_each_process_thread(g, p)
		init_monitor(p);

	for_each_present_cpu(cpu)
		init_monitor(idle_task(cpu));

	read_unlock(&tasklist_lock);

	return 0;
}

void rv_rtapp_block_destroy(void)
{
	rv_put_task_monitor_slot(rv_rtapp_block_task_slot);
	rv_rtapp_block_task_slot = RV_PER_TASK_MONITOR_INIT;

	rv_detach_trace_probe("rtapp_block", task_newtask, handle_task_newtask);
}

static void illegal_state(struct task_struct *task, struct ltl_monitor *mon)
{
	mon->state = INIT;
	rv_rtapp_block_error(task, mon);
}

static void rv_rtapp_block_attempt_start(struct task_struct *task, struct ltl_monitor *mon)
{
	int i;

	mon = rv_rtapp_block_get_monitor(task);

	rv_rtapp_block_atoms_fetch(task, mon);

	for (i = 0; i < NUM_ATOM; ++i) {
		if (mon->atoms[i] == LTL_UNDETERMINED)
			return;
	}

	if (((!mon->atoms[WAKEUP_RT_TASK] || (mon->atoms[RT] || (mon->atoms[RT_MUTEX_WAKING_WAITER]
	   || (mon->atoms[STOPPING_WOKEN_TASK] || (mon->atoms[WOKEN_TASK_IS_MIGRATION] ||
	   mon->atoms[WOKEN_TASK_IS_RCU])))))) && (((!mon->atoms[USER_TASK] || !mon->atoms[RT]) ||
	   (!mon->atoms[SLEEP] || (mon->atoms[DO_NANOSLEEP] || mon->atoms[FUTEX_LOCK_WITH_PI])))))
		mon->state = S3;
	else
		illegal_state(task, mon);
}

static void rv_rtapp_block_step(struct task_struct *task, struct ltl_monitor *mon)
{
	switch (mon->state) {
	case S3:
		if (((!mon->atoms[WAKEUP_RT_TASK] || (mon->atoms[RT] ||
		   (mon->atoms[RT_MUTEX_WAKING_WAITER] || (mon->atoms[STOPPING_WOKEN_TASK] ||
		   (mon->atoms[WOKEN_TASK_IS_MIGRATION] || mon->atoms[WOKEN_TASK_IS_RCU])))))) &&
		   (((!mon->atoms[USER_TASK] || !mon->atoms[RT]) || (!mon->atoms[SLEEP] ||
		   (mon->atoms[DO_NANOSLEEP] || mon->atoms[FUTEX_LOCK_WITH_PI])))))
			mon->state = S3;
		else
			illegal_state(task, mon);
		break;
	case DEAD:
	case INIT:
		break;
	default:
		WARN_ON_ONCE(1);
	}
}

void rv_rtapp_block_atom_update(struct task_struct *task, unsigned int atom, bool value)
{
	struct ltl_monitor *mon = rv_rtapp_block_get_monitor(task);

	rv_rtapp_block_atom_set(mon, atom, value);

	if (mon->state == DEAD)
		return;

	if (mon->state == INIT)
		rv_rtapp_block_attempt_start(task, mon);
	if (mon->state == INIT)
		return;

	mon->atoms[atom] = value;

	rv_rtapp_block_atoms_fetch(task, mon);

	rv_rtapp_block_step(task, mon);
}
