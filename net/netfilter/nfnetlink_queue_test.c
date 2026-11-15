// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/mm.h>

static void nfqnl_test_destroy_map(void *data)
{
	struct nfqnl_instance *queue = data;

	rhashtable_destroy(&queue->nfqnl_packet_map);
}

static void nfqnl_test_free_skb(void *data)
{
	kfree_skb(data);
}

static int nfqnl_test_init(struct kunit *test)
{
	struct nfqnl_instance *queue;
	int err;

	queue = kunit_kzalloc(test, sizeof(*queue), GFP_KERNEL);
	if (!queue)
		return -ENOMEM;
	spin_lock_init(&queue->lock);
	mutex_init(&queue->mem_mutex);
	INIT_WORK(&queue->mem_work, nfqnl_mem_work);
	INIT_LIST_HEAD(&queue->queue_list);
	err = rhashtable_init(&queue->nfqnl_packet_map,
			      &nfqnl_rhashtable_params);
	if (err)
		return err;
	test->priv = queue;
	return kunit_add_action_or_reset(test, nfqnl_test_destroy_map, queue);
}

static void nfqnl_test_boundaries(struct kunit *test)
{
	struct nfqnl_instance *queue = test->priv;

	queue->mem_limit = 8192;
	queue->mem_used = 4096;
	KUNIT_EXPECT_FALSE(test, nfqnl_mem_full(queue, 4096));
	KUNIT_EXPECT_TRUE(test, nfqnl_mem_full(queue, 4097));
	queue->mem_used = 8193;
	KUNIT_EXPECT_TRUE(test, nfqnl_mem_full(queue, 1));
	queue->mem_used = 0;
	queue->mem_limit = 0;
	KUNIT_EXPECT_TRUE(test, nfqnl_mem_full(queue, 1));
	queue->mem_limit = U64_MAX;
	queue->mem_used = U64_MAX - 10;
	KUNIT_EXPECT_FALSE(test, nfqnl_mem_full(queue, 10));
	KUNIT_EXPECT_TRUE(test, nfqnl_mem_full(queue, 11));

	KUNIT_EXPECT_GT(test, nfqnl_mem_limit(U32_MAX), (u64)U32_MAX);
	KUNIT_EXPECT_EQ(test, nfqnl_mem_pages(0), 0ULL);
	KUNIT_EXPECT_EQ(test, nfqnl_mem_pages(PAGE_SIZE), 1ULL);
	KUNIT_EXPECT_EQ(test, nfqnl_mem_pages(PAGE_SIZE + 1), 2ULL);
	KUNIT_EXPECT_EQ(test, nfqnl_mem_pages(U64_MAX),
			(U64_MAX >> PAGE_SHIFT) + 1);
}

static void nfqnl_test_small_queue(struct kunit *test)
{
	struct nfqnl_instance *queue = test->priv;
	struct sk_buff *skb;

	skb = alloc_skb(SKB_WITH_OVERHEAD(2048), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_EQ(
		test, kunit_add_action_or_reset(test, nfqnl_test_free_skb, skb),
		0);
	KUNIT_ASSERT_EQ(test, nfqnl_set_config(queue, 1, NULL, 0, 0), 0);
	/* A standard 2 KiB head must fit in an empty length-one queue. */
	KUNIT_EXPECT_FALSE(test, nfqnl_mem_full(queue, skb->truesize));
}

static void nfqnl_test_page_head(struct kunit *test)
{
	struct nfqnl_instance *queue = test->priv;
	struct sk_buff *skb;
	struct page *page;

	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	skb = build_skb(page_address(page), PAGE_SIZE);
	if (!skb)
		__free_page(page);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_EQ(
		test, kunit_add_action_or_reset(test, nfqnl_test_free_skb, skb),
		0);
	skb_put_zero(skb, 64);

	/* A small payload still accounts for the complete receive allocation. */
	KUNIT_ASSERT_EQ(test, nfqnl_set_config(queue, 1, NULL, 0, 0), 0);
	if (PAGE_SIZE > 2048)
		KUNIT_EXPECT_TRUE(test, nfqnl_mem_full(queue, skb->truesize));
	else
		KUNIT_EXPECT_FALSE(test, nfqnl_mem_full(queue, skb->truesize));
	KUNIT_ASSERT_EQ(test,
			nfqnl_set_config(queue, DIV_ROUND_UP(PAGE_SIZE, 2048),
					 NULL, 0, 0),
			0);
	KUNIT_EXPECT_FALSE(test, nfqnl_mem_full(queue, skb->truesize));

	queue->mem_limit = skb->truesize - 1;
	KUNIT_EXPECT_TRUE(test, nfqnl_mem_full(queue, skb->truesize));
	queue->mem_limit = skb->truesize;
	KUNIT_EXPECT_FALSE(test, nfqnl_mem_full(queue, skb->truesize));
}

static void nfqnl_test_detached_charge(struct kunit *test)
{
	struct nfqnl_instance *queue = test->priv;
	struct nf_queue_entry entry = {};
	u64 detached, released;
	unsigned int saved;
	int err;

	entry.skb = alloc_skb(512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, entry.skb);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, nfqnl_test_free_skb,
						  entry.skb),
			0);
	saved = entry.skb->truesize;
	queue->mem_limit = saved;

	spin_lock_bh(&queue->lock);
	err = __enqueue_entry(queue, &entry);
	if (!err) {
		__dequeue_entry(queue, &entry);
		detached = queue->mem_used;
		/* Verdict payload replacement must not change the saved refund. */
		entry.skb->truesize += 128;
		nfqnl_mem_uncharge(queue, &entry);
		released = queue->mem_used;
		entry.skb->truesize = saved;
	}
	spin_unlock_bh(&queue->lock);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, detached, (u64)saved);
	KUNIT_EXPECT_EQ(test, released, 0ULL);
	KUNIT_EXPECT_EQ(test, entry.truesize, 0U);
	KUNIT_EXPECT_EQ(test, queue->queue_total, 0U);
	KUNIT_EXPECT_TRUE(test, list_empty(&queue->queue_list));
	KUNIT_EXPECT_FALSE(test, nfqnl_mem_full(queue, saved));

	/* FAIL_OPEN and notification rollback can reach reinjection uncharged. */
	spin_lock_bh(&queue->lock);
	nfqnl_mem_uncharge(queue, &entry);
	spin_unlock_bh(&queue->lock);
	KUNIT_EXPECT_EQ(test, queue->mem_used, 0ULL);
}

