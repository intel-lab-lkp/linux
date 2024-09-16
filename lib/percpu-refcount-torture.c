// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/percpu-refcount.h>
#include <linux/torture.h>

#include "percpu-refcount.h"

static int busted_early_ref_release;
module_param(busted_early_ref_release, int, 0444);
MODULE_PARM_DESC(busted_early_ref_release,
		 "Enable busted premature release of ref (default = 0), 0 = disable");

static int busted_late_ref_release;
module_param(busted_late_ref_release, int, 0444);
MODULE_PARM_DESC(busted_late_ref_release,
		 "Enable busted late release of ref (default = 0), 0 = disable");

static long delay_us = 10;
module_param(delay_us, long, 0444);
MODULE_PARM_DESC(delay_us,
		 "delay between reader refcount operations in microseconds (default = 10)");

static long nrefs = 2;
module_param(nrefs, long, 0444);
MODULE_PARM_DESC(nrefs, "Number of percpu refs (default = 2)");

static long niterations = 100;
module_param(niterations, long, 0444);
MODULE_PARM_DESC(niterations,
		 "Number of iterations of ref increment and decrement (default = 100)");

static long nusers = 2;
module_param(nusers, long, 0444);
MODULE_PARM_DESC(nusers, "Number of refcount users (default = 2)");

static int onoff_holdoff;
module_param(onoff_holdoff, int, 0444);
MODULE_PARM_DESC(onoff_holdoff, "Time after boot before CPU hotplugs (seconds)");

static int onoff_interval;
module_param(onoff_interval, int, 0444);
MODULE_PARM_DESC(onoff_interval, "Time between CPU hotplugs (jiffies), 0=disable");

static int stutter;
module_param(stutter, int, 0444);
MODULE_PARM_DESC(stutter, "Stutter period in jiffies (default = 0), 0 = disable");

static int verbose = 1;
module_param(verbose, int, 0444);
MODULE_PARM_DESC(verbose, "Enable verbose debugging printk()s");

static struct task_struct **ref_user_tasks;
static struct task_struct *ref_manager_task;
static struct task_struct **busted_early_release_tasks;
static struct task_struct **busted_late_release_tasks;

static struct percpu_ref *refs;
static long *num_per_ref_users;

static atomic_t running;
static atomic_t *ref_running;

static char *torture_type = "percpu-refcount";

static int percpu_ref_manager_thread(void *data)
{
	int i;

	while (atomic_read(&running) != 0) {
		percpu_ref_test_flush_release_work();
		stutter_wait("percpu_ref_manager_thread");
	}
	/* Ensure ordering with ref users */
	smp_mb();

	percpu_ref_test_flush_release_work();

	for (i = 0; i < nrefs; i++) {
		WARN(percpu_ref_test_is_percpu(&refs[i]),
			"!!! released ref %d should be in atomic mode", i);
		WARN(!percpu_ref_is_zero(&refs[i]),
			"!!! released ref %d should have 0 refcount", i);
	}

	do {
		stutter_wait("percpu_ref_manager_thread");
	} while (!torture_must_stop());

	torture_kthread_stopping("percpu_ref_manager_thread");

	return 0;
}

static int percpu_ref_test_thread(void *data)
{
	struct percpu_ref *ref = (struct percpu_ref *)data;
	int i = 0;

	percpu_ref_get(ref);

	do {
		percpu_ref_get(ref);
		udelay(delay_us);
		percpu_ref_put(ref);
		stutter_wait("percpu_ref_test_thread");
		i++;
	} while (i < niterations);

	atomic_dec(&ref_running[ref - refs]);
	/* Order ref release with ref_running[ref_idx] == 0 */
	smp_mb();
	percpu_ref_put(ref);
	/* Order ref decrement with running == 0 */
	smp_mb();
	atomic_dec(&running);

	do {
		stutter_wait("percpu_ref_test_thread");
	} while (!torture_must_stop());

	torture_kthread_stopping("percpu_ref_test_thread");

	return 0;
}

static int percpu_ref_busted_early_thread(void *data)
{
	struct percpu_ref *ref = (struct percpu_ref *)data;
	int ref_idx = ref - refs;
	int i = 0, j;

	do {
		/* Extra ref put momemtarily */
		for (j = 0; j < num_per_ref_users[ref_idx]; j++)
			percpu_ref_put(ref);
		stutter_wait("percpu_ref_busted_early_thread");
		for (j = 0; j < num_per_ref_users[ref_idx]; j++)
			percpu_ref_get(ref);
		i++;
		stutter_wait("percpu_ref_busted_early_thread");
	} while (i < niterations * 10);

	do {
		stutter_wait("percpu_ref_busted_early_thread");
	} while (!torture_must_stop());

	torture_kthread_stopping("percpu_ref_busted_early_thread");

	return 0;
}

static int percpu_ref_busted_late_thread(void *data)
{
	struct percpu_ref *ref = (struct percpu_ref *)data;
	int i = 0;

	do {
		/* Extra ref get momemtarily */
		percpu_ref_get(ref);
		stutter_wait("percpu_ref_busted_late_thread");
		percpu_ref_put(ref);
		i++;
	} while (i < niterations);

	do {
		stutter_wait("percpu_ref_busted_late_thread");
	} while (!torture_must_stop());

	torture_kthread_stopping("percpu_ref_busted_late_thread");

	return 0;
}

