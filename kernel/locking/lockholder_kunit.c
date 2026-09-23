// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for sleeping lock holder tracking: check that
 * task_nr_tracked_locks(current) always equals the number of opted-in
 * locks the task holds.
 */
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/lockholder.h>
#include <linux/mutex.h>
#include <linux/percpu-rwsem.h>
#include <linux/rwsem.h>
#include <linux/sched.h>

struct lockholder_ctx {
	struct mutex		tmutex;
	struct mutex		tmutex2;
	struct mutex		pmutex;
	struct rw_semaphore	tsem;
	struct rw_semaphore	tsem2;
	struct rw_semaphore	psem;
	struct percpu_rw_semaphore	tpcpu;
	struct percpu_rw_semaphore	ppcpu;
	bool			pcpu_ready;

	/* Count at test start. Tests compare against it, not against zero. */
	unsigned int		base;

	/* Helper thread that holds a lock until told to release it. */
	struct completion	helper_holds;
	struct completion	helper_may_release;
	struct completion	helper_done;
	unsigned int		helper_depth_held;
	unsigned int		helper_depth_after;
};

/* Check the count relative to the start of the test. */
#define EXPECT_DEPTH(test, ctx, n)					\
	KUNIT_EXPECT_EQ((test), (ctx)->base + (unsigned int)(n),	\
			task_nr_tracked_locks(current))

#define ASSERT_DEPTH(test, ctx, n)					\
	KUNIT_ASSERT_EQ((test), (ctx)->base + (unsigned int)(n),	\
			task_nr_tracked_locks(current))

static int lockholder_init(struct kunit *test)
{
	struct lockholder_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	mutex_init(&ctx->tmutex);
	mutex_init(&ctx->tmutex2);
	mutex_init(&ctx->pmutex);
	init_rwsem(&ctx->tsem);
	init_rwsem(&ctx->tsem2);
	init_rwsem(&ctx->psem);

	mutex_track_holder(&ctx->tmutex);
	mutex_track_holder(&ctx->tmutex2);
	rwsem_track_holder(&ctx->tsem);
	rwsem_track_holder(&ctx->tsem2);

	KUNIT_ASSERT_EQ(test, 0, percpu_init_rwsem(&ctx->tpcpu));
	if (percpu_init_rwsem(&ctx->ppcpu)) {
		percpu_free_rwsem(&ctx->tpcpu);
		KUNIT_ASSERT_TRUE(test, false);
	}
	ctx->pcpu_ready = true;
	percpu_rwsem_track_holder(&ctx->tpcpu);

	init_completion(&ctx->helper_holds);
	init_completion(&ctx->helper_may_release);
	init_completion(&ctx->helper_done);

	ctx->base = task_nr_tracked_locks(current);
	test->priv = ctx;
	return 0;
}

static void lockholder_exit(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	if (ctx && ctx->pcpu_ready) {
		percpu_free_rwsem(&ctx->tpcpu);
		percpu_free_rwsem(&ctx->ppcpu);
		ctx->pcpu_ready = false;
	}
}

/* Locks that did not opt in are never counted. */
static void untracked_locks_are_not_counted(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	mutex_lock(&ctx->pmutex);
	EXPECT_DEPTH(test, ctx, 0);
	mutex_unlock(&ctx->pmutex);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_EXPECT_TRUE(test, mutex_trylock(&ctx->pmutex));
	EXPECT_DEPTH(test, ctx, 0);
	mutex_unlock(&ctx->pmutex);

	down_read(&ctx->psem);
	EXPECT_DEPTH(test, ctx, 0);
	up_read(&ctx->psem);

	down_write(&ctx->psem);
	EXPECT_DEPTH(test, ctx, 0);
	up_write(&ctx->psem);
	EXPECT_DEPTH(test, ctx, 0);

	percpu_down_read(&ctx->ppcpu);
	EXPECT_DEPTH(test, ctx, 0);
	percpu_up_read(&ctx->ppcpu);

	percpu_down_write(&ctx->ppcpu);
	EXPECT_DEPTH(test, ctx, 0);
	percpu_up_write(&ctx->ppcpu);
	EXPECT_DEPTH(test, ctx, 0);
}