static void nfqnl_test_limit_update(struct kunit *test)
{
	struct nfqnl_instance *queue = test->priv;
	u64 used = nfqnl_mem_limit(8);

	queue->mem_used = used;
	queue->queue_total = 2;
	KUNIT_ASSERT_EQ(test, nfqnl_set_config(queue, 1, NULL, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, queue->queue_maxlen, 1U);
	KUNIT_EXPECT_EQ(test, queue->mem_limit, nfqnl_mem_limit(1));
	KUNIT_EXPECT_EQ(test, queue->mem_used, used);
	KUNIT_EXPECT_EQ(test, queue->queue_total, 2U);
	KUNIT_EXPECT_TRUE(test, nfqnl_mem_full(queue, 1));
	KUNIT_ASSERT_EQ(test, nfqnl_set_config(queue, 16, NULL, 0, 0), 0);
	KUNIT_EXPECT_FALSE(test, nfqnl_mem_full(queue, 1));
	KUNIT_EXPECT_EQ(test, queue->mem_used, used);
	KUNIT_EXPECT_EQ(test, queue->mem_reserved, 0ULL);
}

struct nfqnl_test_creation {
	struct nfnl_queue_net queues;
	struct nfqnl_instance *instance;
	unsigned int config_calls;
	int config_error;
};

static struct nfqnl_instance *nfqnl_test_lookup(struct nfqnl_test_creation *ctx,
						u16 queue_num)
{
	struct nfqnl_instance *queue;

	rcu_read_lock();
	queue = instance_lookup(&ctx->queues, queue_num);
	rcu_read_unlock();
	return queue;
}

static int
nfqnl_test_config_private(struct nfqnl_instance *queue, unsigned int maxlen,
			  const struct nfqnl_msg_config_params *params,
			  u32 flags, u32 mask)
{
	struct kunit *test = kunit_get_current_test();
	struct nfqnl_test_creation *ctx = test->priv;
	int err;

	ctx->config_calls++;
	KUNIT_EXPECT_PTR_EQ(test, nfqnl_test_lookup(ctx, queue->queue_num),
			    NULL);
	if (ctx->config_error)
		return ctx->config_error;

	/* Exercise the real configuration while checking its visibility. */
	kunit_deactivate_static_stub(test, nfqnl_set_config);
	err = nfqnl_set_config(queue, maxlen, params, flags, mask);
	KUNIT_EXPECT_PTR_EQ(test, nfqnl_test_lookup(ctx, queue->queue_num),
			    NULL);
	return err;
}

static void nfqnl_test_destroy_instance(void *data)
{
	struct nfqnl_test_creation *ctx = data;

	if (!IS_ERR_OR_NULL(ctx->instance)) {
		instance_destroy(&ctx->queues, ctx->instance);
		rcu_barrier();
		flush_workqueue(nfq_cleanup_wq);
	}
}

static struct nfqnl_test_creation *nfqnl_test_creation_init(struct kunit *test)
{
	struct nfqnl_test_creation *ctx;
	unsigned int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	spin_lock_init(&ctx->queues.instances_lock);
	for (i = 0; i < INSTANCE_BUCKETS; i++)
		INIT_HLIST_HEAD(&ctx->queues.instance_table[i]);
	if (kunit_add_action_or_reset(test, nfqnl_test_destroy_instance, ctx))
		return NULL;
	test->priv = ctx;
	kunit_activate_static_stub(test, nfqnl_set_config,
				   nfqnl_test_config_private);
	return ctx;
}

static void nfqnl_test_create_active(struct kunit *test)
{
	const struct nfqnl_msg_config_params params = {
		.copy_range = htonl(512),
		.copy_mode = NFQNL_COPY_PACKET,
	};
	struct nfqnl_test_creation *ctx = nfqnl_test_creation_init(test);
	struct nfqnl_instance *queue, *duplicate;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	queue = instance_create(&init_net, &ctx->queues, 7, 1, 32, &params,
				NFQA_CFG_F_FAIL_OPEN, NFQA_CFG_F_FAIL_OPEN);
	ctx->instance = queue;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, queue);
	KUNIT_EXPECT_EQ(test, ctx->config_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, nfqnl_test_lookup(ctx, 7), queue);
	KUNIT_EXPECT_TRUE(test, queue->mem_activated);
	KUNIT_EXPECT_EQ(test, queue->queue_maxlen, 32U);
	KUNIT_EXPECT_EQ(test, queue->mem_limit, nfqnl_mem_limit(32));
	KUNIT_EXPECT_EQ(test, queue->copy_mode, (u8)NFQNL_COPY_PACKET);
	KUNIT_EXPECT_EQ(test, queue->copy_range, 512U);
	KUNIT_EXPECT_EQ(test, queue->flags, (u32)NFQA_CFG_F_FAIL_OPEN);

	duplicate =
		instance_create(&init_net, &ctx->queues, 7, 1, 1, NULL, 0, 0);
	KUNIT_EXPECT_TRUE(test, IS_ERR(duplicate));
	if (IS_ERR(duplicate)) {
		KUNIT_EXPECT_EQ(test, PTR_ERR(duplicate), (long)-EEXIST);
	} else {
		instance_destroy(&ctx->queues, duplicate);
		rcu_barrier();
		flush_workqueue(nfq_cleanup_wq);
	}
	KUNIT_EXPECT_PTR_EQ(test, nfqnl_test_lookup(ctx, 7), queue);
	KUNIT_EXPECT_EQ(test, queue->queue_maxlen, 32U);
}

