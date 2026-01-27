// SPDX-License-Identifier: GPL-2.0+
/*
 * Experimental bug-reproducer kernel module
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 *
 * Authors: Paul E. McKenney <paulmck@kernel.org>
 */

#define pr_fmt(fmt) fmt

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/torture.h>

MODULE_DESCRIPTION("Experimental bug-reproducr facility");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Paul E. McKenney <paulmckrcu@meta.com>");

#define REPRO_FLAG "-repro:"
#define REPROOUT_STRING(s) \
	pr_alert("%s" REPRO_FLAG " %s\n", scale_type, s)
#define VERBOSE_REPROOUT_STRING(s) \
	do { if (verbose) pr_alert("%s" REPRO_FLAG " %s\n", scale_type, s); } while (0)
#define REPROOUT_ERRSTRING(s) \
	pr_alert("%s" REPRO_FLAG "!!! %s\n", scale_type, s)

/*
 * The intended use cases for the nreaders and nwriters module parameters
 * are as follows:
 *
 * 1.	Specify neither the nreaders nor nwriters kernel boot parameters.
 *	This will set nreaders to the number of online CPUs and nwriters
 *	to 1.
 *
 * 2.	Specify only the nreaders kernel boot parameter.  This will
 *	set nwriters to 1.
 *
 * 3.	Specify only the nwriters kernel boot parameter.  This will
 *	set nreaders to the number of online CPUs.
 *
 * 4.	Specifying both does what you would expect.
 */

#ifdef MODULE
# define REPRO_SHUTDOWN 0
#else
# define REPRO_SHUTDOWN 1
#endif

torture_param(int, holdoff, 10, "Holdoff time before test start (s)");
torture_param(int, nreaders, -1, "Number of repro reader threads");
torture_param(int, nspinners, -1, "Number of repro spinner threads");
torture_param(int, ntimers, -1, "Number of repro timer threads");
torture_param(int, nwriters, -1, "Number of repro updater threads");
torture_param(int, reader_hold, 100, "Time to spin (us)");
torture_param(int, reader_wait, 10, "Time to wait between spins (us)");
torture_param(int, shutdown_secs, 0, "Shutdown time (s), <= zero to disable");
torture_param(int, spinner_hold, 100, "Time to spin on each pass through loop (us)");
torture_param(int, spinner_nice, -10, "Nice-value for spinner priority");
torture_param(int, stat_interval, 60, "Number of seconds between stats printk()s");
torture_param(int, verbose, 1, "Enable verbose debugging printk()s");
torture_param(int, writer_hold, 1000, "Time to write-hold lock (us)");
torture_param(int, writer_wait, 1000, "Time to between write acquisitions (us)");

static char *scale_type = "rwsem";
module_param(scale_type, charp, 0444);
MODULE_PARM_DESC(scale_type, "Type of sleeplock to use (rwsem, ...)");

static int nrealreaders;
static int nrealspinners;
static int nrealtimers;
static int nrealwriters;
static struct task_struct **reader_tasks;
static struct task_struct **spinner_tasks;
static struct task_struct *stats_task;
static struct task_struct **timer_tasks;
static struct task_struct **writer_tasks;
static struct task_struct *shutdown_task;

static atomic_t n_repro_reader_started;
static atomic_t n_repro_reader_finished;
static atomic_t n_repro_spinner_started;
static atomic_t n_repro_spinner_finished;
static atomic_long_t n_repro_stats_jmax;
static atomic_t n_repro_timer_started;
static atomic_t n_repro_timer_finished;
static atomic_long_t n_repro_timer_jmax;
static atomic_t n_repro_writer_started;
static atomic_t n_repro_writer_finished;
static atomic_long_t n_repro_writer_jmax;

/*
 * Operations vector for selecting different types of tests.
 */

struct repro_ops {
	void (*init)(void);
	void (*cleanup)(void);
	void (*readlock)(void);
	void (*readunlock)(void);
	void (*writelock)(void);
	void (*writeunlock)(void);
	void (*stats)(void);
	const char *name;
};