static void percpu_ref_test_cleanup(void)
{
	int i;

	if (torture_cleanup_begin())
		return;

	if (busted_late_release_tasks) {
		for (i = 0; i < nrefs; i++)
			torture_stop_kthread(busted_late_task, busted_late_release_tasks[i]);
		kfree(busted_late_release_tasks);
		busted_late_release_tasks = NULL;
	}

	if (busted_early_release_tasks) {
		for (i = 0; i < nrefs; i++)
			torture_stop_kthread(busted_early_task, busted_early_release_tasks[i]);
		kfree(busted_early_release_tasks);
		busted_early_release_tasks = NULL;
	}

	if (ref_manager_task) {
		torture_stop_kthread(ref_manager, ref_manager_task);
		ref_manager_task = NULL;
	}

	if (ref_user_tasks) {
		for (i = 0; i < nusers; i++)
			torture_stop_kthread(ref_user, ref_user_tasks[i]);
		kfree(ref_user_tasks);
		ref_user_tasks = NULL;
	}

	kfree(ref_running);
	ref_running = NULL;

	kfree(num_per_ref_users);
	num_per_ref_users = NULL;

	if (refs) {
		for (i = 0; i < nrefs; i++)
			percpu_ref_exit(&refs[i]);
		kfree(refs);
		refs = NULL;
	}

	torture_cleanup_end();
}

static void percpu_ref_test_release(struct percpu_ref *ref)
{
	WARN(!!atomic_add_return(0, &ref_running[ref-refs]), "!!! Premature ref release");
}

static int __init percpu_ref_torture_init(void)
{
	DEFINE_TORTURE_RANDOM(rand);
	struct torture_random_state *trsp = &rand;
	int flags;
	int err;
	int ref_idx;
	int i;

	if (!torture_init_begin("percpu-refcount", verbose))
		return -EBUSY;

	atomic_set(&running, nusers);
	/* Order @running with later increment and decrement operations */
	smp_mb();

	refs = kcalloc(nrefs, sizeof(refs[0]), GFP_KERNEL);
	if (!refs) {
		TOROUT_ERRSTRING("out of memory");
		err = -ENOMEM;
		goto init_err;
	}
	for (i = 0; i < nrefs; i++) {
		flags = torture_random(trsp) & 1 ? PERCPU_REF_INIT_ATOMIC : PERCPU_REF_REL_MANAGED;
		err = percpu_ref_init(&refs[i], percpu_ref_test_release,
				      flags, GFP_KERNEL);
		if (err)
			goto init_err;
		if (!(flags & PERCPU_REF_REL_MANAGED))
			percpu_ref_switch_to_managed(&refs[i]);
	}

	num_per_ref_users = kcalloc(nrefs, sizeof(num_per_ref_users[0]), GFP_KERNEL);
	if (!num_per_ref_users) {
		TOROUT_ERRSTRING("out of memory");
		err = -ENOMEM;
		goto init_err;
	}
	for (i = 0; i < nrefs; i++)
		num_per_ref_users[i] = 0;

	ref_user_tasks = kcalloc(nusers, sizeof(ref_user_tasks[0]), GFP_KERNEL);
	if (!ref_user_tasks) {
		TOROUT_ERRSTRING("out of memory");
		err = -ENOMEM;
		goto init_err;
	}

	ref_running = kcalloc(nrefs, sizeof(ref_running[0]), GFP_KERNEL);
	if (!ref_running) {
		TOROUT_ERRSTRING("out of memory");
		err = -ENOMEM;
		goto init_err;
	}

	for (i = 0; i < nusers; i++) {
		ref_idx = torture_random(trsp) % nrefs;
		atomic_inc(&ref_running[ref_idx]);
		num_per_ref_users[ref_idx]++;
		/* Order increments with subquent reads */
		smp_mb();
		err = torture_create_kthread(percpu_ref_test_thread,
					     &refs[ref_idx], ref_user_tasks[i]);
		if (torture_init_error(err))
			goto init_err;
	}

	err = torture_create_kthread(percpu_ref_manager_thread, NULL, ref_manager_task);
	if (torture_init_error(err))
		goto init_err;

	/* Drop initial reference, after test threads have started running */
	udelay(1);
	for (i = 0; i < nrefs; i++)
		percpu_ref_put(&refs[i]);


	if (busted_early_ref_release) {
		busted_early_release_tasks = kcalloc(nrefs,
						     sizeof(busted_early_release_tasks[0]),
						     GFP_KERNEL);
		if (!busted_early_release_tasks) {
			TOROUT_ERRSTRING("out of memory");
			err = -ENOMEM;
			goto init_err;
		}
		for (i = 0; i < nrefs; i++) {
			err = torture_create_kthread(percpu_ref_busted_early_thread,
					     &refs[i], busted_early_release_tasks[i]);
			if (torture_init_error(err))
				goto init_err;
		}
	}

	if (busted_late_ref_release) {
		busted_late_release_tasks = kcalloc(nrefs, sizeof(busted_late_release_tasks[0]),
						    GFP_KERNEL);
		if (!busted_late_release_tasks) {
			TOROUT_ERRSTRING("out of memory");
			err = -ENOMEM;
			goto init_err;
		}
		for (i = 0; i < nrefs; i++) {
			err = torture_create_kthread(percpu_ref_busted_late_thread,
					     &refs[i], busted_late_release_tasks[i]);
			if (torture_init_error(err))
				goto init_err;
		}
	}
	if (stutter) {
		err = torture_stutter_init(stutter, stutter);
		if (torture_init_error(err))
			goto init_err;
	}

	err = torture_onoff_init(onoff_holdoff * HZ, onoff_interval, NULL);
	if (torture_init_error(err))
		goto init_err;

	torture_init_end();
	return 0;
init_err:
	torture_init_end();
	percpu_ref_test_cleanup();
	return err;
}

static void __exit percpu_ref_torture_exit(void)
{
	percpu_ref_test_cleanup();
}

module_init(percpu_ref_torture_init);
module_exit(percpu_ref_torture_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("percpu refcount torture test");
