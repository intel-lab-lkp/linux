// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the SPI core DMA mapping error paths.
 *
 * Included from spi.c so the tests can drive the static
 * __spi_map_msg()/__spi_unmap_msg() state machine directly, without adding
 * any indirection to the production mapping path.  Same arrangement as
 * drivers/scsi/scsi_lib.c and lib/kunit/executor.c.
 *
 * The invariant under test:
 *
 *   When __spi_map_msg() returns an error, no transfer in the message may
 *   still claim a DMA mapping.  Any transfer that was mapped before the
 *   failure must have an empty SG table and a cleared *_sg_mapped flag.
 *   ctlr->cur_{tx,rx}_dma_dev must identify the device used for this map,
 *   rather than a device retained from an earlier message.
 *
 * That invariant is what makes the subsequent
 * spi_finalize_current_message() -> spi_unmap_msg() -> __spi_unmap_msg()
 * pass a no-op.  Without it, __spi_unmap_msg() unmaps again using
 * ctlr->cur_{tx,rx}_dma_dev, which __spi_map_msg() only publishes after the
 * whole loop succeeds -- so it is NULL on the controller's first
 * DMA-mapped message.  dma_unmap_sg_attrs() dereferences that device before
 * it ever looks at nents, so the second unmap is a NULL dereference
 * regardless of the sgt having been emptied.
 *
 * How the failure is injected: a transfer with len == 0 makes
 * spi_map_buf_attrs() compute sgs = DIV_ROUND_UP(0, desc_len) = 0, and
 * __sg_alloc_table() rejects nents == 0 with -EINVAL.  That is instant,
 * arch-independent and warning-free.  It is a test artifice standing in for
 * any real map failure (-ENOMEM from sg_alloc_table(), -EIO from swiotlb
 * exhaustion, -EINVAL from an unmappable buffer); the core's error handling
 * does not depend on which one occurred.
 */

#include <kunit/device.h>
#include <kunit/test.h>

#define SPI_TEST_LEN		256
#define SPI_TEST_XFERS		2

struct spi_test_ctx {
	struct spi_controller	*ctlr;
	struct spi_device	*spi;
	struct device		*dma_dev;
	struct device		*stale_dma_dev;
	struct spi_transfer	xfer[SPI_TEST_XFERS];
	struct spi_message	msg;
	void			*buf[SPI_TEST_XFERS * 2];
};

static bool spi_test_can_dma(struct spi_controller *ctlr,
			     struct spi_device *spi,
			     struct spi_transfer *xfer)
{
	/* Opt every transfer into the core DMA mapping path. */
	return true;
}

/*
 * A bare kzalloc'd controller is enough: __spi_map_msg() and
 * __spi_unmap_msg() only touch can_dma, dma_tx, dma_rx, dma_map_dev,
 * max_dma_len and cur_*_dma_dev.  Leaving dma_tx/dma_rx NULL makes both
 * directions resolve to dma_map_dev, so there is no need to fake dmaengine
 * channels, and ctlr->dev is never dereferenced.  Skipping
 * spi_alloc_host()/spi_register_controller() keeps the fixture free of
 * device and queue lifecycle.
 */
static struct spi_test_ctx *spi_test_ctx_new(struct kunit *test)
{
	struct spi_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	ctx->dma_dev = kunit_device_register(test, "spi-core-error-path");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->dma_dev);
	ctx->stale_dma_dev =
		kunit_device_register(test, "spi-core-stale-dma-device");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->stale_dma_dev);

	/* Real masks keep either device safe if a failing assertion aborts. */
	KUNIT_ASSERT_EQ(test, 0,
			dma_coerce_mask_and_coherent(ctx->dma_dev,
						     DMA_BIT_MASK(64)));
	KUNIT_ASSERT_EQ(test, 0,
			dma_coerce_mask_and_coherent(ctx->stale_dma_dev,
						     DMA_BIT_MASK(64)));

	ctx->ctlr = kunit_kzalloc(test, sizeof(*ctx->ctlr), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->ctlr);

	ctx->spi = kunit_kzalloc(test, sizeof(*ctx->spi), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->spi);

	ctx->ctlr->can_dma = spi_test_can_dma;
	ctx->ctlr->dma_map_dev = ctx->dma_dev;
	/* spi_register_controller() would do this; we are not registering. */
	ctx->ctlr->max_dma_len = INT_MAX;

	ctx->spi->controller = ctx->ctlr;
	spi_message_init(&ctx->msg);
	ctx->msg.spi = ctx->spi;

	return ctx;
}