static struct repro_ops *cur_ops;

/*
 * Definitions for rwsem reproducer testing.
 */

static DECLARE_RWSEM(repro_rwsem);

static void rwsem_read_lock(void)
{
	down_read(&repro_rwsem);
}

static void rwsem_read_unlock(void)
{
	up_read(&repro_rwsem);
}

static void rwsem_write_lock(void)
{
	down_write(&repro_rwsem);
}

static void rwsem_write_unlock(void)
{
	up_write(&repro_rwsem);
}

static struct repro_ops rwsem_ops = {
	.readlock	= rwsem_read_lock,
	.readunlock	= rwsem_read_unlock,
	.writelock	= rwsem_write_lock,
	.writeunlock	= rwsem_write_unlock,
	.name		= "rwsem"
};

/*
 * If scalability tests complete, wait for shutdown to commence.
 */
static void repro_wait_shutdown(void)
{
	if (atomic_read(&n_repro_writer_finished) < nrealwriters ||
	    atomic_read(&n_repro_timer_finished) < nrealwriters ||
	    atomic_read(&n_repro_reader_finished) < nrealreaders) {
		cond_resched();
		return;
	}
	while (!torture_must_stop())
		schedule_timeout_uninterruptible(1);
}

/*
 * Reproducer reader kthread.  Repeatedly does read-side critical section,
 * minimizing update-side interference.
 */
static int
repro_reader(void *arg)
{
	long me = (long)arg;
	DEFINE_TORTURE_RANDOM(trs);

	VERBOSE_REPROOUT_STRING("repro_reader task started");
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	atomic_inc(&n_repro_reader_started);

	if (holdoff) {
		schedule_timeout_idle(holdoff * HZ);
		VERBOSE_REPROOUT_STRING("repro_reader holdoff complete");
	}

	do {
		cur_ops->readlock();
		udelay(reader_hold);
		cur_ops->readunlock();
		torture_hrtimeout_us(reader_wait, reader_wait, &trs);
		repro_wait_shutdown();
	} while (!torture_must_stop());
	atomic_inc(&n_repro_reader_finished);
	torture_kthread_stopping("repro_reader");
	return 0;
}

/*
 * Reproducer spinner (CPU hog) kthread.  Repeatedly does pretty much
 * nothing.
 */
static int
repro_spinner(void *arg)
{
	long me = (long)arg;

	VERBOSE_REPROOUT_STRING("repro_spinner task started");
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	sched_set_normal(current, spinner_nice);
	atomic_inc(&n_repro_spinner_started);

	if (holdoff) {
		schedule_timeout_idle(holdoff * HZ);
		VERBOSE_REPROOUT_STRING("repro_spinner holdoff complete");
	}

	do {
		udelay(spinner_hold);
		repro_wait_shutdown();
	} while (!torture_must_stop());
	atomic_inc(&n_repro_spinner_finished);
	torture_kthread_stopping("repro_spinner");
	return 0;
}

static void repro_stats_print(void)
{
	pr_alert("%s" REPRO_FLAG
		 "--- repro_stats n_repro_stats_jmax=%lu n_repro_timer_jmax=%lu writer_jmax=%lu (jiffies)\n",
		 scale_type, atomic_long_read(&n_repro_stats_jmax),
		 atomic_long_read(&n_repro_timer_jmax), atomic_long_read(&n_repro_writer_jmax));
}

/*
 * Periodically prints torture statistics, if periodic statistics printing
 * was specified via the stat_interval module parameter.
 */
