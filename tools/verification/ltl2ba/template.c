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
%%STATE_LIST%%
	DEAD,
};

int rv_%%MODEL_NAME%%_task_slot = RV_PER_TASK_MONITOR_INIT;

static void init_monitor(struct task_struct *task)
{
	struct ltl_monitor *mon = rv_%%MODEL_NAME%%_get_monitor(task);

	for (int i = 0; i < NUM_ATOM; ++i)
		mon->atoms[i] = LTL_UNDETERMINED;
	mon->state = INIT;
}

static void handle_task_newtask(void *data, struct task_struct *task, unsigned long flags)
{
	struct ltl_monitor *mon = rv_%%MODEL_NAME%%_get_monitor(task);

	init_monitor(task);

	rv_%%MODEL_NAME%%_atoms_init(task, mon);
	rv_%%MODEL_NAME%%_atoms_fetch(task, mon);
}

int rv_%%MODEL_NAME%%_init(size_t data_size)
{
	struct task_struct *g, *p;
	int ret, cpu;

	if (WARN_ON(data_size > RV_MAX_DATA_SIZE))
		return -EINVAL;

	ret = rv_get_task_monitor_slot();
	if (ret < 0)
		return ret;

	rv_%%MODEL_NAME%%_task_slot = ret;

	rv_attach_trace_probe("%%MODEL_NAME%%", task_newtask, handle_task_newtask);

	read_lock(&tasklist_lock);

	for_each_process_thread(g, p)
		init_monitor(p);

	for_each_present_cpu(cpu)
		init_monitor(idle_task(cpu));

	read_unlock(&tasklist_lock);

	return 0;
}

void rv_%%MODEL_NAME%%_destroy(void)
{
	rv_put_task_monitor_slot(rv_%%MODEL_NAME%%_task_slot);
	rv_%%MODEL_NAME%%_task_slot = RV_PER_TASK_MONITOR_INIT;

	rv_detach_trace_probe("%%MODEL_NAME%%", task_newtask, handle_task_newtask);
}

static void illegal_state(struct task_struct *task, struct ltl_monitor *mon)
{
	mon->state = INIT;
	rv_%%MODEL_NAME%%_error(task, mon);
}

static void rv_%%MODEL_NAME%%_attempt_start(struct task_struct *task, struct ltl_monitor *mon)
{
	int i;

	mon = rv_%%MODEL_NAME%%_get_monitor(task);

	rv_%%MODEL_NAME%%_atoms_fetch(task, mon);

	for (i = 0; i < NUM_ATOM; ++i) {
		if (mon->atoms[i] == LTL_UNDETERMINED)
			return;
	}

%%BUCHI_START%%
}

static void rv_%%MODEL_NAME%%_step(struct task_struct *task, struct ltl_monitor *mon)
{
	switch (mon->state) {
%%BUCHI_TRANSITIONS%%
	case DEAD:
	case INIT:
		break;
	default:
		WARN_ON_ONCE(1);
	}
}

void rv_%%MODEL_NAME%%_atom_update(struct task_struct *task, unsigned int atom, bool value)
{
	struct ltl_monitor *mon = rv_%%MODEL_NAME%%_get_monitor(task);

	rv_%%MODEL_NAME%%_atom_set(mon, atom, value);

	if (mon->state == DEAD)
		return;

	if (mon->state == INIT)
		rv_%%MODEL_NAME%%_attempt_start(task, mon);
	if (mon->state == INIT)
		return;

	mon->atoms[atom] = value;

	rv_%%MODEL_NAME%%_atoms_fetch(task, mon);

	rv_%%MODEL_NAME%%_step(task, mon);
}
