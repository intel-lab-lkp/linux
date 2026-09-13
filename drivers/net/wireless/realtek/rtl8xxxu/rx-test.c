// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/usb.h>

#include "rtl8xxxu.h"
#include "rx-test.h"

#define RX_TEST_URBS 32
#define RX_TEST_SKBS 512

struct rx_test {
	struct rtl8xxxu_priv priv;
	struct ieee80211_hw hw;
	struct rtl8xxxu_fileops fops;
	struct usb_device udev;
	struct rtl8xxxu_rx_urb *urbs[RX_TEST_URBS];
	struct sk_buff *skbs[RX_TEST_SKBS];
	unsigned int allocated;
	unsigned int buffers;
	unsigned int alloc_calls;
	unsigned int alloc_fail_at;
	unsigned int submit_calls;
	unsigned int submit_fail_at;
	int submit_error;
	bool fail_skb;
	bool retry_pending;
	unsigned int retry_arms;
	atomic_t normal_runs;
};

static struct rx_test *rx_current(void)
{
	return kunit_get_current_test()->priv;
}

static struct rtl8xxxu_rx_urb *rx_alloc_object(void)
{
	struct rx_test *ctx = rx_current();
	struct rtl8xxxu_rx_urb *rx;

	ctx->alloc_calls++;
	if (ctx->alloc_calls == ctx->alloc_fail_at)
		return NULL;
	rx = kmalloc_obj(struct rtl8xxxu_rx_urb);
	if (rx)
		ctx->urbs[ctx->allocated++] = rx;
	return rx;
}

static struct sk_buff *rx_alloc_buffer(unsigned int size)
{
	struct rx_test *ctx = rx_current();
	struct sk_buff *skb;

	if (ctx->fail_skb)
		return NULL;
	skb = alloc_skb(size, GFP_KERNEL);
	if (skb) {
		/* Retain an observer reference to detect a lost driver reference. */
		skb_get(skb);
		ctx->skbs[ctx->buffers++] = skb;
	}
	return skb;
}

static int rx_submit(struct urb *urb, gfp_t flags)
{
	struct rx_test *ctx = rx_current();

	ctx->submit_calls++;
	if (!ctx->submit_fail_at ||
	    ctx->submit_calls == ctx->submit_fail_at)
		return ctx->submit_error;
	return 0;
}

static void rx_schedule_retry(struct rtl8xxxu_priv *priv)
{
	struct rx_test *ctx = rx_current();

	/* Model the single pending delayed-work ticket, not a submission. */
	if (!ctx->retry_pending) {
		ctx->retry_pending = true;
		ctx->retry_arms++;
	}
}

static void rx_observe_work(struct work_struct *work)
{
	struct rtl8xxxu_priv *priv;
	struct rx_test *ctx;

	priv = container_of(work, struct rtl8xxxu_priv, rx_urb_wq);
	ctx = container_of(priv, struct rx_test, priv);
	atomic_inc(&ctx->normal_runs);
}

static struct rx_test *rx_init(struct kunit *test, bool fake_timer)
{
	struct rx_test *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	test->priv = ctx;
	ctx->hw.priv = &ctx->priv;
	ctx->priv.hw = &ctx->hw;
	ctx->priv.udev = &ctx->udev;
	ctx->priv.fops = &ctx->fops;
	ctx->fops.rx_desc_size = sizeof(struct rtl8xxxu_rxdesc16);
	spin_lock_init(&ctx->priv.rx_urb_lock);
	INIT_LIST_HEAD(&ctx->priv.rx_urb_pending_list);
	INIT_LIST_HEAD(&ctx->priv.rx_urb_retry_list);
	init_usb_anchor(&ctx->priv.rx_anchor);
	INIT_WORK(&ctx->priv.rx_urb_wq, rx_observe_work);
	INIT_DELAYED_WORK(&ctx->priv.rx_urb_retry_wq,
			  rtl8xxxu_rx_urb_retry_work);
	atomic_set(&ctx->normal_runs, 0);

	kunit_activate_static_stub(test, rtl8xxxu_alloc_rx_urb, rx_alloc_object);
	kunit_activate_static_stub(test, rtl8xxxu_alloc_rx_skb, rx_alloc_buffer);
	kunit_activate_static_stub(test, rtl8xxxu_rx_usb_submit, rx_submit);
	if (fake_timer)
		kunit_activate_static_stub(test, rtl8xxxu_schedule_rx_retry,
					   rx_schedule_retry);
	return ctx;
}