static void nfqnl_test_create_inactive(struct kunit *test)
{
	struct nfqnl_test_creation *ctx = nfqnl_test_creation_init(test);
	struct nfqnl_instance *queue;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	queue = instance_create(&init_net, &ctx->queues, 7, 1,
				NFQNL_QMAX_DEFAULT, NULL, 0, 0);
	ctx->instance = queue;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, queue);
	KUNIT_EXPECT_EQ(test, ctx->config_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, nfqnl_test_lookup(ctx, 7), queue);
	KUNIT_EXPECT_FALSE(test, queue->mem_activated);
	KUNIT_EXPECT_EQ(test, queue->copy_mode, (u8)NFQNL_COPY_NONE);
	KUNIT_EXPECT_EQ(test, queue->queue_maxlen, NFQNL_QMAX_DEFAULT);
	KUNIT_EXPECT_EQ(test, queue->memcg.pages, 0ULL);
}

static void nfqnl_test_create_failure(struct kunit *test)
{
	const struct nfqnl_msg_config_params params = {
		.copy_mode = NFQNL_COPY_PACKET,
	};
	struct nfqnl_test_creation *ctx = nfqnl_test_creation_init(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->config_error = -ENOMEM;
	ctx->instance = instance_create(&init_net, &ctx->queues, 7, 1, 32,
					&params, 0, 0);
	KUNIT_ASSERT_TRUE(test, IS_ERR(ctx->instance));
	KUNIT_EXPECT_EQ(test, PTR_ERR(ctx->instance), (long)-ENOMEM);
	KUNIT_EXPECT_EQ(test, ctx->config_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, nfqnl_test_lookup(ctx, 7), NULL);
}

static struct kunit_case nfqnl_mem_test_cases[] = {
	KUNIT_CASE(nfqnl_test_boundaries),
	KUNIT_CASE(nfqnl_test_small_queue),
	KUNIT_CASE(nfqnl_test_page_head),
	KUNIT_CASE(nfqnl_test_detached_charge),
	KUNIT_CASE(nfqnl_test_limit_update),
	KUNIT_CASE(nfqnl_test_create_active),
	KUNIT_CASE(nfqnl_test_create_inactive),
	KUNIT_CASE(nfqnl_test_create_failure),
	{}
};

static struct kunit_suite nfqnl_mem_test_suite = {
	.name = "nfnetlink_queue_memory",
	.init = nfqnl_test_init,
	.test_cases = nfqnl_mem_test_cases,
};

kunit_test_suite(nfqnl_mem_test_suite);
