// SPDX-License-Identifier: GPL-2.0-or-later

#include <kunit/test.h>
#include <linux/stop_machine.h>

struct memcg_reservation_test {
	struct obj_cgroup owner;
	struct mem_cgroup_reservation res;
	u64 refunds;
	unsigned int calls;
	unsigned int fail_call;
	bool uncharged;
};

static int memcg_reservation_test_charge(struct obj_cgroup *objcg,
					 unsigned int pages, bool *charged)
{
	struct memcg_reservation_test *ctx = kunit_get_current_test()->priv;

	*charged = !ctx->uncharged;
	if (++ctx->calls == ctx->fail_call)
		return -ENOMEM;
	return 0;
}

static void memcg_reservation_test_uncharge(struct obj_cgroup *objcg, u64 pages)
{
	struct memcg_reservation_test *ctx = kunit_get_current_test()->priv;

	ctx->refunds += pages;
}

static int memcg_reservation_test_init(struct kunit *test)
{
	struct memcg_reservation_test *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	/* Transaction cases stub accounting; the root refund case calls the batch. */
	ctx->owner.is_root = true;
	ctx->res.objcg = &ctx->owner;
	test->priv = ctx;
	kunit_activate_static_stub(test, mem_cgroup_reservation_charge,
				   memcg_reservation_test_charge);
	kunit_activate_static_stub(test, mem_cgroup_reservation_uncharge,
				   memcg_reservation_test_uncharge);
	return 0;
}

static void memcg_reservation_test_rollback(struct kunit *test)
{
	struct memcg_reservation_test *ctx = test->priv;
	u64 original = 7;

	if (!IS_ENABLED(CONFIG_64BIT)) {
		kunit_skip(test,
			   "Charged reservations require 64-bit counters");
		return;
	}
	KUNIT_ASSERT_EQ(test,
			mem_cgroup_reservation_resize(&ctx->res, original), 0);
	ctx->calls = 0;
	ctx->fail_call = 2;
	KUNIT_EXPECT_EQ(test,
			mem_cgroup_reservation_resize(
				&ctx->res,
				original + MEMCG_RESERVATION_BATCH + 1),
			-ENOMEM);
	KUNIT_EXPECT_EQ(test, ctx->res.pages, original);
	KUNIT_EXPECT_EQ(test, ctx->res.charged_pages, original);
	KUNIT_EXPECT_EQ(test, ctx->refunds, (u64)MEMCG_RESERVATION_BATCH);
	KUNIT_EXPECT_EQ(test, ctx->calls, 2U);
	mem_cgroup_reservation_destroy(&ctx->res);
	KUNIT_EXPECT_EQ(test, ctx->refunds, MEMCG_RESERVATION_BATCH + original);
	KUNIT_EXPECT_PTR_EQ(test, ctx->res.objcg, NULL);
}

static void memcg_reservation_test_root_growth(struct kunit *test)
{
	struct memcg_reservation_test *ctx = test->priv;

	if (!IS_ENABLED(CONFIG_64BIT)) {
		kunit_skip(test,
			   "Charged reservations require 64-bit counters");
		return;
	}
	KUNIT_ASSERT_EQ(test, mem_cgroup_reservation_resize(&ctx->res, 8), 0);
	/* Simulate the charge helper observing reparenting to root. */
	ctx->uncharged = true;
	KUNIT_ASSERT_EQ(test, mem_cgroup_reservation_resize(&ctx->res, 16), 0);
	KUNIT_EXPECT_EQ(test, ctx->res.charged_pages, 8ULL);
	KUNIT_ASSERT_EQ(test, mem_cgroup_reservation_resize(&ctx->res, 10), 0);
	KUNIT_EXPECT_EQ(test, ctx->refunds, 0ULL);
	KUNIT_ASSERT_EQ(test, mem_cgroup_reservation_resize(&ctx->res, 3), 0);
	KUNIT_EXPECT_EQ(test, ctx->res.charged_pages, 3ULL);
	KUNIT_EXPECT_EQ(test, ctx->refunds, 5ULL);
	mem_cgroup_reservation_destroy(&ctx->res);
	KUNIT_EXPECT_EQ(test, ctx->refunds, 8ULL);
	KUNIT_EXPECT_EQ(test, ctx->res.pages, 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->res.charged_pages, 0ULL);
}

static void memcg_reservation_test_range(struct kunit *test)
{
	struct memcg_reservation_test *ctx = test->priv;

	KUNIT_EXPECT_EQ(test,
			mem_cgroup_reservation_resize(
				&ctx->res, (u64)PAGE_COUNTER_MAX + 1),
			-E2BIG);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0U);
	KUNIT_EXPECT_EQ(test, ctx->res.pages, 0ULL);
	mem_cgroup_reservation_destroy(&ctx->res);
	/* Disabled/root ownership still records the logical reservation. */
	KUNIT_EXPECT_EQ(test, mem_cgroup_reservation_resize(&ctx->res, 9), 0);
	KUNIT_EXPECT_EQ(test, ctx->res.pages, 9ULL);
	KUNIT_EXPECT_EQ(test, ctx->res.charged_pages, 0ULL);
	mem_cgroup_reservation_destroy(&ctx->res);
}

