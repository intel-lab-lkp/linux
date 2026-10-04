// SPDX-License-Identifier: GPL-2.0

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/mempool.h>
#include <linux/pci.h>

#include <scsi/scsi.h>
#include <scsi/scsi_transport_fc.h>

#include "lpfc_hw4.h"
#include "lpfc_hw.h"
#include "lpfc_sli.h"
#include "lpfc_sli4.h"
#include "lpfc_nl.h"
#include "lpfc_disc.h"
#include "lpfc.h"
#include "lpfc_scsi.h"
#include "lpfc_crtn.h"

enum lpfc_mbox_issue_mode {
	LPFC_MBOX_TEST_CAPTURE,
	LPFC_MBOX_TEST_COMPLETE,
	LPFC_MBOX_TEST_EXPIRE,
};

struct lpfc_mbox_test {
	struct lpfc_hba phba;
	struct lpfc_vport pport;
	LPFC_MBOXQ_t storage;
	mempool_t pool;
	struct lpfc_dmabuf mbuf_slots[2];
	LPFC_MBOXQ_t *mbox;
	void (*captured_cmpl)(struct lpfc_hba *phba, LPFC_MBOXQ_t *mbox);
	enum lpfc_mbox_issue_mode mode;
	int issue_result;
	unsigned int good_issues;
	bool storage_allocated;
	unsigned int excess_frees;
};

static void *lpfc_mbox_test_alloc(gfp_t gfp_mask, void *pool_data)
{
	struct lpfc_mbox_test *ctx = pool_data;

	(void)gfp_mask;
	if (ctx->storage_allocated)
		return NULL;

	ctx->storage_allocated = true;
	return &ctx->storage;
}

static void lpfc_mbox_test_free(void *element, void *pool_data)
{
	struct lpfc_mbox_test *ctx = pool_data;

	(void)element;
	ctx->excess_frees++;
}

static int lpfc_mbox_test_issue(struct lpfc_hba *phba, LPFC_MBOXQ_t *mbox,
				uint32_t flag)
{
	struct lpfc_mbox_test *ctx;

	(void)flag;
	ctx = container_of(phba, struct lpfc_mbox_test, phba);
	ctx->captured_cmpl = mbox->mbox_cmpl;
	if (ctx->good_issues) {
		ctx->good_issues--;
		ctx->captured_cmpl(phba, mbox);
		return MBX_SUCCESS;
	}

	if (ctx->mode == LPFC_MBOX_TEST_COMPLETE)
		ctx->captured_cmpl(phba, mbox);
	else if (ctx->mode == LPFC_MBOX_TEST_EXPIRE)
		/* End the wait without completing the command. */
		complete(mbox->ctx_u.mbox_wait);

	return ctx->issue_result;
}

static LPFC_MBOXQ_t *lpfc_mbox_test_take_from_pool(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;
	LPFC_MBOXQ_t *mbox;

	mbox = mempool_alloc_preallocated(&ctx->pool);
	KUNIT_EXPECT_PTR_EQ(test, mbox, &ctx->storage);
	return mbox;
}

static void lpfc_mbox_test_expect_released(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;
	LPFC_MBOXQ_t *mbox;

	mbox = lpfc_mbox_test_take_from_pool(test);
	if (mbox)
		mempool_free(mbox, &ctx->pool);
	else
		mempool_free(&ctx->storage, &ctx->pool);
	KUNIT_EXPECT_EQ(test, ctx->excess_frees, 0U);
}

static void lpfc_mbox_test_expect_caller_owned(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;

	KUNIT_EXPECT_PTR_EQ(test, mempool_alloc_preallocated(&ctx->pool), NULL);
}

static void lpfc_mbox_test_mbuf_exit(void *data)
{
	struct lpfc_hba *phba = data;
	struct lpfc_dma_pool *safety = &phba->lpfc_mbuf_safety_pool;

	while (safety->current_count) {
		safety->current_count--;
		dma_pool_free(phba->lpfc_mbuf_pool,
			      safety->elements[safety->current_count].virt,
			      safety->elements[safety->current_count].phys);
	}
	dma_pool_destroy(phba->lpfc_mbuf_pool);
}

