// SPDX-License-Identifier: GPL-2.0
// Copyright 2023 Maxime Ripard <mripard@kernel.org>

#include <kunit/resource.h>

#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/kernfs.h>
#include <linux/kthread.h>
#include <linux/sort.h>

#define DEVICE_NAME "test"

struct test_priv {
	bool probe_done;
	bool release_done;
	wait_queue_head_t release_wq;
	struct device *dev;
};

static int root_device_devm_init(struct kunit *test)
{
	struct test_priv *priv;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);
	init_waitqueue_head(&priv->release_wq);

	test->priv = priv;

	return 0;
}

static void devm_device_action(void *ptr)
{
	struct test_priv *priv = ptr;

	priv->release_done = true;
	wake_up_interruptible(&priv->release_wq);
}

#define RELEASE_TIMEOUT_MS	100

/*
 * Tests that a bus-less, non-probed device will run its device-managed
 * actions when unregistered.
 */
static void root_device_devm_register_unregister_test(struct kunit *test)
{
	struct test_priv *priv = test->priv;
	int ret;

	priv->dev = root_device_register(DEVICE_NAME);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->dev);

	ret = devm_add_action_or_reset(priv->dev, devm_device_action, priv);
	KUNIT_ASSERT_EQ(test, ret, 0);

	root_device_unregister(priv->dev);

	ret = wait_event_interruptible_timeout(priv->release_wq, priv->release_done,
					       msecs_to_jiffies(RELEASE_TIMEOUT_MS));
	KUNIT_EXPECT_GT(test, ret, 0);
}

static void devm_put_device_action(void *ptr)
{
	struct test_priv *priv = ptr;

	put_device(priv->dev);
	priv->release_done = true;
	wake_up_interruptible(&priv->release_wq);
}

/*
 * Tests that a bus-less, non-probed device will run its device-managed
 * actions when unregistered, even if someone still holds a reference to
 * it.
 */
static void root_device_devm_register_get_unregister_with_devm_test(struct kunit *test)
{
	struct test_priv *priv = test->priv;
	int ret;

	priv->dev = root_device_register(DEVICE_NAME);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->dev);

	get_device(priv->dev);

	ret = devm_add_action_or_reset(priv->dev, devm_put_device_action, priv);
	KUNIT_ASSERT_EQ(test, ret, 0);

	root_device_unregister(priv->dev);

	ret = wait_event_interruptible_timeout(priv->release_wq, priv->release_done,
					       msecs_to_jiffies(RELEASE_TIMEOUT_MS));
	KUNIT_EXPECT_GT(test, ret, 0);
}

#if IS_ENABLED(CONFIG_SYSFS)
/*
 * Inode IDs may be handed out from per-CPU batches (KERNFS_ROOT_INO_BATCH on
 * the sysfs root); one task's loop only draws from the CPU it runs on and
 * would pass unchanged if batching were inert.  Hence several tasks at
 * once.  Per task: more than INO_BATCH IDs for every online CPU, so a
 * refill is crossed even if a task migrates across all of them, and never
 * a whole number of batches, so the run also ends mid-batch.
 */
#define INO_THREADS		4
#define INO_BATCH		16	/* KERNFS_INO_BATCH (kernfs-private) */
#define INO_MIN_PER_THREAD	200
/* one worker's registrations scale with the CPU count: ~0.5ms each on 4 CPUs */
#define INO_TIMEOUT_BASE_MS	5000
#define INO_TIMEOUT_PER_DEV_MS	10

struct ino_worker {
	int idx;
	int nr;				/* devices to register */
	int created;			/* devices actually registered */
	int err;			/* first registration error */
	bool joined;			/* completion was waited out */
	struct device **devs;
	ino_t *inos;
	struct completion done;
};

struct ino_test_ctx {
	struct ino_worker worker[INO_THREADS];
	int spawned;
};

static int root_device_ino_worker(void *data)
{
	struct ino_worker *w = data;
	int i;

	for (i = 0; i < w->nr; i++) {
		char name[32];
		struct device *dev;

		snprintf(name, sizeof(name), DEVICE_NAME "-ino-%d-%d",
			 w->idx, i);
		dev = root_device_register(name);
		if (IS_ERR(dev)) {
			w->err = PTR_ERR(dev);
			break;
		}
		w->devs[i] = dev;
		w->inos[i] = dev->kobj.sd ? kernfs_ino(dev->kobj.sd) : 0;
		w->created = i + 1;
	}

	/*
	 * complete_all(), not complete(): this is waited on twice, once by
	 * the test and once by the teardown action, and a plain completion
	 * is consumed by the first waiter.
	 */
	complete_all(&w->done);
	return 0;
}

/*
 * Registered before the first worker is spawned, so it also runs if the test
 * aborts: join every worker, then undo what it did.
 */
static void root_device_ino_teardown(void *data)
{
	struct ino_test_ctx *ctx = data;
	int t, i;

	for (t = 0; t < ctx->spawned; t++)
		wait_for_completion(&ctx->worker[t].done);

	for (t = 0; t < INO_THREADS; t++)
		for (i = 0; i < ctx->worker[t].created; i++)
			root_device_unregister(ctx->worker[t].devs[i]);
}

