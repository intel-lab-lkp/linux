/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file is generated, do not edit.
 *
 * This file includes necessary functions to glue the Buchi automaton and the kernel together.
 * Some of these functions must be manually implemented (look for "Must be implemented", or just
 * let the compiler tells you).
 *
 * Essentially, you need to manually define the meaning of the atomic propositions in the LTL
 * property. The primary function for that is rv_%%MODEL_NAME%%_atom_update(), which can be called
 * in tracepoints' handlers for example. In some specific cases where
 * rv_%%MODEL_NAME%%_atom_update() is not convenient, rv_%%MODEL_NAME%%_atoms_fetch() can be used.
 *
 * rv_%%MODEL_NAME%%_init()/rv_%%MODEL_NAME%%_destroy() must be called while enabling/disabling
 * the monitor.
 *
 * If the fields in struct ltl_monitor is not enough, extra custom data can be used. See
 * rv_%%MODEL_NAME%%_get_data().
 */

#include <linux/sched.h>

enum %%MODEL_NAME%%_atom {
%%ATOM_LIST%%
	NUM_ATOM
};

/**
 * rv_%%MODEL_NAME%%_init
 * @data_size:	required custom data size, can be zero
 *
 * Must be called while enabling the monitor
 */
int rv_%%MODEL_NAME%%_init(size_t data_size);

/**
 * rv_%%MODEL_NAME%%_destroy
 *
 * must be called while disabling the monitor
 */
void rv_%%MODEL_NAME%%_destroy(void);

/**
 * rv_%%MODEL_NAME%%_error - report violation of the LTL property
 * @task:	the task violating the LTL property
 * @mon:	the LTL monitor
 *
 * Must be implemented. This function should invoke the RV reactor and the monitor's tracepoints.
 */
void rv_%%MODEL_NAME%%_error(struct task_struct *task, struct ltl_monitor *mon);

extern int rv_%%MODEL_NAME%%_task_slot;

/**
 * rv_%%MODEL_NAME%%_get_monitor - get the struct ltl_monitor of a task
 */
static inline struct ltl_monitor *rv_%%MODEL_NAME%%_get_monitor(struct task_struct *task)
{
	return &task->rv[rv_%%MODEL_NAME%%_task_slot].ltl_mon;
}

/**
 * rv_%%MODEL_NAME%%_atoms_init - initialize the atomic propositions
 * @task:	the task
 * @mon:	the LTL monitor
 *
 * Must be implemented. This function is called during task creation, and should initialize all
 * atomic propositions. rv_%%MODEL_NAME%%_atom_set() should be used to implement this function.
 *
 * This function does not have to initialize atomic propositions that are updated by
 * rv_%%MODEL_NAME%%_atoms_fetch(), because the two functions are called together.
 */
void rv_%%MODEL_NAME%%_atoms_init(struct task_struct *task, struct ltl_monitor *mon);

/**
 * rv_%%MODEL_NAME%%_atoms_fetch - fetch the atomic propositions
 * @task:	the task
 * @mon:	the LTL monitor
 *
 * Must be implemented. This function is called anytime the Buchi automaton is triggered. Its
 * intended purpose is to update the atomic propositions which are expensive to trace and can be
 * easily read from @task. rv_%%MODEL_NAME%%_atom_set() should be used to implement this function.
 *
 * Using this function may cause incorrect verification result if it is important for the LTL that
 * the atomic propositions must be updated at the correct time. Therefore, if it is possible,
 * updating atomic propositions should be done with rv_%%MODEL_NAME%%_atom_update() instead.
 *
 * An example where this function is useful is with the LTL property:
 *    always (RT imply not PAGEFAULT)
 * (a realtime task does not raise page faults)
 *
 * In this example, adding tracepoints to track RT is complicated, because it is changed in
 * differrent places (mutex's priority boosting, sched_setscheduler). Furthermore, for this LTL
 * property, we don't care exactly when RT changes, as long as we have its correct value when
 * PAGEFAULT==true. Therefore, it is better to update RT in rv_%%MODEL_NAME%%_atoms_fetch(), as it
 * can easily be retrieved from task_struct.
 *
 * This function can be empty.
 */
void rv_%%MODEL_NAME%%_atoms_fetch(struct task_struct *task, struct ltl_monitor *mon);

/**
 * rv_%%MODEL_NAME%%_atom_update - update an atomic proposition
 * @task:	the task
 * @atom:	the atomic proposition, one of enum %%MODEL_NAME%%_atom
 * @value:	the new value for @atom
 *
 * Update an atomic proposition and trigger the Buchi atomaton to check for violation of the LTL
 * property. This function can be called in tracepoints' handler, for example.
 */
void rv_%%MODEL_NAME%%_atom_update(struct task_struct *task, unsigned int atom, bool value);

/**
 * rv_%%MODEL_NAME%%_atom_get - get an atomic proposition
 * @mon:	the monitor
 * @atom:	the atomic proposition, one of enum %%MODEL_NAME%%_atom
 *
 * Returns the value of an atomic proposition.
 */
static inline
enum ltl_truth_value rv_%%MODEL_NAME%%_atom_get(struct ltl_monitor *mon, unsigned int atom)
{
	return mon->atoms[atom];
}

/**
 * rv_%%MODEL_NAME%%_atom_set - set an atomic proposition
 * @mon:	the monitor
 * @atom:	the atomic proposition, one of enum %%MODEL_NAME%%_atom
 * @value:	the new value for @atom
 *
 * Update an atomic proposition without triggering the Buchi automaton. This can be useful to
 * implement rv_%%MODEL_NAME%%_atoms_fetch() and rv_%%MODEL_NAME%%_atoms_init().
 *
 * Another use case for this function is when multiple atomic propositions change at the same time,
 * because calling rv_%%MODEL_NAME%%_atom_update() (and thus triggering the Buchi automaton)
 * multiple times may be incorrect. In that case, rv_%%MODEL_NAME%%_atom_set() can be used to avoid
 * triggering the Buchi automaton, and rv_%%MODEL_NAME%%_atom_update() is only used for the last
 * atomic proposition.
 */
static inline
void rv_%%MODEL_NAME%%_atom_set(struct ltl_monitor *mon, unsigned int atom, bool value)
{
	mon->atoms[atom] = value;
}

/**
 * rv_%%MODEL_NAME%%_get_data - get the custom data of this monitor.
 * @mon: the monitor
 *
 * If this function is used, rv_%%MODEL_NAME%%_init() must have been called with a positive
 * data_size.
 */
static inline void *rv_%%MODEL_NAME%%_get_data(struct ltl_monitor *mon)
{
	return &mon->data;
}
