// SPDX-License-Identifier: GPL-2.0-only

/*
 * Test the sg_table that dma_buf_map_attachment() hands to importers.
 */

#include <kunit/device.h>
#include <kunit/test.h>

#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/scatterlist.h>

#define MOCK_DMA_ADDR	0x12340000
#define MOCK_ORDER	2

struct mock_param {
	const char *desc;
	/* Entries of the CPU side. The DMA side always has one. */
	unsigned int orig_nents;
};

struct mock_buf {
	struct kunit *test;
	struct sg_table sgt;
	unsigned int alloc_nents;
	struct page *pages;
	bool unmapped;
};

static struct sg_table *mock_map(struct dma_buf_attachment *attach,
				 enum dma_data_direction dir)
{
	struct mock_buf *buf = attach->dmabuf->priv;

	return &buf->sgt;
}

static void mock_unmap(struct dma_buf_attachment *attach, struct sg_table *sgt,
		       enum dma_data_direction dir)
{
	struct mock_buf *buf = attach->dmabuf->priv;

	/* The exporter gets its own table back, not the copy */
	KUNIT_EXPECT_PTR_EQ(buf->test, sgt, &buf->sgt);
	buf->unmapped = true;
}

static void mock_free(struct mock_buf *buf)
{
	buf->sgt.orig_nents = buf->alloc_nents;
	sg_free_table(&buf->sgt);
	__free_pages(buf->pages, MOCK_ORDER);
	kfree(buf);
}

/* Runs from delayed fput, the test may be gone by then */
static void mock_release(struct dma_buf *dmabuf)
{
	mock_free(dmabuf->priv);
}

static const struct dma_buf_ops mock_ops = {
	.map_dma_buf = mock_map,
	.unmap_dma_buf = mock_unmap,
	.release = mock_release,
};

/* A table as an exporter would return it, with a made up DMA mapping */
static struct mock_buf *mock_alloc(struct kunit *test, unsigned int orig_nents)
{
	struct scatterlist *sg;
	struct mock_buf *buf;
	unsigned int i;

	buf = kzalloc_obj(*buf);
	if (!buf)
		return NULL;

	buf->pages = alloc_pages(GFP_KERNEL, MOCK_ORDER);
	if (!buf->pages)
		goto err_buf;

	buf->alloc_nents = max(orig_nents, 1U);
	if (sg_alloc_table(&buf->sgt, buf->alloc_nents, GFP_KERNEL))
		goto err_pages;

	/* An offset of a page keeps the lengths page aligned */
	for_each_sg(buf->sgt.sgl, sg, orig_nents, i)
		sg_set_page(sg, buf->pages + 2 * i, PAGE_SIZE, PAGE_SIZE);

	sg = buf->sgt.sgl;
	sg_dma_address(sg) = MOCK_DMA_ADDR;
	/* Without NEED_SG_DMA_LENGTH the DMA length is the CPU length */
	if (IS_ENABLED(CONFIG_NEED_SG_DMA_LENGTH) || !orig_nents)
		sg_dma_len(sg) = buf->alloc_nents * PAGE_SIZE;
#ifdef CONFIG_NEED_SG_DMA_FLAGS
	sg_dma_mark_bus_address(sg);
	sg_dma_mark_swiotlb(sg);
#endif
	buf->sgt.nents = 1;
	buf->sgt.orig_nents = orig_nents;
	buf->test = test;

	return buf;

err_pages:
	__free_pages(buf->pages, MOCK_ORDER);
err_buf:
	kfree(buf);
	return NULL;
}

/* Strict mode: only the DMA side, the CPU fields are cleared */
static void check_strict(struct kunit *test, struct sg_table *sgt)
{
	struct scatterlist *sg;
	int i;

	KUNIT_EXPECT_EQ(test, sgt->orig_nents, sgt->nents);

	for_each_sgtable_sg(sgt, sg, i) {
		KUNIT_EXPECT_NULL(test, sg_page(sg));
		KUNIT_EXPECT_EQ(test, sg->offset, 0U);
		if (IS_ENABLED(CONFIG_NEED_SG_DMA_LENGTH))
			KUNIT_EXPECT_EQ(test, sg->length, 0U);
	}
}

#ifdef CONFIG_DMABUF_DEBUG_WARN
/* Warn mode: the CPU side is all there, and all entries are marked */
static void check_warn(struct kunit *test, struct sg_table *sgt,
		       struct sg_table *orig)
{
	struct scatterlist *orig_sg = orig->sgl;
	struct scatterlist *sg;
	u64 len = 0;
	int i;