static void tracked_mutex_is_counted(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	mutex_lock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 1);
	mutex_unlock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_EQ(test, 0, mutex_lock_interruptible(&ctx->tmutex));
	EXPECT_DEPTH(test, ctx, 1);
	mutex_unlock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_EQ(test, 0, mutex_lock_killable(&ctx->tmutex));
	EXPECT_DEPTH(test, ctx, 1);
	mutex_unlock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);

	mutex_lock_io(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 1);
	mutex_unlock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_TRUE(test, mutex_trylock(&ctx->tmutex));
	EXPECT_DEPTH(test, ctx, 1);
	mutex_unlock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);
}

static void tracked_rwsem_read_is_counted(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	down_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 1);
	up_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_EQ(test, 0, down_read_killable(&ctx->tsem));
	EXPECT_DEPTH(test, ctx, 1);
	up_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_EQ(test, 0, down_read_interruptible(&ctx->tsem));
	EXPECT_DEPTH(test, ctx, 1);
	up_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_EQ(test, 1, down_read_trylock(&ctx->tsem));
	EXPECT_DEPTH(test, ctx, 1);
	up_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);
}

static void tracked_rwsem_write_is_counted(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	down_write(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 1);
	up_write(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_EQ(test, 0, down_write_killable(&ctx->tsem));
	EXPECT_DEPTH(test, ctx, 1);
	up_write(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_EQ(test, 1, down_write_trylock(&ctx->tsem));
	EXPECT_DEPTH(test, ctx, 1);
	up_write(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);
}

static void tracked_percpu_rwsem_is_counted(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	percpu_down_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);

	percpu_down_read_freezable(&ctx->tpcpu, false);
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_ASSERT_TRUE(test, percpu_down_read_trylock(&ctx->tpcpu));
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);

	percpu_down_write(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_write(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);

	scoped_guard(percpu_read, &ctx->tpcpu)
		EXPECT_DEPTH(test, ctx, 1);
	EXPECT_DEPTH(test, ctx, 0);

	scoped_guard(percpu_write, &ctx->tpcpu)
		EXPECT_DEPTH(test, ctx, 1);
	EXPECT_DEPTH(test, ctx, 0);
}

/*
 * percpu_rwsem_release()/percpu_rwsem_acquire() move the count with the
 * lock.
 */
static void percpu_rwsem_handover_moves_the_count(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	percpu_down_write(&ctx->tpcpu);
	ASSERT_DEPTH(test, ctx, 1);

	/* As if returning to user space with the lock held. */
	percpu_rwsem_release(&ctx->tpcpu, _THIS_IP_);
	EXPECT_DEPTH(test, ctx, 0);

	/* Take it back before releasing it. */
	percpu_rwsem_acquire(&ctx->tpcpu, false, _THIS_IP_);
	EXPECT_DEPTH(test, ctx, 1);

	percpu_up_write(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);
}

/* The percpu read slow path, used right after a writer, is counted too. */
static void percpu_rwsem_slow_read_path_is_counted(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	/* A writer forces readers onto the slow path for a while. */
	percpu_down_write(&ctx->tpcpu);
	ASSERT_DEPTH(test, ctx, 1);
	percpu_up_write(&ctx->tpcpu);
	ASSERT_DEPTH(test, ctx, 0);

	/*
	 * rcu_sync stays non-idle for a grace period after a writer, so
	 * this most likely uses the slow path.
	 */
	percpu_down_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_EXPECT_TRUE(test, percpu_down_read_trylock(&ctx->tpcpu));
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);
}

/* downgrade_write() keeps the lock held, so the count does not change. */
static void downgrade_write_keeps_the_count(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	down_write(&ctx->tsem);
	ASSERT_DEPTH(test, ctx, 1);

	downgrade_write(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 1);

	up_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);
}

/* Tracked locks add up; untracked ones in between do not count. */
static void nested_locks_stack(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	mutex_lock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 1);

	mutex_lock(&ctx->pmutex);
	EXPECT_DEPTH(test, ctx, 1);

	down_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 2);

	down_write(&ctx->psem);
	EXPECT_DEPTH(test, ctx, 2);

	mutex_lock(&ctx->tmutex2);
	EXPECT_DEPTH(test, ctx, 3);

	down_read(&ctx->tsem2);
	EXPECT_DEPTH(test, ctx, 4);

	percpu_down_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 5);

	percpu_down_read(&ctx->ppcpu);
	EXPECT_DEPTH(test, ctx, 5);

	/* Release in a different order than taken. */
	up_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 4);
	percpu_up_read(&ctx->ppcpu);
	EXPECT_DEPTH(test, ctx, 4);
	up_write(&ctx->psem);
	EXPECT_DEPTH(test, ctx, 4);
	percpu_up_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 3);
	up_read(&ctx->tsem2);
	EXPECT_DEPTH(test, ctx, 2);
	mutex_unlock(&ctx->pmutex);
	EXPECT_DEPTH(test, ctx, 2);
	mutex_unlock(&ctx->tmutex2);
	EXPECT_DEPTH(test, ctx, 1);
	mutex_unlock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);
}