static void *spi_test_buf(struct kunit *test, struct spi_test_ctx *ctx,
			  unsigned int slot)
{
	KUNIT_ASSERT_LT(test, slot, ARRAY_SIZE(ctx->buf));

	ctx->buf[slot] = kunit_kzalloc(test, SPI_TEST_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->buf[slot]);

	return ctx->buf[slot];
}

/*
 * Pin cur_*_dma_dev to a different valid device before the failing map.  This
 * emulates state retained from an earlier message and lets the test verify
 * that __spi_map_msg() publishes the device which owns the new mappings.
 * It also keeps an unfixed tree from dereferencing NULL while reporting the
 * regression.  ASSERTs abort the case before cleanup can use the stale device.
 */
static void spi_test_pin_stale_dma_devs(struct spi_test_ctx *ctx)
{
	ctx->ctlr->cur_tx_dma_dev = ctx->stale_dma_dev;
	ctx->ctlr->cur_rx_dma_dev = ctx->stale_dma_dev;
}

static void spi_test_assert_dma_devs_published(struct kunit *test,
					       struct spi_test_ctx *ctx)
{
	KUNIT_ASSERT_PTR_EQ(test, ctx->ctlr->cur_tx_dma_dev, ctx->dma_dev);
	KUNIT_ASSERT_PTR_EQ(test, ctx->ctlr->cur_rx_dma_dev, ctx->dma_dev);
}

static void spi_test_assert_nothing_mapped(struct kunit *test,
					   struct spi_test_ctx *ctx,
					   unsigned int nr_xfers)
{
	unsigned int i;

	for (i = 0; i < nr_xfers; i++) {
		KUNIT_ASSERT_FALSE_MSG(test, ctx->xfer[i].tx_sg_mapped,
				       "xfer[%u] still claims a TX mapping after __spi_map_msg() failed",
				       i);
		KUNIT_ASSERT_FALSE_MSG(test, ctx->xfer[i].rx_sg_mapped,
				       "xfer[%u] still claims an RX mapping after __spi_map_msg() failed",
				       i);
		KUNIT_EXPECT_PTR_EQ(test, ctx->xfer[i].tx_sg.sgl, NULL);
		KUNIT_EXPECT_EQ(test, ctx->xfer[i].tx_sg.orig_nents, 0U);
		KUNIT_EXPECT_EQ(test, ctx->xfer[i].tx_sg.nents, 0U);
		KUNIT_EXPECT_PTR_EQ(test, ctx->xfer[i].rx_sg.sgl, NULL);
		KUNIT_EXPECT_EQ(test, ctx->xfer[i].rx_sg.orig_nents, 0U);
		KUNIT_EXPECT_EQ(test, ctx->xfer[i].rx_sg.nents, 0U);
	}
}

/*
 * xfer0 maps TX and RX; xfer1's TX map fails.
 *
 * This exits __spi_map_msg() through the bare `return ret` after the TX
 * spi_map_buf_attrs() call, which has no rollback code at all -- not even
 * the ad-hoc one the RX branch has.  xfer0 is left fully mapped with both
 * flags set and cur_*_dma_dev unpublished.
 *
 * This is the deterministic real-world shape: a driver whose second transfer
 * hands over a buffer the core cannot map (e.g. a static const payload table
 * after a kmalloc'd command byte) hits it with no memory pressure at all.
 */
static void spi_later_tx_fail_rolls_back_earlier(struct kunit *test)
{
	struct spi_test_ctx *ctx = spi_test_ctx_new(test);
	int ret;

	ctx->xfer[0].tx_buf = spi_test_buf(test, ctx, 0);
	ctx->xfer[0].rx_buf = spi_test_buf(test, ctx, 1);
	ctx->xfer[0].len = SPI_TEST_LEN;

	ctx->xfer[1].tx_buf = spi_test_buf(test, ctx, 2);
	ctx->xfer[1].rx_buf = NULL;
	ctx->xfer[1].len = 0;			/* forces -EINVAL */

	spi_message_add_tail(&ctx->xfer[0], &ctx->msg);
	spi_message_add_tail(&ctx->xfer[1], &ctx->msg);

	spi_test_pin_stale_dma_devs(ctx);

	ret = __spi_map_msg(ctx->ctlr, &ctx->msg);
	KUNIT_ASSERT_EQ(test, ret, -EINVAL);

	spi_test_assert_dma_devs_published(test, ctx);
	spi_test_assert_nothing_mapped(test, ctx, SPI_TEST_XFERS);

	/* Only reached once the invariant holds: cleanup must be a no-op. */
	KUNIT_EXPECT_EQ(test, 0, __spi_unmap_msg(ctx->ctlr, &ctx->msg));
}

