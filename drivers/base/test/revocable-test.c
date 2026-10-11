// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 Google LLC
 *
 * KUnit tests for the revocable API.
 *
 * The test cases cover the following scenarios:
 *
 * - Basic: Verifies that a consumer can successfully access the resource.
 *
 * - Revocation: Verifies that after the provider revokes the resource,
 *   the consumer correctly receives a NULL pointer on a subsequent access.
 *
 * - Try Access Macro: Same as "Revocation" but uses the macro level
 *   helpers.
 *
 * - Concurrent Access: Verifies multiple threads can access the resource.
 */

#include <kunit/test.h>

#include <linux/completion.h>
#include <linux/iopoll.h>
#include <linux/kthread.h>
#include <linux/refcount.h>
#include <linux/revocable.h>
#include <linux/sched/task.h>

static int get_refcount(struct revocable *rev)
{
	return refcount_read(&rev->kref.refcount);
}

static void _revocable_embedded_test_basic(struct kunit *test,
					   struct revocable *rev)
{
	struct revocable_handle rh;
	void *real_res = (void *)0x12345678, *res;
	int ret;

	ret = revocable_init(rev, real_res);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, rev->dynamic);

	revocable_handle_init(rev, &rh);

	res = revocable_try_access(&rh);
	KUNIT_EXPECT_PTR_EQ(test, res, real_res);
	revocable_withdraw_access(&rh);

	revocable_handle_destroy(&rh);
	revocable_revoke(rev);
	revocable_destroy(rev);
}

static void revocable_embedded_test_basic_stack(struct kunit *test)
{
	struct revocable rev;

	_revocable_embedded_test_basic(test, &rev);
}

static void revocable_embedded_test_basic_heap(struct kunit *test)
{
	struct revocable *rev;

	rev = kunit_kzalloc(test, sizeof(*rev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rev);

	_revocable_embedded_test_basic(test, rev);
}

static void revocable_dynamic_test_basic(struct kunit *test)
{
	struct revocable *rev;
	struct revocable_handle rh;
	void *real_res = (void *)0x12345678, *res;

	rev = revocable_alloc(real_res);
	KUNIT_ASSERT_NOT_NULL(test, rev);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);
	KUNIT_EXPECT_TRUE(test, rev->dynamic);

	revocable_handle_init(rev, &rh);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);

	res = revocable_try_access(&rh);
	KUNIT_EXPECT_PTR_EQ(test, res, real_res);
	revocable_withdraw_access(&rh);

	KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);
	revocable_handle_destroy(&rh);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);
	revocable_revoke(rev);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);
	revocable_put(rev);
}

static void _revocable_embedded_test_revocation(struct kunit *test,
						struct revocable *rev)
{
	struct revocable_handle rh;
	void *real_res = (void *)0x12345678, *res;
	int ret;

	ret = revocable_init(rev, real_res);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, rev->dynamic);

	revocable_handle_init(rev, &rh);

	res = revocable_try_access(&rh);
	KUNIT_EXPECT_PTR_EQ(test, res, real_res);
	revocable_withdraw_access(&rh);

	revocable_revoke(rev);

	res = revocable_try_access(&rh);
	KUNIT_EXPECT_PTR_EQ(test, res, NULL);
	revocable_withdraw_access(&rh);

	revocable_handle_destroy(&rh);
	revocable_destroy(rev);
}

static void revocable_embedded_test_revocation_stack(struct kunit *test)
{
	struct revocable rev;

	_revocable_embedded_test_revocation(test, &rev);
}