static int repro_stats(void *arg)
{
	int cpu = cpumask_first(cpu_online_mask);
	unsigned long j;
	unsigned long jmax = 0; // Maximum excess hrtimer delay in jiffies.

	VERBOSE_REPROOUT_STRING("repro_stats task started");
	if (cpu < nr_cpu_ids)
		set_cpus_allowed_ptr(current, cpumask_of(cpu));
	sched_set_normal(current, -20);
	do {
		j = jiffies;
		torture_hrtimeout_s(stat_interval, 0, NULL);
		j = jiffies - j - stat_interval * HZ;
		if (j > (unsigned long)LONG_MAX)
			j = 0;
		if (j > jmax)
			jmax = j;
		j = atomic_long_read(&n_repro_stats_jmax);
		if (jmax > j)
			(void)atomic_long_try_cmpxchg(&n_repro_stats_jmax, &j, jmax);
		repro_stats_print();
		torture_shutdown_absorb("repro_stats");
	} while (!torture_must_stop());
	j = atomic_long_read(&n_repro_stats_jmax);
	while (jmax > j)
		(void)atomic_long_try_cmpxchg(&n_repro_stats_jmax, &j, jmax);
	torture_kthread_stopping("repro_stats");
	return 0;
}

/*
 * Each pass waits a jiffy, then records delay.  Default priority and
 * not bound to a CPU.
 */
static int repro_timer(void *arg)
{
	unsigned long j;
	unsigned long jmax = 0; // Maximum timer delay in jiffies.
	const unsigned long jwait = 10;

	VERBOSE_REPROOUT_STRING("repro_timer task started");
	atomic_inc(&n_repro_timer_started);

	if (holdoff) {
		schedule_timeout_idle(holdoff * HZ);
		VERBOSE_REPROOUT_STRING("repro_timer holdoff complete");
	}
	do {
		j = jiffies;
		torture_hrtimeout_jiffies(jwait, NULL);
		j = jiffies - j - jwait;
		if (j > (unsigned long)LONG_MAX)
			j = 0;
		if (j > jmax)
			jmax = j;
		j = atomic_long_read(&n_repro_timer_jmax);
		if (jmax > j)
			(void)atomic_long_try_cmpxchg(&n_repro_timer_jmax, &j, jmax);
		torture_shutdown_absorb("repro_timer");
	} while (!torture_must_stop());
	j = atomic_long_read(&n_repro_timer_jmax);
	while (jmax > j)
		(void)atomic_long_try_cmpxchg(&n_repro_timer_jmax, &j, jmax);
	atomic_inc(&n_repro_timer_finished);
	torture_kthread_stopping("repro_timer");
	return 0;
}

/*
 * Reproducer writer kthread.  Repeatedly write-acquires.
 */
static int
repro_writer(void *arg)
{
	unsigned long j;
	unsigned long jmax = 0; // Maximum lock-acquisition delay in jiffies.
	long me = (long)arg;
	DEFINE_TORTURE_RANDOM(trs);

	VERBOSE_REPROOUT_STRING("repro_writer task started");
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	current->flags |= PF_NO_SETAFFINITY;
	atomic_inc(&n_repro_writer_started);

	if (holdoff) {
		schedule_timeout_idle(holdoff * HZ);
		VERBOSE_REPROOUT_STRING("repro_writer holdoff complete");
	}

	/*
	 * Wait until rcu_end_inkernel_boot() is called in order to
	 * avoid competing with the boot sequence.
	 */
	while (system_state != SYSTEM_RUNNING)
		schedule_timeout_uninterruptible(1);

	do {
		j = jiffies;
		cur_ops->writelock();
		j = jiffies - j;
		if (j > jmax)
			jmax = j;
		j = atomic_long_read(&n_repro_writer_jmax);
		if (jmax > j)
			(void)atomic_long_try_cmpxchg(&n_repro_writer_jmax, &j, jmax);
		udelay(writer_hold);
		cur_ops->writeunlock();
		torture_hrtimeout_us(writer_wait, writer_wait, &trs);
		repro_wait_shutdown();
	} while (!torture_must_stop());
	j = atomic_long_read(&n_repro_writer_jmax);
	while (jmax > j)
		(void)atomic_long_try_cmpxchg(&n_repro_writer_jmax, &j, jmax);
	atomic_inc(&n_repro_writer_finished);
	torture_kthread_stopping("repro_writer");
	return 0;
}