static void rx_get_observers(struct rx_test *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->allocated; i++)
		usb_get_urb(&ctx->urbs[i]->urb);
}

static int rx_pool(struct rx_test *ctx, unsigned int count)
{
	struct rtl8xxxu_rx_urb *rx;
	unsigned int i;
	int ret;

	ret = rtl8xxxu_alloc_rx_urbs(&ctx->priv);
	rx_get_observers(ctx);
	if (ret)
		return ret;

	/* A reduced pool also tests recovery without the normal watermark. */
	for (i = count; i < ctx->allocated; i++) {
		rx = ctx->urbs[i];
		list_del_init(&rx->list);
		ctx->priv.rx_urb_pending_count--;
		usb_free_urb(&rx->urb);
	}
	return 0;
}

static void rx_giveback(struct rx_test *ctx, unsigned int i, int status)
{
	struct urb *urb = &ctx->urbs[i]->urb;

	/* The fake USB boundary does not emulate HCD giveback. */
	usb_unanchor_urb(urb);
	urb->status = status;
	urb->actual_length = 0;
	rtl8xxxu_rx_complete(urb);
}

static void rx_finish(struct kunit *test, struct rx_test *ctx)
{
	struct urb *urb;
	unsigned int i;

	ctx->priv.shutdown = true;
	cancel_delayed_work_sync(&ctx->priv.rx_urb_retry_wq);
	cancel_work_sync(&ctx->priv.rx_urb_wq);
	for (i = 0; i < ctx->allocated; i++) {
		urb = &ctx->urbs[i]->urb;
		if (urb->anchor)
			rx_giveback(ctx, i, -ENOENT);
	}
	rtl8xxxu_free_rx_resources(&ctx->priv);
	KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_pending_count, 0);
	KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 0);
	KUNIT_EXPECT_TRUE(test, usb_anchor_empty(&ctx->priv.rx_anchor));
	for (i = 0; i < ctx->allocated; i++) {
		urb = &ctx->urbs[i]->urb;
		KUNIT_EXPECT_EQ(test, refcount_read(&urb->kref.refcount), 1);
		usb_free_urb(urb);
	}
	for (i = 0; i < ctx->buffers; i++) {
		KUNIT_EXPECT_EQ(test, refcount_read(&ctx->skbs[i]->users), 1);
		kfree_skb(ctx->skbs[i]);
	}
}

static void rx_retry_step(struct kunit *test, struct rx_test *ctx)
{
	KUNIT_ASSERT_TRUE(test, ctx->retry_pending);
	ctx->retry_pending = false;
	rtl8xxxu_rx_urb_retry_work(&ctx->priv.rx_urb_retry_wq.work);
	flush_work(&ctx->priv.rx_urb_wq);
	/* Execute the actual worker in the test task so boundary stubs apply. */
	rtl8xxxu_rx_urb_work(&ctx->priv.rx_urb_wq);
}

static void rx_completion_submit_chain(struct kunit *test)
{
	static const int errors[] = {
		-EPROTO, -EILSEQ, -ETIME, -EOVERFLOW, -ECOMM, -ENOSR,
	};
	static const int submissions[] = { -ENOMEM, -EAGAIN };
	struct rx_test *ctx;
	unsigned int e, f, i;
	int runs;

	for (e = 0; e < ARRAY_SIZE(errors); e++) {
		for (f = 0; f < ARRAY_SIZE(submissions); f++) {
			ctx = rx_init(test, true);
			KUNIT_ASSERT_NOT_NULL(test, ctx);
			KUNIT_ASSERT_EQ(test, rx_pool(ctx, RX_TEST_URBS), 0);
			KUNIT_ASSERT_EQ(test, rtl8xxxu_start_rx(&ctx->priv), 0);
			for (i = 0; i < RX_TEST_URBS; i++)
				rx_giveback(ctx, i, errors[e]);
			KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 32);
			ctx->submit_error = submissions[f];
			rx_retry_step(test, ctx);
			runs = atomic_read(&ctx->normal_runs);
			flush_work(&ctx->priv.rx_urb_wq);
			KUNIT_EXPECT_EQ(test, atomic_read(&ctx->normal_runs), runs);
			KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_pending_count, 0);
			KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 32);
			KUNIT_EXPECT_TRUE(test, ctx->retry_pending);
			for (i = 0; i < ctx->buffers; i++)
				KUNIT_EXPECT_EQ(test,
						refcount_read(&ctx->skbs[i]->users), 1);
			ctx->submit_error = 0;
			rx_retry_step(test, ctx);
			KUNIT_EXPECT_FALSE(test, ctx->retry_pending);
			KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 0);
			KUNIT_EXPECT_FALSE(test, usb_anchor_empty(&ctx->priv.rx_anchor));
			rx_finish(test, ctx);
		}
	}
}