/*
 * xfer0 maps TX and RX; the RX-only xfer1 then fails to map.
 *
 * The old RX failure branch attempts to unmap xfer1's never-mapped TX table,
 * then returns without rolling back xfer0 or publishing cur_*_dma_dev.
 */
static void spi_later_rx_fail_rolls_back_earlier(struct kunit *test)
{
	struct spi_test_ctx *ctx = spi_test_ctx_new(test);
	int ret;

	ctx->xfer[0].tx_buf = spi_test_buf(test, ctx, 0);
	ctx->xfer[0].rx_buf = spi_test_buf(test, ctx, 1);
	ctx->xfer[0].len = SPI_TEST_LEN;

	ctx->xfer[1].tx_buf = NULL;
	ctx->xfer[1].rx_buf = spi_test_buf(test, ctx, 2);
	ctx->xfer[1].len = 0;			/* forces -EINVAL */

	spi_message_add_tail(&ctx->xfer[0], &ctx->msg);
	spi_message_add_tail(&ctx->xfer[1], &ctx->msg);

	spi_test_pin_stale_dma_devs(ctx);

	ret = __spi_map_msg(ctx->ctlr, &ctx->msg);
	KUNIT_ASSERT_EQ(test, ret, -EINVAL);

	spi_test_assert_dma_devs_published(test, ctx);
	spi_test_assert_nothing_mapped(test, ctx, SPI_TEST_XFERS);

	KUNIT_EXPECT_EQ(test, 0, __spi_unmap_msg(ctx->ctlr, &ctx->msg));
}

/*
 * Happy-path guard, so a fix that unwinds too eagerly cannot pass: a fully
 * mappable message must still map both directions, publish both devices, and
 * unmap cleanly.
 */
static void spi_map_success_publishes_dma_devs(struct kunit *test)
{
	struct spi_test_ctx *ctx = spi_test_ctx_new(test);
	int ret;

	ctx->xfer[0].tx_buf = spi_test_buf(test, ctx, 0);
	ctx->xfer[0].rx_buf = spi_test_buf(test, ctx, 1);
	ctx->xfer[0].len = SPI_TEST_LEN;

	spi_message_add_tail(&ctx->xfer[0], &ctx->msg);

	ret = __spi_map_msg(ctx->ctlr, &ctx->msg);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_TRUE(test, ctx->xfer[0].tx_sg_mapped);
	KUNIT_EXPECT_TRUE(test, ctx->xfer[0].rx_sg_mapped);
	KUNIT_EXPECT_PTR_EQ(test, ctx->ctlr->cur_tx_dma_dev, ctx->dma_dev);
	KUNIT_EXPECT_PTR_EQ(test, ctx->ctlr->cur_rx_dma_dev, ctx->dma_dev);

	KUNIT_EXPECT_EQ(test, 0, __spi_unmap_msg(ctx->ctlr, &ctx->msg));

	KUNIT_EXPECT_FALSE(test, ctx->xfer[0].tx_sg_mapped);
	KUNIT_EXPECT_FALSE(test, ctx->xfer[0].rx_sg_mapped);
	KUNIT_EXPECT_PTR_EQ(test, ctx->xfer[0].tx_sg.sgl, NULL);
	KUNIT_EXPECT_PTR_EQ(test, ctx->xfer[0].rx_sg.sgl, NULL);
}

/*
 * The "no transfer has been mapped, bail out with success" path: a message
 * whose only transfer has neither buffer maps nothing and must still return
 * success with no flags set.
 */
static void spi_map_nothing_is_success(struct kunit *test)
{
	struct spi_test_ctx *ctx = spi_test_ctx_new(test);
	int ret;

	ctx->xfer[0].tx_buf = NULL;
	ctx->xfer[0].rx_buf = NULL;
	ctx->xfer[0].len = SPI_TEST_LEN;

	spi_message_add_tail(&ctx->xfer[0], &ctx->msg);

	ret = __spi_map_msg(ctx->ctlr, &ctx->msg);
	KUNIT_EXPECT_EQ(test, ret, 0);

	spi_test_assert_nothing_mapped(test, ctx, 1);
}

static struct kunit_case spi_core_error_path_cases[] = {
	KUNIT_CASE(spi_later_tx_fail_rolls_back_earlier),
	KUNIT_CASE(spi_later_rx_fail_rolls_back_earlier),
	KUNIT_CASE(spi_map_success_publishes_dma_devs),
	KUNIT_CASE(spi_map_nothing_is_success),
	{}
};

static struct kunit_suite spi_core_error_path_suite = {
	.name = "spi_core_error_path",
	.test_cases = spi_core_error_path_cases,
};

kunit_test_suite(spi_core_error_path_suite);