static void memcg_reservation_test_arch_support(struct kunit *test)
{
	struct memcg_reservation_test *ctx = test->priv;
	bool supported = IS_ENABLED(CONFIG_64BIT);

	KUNIT_ASSERT_EQ(test, mem_cgroup_reservation_resize(&ctx->res, 0), 0);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0U);
	KUNIT_EXPECT_EQ(test, mem_cgroup_reservation_resize(&ctx->res, 9),
			supported ? 0 : -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ctx->calls, supported ? 1U : 0U);
	KUNIT_EXPECT_EQ(test, ctx->res.pages, supported ? 9ULL : 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->res.charged_pages, supported ? 9ULL : 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->refunds, 0ULL);
	mem_cgroup_reservation_destroy(&ctx->res);
	KUNIT_EXPECT_EQ(test, ctx->refunds, supported ? 9ULL : 0ULL);
	KUNIT_EXPECT_PTR_EQ(test, ctx->res.objcg, NULL);

	/* An uncharged reservation remains usable on either architecture. */
	KUNIT_ASSERT_EQ(test, mem_cgroup_reservation_resize(&ctx->res, 9), 0);
	KUNIT_EXPECT_EQ(test, ctx->res.pages, 9ULL);
	KUNIT_EXPECT_EQ(test, ctx->res.charged_pages, 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->calls, supported ? 1U : 0U);
	mem_cgroup_reservation_destroy(&ctx->res);
	KUNIT_EXPECT_EQ(test, ctx->refunds, supported ? 9ULL : 0ULL);
}

struct memcg_reservation_root_refund {
	unsigned long memory_before, memory_shrunk, memory_after;
	unsigned long memsw_before, memsw_shrunk, memsw_after;
	bool memsw;
};

static void memcg_reservation_test_restore_counter(struct page_counter *counter,
						   unsigned long before)
{
	unsigned long after = page_counter_read(counter);

	if (after > before)
		page_counter_uncharge(counter, after - before);
	else if (after < before)
		page_counter_charge(counter, before - after);
}

static int memcg_reservation_test_root_refund_stopped(void *data)
{
	struct memcg_reservation_root_refund *result = data;
	struct mem_cgroup *memcg = root_mem_cgroup;

	/* Model charged capacity inherited from a removed child. */
	result->memory_before = page_counter_read(&memcg->memory);
	result->memsw_before = page_counter_read(&memcg->memsw);
	page_counter_charge(&memcg->memory, 8);
	if (result->memsw)
		page_counter_charge(&memcg->memsw, 8);
	account_kmem_nmi_safe(memcg, 8);
	memcg1_account_kmem(memcg, 8);

	mem_cgroup_reservation_uncharge_batch(memcg, 3);
	result->memory_shrunk = page_counter_read(&memcg->memory);
	result->memsw_shrunk = page_counter_read(&memcg->memsw);
	mem_cgroup_reservation_uncharge_batch(memcg, 5);
	result->memory_after = page_counter_read(&memcg->memory);
	result->memsw_after = page_counter_read(&memcg->memsw);

	/* Leave no counter imbalance behind if the refund regression returns. */
	memcg_reservation_test_restore_counter(&memcg->memory,
					       result->memory_before);
	if (result->memsw)
		memcg_reservation_test_restore_counter(&memcg->memsw,
						       result->memsw_before);
	return 0;
}

static void memcg_reservation_test_root_refund(struct kunit *test)
{
	struct memcg_reservation_root_refund result = {
		.memsw = do_memsw_account(),
	};

	if (mem_cgroup_disabled() || mem_cgroup_kmem_disabled()) {
		kunit_skip(test, "Memory cgroup accounting is disabled");
		return;
	}
	/* Exclude background charges while checking the actual root counters. */
	KUNIT_ASSERT_EQ(test,
			stop_machine(memcg_reservation_test_root_refund_stopped,
				     &result, NULL),
			0);
	KUNIT_EXPECT_EQ(test, result.memory_shrunk, result.memory_before + 5);
	KUNIT_EXPECT_EQ(test, result.memory_after, result.memory_before);
	KUNIT_EXPECT_EQ(test, result.memsw_shrunk,
			result.memsw_before + (result.memsw ? 5 : 0));
	KUNIT_EXPECT_EQ(test, result.memsw_after, result.memsw_before);
}

static struct kunit_case memcg_reservation_test_cases[] = {
	KUNIT_CASE(memcg_reservation_test_rollback),
	KUNIT_CASE(memcg_reservation_test_root_growth),
	KUNIT_CASE(memcg_reservation_test_range),
	KUNIT_CASE(memcg_reservation_test_arch_support),
	KUNIT_CASE(memcg_reservation_test_root_refund),
	{}
};

static struct kunit_suite memcg_reservation_test_suite = {
	.name = "memcg_reservation",
	.init = memcg_reservation_test_init,
	.test_cases = memcg_reservation_test_cases,
};

kunit_test_suite(memcg_reservation_test_suite);