	KUNIT_EXPECT_EQ(test, sgt->orig_nents, orig->orig_nents);
	KUNIT_EXPECT_TRUE(test, sgt->sgl->dma_flags & SG_DMA_DMABUF_DEBUG);
	KUNIT_EXPECT_FALSE(test, orig->sgl->dma_flags & SG_DMA_DMABUF_DEBUG);

	/* sg_page() and sg_nents_for_len() report this, rate limited */
	for_each_sgtable_sg(sgt, sg, i) {
		KUNIT_EXPECT_TRUE(test, sg->dma_flags & SG_DMA_DMABUF_DEBUG);
		KUNIT_EXPECT_PTR_EQ(test, sg_page(sg), sg_page(orig_sg));
		KUNIT_EXPECT_EQ(test, sg->offset, orig_sg->offset);
		KUNIT_EXPECT_EQ(test, sg->length, orig_sg->length);
		len += sg->length;
		orig_sg = sg_next(orig_sg);
	}

	if (len)
		KUNIT_EXPECT_EQ(test, sg_nents_for_len(sgt->sgl, len),
				(int)orig->orig_nents);
	else
		KUNIT_EXPECT_NULL(test, sg_page(sgt->sgl));
}
#else
static void check_warn(struct kunit *test, struct sg_table *sgt,
		       struct sg_table *orig)
{
}
#endif

static void test_debug_sg_table(struct kunit *test)
{
	const struct mock_param *param = test->param_value;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf_attachment *attach;
	struct dma_buf *dmabuf;
	struct sg_table *sgt;
	struct mock_buf *buf;
	struct device *dev;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	buf = mock_alloc(test, param->orig_nents);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	exp_info.ops = &mock_ops;
	exp_info.size = buf->alloc_nents * PAGE_SIZE;
	exp_info.flags = O_RDWR;
	exp_info.priv = buf;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		mock_free(buf);
		KUNIT_FAIL(test, "dma_buf_export: %pe", dmabuf);
		return;
	}

	attach = dma_buf_attach(dmabuf, dev);
	if (IS_ERR(attach)) {
		KUNIT_FAIL(test, "dma_buf_attach: %pe", attach);
		goto out_put;
	}

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		KUNIT_FAIL(test, "dma_buf_map_attachment: %pe", sgt);
		goto out_detach;
	}

	/* The DMA side is the same in all modes */
	KUNIT_EXPECT_EQ(test, sgt->nents, 1U);
	KUNIT_EXPECT_EQ(test, sg_dma_address(sgt->sgl), (dma_addr_t)MOCK_DMA_ADDR);
	KUNIT_EXPECT_EQ(test, sg_dma_len(sgt->sgl), sg_dma_len(buf->sgt.sgl));
#ifdef CONFIG_NEED_SG_DMA_FLAGS
	KUNIT_EXPECT_TRUE(test, sg_dma_is_bus_address(sgt->sgl));
	KUNIT_EXPECT_TRUE(test, sg_dma_is_swiotlb(sgt->sgl));
#endif

	if (!IS_ENABLED(CONFIG_DMABUF_DEBUG))
		KUNIT_EXPECT_PTR_EQ(test, sgt, &buf->sgt);
	else if (IS_ENABLED(CONFIG_DMABUF_DEBUG_WARN))
		check_warn(test, sgt, &buf->sgt);
	else
		check_strict(test, sgt);

	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
	KUNIT_EXPECT_TRUE(test, buf->unmapped);
	KUNIT_EXPECT_EQ(test, buf->sgt.nents, 1U);
	KUNIT_EXPECT_EQ(test, buf->sgt.orig_nents, param->orig_nents);

out_detach:
	dma_buf_detach(dmabuf, attach);
out_put:
	dma_buf_put(dmabuf);
}

static const struct mock_param mock_params[] = {
	{ .desc = "one CPU entry", .orig_nents = 1 },
	{ .desc = "two CPU entries merged", .orig_nents = 2 },
	{ .desc = "no CPU side", .orig_nents = 0 },
};

KUNIT_ARRAY_PARAM_DESC(mock, mock_params, desc);

static struct kunit_case dma_buf_test_cases[] = {
	KUNIT_CASE_PARAM(test_debug_sg_table, mock_gen_params),
	{}
};

static struct kunit_suite dma_buf_test_suite = {
	.name = "dma-buf",
	.test_cases = dma_buf_test_cases,
};

kunit_test_suite(dma_buf_test_suite);

MODULE_IMPORT_NS("DMA_BUF");