static int ino_cmp(const void *a, const void *b)
{
	ino_t x = *(const ino_t *)a;
	ino_t y = *(const ino_t *)b;

	if (x < y)
		return -1;
	return x > y;
}

static void root_device_ino_uniqueness_test(struct kunit *test)
{
	int zeros = 0, dups = 0, descents = 0, ids = 0;
	struct ino_test_ctx *ctx;
	bool results_valid = true;
	unsigned long timeout;
	struct device **devs;
	ino_t *inos, *sorted;
	int nr, total, t, i;

	nr = INO_BATCH * num_online_cpus() + INO_BATCH / 2;
	if (nr < INO_MIN_PER_THREAD)
		nr = INO_MIN_PER_THREAD;
	total = INO_THREADS * nr;
	timeout = msecs_to_jiffies(INO_TIMEOUT_BASE_MS +
				   nr * INO_TIMEOUT_PER_DEV_MS);

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	devs = kunit_kcalloc(test, total, sizeof(*devs), GFP_KERNEL);
	inos = kunit_kcalloc(test, total, sizeof(*inos), GFP_KERNEL);
	sorted = kunit_kcalloc(test, total, sizeof(*sorted), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, devs);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, inos);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sorted);

	for (t = 0; t < INO_THREADS; t++) {
		struct ino_worker *w = &ctx->worker[t];

		w->idx = t;
		w->nr = nr;
		w->devs = devs + t * nr;
		w->inos = inos + t * nr;
		init_completion(&w->done);
	}

	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  root_device_ino_teardown,
						  ctx), 0);

	for (t = 0; t < INO_THREADS; t++) {
		struct task_struct *task;

		task = kthread_run(root_device_ino_worker, &ctx->worker[t],
				   "root_dev_ino%d", t);
		if (IS_ERR(task))
			break;
		ctx->spawned++;
	}
	KUNIT_EXPECT_EQ(test, ctx->spawned, INO_THREADS);
	if (ctx->spawned != INO_THREADS)
		results_valid = false;

	/*
	 * complete_all() is a worker's last act, so only a join that succeeds
	 * orders its stores before the reads below; a worker that timed out
	 * may still be writing its slice.  The teardown action joins those
	 * unbounded, before any of this memory can be freed.
	 */
	for (t = 0; t < ctx->spawned; t++) {
		struct ino_worker *w = &ctx->worker[t];

		if (wait_for_completion_timeout(&w->done, timeout) > 0) {
			w->joined = true;
		} else {
			KUNIT_FAIL(test, "worker %d timed out", t);
			results_valid = false;
		}
	}

	for (t = 0; t < ctx->spawned; t++) {
		struct ino_worker *w = &ctx->worker[t];

		if (!w->joined)
			continue;

		KUNIT_EXPECT_EQ(test, w->err, 0);
		KUNIT_EXPECT_EQ(test, w->created, nr);
		if (w->err || w->created != nr)
			results_valid = false;

		for (i = 0; i < w->created; i++) {
			if (!w->inos[i])
				zeros++;
			/*
			 * Batches are consumed from the top: IDs step down
			 * within one, while the locked path only ascends, so
			 * one descent is evidence the cache was in use.
			 */
			if (i && w->inos[i] < w->inos[i - 1])
				descents++;
			sorted[ids++] = w->inos[i];
		}
	}
	KUNIT_EXPECT_EQ(test, zeros, 0);

	/* every ID belongs to a node that is still alive here */
	sort(sorted, ids, sizeof(*sorted), ino_cmp, NULL);
	for (i = 1; i < ids; i++)
		if (sorted[i] == sorted[i - 1])
			dups++;
	KUNIT_EXPECT_EQ(test, dups, 0);
	if (zeros || dups)
		results_valid = false;

	/*
	 * The batch cache is best-effort: when its allocation failed, every
	 * ID above came from the locked fallback and no descent can have been
	 * observed.  Make that visibly unavailable coverage rather than a
	 * PASS that never exercised batching -- but only when everything
	 * above held, since kunit_skip() would overwrite a recorded failure.
	 */
	if (IS_ENABLED(CONFIG_SMP) && results_valid && !descents)
		kunit_skip(test, "sysfs inode batching fast path was not observed");
}
#endif

static struct kunit_case root_device_devm_tests[] = {
	KUNIT_CASE(root_device_devm_register_unregister_test),
	KUNIT_CASE(root_device_devm_register_get_unregister_with_devm_test),
#if IS_ENABLED(CONFIG_SYSFS)
	KUNIT_CASE(root_device_ino_uniqueness_test),
#endif
	{}
};

static struct kunit_suite root_device_devm_test_suite = {
	.name = "root-device-devm",
	.init = root_device_devm_init,
	.test_cases = root_device_devm_tests,
};

kunit_test_suite(root_device_devm_test_suite);

MODULE_DESCRIPTION("Test module for root devices");
MODULE_AUTHOR("Maxime Ripard <mripard@kernel.org>");
MODULE_LICENSE("GPL");