static void
repro_print_module_parms(struct repro_ops *cur_ops, const char *tag)
{
	pr_alert("%s" REPRO_FLAG
		 "--- %s: nreaders=%d nspinners=%d ntimers=%d nwriters=%d reader_hold=%d reader_wait=%d shutdown_secs=%d stat_interval=%d writer_hold=%d writer_wait=%d verbose=%d\n",
		 scale_type, tag, nrealreaders, nspinners, nrealtimers, nrealwriters, reader_hold, reader_wait, shutdown_secs, stat_interval, writer_hold, writer_wait, verbose);
}

/*
 * Return the number if non-negative.  If -1, the number of CPUs.
 * If less than -1, that much less than the number of CPUs, but
 * at least one.
 */
static int compute_real(int n)
{
	int nr;

	if (n >= 0) {
		nr = n;
	} else {
		nr = num_online_cpus() + 1 + n;
		if (nr <= 0)
			nr = 1;
	}
	return nr;
}

static void
repro_cleanup(void)
{
	int i;

	// If built-in, just report all of the GP kthread's CPU time.

	if (torture_cleanup_begin())
		return;
	if (!cur_ops) {
		torture_cleanup_end();
		return;
	}

	// Stop spinners first because they are usually higher priority.
	if (spinner_tasks) {
		if (spinner_nice < -20 || spinner_nice > 19) {
			WARN_ON(!IS_MODULE(CONFIG_REPRO_TEST));
			spinner_nice = 0;
		}
		for (i = 0; i < nrealspinners; i++) {
			WARN_ON_ONCE(task_nice(spinner_tasks[i]) != spinner_nice);
			torture_stop_kthread(repro_spinner, spinner_tasks[i]);
		}
		kfree(spinner_tasks);
		spinner_tasks = NULL;
	}
	torture_stop_kthread(repro_stats, stats_task);

	if (reader_tasks) {
		for (i = 0; i < nrealreaders; i++)
			torture_stop_kthread(repro_reader, reader_tasks[i]);
		kfree(reader_tasks);
		reader_tasks = NULL;
	}

	if (timer_tasks) {
		for (i = 0; i < nrealtimers; i++)
			torture_stop_kthread(repro_timer, timer_tasks[i]);
		kfree(timer_tasks);
		timer_tasks = NULL;
	}

	if (writer_tasks) {
		for (i = 0; i < nrealwriters; i++)
			torture_stop_kthread(repro_writer, writer_tasks[i]);
		kfree(writer_tasks);
		writer_tasks = NULL;
	}

	/* Do torture-type-specific cleanup operations.  */
	if (cur_ops->cleanup != NULL)
		cur_ops->cleanup();

	repro_print_module_parms(cur_ops, "End of test: SUCCESS");

	torture_cleanup_end();
}

/*
 * Reproducer shutdown kthread.  Just waits to be awakened, then shuts
 * down system.
 */
static int
repro_shutdown(void *arg)
{
	REPROOUT_STRING("Invoked repro_shutdown.");
	sched_set_normal(current, -20);
	torture_hrtimeout_s(shutdown_secs, 0, NULL);
	REPROOUT_STRING("Reached shutdown_secs in repro_shutdown.");
	WARN(atomic_read(&n_repro_writer_started) < nrealwriters ||
	     atomic_read(&n_repro_spinner_started) < nrealspinners ||
	     atomic_read(&n_repro_reader_started) < nrealreaders,
	     "%s: Initialization incomplete, %d of %d readers, %d of %d timers, and %d of %d writers",
	     __func__,
	     atomic_read(&n_repro_reader_started), nrealreaders,
	     atomic_read(&n_repro_timer_started), nrealtimers,
	     atomic_read(&n_repro_writer_started), nrealwriters);
	repro_cleanup();
	kernel_power_off();
	return -EINVAL;
}