static void rx_worker_batch_sizes(struct kunit *test)
{
	static const unsigned int sizes[] = { 1, 8, 9, 32 };
	struct rx_test *ctx;
	unsigned int n, pass;
	int runs;

	for (n = 0; n < ARRAY_SIZE(sizes); n++) {
		ctx = rx_init(test, true);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		KUNIT_ASSERT_EQ(test, rx_pool(ctx, sizes[n]), 0);
		ctx->submit_error = -EAGAIN;
		rtl8xxxu_rx_urb_work(&ctx->priv.rx_urb_wq);
		for (pass = 0; pass < 3; pass++) {
			runs = atomic_read(&ctx->normal_runs);
			flush_work(&ctx->priv.rx_urb_wq);
			KUNIT_EXPECT_EQ(test, atomic_read(&ctx->normal_runs), runs);
			KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, sizes[n]);
			KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_pending_count, 0);
			rx_retry_step(test, ctx);
		}
		ctx->submit_error = 0;
		rx_retry_step(test, ctx);
		KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 0);
		rx_finish(test, ctx);
	}
}

static void rx_skb_allocation_failure(struct kunit *test)
{
	struct rx_test *ctx;

	ctx = rx_init(test, true);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_EQ(test, rx_pool(ctx, 8), 0);
	ctx->fail_skb = true;
	rtl8xxxu_rx_urb_work(&ctx->priv.rx_urb_wq);
	KUNIT_EXPECT_EQ(test, ctx->submit_calls, 0);
	KUNIT_EXPECT_EQ(test, ctx->buffers, 0);
	KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 8);
	ctx->fail_skb = false;
	rx_retry_step(test, ctx);
	KUNIT_EXPECT_EQ(test, ctx->submit_calls, 8);
	rx_finish(test, ctx);
}

static void rx_start_allocation_failure(struct kunit *test)
{
	static const unsigned int positions[] = { 1, 8, 32 };
	struct rx_test *ctx;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(positions); i++) {
		ctx = rx_init(test, true);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		ctx->alloc_fail_at = positions[i];
		KUNIT_EXPECT_EQ(test, rtl8xxxu_alloc_rx_urbs(&ctx->priv), -ENOMEM);
		rx_get_observers(ctx);
		KUNIT_EXPECT_EQ(test, ctx->allocated, positions[i] - 1);
		KUNIT_EXPECT_EQ(test, ctx->submit_calls, 0);
		KUNIT_EXPECT_FALSE(test, ctx->retry_pending);
		rx_finish(test, ctx);
	}
}

static void rx_start_temporary_failure(struct kunit *test)
{
	static const int errors[] = { -ENOMEM, -EAGAIN };
	struct rx_test *ctx;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(errors); i++) {
		ctx = rx_init(test, true);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		KUNIT_ASSERT_EQ(test, rx_pool(ctx, 32), 0);
		ctx->submit_error = errors[i];
		KUNIT_EXPECT_EQ(test, rtl8xxxu_start_rx(&ctx->priv), 0);
		KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 32);
		KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_pending_count, 0);
		KUNIT_EXPECT_TRUE(test, ctx->retry_pending);
		ctx->submit_error = 0;
		rx_retry_step(test, ctx);
		rx_finish(test, ctx);
	}
}

static void rx_start_fatal_failure(struct kunit *test)
{
	static const unsigned int positions[] = { 1, 8, 32 };
	struct rx_test *ctx;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(positions); i++) {
		ctx = rx_init(test, true);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		KUNIT_ASSERT_EQ(test, rx_pool(ctx, 32), 0);
		ctx->submit_fail_at = positions[i];
		ctx->submit_error = -ENODEV;
		KUNIT_EXPECT_EQ(test, rtl8xxxu_start_rx(&ctx->priv), -ENODEV);
		KUNIT_EXPECT_EQ(test, ctx->submit_calls, positions[i]);
		KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_pending_count, 0);
		KUNIT_EXPECT_FALSE(test, ctx->retry_pending);
		rx_finish(test, ctx);
	}
}