static struct lpfc_rdp_context *lpfc_mbox_test_sfp_init(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;
	struct lpfc_hba *phba = &ctx->phba;
	struct lpfc_rdp_context *rdp;
	struct device *dev;
	int rc;

	/* lpfc_get_sfp_info_wait() allocates its own mailbox. */
	mempool_free(ctx->mbox, &ctx->pool);
	phba->sli_rev = LPFC_SLI_REV4;

	dev = kunit_device_register(test, "lpfc_mbox_test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	rc = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	KUNIT_ASSERT_EQ(test, rc, 0);
	phba->lpfc_mbuf_pool = dma_pool_create("lpfc_mbox_test", dev,
					       LPFC_BPL_SIZE, 8, 0);
	KUNIT_ASSERT_NOT_NULL(test, phba->lpfc_mbuf_pool);

	/* Park freed mbufs in the safety pool so that they can be counted. */
	phba->lpfc_mbuf_safety_pool.elements = ctx->mbuf_slots;
	phba->lpfc_mbuf_safety_pool.max_count = ARRAY_SIZE(ctx->mbuf_slots);
	rc = kunit_add_action_or_reset(test, lpfc_mbox_test_mbuf_exit, phba);
	KUNIT_ASSERT_EQ(test, rc, 0);

	rdp = kunit_kzalloc(test, sizeof(*rdp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rdp);
	return rdp;
}

static void lpfc_mbox_test_expect_sfp_released(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;

	lpfc_mbox_test_expect_released(test);
	KUNIT_EXPECT_EQ(test, ctx->phba.lpfc_mbuf_safety_pool.current_count,
			1U);
}

static int lpfc_mbox_test_init(struct kunit *test)
{
	struct lpfc_mbox_test *ctx;
	int rc;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	spin_lock_init(&ctx->phba.hbalock);
	ctx->phba.pport = &ctx->pport;
	ctx->phba.lpfc_sli_issue_mbox = lpfc_mbox_test_issue;
	ctx->pport.phba = &ctx->phba;
	ctx->issue_result = MBX_SUCCESS;
	rc = mempool_init(&ctx->pool, 1, lpfc_mbox_test_alloc,
			  lpfc_mbox_test_free, ctx);
	if (rc)
		return rc;

	ctx->phba.mbox_mem_pool = &ctx->pool;
	ctx->mbox = mempool_alloc_preallocated(&ctx->pool);
	if (!ctx->mbox) {
		mempool_exit(&ctx->pool);
		return -ENOMEM;
	}
	ctx->mbox->u.mb.mbxCommand = MBX_HEARTBEAT;
	test->priv = ctx;
	return 0;
}

static void lpfc_mbox_test_exit(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;
	LPFC_MBOXQ_t *mbox;

	mbox = mempool_alloc_preallocated(&ctx->pool);
	if (mbox)
		mempool_free(mbox, &ctx->pool);
	else
		mempool_free(&ctx->storage, &ctx->pool);
	mempool_exit(&ctx->pool);
}

static void lpfc_mbox_completion_before_timeout(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;
	int rc;

	ctx->mode = LPFC_MBOX_TEST_COMPLETE;
	rc = lpfc_sli_issue_mbox_wait(&ctx->phba, ctx->mbox, 0);
	KUNIT_EXPECT_EQ(test, rc, MBX_SUCCESS);
	lpfc_mbox_test_expect_caller_owned(test);

	lpfc_mbox_rsrc_cleanup(&ctx->phba, ctx->mbox, MBOX_THD_UNLOCKED);
	lpfc_mbox_test_expect_released(test);
}

static void lpfc_mbox_busy_completion_before_timeout(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;

	/* MBX_BUSY means the command was queued, not rejected. */
	ctx->issue_result = MBX_BUSY;
	lpfc_mbox_completion_before_timeout(test);
}

static void lpfc_mbox_timeout_then_current_callback(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;
	void (*callback)(struct lpfc_hba *phba, LPFC_MBOXQ_t *mbox);
	int rc;

	rc = lpfc_sli_issue_mbox_wait(&ctx->phba, ctx->mbox, 0);
	KUNIT_ASSERT_EQ(test, rc, MBX_TIMEOUT);
	callback = ctx->mbox->mbox_cmpl;
	callback(&ctx->phba, ctx->mbox);

	lpfc_mbox_test_expect_released(test);
}

static void lpfc_mbox_timeout_then_stale_callback(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;
	int rc;

	rc = lpfc_sli_issue_mbox_wait(&ctx->phba, ctx->mbox, 0);
	KUNIT_ASSERT_EQ(test, rc, MBX_TIMEOUT);
	KUNIT_ASSERT_TRUE(test,
			  ctx->captured_cmpl == lpfc_sli_wake_mbox_wait);
	ctx->captured_cmpl(&ctx->phba, ctx->mbox);

	lpfc_mbox_test_expect_released(test);
}

static void lpfc_mbox_immediate_issue_failure(struct kunit *test)
{
	struct lpfc_mbox_test *ctx = test->priv;
	int rc;

	ctx->issue_result = MBX_NOT_FINISHED;
	rc = lpfc_sli_issue_mbox_wait(&ctx->phba, ctx->mbox, 0);
	KUNIT_EXPECT_EQ(test, rc, MBX_NOT_FINISHED);
	lpfc_mbox_test_expect_caller_owned(test);

	lpfc_mbox_rsrc_cleanup(&ctx->phba, ctx->mbox, MBOX_THD_UNLOCKED);
	lpfc_mbox_test_expect_released(test);
}

static void lpfc_mbox_sfp_success(struct kunit *test)
{
	struct lpfc_rdp_context *rdp = lpfc_mbox_test_sfp_init(test);
	struct lpfc_mbox_test *ctx = test->priv;

	ctx->mode = LPFC_MBOX_TEST_COMPLETE;
	KUNIT_EXPECT_EQ(test, lpfc_get_sfp_info_wait(&ctx->phba, rdp), 0);
	lpfc_mbox_test_expect_sfp_released(test);
}

static void lpfc_mbox_sfp_a0_issue_failure(struct kunit *test)
{
	struct lpfc_rdp_context *rdp = lpfc_mbox_test_sfp_init(test);
	struct lpfc_mbox_test *ctx = test->priv;

	ctx->issue_result = MBX_NOT_FINISHED;
	KUNIT_EXPECT_NE(test, lpfc_get_sfp_info_wait(&ctx->phba, rdp), 0);
	lpfc_mbox_test_expect_sfp_released(test);
}

static void lpfc_mbox_sfp_a2_issue_failure(struct kunit *test)
{
	struct lpfc_rdp_context *rdp = lpfc_mbox_test_sfp_init(test);
	struct lpfc_mbox_test *ctx = test->priv;

	ctx->good_issues = 1;
	ctx->issue_result = MBX_NOT_FINISHED;
	KUNIT_EXPECT_NE(test, lpfc_get_sfp_info_wait(&ctx->phba, rdp), 0);
	lpfc_mbox_test_expect_sfp_released(test);
}

static void lpfc_mbox_test_sfp_a0_timeout(struct kunit *test, bool stale)
{
	struct lpfc_rdp_context *rdp = lpfc_mbox_test_sfp_init(test);
	struct lpfc_mbox_test *ctx = test->priv;
	int rc;

	ctx->mode = LPFC_MBOX_TEST_EXPIRE;
	rc = lpfc_get_sfp_info_wait(&ctx->phba, rdp);
	KUNIT_ASSERT_EQ(test, rc, MBX_TIMEOUT);
	lpfc_mbox_test_expect_caller_owned(test);
	KUNIT_EXPECT_EQ(test, ctx->phba.lpfc_mbuf_safety_pool.current_count,
			0U);

	/* The late completion now owns and releases the mailbox and mbuf. */
	if (stale)
		ctx->captured_cmpl(&ctx->phba, &ctx->storage);
	else
		ctx->storage.mbox_cmpl(&ctx->phba, &ctx->storage);
	lpfc_mbox_test_expect_sfp_released(test);
}

static void lpfc_mbox_sfp_a0_timeout(struct kunit *test)
{
	lpfc_mbox_test_sfp_a0_timeout(test, false);
}

static void lpfc_mbox_sfp_a0_timeout_stale_callback(struct kunit *test)
{
	lpfc_mbox_test_sfp_a0_timeout(test, true);
}

static struct kunit_case lpfc_mbox_test_cases[] = {
	KUNIT_CASE(lpfc_mbox_completion_before_timeout),
	KUNIT_CASE(lpfc_mbox_busy_completion_before_timeout),
	KUNIT_CASE(lpfc_mbox_timeout_then_current_callback),
	KUNIT_CASE(lpfc_mbox_timeout_then_stale_callback),
	KUNIT_CASE(lpfc_mbox_immediate_issue_failure),
	KUNIT_CASE(lpfc_mbox_sfp_success),
	KUNIT_CASE(lpfc_mbox_sfp_a0_issue_failure),
	KUNIT_CASE(lpfc_mbox_sfp_a2_issue_failure),
	KUNIT_CASE(lpfc_mbox_sfp_a0_timeout),
	KUNIT_CASE(lpfc_mbox_sfp_a0_timeout_stale_callback),
	{}
};

static struct kunit_suite lpfc_mbox_test_suite = {
	.name = "lpfc_mbox",
	.init = lpfc_mbox_test_init,
	.exit = lpfc_mbox_test_exit,
	.test_cases = lpfc_mbox_test_cases,
};

kunit_test_suite(lpfc_mbox_test_suite);
