// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/highmem.h>

#include "opaque_internal.h"

struct opaque_basic {
	struct io_opaque_store *store;
	struct io_opaque_data *data;
	struct page *page;
};

static void opaque_basic_release(void *arg)
{
	struct opaque_basic *c = arg;

	if (!IS_ERR_OR_NULL(c->store)) {
		io_opaque_data_put(c->store, c->data);
		/* Also release a budget assertion's unfinished reservation. */
		if (atomic64_read(&c->store->bytes) > 0)
			io_opaque_uncharge(c->store, atomic64_read(&c->store->bytes));
		io_opaque_free(c->store);
	}
	if (c->page)
		put_page(c->page);
}

static struct opaque_basic *opaque_basic_new(struct kunit *test)
{
	struct io_uring_opaque_config config = {
		.hard_limit = 8 * PAGE_SIZE,
		.compact_headroom = 2 * PAGE_SIZE,
		.reply_reserve = PAGE_SIZE,
		.high_watermark = 4 * PAGE_SIZE,
		.low_watermark = 2 * PAGE_SIZE,
		.max_object_size = PAGE_SIZE,
		.max_objects = 4,
		.max_streams = 1,
		.max_extents = 8,
	};
	struct opaque_basic *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	struct io_ring_ctx *ctx;

	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, opaque_basic_release, c), 0);
	/* No user/mm accounting in this internal-helper fixture. */
	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	c->store = io_opaque_alloc(ctx, &config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c->store);
	return c;
}

static void opaque_basic_budget(struct kunit *test)
{
	struct opaque_basic *c = opaque_basic_new(test);
	struct io_opaque_store *store = c->store;

	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 3 * PAGE_SIZE, true));
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, PAGE_SIZE, true));
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_LOWMEM);
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, 2 * PAGE_SIZE, true));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)(4 * PAGE_SIZE));
	io_opaque_uncharge(store, 2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	io_opaque_uncharge(store, 2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

static void opaque_basic_version(struct kunit *test)
{
	static const char payload[] = "opaque";
	struct opaque_basic *c = opaque_basic_new(test);
	struct io_opaque_extent *extent;
	void *mapped;

	c->data = io_opaque_data_alloc();
	KUNIT_ASSERT_NOT_NULL(test, c->data);
	c->page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	KUNIT_ASSERT_NOT_NULL(test, c->page);
	mapped = kmap_local_page(c->page);
	memcpy(mapped + 17, payload, sizeof(payload));
	kunmap_local(mapped);
	get_page(c->page);
	extent = io_opaque_extent_new(c->store, c->page, 17, sizeof(payload), 0, false);
	if (IS_ERR(extent))
		put_page(c->page);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, extent);
	list_add_tail(&extent->list, &c->data->extents);
	c->data->length = sizeof(payload);
	KUNIT_ASSERT_EQ(test, io_opaque_vector(c->data), 0);
	KUNIT_ASSERT_EQ(test, c->data->nr, 1U);
	KUNIT_EXPECT_PTR_EQ(test, c->data->bvec[0].bv_page, c->page);
	KUNIT_EXPECT_EQ(test, c->data->bvec[0].bv_offset, 17U);
	KUNIT_EXPECT_EQ(test, c->data->bvec[0].bv_len, (unsigned int)sizeof(payload));
	KUNIT_EXPECT_EQ(test, c->data->charge, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&c->store->bytes), (s64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->store->extents), 1);
	refcount_inc(&c->data->refs);
	io_opaque_data_put(c->store, c->data);
	KUNIT_EXPECT_EQ(test, refcount_read(&c->data->refs), 1U);
	KUNIT_EXPECT_EQ(test, page_ref_count(c->page), 2);
	mapped = kmap_local_page(c->data->bvec[0].bv_page);
	KUNIT_EXPECT_MEMEQ(test, mapped + c->data->bvec[0].bv_offset, payload, sizeof(payload));
	kunmap_local(mapped);
	io_opaque_data_put(c->store, c->data);
	c->data = NULL;
	KUNIT_EXPECT_EQ(test, page_ref_count(c->page), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&c->store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->store->extents), 0);
}

static struct kunit_case opaque_basic_cases[] = {
	KUNIT_CASE(opaque_basic_budget),
	KUNIT_CASE(opaque_basic_version),
	{}
};

static struct kunit_suite opaque_basic_suite = {
	.name = "io_opaque_basic",
	.test_cases = opaque_basic_cases,
};

kunit_test_suite(opaque_basic_suite);

MODULE_LICENSE("GPL");