static void revocable_embedded_test_revocation_heap(struct kunit *test)
{
	struct revocable *rev;

	rev = kunit_kzalloc(test, sizeof(*rev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rev);

	_revocable_embedded_test_revocation(test, rev);
}

static void revocable_dynamic_test_revocation(struct kunit *test)
{
	struct revocable *rev;
	struct revocable_handle rh;
	void *real_res = (void *)0x12345678, *res;

	rev = revocable_alloc(real_res);
	KUNIT_ASSERT_NOT_NULL(test, rev);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);
	KUNIT_EXPECT_TRUE(test, rev->dynamic);

	revocable_handle_init(rev, &rh);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);

	res = revocable_try_access(&rh);
	KUNIT_EXPECT_PTR_EQ(test, res, real_res);
	revocable_withdraw_access(&rh);

	KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);
	revocable_revoke(rev);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);

	res = revocable_try_access(&rh);
	KUNIT_EXPECT_PTR_EQ(test, res, NULL);
	revocable_withdraw_access(&rh);

	KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);
	revocable_handle_destroy(&rh);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);
	revocable_put(rev);
}

static int call_revocable_try_access_or_return_err(struct revocable *rev)
{
	void *res;

	revocable_try_access_or_return_err(rev, res, -ENXIO);
	return 0;
}

static int call_revocable_try_access_or_return(struct revocable *rev)
{
	void *res;

	revocable_try_access_or_return(rev, res);
	return 0;
}

static void call_revocable_try_access_or_return_void(struct kunit *test,
						     struct revocable *rev)
{
	void *res;

	revocable_try_access_or_return_void(rev, res);
	KUNIT_FAIL(test, "unreachable");
}

static int call_revocable_try_access_or_return_err_scoped(struct revocable *rev)
{
	void *res;

	revocable_try_access_or_return_err_scoped(rev, res, -ENXIO) {}
	return 0;
}

static int call_revocable_try_access_or_return_scoped(struct revocable *rev)
{
	void *res;

	revocable_try_access_or_return_scoped(rev, res) {}
	return 0;
}

static void call_revocable_try_access_or_return_void_scoped(struct kunit *test,
							    struct revocable *rev)
{
	void *res;

	revocable_try_access_or_return_void_scoped(rev, res) {}
	KUNIT_FAIL(test, "unreachable");
}

static void revocable_embedded_test_try_access_macro(struct kunit *test)
{
	struct revocable rev_on_stack, *rev = &rev_on_stack;
	void *real_res = (void *)0x12345678, *res;
	int ret;
	bool accessed;

	ret = revocable_init(rev, real_res);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, rev->dynamic);

	{
		revocable_try_access_with(rev, res);
		KUNIT_EXPECT_PTR_EQ(test, res, real_res);
	}

	accessed = false;
	revocable_try_access_with_scoped(rev, res) {
		KUNIT_EXPECT_PTR_EQ(test, res, real_res);
		accessed = true;
	}
	KUNIT_EXPECT_TRUE(test, accessed);

	revocable_revoke(rev);

	{
		revocable_try_access_with(rev, res);
		KUNIT_EXPECT_PTR_EQ(test, res, NULL);
	}

	accessed = false;
	revocable_try_access_with_scoped(rev, res) {
		KUNIT_EXPECT_PTR_EQ(test, res, NULL);
		accessed = true;
	}
	KUNIT_EXPECT_TRUE(test, accessed);

	ret = call_revocable_try_access_or_return_err(rev);
	KUNIT_EXPECT_EQ(test, ret, -ENXIO);

	ret = call_revocable_try_access_or_return(rev);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);

	call_revocable_try_access_or_return_void(test, rev);

	ret = call_revocable_try_access_or_return_err_scoped(rev);
	KUNIT_EXPECT_EQ(test, ret, -ENXIO);

	ret = call_revocable_try_access_or_return_scoped(rev);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);

	call_revocable_try_access_or_return_void_scoped(test, rev);

	accessed = false;
	revocable_try_access_or_skip_scoped(rev, res)
		accessed = true;
	KUNIT_EXPECT_FALSE(test, accessed);

	revocable_destroy(rev);
}