static void rx_terminal_completion(struct kunit *test)
{
	static const int errors[] = {
		-ENOENT, -ECONNRESET, -ENODEV, -ESHUTDOWN, -EPIPE,
	};
	struct rx_test *ctx;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(errors); i++) {
		ctx = rx_init(test, true);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		KUNIT_ASSERT_EQ(test, rx_pool(ctx, 1), 0);
		KUNIT_ASSERT_EQ(test, rtl8xxxu_start_rx(&ctx->priv), 0);
		rx_giveback(ctx, 0, errors[i]);
		KUNIT_EXPECT_FALSE(test, ctx->retry_pending);
		KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 0);
		rx_finish(test, ctx);
	}
}

static void rx_shutdown_completion(struct kunit *test)
{
	struct rx_test *ctx;

	ctx = rx_init(test, true);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_EQ(test, rx_pool(ctx, 1), 0);
	KUNIT_ASSERT_EQ(test, rtl8xxxu_start_rx(&ctx->priv), 0);
	ctx->priv.shutdown = true;
	rx_giveback(ctx, 0, -EPROTO);
	KUNIT_EXPECT_FALSE(test, ctx->retry_pending);
	KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 0);
	rx_finish(test, ctx);
}

static void rx_shutdown_pending_retry(struct kunit *test)
{
	struct rx_test *ctx;

	ctx = rx_init(test, true);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_EQ(test, rx_pool(ctx, 8), 0);
	ctx->submit_error = -ENOMEM;
	rtl8xxxu_rx_urb_work(&ctx->priv.rx_urb_wq);
	ctx->priv.shutdown = true;
	rtl8xxxu_rx_urb_retry_work(&ctx->priv.rx_urb_retry_wq.work);
	flush_work(&ctx->priv.rx_urb_wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->normal_runs), 0);
	KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 8);
	rx_finish(test, ctx);
}

static void rx_shutdown_submission_failure(struct kunit *test)
{
	struct rx_test *ctx;

	ctx = rx_init(test, true);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_EQ(test, rx_pool(ctx, 8), 0);
	ctx->priv.shutdown = true;
	ctx->submit_error = -ENOMEM;
	rtl8xxxu_rx_urb_work(&ctx->priv.rx_urb_wq);
	KUNIT_EXPECT_FALSE(test, ctx->retry_pending);
	KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 0);
	rx_finish(test, ctx);
}

static void rx_real_delayed_bridge(struct kunit *test)
{
	struct rx_test *ctx;

	ctx = rx_init(test, false);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_EQ(test, rx_pool(ctx, 1), 0);
	KUNIT_ASSERT_EQ(test, rtl8xxxu_start_rx(&ctx->priv), 0);
	rx_giveback(ctx, 0, -EPROTO);
	flush_delayed_work(&ctx->priv.rx_urb_retry_wq);
	flush_work(&ctx->priv.rx_urb_wq);
	KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_retry_count, 0);
	KUNIT_EXPECT_EQ(test, ctx->priv.rx_urb_pending_count, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->normal_runs), 1);
	rx_finish(test, ctx);
}

static struct kunit_case rx_cases[] = {
	KUNIT_CASE(rx_completion_submit_chain),
	KUNIT_CASE(rx_worker_batch_sizes),
	KUNIT_CASE(rx_skb_allocation_failure),
	KUNIT_CASE(rx_start_allocation_failure),
	KUNIT_CASE(rx_start_temporary_failure),
	KUNIT_CASE(rx_start_fatal_failure),
	KUNIT_CASE(rx_terminal_completion),
	KUNIT_CASE(rx_shutdown_completion),
	KUNIT_CASE(rx_shutdown_pending_retry),
	KUNIT_CASE(rx_shutdown_submission_failure),
	KUNIT_CASE(rx_real_delayed_bridge),
	{}
};

static struct kunit_suite rx_suite = {
	.name = "rtl8xxxu-rx",
	.test_cases = rx_cases,
};

kunit_test_suite(rx_suite);
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("rtl8xxxu RX recovery KUnit tests");
MODULE_LICENSE("GPL");