/* cleanup.h guards are counted too. */
static void guards_are_counted(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	scoped_guard(mutex, &ctx->tmutex)
		EXPECT_DEPTH(test, ctx, 1);
	EXPECT_DEPTH(test, ctx, 0);

	scoped_guard(rwsem_read, &ctx->tsem)
		EXPECT_DEPTH(test, ctx, 1);
	EXPECT_DEPTH(test, ctx, 0);

	scoped_guard(rwsem_write, &ctx->tsem)
		EXPECT_DEPTH(test, ctx, 1);
	EXPECT_DEPTH(test, ctx, 0);
}

/*
 * The helper thread takes a lock and records its own count. It is used to
 * create contention and to check that counts are per task.
 */
enum helper_lock {
	HELPER_TMUTEX,
	HELPER_TSEM_WRITE,
	HELPER_TPCPU_WRITE,
};

struct helper_arg {
	struct lockholder_ctx	*ctx;
	enum helper_lock	which;
};

static int lockholder_helper(void *data)
{
	struct helper_arg *arg = data;
	struct lockholder_ctx *ctx = arg->ctx;

	switch (arg->which) {
	case HELPER_TMUTEX:
		mutex_lock(&ctx->tmutex);
		break;
	case HELPER_TSEM_WRITE:
		down_write(&ctx->tsem);
		break;
	case HELPER_TPCPU_WRITE:
		percpu_down_write(&ctx->tpcpu);
		break;
	}

	ctx->helper_depth_held = task_nr_tracked_locks(current);
	complete(&ctx->helper_holds);

	wait_for_completion(&ctx->helper_may_release);

	switch (arg->which) {
	case HELPER_TMUTEX:
		mutex_unlock(&ctx->tmutex);
		break;
	case HELPER_TSEM_WRITE:
		up_write(&ctx->tsem);
		break;
	case HELPER_TPCPU_WRITE:
		percpu_up_write(&ctx->tpcpu);
		break;
	}

	ctx->helper_depth_after = task_nr_tracked_locks(current);
	complete(&ctx->helper_done);

	/* Wait for kthread_stop(). */
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ / 10);
	return 0;
}

static struct task_struct *start_helper(struct kunit *test,
					enum helper_lock which)
{
	struct lockholder_ctx *ctx = test->priv;
	struct helper_arg *arg;
	struct task_struct *t;

	arg = kunit_kzalloc(test, sizeof(*arg), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, arg);
	arg->ctx = ctx;
	arg->which = which;

	t = kthread_run(lockholder_helper, arg, "lockholder_kunit");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t));

	wait_for_completion(&ctx->helper_holds);
	return t;
}

static void stop_helper(struct kunit *test, struct task_struct *t)
{
	struct lockholder_ctx *ctx = test->priv;

	complete(&ctx->helper_may_release);
	wait_for_completion(&ctx->helper_done);
	kthread_stop(t);
}

/* Each task has its own count, and a new task starts at zero. */
static void the_count_is_per_task(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;
	struct task_struct *t;

	down_read(&ctx->tsem2);
	ASSERT_DEPTH(test, ctx, 1);

	t = start_helper(test, HELPER_TMUTEX);

	/* The helper's lock is not in our count. */
	EXPECT_DEPTH(test, ctx, 1);
	/* The helper started from zero, not from our count. */
	KUNIT_EXPECT_EQ(test, 1u, ctx->helper_depth_held);

	stop_helper(test, t);
	KUNIT_EXPECT_EQ(test, 0u, ctx->helper_depth_after);

	EXPECT_DEPTH(test, ctx, 1);
	up_read(&ctx->tsem2);
	EXPECT_DEPTH(test, ctx, 0);
}

/* A failed trylock is not counted. */
static void failed_trylock_is_not_counted(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;
	struct task_struct *t;

	t = start_helper(test, HELPER_TSEM_WRITE);

	KUNIT_EXPECT_EQ(test, 0, down_read_trylock(&ctx->tsem));
	EXPECT_DEPTH(test, ctx, 0);

	KUNIT_EXPECT_EQ(test, 0, down_write_trylock(&ctx->tsem));
	EXPECT_DEPTH(test, ctx, 0);

	stop_helper(test, t);

	/* Once the lock is free, the trylock succeeds and counts. */
	KUNIT_ASSERT_EQ(test, 1, down_read_trylock(&ctx->tsem));
	EXPECT_DEPTH(test, ctx, 1);
	up_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);
}