static int __init
repro_init(void)
{
	int firsterr = 0;
	long i;
	static struct repro_ops *scale_ops[] = {
		&rwsem_ops,
	};

	if (!torture_init_begin(scale_type, verbose))
		return -EBUSY;

	/* Process args and announce that the alleged reproducer is on the job. */
	for (i = 0; i < ARRAY_SIZE(scale_ops); i++) {
		cur_ops = scale_ops[i];
		if (strcmp(scale_type, cur_ops->name) == 0)
			break;
	}
	if (i == ARRAY_SIZE(scale_ops)) {
		pr_alert("torture-repro: invalid repro type: \"%s\"\n", scale_type);
		pr_alert("torture-repro types:");
		for (i = 0; i < ARRAY_SIZE(scale_ops); i++)
			pr_cont(" %s", scale_ops[i]->name);
		pr_cont("\n");
		firsterr = -EINVAL;
		cur_ops = NULL;
		goto unwind;
	}
	if (cur_ops->init)
		cur_ops->init();


	if (nwriters == -1) {
		nrealwriters = 1;
	} else {
		nrealwriters = compute_real(nwriters);
	}
	nrealspinners = compute_real(nspinners);
	nrealtimers = compute_real(ntimers);
	nrealreaders = compute_real(nreaders);
	atomic_set(&n_repro_reader_started, 0);
	atomic_set(&n_repro_spinner_started, 0);
	atomic_set(&n_repro_writer_started, 0);
	atomic_set(&n_repro_writer_finished, 0);
	atomic_set(&n_repro_spinner_finished, 0);
	atomic_set(&n_repro_reader_finished, 0);
	atomic_long_set(&n_repro_timer_jmax, 0);
	atomic_long_set(&n_repro_writer_jmax, 0);
	repro_print_module_parms(cur_ops, "Start of test");

	/* Start up the kthreads. */

	if (shutdown_secs > 0) {
		firsterr = torture_create_kthread(repro_shutdown, NULL,
						  shutdown_task);
		if (torture_init_error(firsterr))
			goto unwind;
		schedule_timeout_uninterruptible(1);
	}
	reader_tasks = kcalloc(nrealreaders, sizeof(reader_tasks[0]), GFP_KERNEL);
	if (!reader_tasks) {
		REPROOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}
	for (i = 0; i < nrealreaders; i++) {
		firsterr = torture_create_kthread(repro_reader, (void *)i, reader_tasks[i]);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	while (atomic_read(&n_repro_reader_started) < nrealreaders)
		schedule_timeout_uninterruptible(1);
	timer_tasks = kcalloc(nrealtimers, sizeof(timer_tasks[0]), GFP_KERNEL);
	if (!timer_tasks) {
		REPROOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}
	for (i = 0; i < nrealtimers; i++) {
		firsterr = torture_create_kthread(repro_timer, (void *)i, timer_tasks[i]);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	while (atomic_read(&n_repro_timer_started) < nrealtimers)
		schedule_timeout_uninterruptible(1);
	writer_tasks = kcalloc(nrealwriters, sizeof(writer_tasks[0]), GFP_KERNEL);
	if (!writer_tasks) {
		REPROOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}
	for (i = 0; i < nrealwriters; i++) {
		firsterr = torture_create_kthread(repro_writer, (void *)i, writer_tasks[i]);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	while (atomic_read(&n_repro_writer_started) < nrealwriters)
		schedule_timeout_uninterruptible(1);

	if (stat_interval > 0) {
		firsterr = torture_create_kthread(repro_stats, NULL, stats_task);
		if (torture_init_error(firsterr))
			goto unwind;
	}

	// Spinners last!  They might prevent others from getting started.
	spinner_tasks = kcalloc(nrealspinners, sizeof(spinner_tasks[0]), GFP_KERNEL);
	if (!spinner_tasks) {
		REPROOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}
	for (i = 0; i < nrealspinners; i++) {
		firsterr = torture_create_kthread(repro_spinner, (void *)i, spinner_tasks[i]);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	while (atomic_read(&n_repro_spinner_started) < nrealspinners)
		schedule_timeout_uninterruptible(1);
	torture_init_end();
	return 0;

unwind:
	torture_init_end();
	repro_cleanup();
	if (shutdown_secs > 0) {
		WARN_ON(!IS_MODULE(CONFIG_REPRO_TEST));
		kernel_power_off();
	}
	return firsterr;
}

module_init(repro_init);
module_exit(repro_cleanup);