static void revocable_dynamic_test_try_access_macro(struct kunit *test)
{
	struct revocable *rev;
	void *real_res = (void *)0x12345678, *res;
	int ret;
	bool accessed;

	rev = revocable_alloc(real_res);
	KUNIT_ASSERT_NOT_NULL(test, rev);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);
	KUNIT_EXPECT_TRUE(test, rev->dynamic);

	{
		revocable_try_access_with(rev, res);
		KUNIT_EXPECT_PTR_EQ(test, res, real_res);
		KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);
	}
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);

	accessed = false;
	revocable_try_access_with_scoped(rev, res) {
		KUNIT_EXPECT_PTR_EQ(test, res, real_res);
		KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);
		accessed = true;
	}
	KUNIT_EXPECT_TRUE(test, accessed);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);

	revocable_revoke(rev);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);

	{
		revocable_try_access_with(rev, res);
		KUNIT_EXPECT_PTR_EQ(test, res, NULL);
		KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);
	}
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);

	accessed = false;
	revocable_try_access_with_scoped(rev, res) {
		KUNIT_EXPECT_PTR_EQ(test, res, NULL);
		KUNIT_EXPECT_EQ(test, get_refcount(rev), 2);
		accessed = true;
	}
	KUNIT_EXPECT_TRUE(test, accessed);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);

	ret = call_revocable_try_access_or_return_err(rev);
	KUNIT_EXPECT_EQ(test, ret, -ENXIO);

	ret = call_revocable_try_access_or_return(rev);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);

	call_revocable_try_access_or_return_void(test, rev);

	ret = call_revocable_try_access_or_return_err_scoped(rev);
	KUNIT_EXPECT_EQ(test, ret, -ENXIO);

	ret = call_revocable_try_access_or_return_scoped(rev);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);

	call_revocable_try_access_or_return_void_scoped(test, rev);

	accessed = false;
	revocable_try_access_or_skip_scoped(rev, res)
		accessed = true;
	KUNIT_EXPECT_FALSE(test, accessed);

	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);
	revocable_put(rev);
}

struct test_concurrent_access_context {
	struct completion started, enter;
	struct task_struct *thread;

	union {
		/* Used by test provider. */
		struct revocable *rev;

		/* Used by test consumer. */
		struct {
			struct kunit *test;
			struct completion entered, exit;
			struct revocable_handle rh;
			void *expected_res;
		};
	};
};

static int test_concurrent_access_provider(void *data)
{
	struct test_concurrent_access_context *ctx = data;

	complete(&ctx->started);

	wait_for_completion(&ctx->enter);
	revocable_revoke(ctx->rev);

	return 0;
}

static int test_concurrent_access_consumer(void *data)
{
	struct test_concurrent_access_context *ctx = data;
	void *res;

	complete(&ctx->started);

	wait_for_completion(&ctx->enter);
	res = revocable_try_access(&ctx->rh);
	KUNIT_EXPECT_PTR_EQ(ctx->test, res, ctx->expected_res);

	complete(&ctx->entered);

	wait_for_completion(&ctx->exit);
	revocable_withdraw_access(&ctx->rh);

	return 0;
}

static struct test_concurrent_access_context *
revocable_test_concurrent_access_prepare(struct kunit *test,
					 struct revocable *rev, bool dynamic)
{
	struct test_concurrent_access_context *ctx;
	int i;

	ctx = kunit_kmalloc_array(test, 3, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	for (i = 0; i < 3; ++i) {
		init_completion(&ctx[i].started);
		init_completion(&ctx[i].enter);

		if (i == 0) {
			ctx[i].rev = rev;
			ctx[i].thread = kthread_run(
				test_concurrent_access_provider, ctx + i,
				"provider%d", i);
		} else {
			ctx[i].test = test;
			init_completion(&ctx[i].entered);
			init_completion(&ctx[i].exit);
			revocable_handle_init(rev, &ctx[i].rh);
			if (dynamic)
				KUNIT_EXPECT_EQ(test, get_refcount(rev), 1 + i);

			ctx[i].thread = kthread_run(
				test_concurrent_access_consumer, ctx + i,
				"consumer%d", i);
		}
		KUNIT_ASSERT_FALSE(test, IS_ERR(ctx[i].thread));
		get_task_struct(ctx[i].thread);

		wait_for_completion(&ctx[i].started);
	}

	return ctx;
}

static void revocable_embedded_test_concurrent_access(struct kunit *test)
{
	struct revocable rev_on_stack, *rev = &rev_on_stack;
	void *real_res = (void *)0x12345678, *res;
	struct test_concurrent_access_context *ctx;
	int ret, i;

	ret = revocable_init(rev, real_res);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, rev->dynamic);