/*
 * A failed mutex_trylock() is not counted, and a contended mutex_lock()
 * is counted exactly once: not zero (slow path missed) and not twice
 * (fast and slow path both counted).
 */
static void contended_mutex_is_counted_once(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;
	struct task_struct *t;

	t = start_helper(test, HELPER_TMUTEX);

	KUNIT_EXPECT_FALSE(test, mutex_trylock(&ctx->tmutex));
	EXPECT_DEPTH(test, ctx, 0);

	/*
	 * Let the helper release the lock. mutex_lock() below most likely
	 * finds it still held and takes the slow path.
	 */
	complete(&ctx->helper_may_release);

	mutex_lock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 1);
	mutex_unlock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);

	wait_for_completion(&ctx->helper_done);
	kthread_stop(t);
}

/* The same for a contended rwsem. */
static void contended_rwsem_is_counted_once(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;
	struct task_struct *t;

	t = start_helper(test, HELPER_TSEM_WRITE);

	complete(&ctx->helper_may_release);

	down_write(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 1);
	up_write(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);

	down_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 1);
	up_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);

	wait_for_completion(&ctx->helper_done);
	kthread_stop(t);
}

/* The same for a percpu_rw_semaphore, starting with a failed read trylock. */
static void contended_percpu_rwsem_is_counted_once(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;
	struct task_struct *t;

	t = start_helper(test, HELPER_TPCPU_WRITE);
	KUNIT_EXPECT_EQ(test, 1u, ctx->helper_depth_held);

	KUNIT_EXPECT_FALSE(test, percpu_down_read_trylock(&ctx->tpcpu));
	EXPECT_DEPTH(test, ctx, 0);

	complete(&ctx->helper_may_release);

	percpu_down_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);

	percpu_down_write(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_write(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);

	wait_for_completion(&ctx->helper_done);
	KUNIT_EXPECT_EQ(test, 0u, ctx->helper_depth_after);
	kthread_stop(t);
}

/* Initializing a lock again clears its opt-in. */
/*
 * The opt-in is a sticky bit in the same word that says whether the lock is
 * held, so an opted-in lock that nobody holds must still read as unlocked.
 */
static void opted_in_lock_still_reads_unlocked(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	KUNIT_EXPECT_FALSE(test, rwsem_is_locked(&ctx->tsem));
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&ctx->tmutex));
	KUNIT_EXPECT_FALSE(test, percpu_is_read_locked(&ctx->tpcpu));
	KUNIT_EXPECT_FALSE(test, percpu_is_write_locked(&ctx->tpcpu));

	down_read(&ctx->tsem);
	KUNIT_EXPECT_TRUE(test, rwsem_is_locked(&ctx->tsem));
	up_read(&ctx->tsem);
	KUNIT_EXPECT_FALSE(test, rwsem_is_locked(&ctx->tsem));

	down_write(&ctx->tsem);
	KUNIT_EXPECT_TRUE(test, rwsem_is_locked(&ctx->tsem));
	up_write(&ctx->tsem);
	KUNIT_EXPECT_FALSE(test, rwsem_is_locked(&ctx->tsem));

	mutex_lock(&ctx->tmutex);
	KUNIT_EXPECT_TRUE(test, mutex_is_locked(&ctx->tmutex));
	mutex_unlock(&ctx->tmutex);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&ctx->tmutex));

	percpu_down_write(&ctx->tpcpu);
	KUNIT_EXPECT_TRUE(test, percpu_is_write_locked(&ctx->tpcpu));
	percpu_up_write(&ctx->tpcpu);
	KUNIT_EXPECT_FALSE(test, percpu_is_write_locked(&ctx->tpcpu));
}