	ctx = revocable_test_concurrent_access_prepare(test, rev, false);

	ctx[1].expected_res = real_res;
	/* consumer1 enters read-side critical section. */
	complete(&ctx[1].enter);
	wait_for_completion(&ctx[1].entered);

	/* provider0 revokes the resource. */
	complete(&ctx[0].enter);
	ret = read_poll_timeout(rcu_access_pointer, res, !res, 1000, 100000,
				false, rev->res);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* provider0 can't exit.  It's waiting for the grace period. */

	ctx[2].expected_res = NULL;
	/* consumer2 enters read-side critical section. */
	complete(&ctx[2].enter);
	wait_for_completion(&ctx[2].entered);

	/* consumer{1,2} exit read-side critical section. */
	for (i = 1; i < 3; ++i) {
		complete(&ctx[i].exit);
		kthread_stop_put(ctx[i].thread);
		revocable_handle_destroy(&ctx[i].rh);
	}

	/* provider0 exits as all readers exit their critical section. */
	kthread_stop_put(ctx[0].thread);

	revocable_destroy(rev);
}

static void revocable_dynamic_test_concurrent_access(struct kunit *test)
{
	struct revocable *rev;
	void *real_res = (void *)0x12345678, *res;
	struct test_concurrent_access_context *ctx;
	int ret, i;

	rev = revocable_alloc(real_res);
	KUNIT_ASSERT_NOT_NULL(test, rev);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);
	KUNIT_EXPECT_TRUE(test, rev->dynamic);

	ctx = revocable_test_concurrent_access_prepare(test, rev, true);

	ctx[1].expected_res = real_res;
	/* consumer1 enters read-side critical section. */
	complete(&ctx[1].enter);
	wait_for_completion(&ctx[1].entered);

	/* provider0 revokes the resource. */
	complete(&ctx[0].enter);
	ret = read_poll_timeout(rcu_access_pointer, res, !res, 1000, 100000,
				false, rev->res);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* provider0 can't exit.  It's waiting for the grace period. */
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 3);

	ctx[2].expected_res = NULL;
	/* consumer2 enters read-side critical section. */
	complete(&ctx[2].enter);
	wait_for_completion(&ctx[2].entered);

	/* consumer{1,2} exit read-side critical section. */
	for (i = 1; i < 3; ++i) {
		complete(&ctx[i].exit);
		kthread_stop_put(ctx[i].thread);
		revocable_handle_destroy(&ctx[i].rh);
	}

	/* provider0 exits as all readers exit their critical section. */
	kthread_stop_put(ctx[0].thread);
	KUNIT_EXPECT_EQ(test, get_refcount(rev), 1);

	/* Drop the provider reference. */
	revocable_put(rev);
}

static struct kunit_case revocable_test_cases[] = {
	KUNIT_CASE(revocable_embedded_test_basic_stack),
	KUNIT_CASE(revocable_embedded_test_basic_heap),
	KUNIT_CASE(revocable_dynamic_test_basic),
	KUNIT_CASE(revocable_embedded_test_revocation_stack),
	KUNIT_CASE(revocable_embedded_test_revocation_heap),
	KUNIT_CASE(revocable_dynamic_test_revocation),
	KUNIT_CASE(revocable_embedded_test_try_access_macro),
	KUNIT_CASE(revocable_dynamic_test_try_access_macro),
	KUNIT_CASE(revocable_embedded_test_concurrent_access),
	KUNIT_CASE(revocable_dynamic_test_concurrent_access),
	{}
};

static struct kunit_suite revocable_test_suite = {
	.name = "revocable_test",
	.test_cases = revocable_test_cases,
};

kunit_test_suite(revocable_test_suite);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tzung-Bi Shih <tzungbi@kernel.org>");
MODULE_DESCRIPTION("KUnit tests for the revocable API");