/* Opting in a lock that is already held is refused, and warns. */
static void track_holder_on_a_held_lock_is_refused(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	mutex_lock(&ctx->pmutex);
	down_read(&ctx->psem);
	percpu_down_read(&ctx->ppcpu);
	EXPECT_DEPTH(test, ctx, 0);

	kunit_warning_suppress(test) {
		mutex_track_holder(&ctx->pmutex);
		rwsem_track_holder(&ctx->psem);
		percpu_rwsem_track_holder(&ctx->ppcpu);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 3);
	}

	/* Refused, so the locks are still untracked and nothing is counted. */
	EXPECT_DEPTH(test, ctx, 0);
	percpu_up_read(&ctx->ppcpu);
	up_read(&ctx->psem);
	mutex_unlock(&ctx->pmutex);
	EXPECT_DEPTH(test, ctx, 0);

	mutex_lock(&ctx->pmutex);
	EXPECT_DEPTH(test, ctx, 0);
	mutex_unlock(&ctx->pmutex);
}

/*
 * The non_owner calls can run in different tasks, which a per-task count
 * cannot follow. Both sides warn on a tracked rwsem and neither counts.
 *
 * Without CONFIG_DEBUG_LOCK_ALLOC they are plain down_read()/up_read(),
 * which are counted and balanced, so there is nothing to test.
 */
static void non_owner_on_a_tracked_rwsem_warns(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	if (!IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC))
		kunit_skip(test, "non_owner API needs CONFIG_DEBUG_LOCK_ALLOC");

	kunit_warning_suppress(test) {
		down_read_non_owner(&ctx->tsem);
		EXPECT_DEPTH(test, ctx, 0);
		up_read_non_owner(&ctx->tsem);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 2);
	}
	EXPECT_DEPTH(test, ctx, 0);

	/* An untracked rwsem uses them without warning, and is not counted. */
	down_read_non_owner(&ctx->psem);
	EXPECT_DEPTH(test, ctx, 0);
	up_read_non_owner(&ctx->psem);
	EXPECT_DEPTH(test, ctx, 0);
}

static void reinit_clears_the_opt_in(struct kunit *test)
{
	struct lockholder_ctx *ctx = test->priv;

	down_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 1);
	up_read(&ctx->tsem);

	init_rwsem(&ctx->tsem);
	down_read(&ctx->tsem);
	EXPECT_DEPTH(test, ctx, 0);
	up_read(&ctx->tsem);

	percpu_down_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 1);
	percpu_up_read(&ctx->tpcpu);

	percpu_free_rwsem(&ctx->tpcpu);
	KUNIT_ASSERT_EQ(test, 0, percpu_init_rwsem(&ctx->tpcpu));
	percpu_down_read(&ctx->tpcpu);
	EXPECT_DEPTH(test, ctx, 0);
	percpu_up_read(&ctx->tpcpu);

	mutex_lock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 1);
	mutex_unlock(&ctx->tmutex);

	mutex_init(&ctx->tmutex);
	mutex_lock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);
	mutex_unlock(&ctx->tmutex);
	EXPECT_DEPTH(test, ctx, 0);
}

static struct kunit_case lockholder_test_cases[] = {
	KUNIT_CASE(untracked_locks_are_not_counted),
	KUNIT_CASE(tracked_mutex_is_counted),
	KUNIT_CASE(tracked_rwsem_read_is_counted),
	KUNIT_CASE(tracked_rwsem_write_is_counted),
	KUNIT_CASE(tracked_percpu_rwsem_is_counted),
	KUNIT_CASE(percpu_rwsem_handover_moves_the_count),
	KUNIT_CASE(percpu_rwsem_slow_read_path_is_counted),
	KUNIT_CASE(downgrade_write_keeps_the_count),
	KUNIT_CASE(nested_locks_stack),
	KUNIT_CASE(guards_are_counted),
	KUNIT_CASE(the_count_is_per_task),
	KUNIT_CASE(failed_trylock_is_not_counted),
	KUNIT_CASE(contended_mutex_is_counted_once),
	KUNIT_CASE(contended_rwsem_is_counted_once),
	KUNIT_CASE(contended_percpu_rwsem_is_counted_once),
	KUNIT_CASE(reinit_clears_the_opt_in),
	KUNIT_CASE(opted_in_lock_still_reads_unlocked),
	KUNIT_CASE(track_holder_on_a_held_lock_is_refused),
	KUNIT_CASE(non_owner_on_a_tracked_rwsem_warns),
	{}
};

static struct kunit_suite lockholder_test_suite = {
	.name = "lockholder",
	.init = lockholder_init,
	.exit = lockholder_exit,
	.test_cases = lockholder_test_cases,
};
kunit_test_suite(lockholder_test_suite);

MODULE_DESCRIPTION("KUnit tests for sleeping lock holder tracking");
MODULE_LICENSE("GPL");
