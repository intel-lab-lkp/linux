// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for dma_buf_match_mapping()
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/dma-buf.h>
#include <linux/dma-buf-mapping.h>
#include <linux/dma-resv.h>
#include <linux/errno.h>

/* Mock tracking state -- reset before each test */
static bool mock_match_called;
static const struct dma_buf_mapping_match *mock_match_exp_arg;
static const struct dma_buf_mapping_match *mock_match_imp_arg;
static struct device *mock_match_dev_arg;
static int mock_match_ret;

static bool mock_finish_called;
static const struct dma_buf_match_args *mock_finish_args_arg;
static const struct dma_buf_mapping_match *mock_finish_exp_arg;
static const struct dma_buf_mapping_match *mock_finish_imp_arg;

/* Mapping types the configurable mock exporter currently offers */
static const struct dma_buf_mapping_match *mock_exp_offer;
static size_t mock_exp_offer_len;

/* Which match set entry the selecting mock exporter picks, or an error */
static int mock_select_idx;
static int mock_select_err;
static int mock_select_count;

/* Make the mock exporter's pin and map invalidate everyone, as a move does */
static bool mock_invalidate_in_callback;

/* What the mock types' unmap saw: the mapping's header type and its map's */
static const struct dma_buf_mapping_type *mock_unmap_type;
static const struct dma_buf_mapping_type *mock_unmap_made_by;

/* Failure injection for the mock types' map and the mock exporter's pin */
static int mock_map_err;
static int mock_pin_err;
static int mock_pin_count;

static int reset_mock_state(struct kunit *test)
{
	mock_match_called = false;
	mock_match_exp_arg = NULL;
	mock_match_imp_arg = NULL;
	mock_match_dev_arg = NULL;
	mock_match_ret = 0;
	mock_finish_called = false;
	mock_finish_args_arg = NULL;
	mock_finish_exp_arg = NULL;
	mock_finish_imp_arg = NULL;
	mock_exp_offer = NULL;
	mock_exp_offer_len = 0;
	mock_select_idx = 0;
	mock_select_err = 0;
	mock_select_count = 0;
	mock_invalidate_in_callback = false;
	mock_unmap_type = NULL;
	mock_unmap_made_by = NULL;
	mock_map_err = 0;
	mock_pin_err = 0;
	mock_pin_count = 0;
	return 0;
}

static int mock_match(struct dma_buf_match_args *args,
		      const struct dma_buf_mapping_match *exp,
		      const struct dma_buf_mapping_match *imp)
{
	mock_match_called = true;
	mock_match_exp_arg = exp;
	mock_match_imp_arg = imp;
	mock_match_dev_arg = args->dev;
	return mock_match_ret;
}

static void mock_finish_match(struct dma_buf_match_args *args,
			      const struct dma_buf_mapping_match *exp,
			      const struct dma_buf_mapping_match *imp,
			      struct dma_buf_mapping_match *match)
{
	mock_finish_called = true;
	mock_finish_args_arg = args;
	mock_finish_exp_arg = exp;
	mock_finish_imp_arg = imp;

	*match = (struct dma_buf_mapping_match){
		.type = exp->type,
		.exp_ops = exp->exp_ops,
	};
}

/* Type with both match and finish_match callbacks */
static struct dma_buf_mapping_type mock_type_a = {
	.name = "mock_type_a",
	.match = mock_match,
	.finish_match = mock_finish_match,
};

/* Second type -- distinct pointer identity from A */
static struct dma_buf_mapping_type mock_type_b = {
	.name = "mock_type_b",
	.match = mock_match,
	.finish_match = mock_finish_match,
};

static void test_match_fail(struct kunit *test)
{
	struct dma_buf_mapping_match matches[] = { { .type = &mock_type_a } };
	struct dma_buf_mapping_match exp[] = { { .type = &mock_type_b } };
	struct dma_buf_attachment attach = {};
	struct dma_buf_match_args args = {
		.attach = &attach,
		.imp_matches = matches,
		.imp_len = ARRAY_SIZE(matches),
	};

	/* Zero-length exporter array returns -EINVAL */
	KUNIT_EXPECT_EQ(test, dma_buf_match_mapping(&args, NULL, 0), -EINVAL);
	KUNIT_EXPECT_FALSE(test, mock_match_called);
	KUNIT_EXPECT_FALSE(test, mock_finish_called);

	/* Zero-length importer array returns -EINVAL */
	args = (struct dma_buf_match_args){ .attach = &attach };
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, matches,
					      ARRAY_SIZE(matches)),
			-EINVAL);
	KUNIT_EXPECT_FALSE(test, mock_match_called);
	KUNIT_EXPECT_FALSE(test, mock_finish_called);

	/* Different types produce no match */
	args.imp_matches = matches;
	args.imp_len = ARRAY_SIZE(matches);
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, exp, ARRAY_SIZE(exp)),
			-EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, mock_match_called);
	KUNIT_EXPECT_FALSE(test, mock_finish_called);
}

/* When type->match() is NULL same types always match */
static void test_match_no_match_callback(struct kunit *test)
{
	static struct dma_buf_mapping_type mock_type_no_match = {
		.name = "mock_type_no_match",
		.finish_match = mock_finish_match,
	};
	struct dma_buf_mapping_match matches[] = {
		{ .type = &mock_type_no_match }
	};
	struct dma_buf_attachment attach = {};
	struct dma_buf_match_args args = {
		.attach = &attach,
		.imp_matches = matches,
		.imp_len = ARRAY_SIZE(matches),
	};

	KUNIT_EXPECT_EQ(test, dma_buf_match_mapping(&args, matches,
						    ARRAY_SIZE(matches)), 0);
	KUNIT_EXPECT_FALSE(test, mock_match_called);
	KUNIT_EXPECT_TRUE(test, mock_finish_called);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_args_arg, &args);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_exp_arg, &matches[0]);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_imp_arg, &matches[0]);
}

static void test_match_callback_returns(struct kunit *test)
{
	struct dma_buf_mapping_match matches[] = { { .type = &mock_type_a } };
	struct dma_buf_attachment attach = {};
	struct dma_buf_match_args args = {
		.attach = &attach,
		.imp_matches = matches,
		.imp_len = ARRAY_SIZE(matches),
	};

	/* type->match() returns -EOPNOTSUPP. Skips to next */
	mock_match_ret = -EOPNOTSUPP;
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, matches,
					      ARRAY_SIZE(matches)),
			-EOPNOTSUPP);
	KUNIT_EXPECT_TRUE(test, mock_match_called);
	KUNIT_EXPECT_FALSE(test, mock_finish_called);

	/* type->match() returns an error code. Stops immediately, returns code */
	mock_match_ret = -ENOMEM;
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, matches,
					      ARRAY_SIZE(matches)),
			-ENOMEM);
	KUNIT_EXPECT_TRUE(test, mock_match_called);
	KUNIT_EXPECT_FALSE(test, mock_finish_called);
}

/*
 * The match set records every compatible pair, in the exporter's order. The
 * first entry is the default selection.
 */
static void test_match_exporter_priority(struct kunit *test)
{
	struct dma_buf_mapping_match exp1[2] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match exp2[] = { { .type = &mock_type_b } };
	struct dma_buf_mapping_match imp[2] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_attachment attach = {};
	struct dma_buf_match_args args = {
		.attach = &attach,
		.imp_matches = imp,
		.imp_len = ARRAY_SIZE(imp),
	};

	/* Both match, exporter order preserved, first is the default */
	KUNIT_EXPECT_EQ(test, dma_buf_match_mapping(&args, exp1,
						    ARRAY_SIZE(exp1)), 0);
	KUNIT_ASSERT_EQ(test, attach.nr_matches, 2);
	KUNIT_EXPECT_PTR_EQ(test, attach.matches[0].type, &mock_type_a);
	KUNIT_EXPECT_PTR_EQ(test, attach.matches[1].type, &mock_type_b);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(&attach)->type,
			    &mock_type_a);

	/* Only the second importer type is offered */
	KUNIT_EXPECT_EQ(test, dma_buf_match_mapping(&args, exp2,
						    ARRAY_SIZE(exp2)), 0);
	KUNIT_ASSERT_EQ(test, attach.nr_matches, 1);
	KUNIT_EXPECT_PTR_EQ(test, attach.matches[0].type, &mock_type_b);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(&attach)->type,
			    &mock_type_b);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_imp_arg, &imp[1]);
}

/* The exporter's order wins over the importer's */
static void test_match_importer_priority(struct kunit *test)
{
	struct dma_buf_mapping_match exp[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp1[] = { { .type = &mock_type_b } };
	struct dma_buf_mapping_match imp2[] = {
		{ .type = &mock_type_b },
		{ .type = &mock_type_a },
	};
	struct dma_buf_attachment attach = {};
	struct dma_buf_match_args args = {
		.attach = &attach,
		.imp_matches = imp1,
		.imp_len = ARRAY_SIZE(imp1),
	};

	/* Single importer type */
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, exp, ARRAY_SIZE(exp)), 0);
	KUNIT_ASSERT_EQ(test, attach.nr_matches, 1);
	KUNIT_EXPECT_PTR_EQ(test, attach.matches[0].type, &mock_type_b);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_imp_arg, &imp1[0]);

	/* Importer prefers B, exporter prefers A: A is the default */
	args.imp_matches = imp2;
	args.imp_len = ARRAY_SIZE(imp2);
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, exp, ARRAY_SIZE(exp)), 0);
	KUNIT_ASSERT_EQ(test, attach.nr_matches, 2);
	KUNIT_EXPECT_PTR_EQ(test, attach.matches[0].type, &mock_type_a);
	KUNIT_EXPECT_PTR_EQ(test, attach.matches[1].type, &mock_type_b);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(&attach)->type,
			    &mock_type_a);
}

static void mock_dmabuf_release(struct dma_buf *dmabuf)
{
}

static struct dma_buf_sgt_mapping *
mock_map_dma_buf(struct dma_buf_attachment *attach, enum dma_data_direction dir)
{
	return ERR_PTR(-ENODEV);
}

static void mock_unmap_dma_buf(struct dma_buf_attachment *attach,
			       struct dma_buf_sgt_mapping *mapping,
			       enum dma_data_direction dir)
{
}

static const struct dma_buf_mapping_sgt_exp_ops mock_sgt_ops = {
	.map_dma_buf = mock_map_dma_buf,
	.unmap_dma_buf = mock_unmap_dma_buf,
};

static const struct dma_buf_ops mock_dmabuf_simple_sgt_ops = {
	.release = mock_dmabuf_release,
	DMA_BUF_SIMPLE_SGT_EXP_MATCH(mock_map_dma_buf, mock_unmap_dma_buf),
};

static int mock_dmabuf_match_mapping(struct dma_buf_match_args *args)
{
	struct dma_buf_mapping_match sgt_match[2];
	unsigned int num_match = 0;

	sgt_match[num_match++] =
		(struct dma_buf_mapping_match){ .type = &mock_type_a };

	sgt_match[num_match++] = DMA_BUF_EMAPPING_SGT(&mock_sgt_ops);

	return dma_buf_match_mapping(args, sgt_match, ARRAY_SIZE(sgt_match));
}

static const struct dma_buf_ops mock_dmabuf_two_exp_ops = {
	.release = mock_dmabuf_release,
	.match_mapping = mock_dmabuf_match_mapping,
};

struct dma_exporter {
	const struct dma_buf_ops *ops;
	const char *desc;
};

static struct dma_buf *mock_dmabuf_export(const struct dma_buf_ops *ops)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	exp_info.ops = ops;
	exp_info.size = PAGE_SIZE;
	exp_info.priv = ERR_PTR(-EINVAL);
	return dma_buf_export(&exp_info);
}

/*
 * Check that a simple SGT exporter with single_exporter_match works with
 * dma_buf_attach()
 */
static void test_sgt_attach(struct kunit *test)
{
	const struct dma_exporter *param = test->param_value;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct device *dev;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(param->ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	attach = dma_buf_attach(dmabuf, dev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &dma_buf_mapping_sgt_type);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_sgt_dma_device(attach), dev);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);
}

static void mock_move_notify(struct dma_buf_attachment *attach)
{
}

/*
 * Mock importer: declares a dma_buf_attach_ops whose supported_mapping_types
 * table is whatever the test put in @_matches.
 */
#define MOCK_IMPORTER_OPS(_name, _matches)                          \
	const struct dma_buf_attach_ops _name = {                   \
		.invalidate_mappings = &mock_move_notify,           \
		.supported_mapping_types = (_matches),              \
		.nr_supported_mapping_types = ARRAY_SIZE(_matches), \
	}

/* Check a dynamic attach with a non-sgt mapping type */
static void test_mock_attach(struct kunit *test)
{
	struct dma_buf_mapping_match imp[] = { { .type = &mock_type_a } };

	MOCK_IMPORTER_OPS(importer_ops, imp);
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct device *dev;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_two_exp_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	attach = dma_buf_dynamic_attach(dmabuf, dev, &importer_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_a);
	KUNIT_EXPECT_PTR_EQ(test, mock_match_dev_arg, dev);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);
}

static int mock_dmabuf_offer_match_mapping(struct dma_buf_match_args *args)
{
	return dma_buf_match_mapping(args, mock_exp_offer, mock_exp_offer_len);
}

static const struct dma_buf_ops mock_dmabuf_offer_ops = {
	.release = mock_dmabuf_release,
	.match_mapping = mock_dmabuf_offer_match_mapping,
};

/*
 * The match set is final once attached. A later importer attaching must not
 * change an existing attachment even if the exporter returns a different set.
 *
 * Later attachments influence what the exporter selects at pin or map time,
 * not what was previously negotiated.
 */
static void test_match_set_is_final(struct kunit *test)
{
	static const struct dma_buf_mapping_match offer_ab[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	static const struct dma_buf_mapping_match offer_b[] = {
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp1[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp2[] = { { .type = &mock_type_b } };

	MOCK_IMPORTER_OPS(importer1_ops, imp1);
	MOCK_IMPORTER_OPS(importer2_ops, imp2);
	struct dma_buf_attachment *attach1, *attach2;
	struct dma_buf *dmabuf;
	struct device *dev;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_offer_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	mock_exp_offer = offer_ab;
	mock_exp_offer_len = ARRAY_SIZE(offer_ab);
	attach1 = dma_buf_dynamic_attach(dmabuf, dev, &importer1_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach1);
	KUNIT_ASSERT_EQ(test, attach1->nr_matches, 2);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach1)->type,
			    &mock_type_a);

	/* Exporter's answer for new importers changes; attach1 does not */
	mock_exp_offer = offer_b;
	mock_exp_offer_len = ARRAY_SIZE(offer_b);
	attach2 = dma_buf_dynamic_attach(dmabuf, dev, &importer2_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach2);
	KUNIT_ASSERT_EQ(test, attach2->nr_matches, 1);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach2)->type,
			    &mock_type_b);
	KUNIT_EXPECT_EQ(test, attach1->nr_matches, 2);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach1)->type,
			    &mock_type_a);

	dma_buf_detach(dmabuf, attach2);
	KUNIT_EXPECT_EQ(test, attach1->nr_matches, 2);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach1)->type,
			    &mock_type_a);

	dma_buf_detach(dmabuf, attach1);
	dma_buf_put(dmabuf);
}

/* Mock mapping result for the generic map path */
struct mock_mapping {
	struct dma_buf_mapping base;
	const struct dma_buf_mapping_type *made_by;
};

static struct dma_buf_mapping *mock_type_map(struct dma_buf_attachment *attach,
					     enum dma_data_direction dir)
{
	struct mock_mapping *m;

	if (mock_invalidate_in_callback)
		dma_buf_invalidate_mappings(attach->dmabuf);
	if (mock_map_err)
		return ERR_PTR(mock_map_err);
	m = kzalloc_obj(*m);
	if (!m)
		return ERR_PTR(-ENOMEM);
	m->made_by = dma_buf_attach_map_type(attach)->type;
	return &m->base;
}

static void mock_type_unmap(struct dma_buf_attachment *attach,
			    struct dma_buf_mapping *mapping,
			    enum dma_data_direction dir)
{
	struct mock_mapping *m = container_of(mapping, struct mock_mapping, base);

	mock_unmap_type = dma_buf_mapping_type_of(mapping);
	mock_unmap_made_by = m->made_by;
	kfree(m);
}

/* Exporter choosing the match set entry the test asks for */
static int mock_dmabuf_select_mapping(struct dma_buf_attachment *attach)
{
	mock_select_count++;
	if (mock_select_err)
		return mock_select_err;
	return mock_select_idx;
}

static int mock_dmabuf_pin(struct dma_buf_attachment *attach)
{
	if (mock_invalidate_in_callback)
		dma_buf_invalidate_mappings(attach->dmabuf);
	if (mock_pin_err)
		return mock_pin_err;
	mock_pin_count++;
	return 0;
}

static void mock_dmabuf_unpin(struct dma_buf_attachment *attach)
{
	mock_pin_count--;
}

static const struct dma_buf_ops mock_dmabuf_select_ops = {
	.release = mock_dmabuf_release,
	.match_mapping = mock_dmabuf_offer_match_mapping,
	.select_mapping = mock_dmabuf_select_mapping,
	.pin = mock_dmabuf_pin,
	.unpin = mock_dmabuf_unpin,
};

/*
 * Exporter decides type at first map from the match set. The result is tagged
 * with it and the core holds the choice across unmap/remap until the attachment
 * has been explicitly invalidated.
 */
static void test_select_at_map(struct kunit *test)
{
	static const struct dma_buf_mapping_match offer_ab[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	MOCK_IMPORTER_OPS(importer_ops, imp);
	struct dma_buf_attachment *attach;
	struct dma_buf_mapping *mapping;
	struct dma_buf *dmabuf;
	struct device *dev;

	mock_type_a.map = mock_type_map;
	mock_type_a.unmap = mock_type_unmap;
	mock_type_b.map = mock_type_map;
	mock_type_b.unmap = mock_type_unmap;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_select_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	mock_exp_offer = offer_ab;
	mock_exp_offer_len = ARRAY_SIZE(offer_ab);
	attach = dma_buf_dynamic_attach(dmabuf, dev, &importer_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);
	KUNIT_EXPECT_FALSE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_a);

	dma_resv_lock(dmabuf->resv, NULL);

	/* Exporter picks the second entry (mock_type_b) */
	mock_select_idx = 1;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_b);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_b);
	KUNIT_EXPECT_TRUE(test, attach->selected);

	/* A pin taken whilst mapping is live gets the same type */
	mock_select_idx = 0;
	KUNIT_EXPECT_EQ(test, dma_buf_pin(attach), 0);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_b);
	dma_buf_unpin(attach);

	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);

	/*
	 * Unmapped the exporter would now pick A, but no invalidate has
	 * happened so remap must get B again to avoid ping-ponging.
	 */
	KUNIT_EXPECT_TRUE(test, attach->selected);
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_b);
	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);

	/* Only an explicit invalidation allows the type to change */
	dma_buf_invalidate_mappings(dmabuf);
	KUNIT_EXPECT_FALSE(test, attach->selected);
	/* Nothing is reselected until the next pin or map */
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_b);

	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_a);
	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);

	/* Exporter cannot serve this importer right now */
	dma_buf_invalidate_mappings(dmabuf);
	mock_select_err = -EBUSY;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mapping), -EBUSY);
	KUNIT_EXPECT_FALSE(test, attach->selected);
	mock_select_err = 0;

	dma_resv_unlock(dmabuf->resv);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);

	mock_type_a.map = NULL;
	mock_type_a.unmap = NULL;
	mock_type_b.map = NULL;
	mock_type_b.unmap = NULL;
}

/*
 * Pin selects too and the choice survives map, unmap and unpin. A pinned
 * importer that is never invalidated keeps its first type for the life of the
 * attachment.
 */
static void test_select_at_pin(struct kunit *test)
{
	static const struct dma_buf_mapping_match offer_ab[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	MOCK_IMPORTER_OPS(importer_ops, imp);
	struct dma_buf_attachment *attach;
	struct dma_buf_mapping *mapping;
	struct dma_buf *dmabuf;
	struct device *dev;

	mock_type_a.map = mock_type_map;
	mock_type_a.unmap = mock_type_unmap;
	mock_type_b.map = mock_type_map;
	mock_type_b.unmap = mock_type_unmap;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_select_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	mock_exp_offer = offer_ab;
	mock_exp_offer_len = ARRAY_SIZE(offer_ab);
	attach = dma_buf_dynamic_attach(dmabuf, dev, &importer_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	dma_resv_lock(dmabuf->resv, NULL);

	mock_select_idx = 1;
	KUNIT_EXPECT_EQ(test, dma_buf_pin(attach), 0);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_b);

	/* Map after pin uses the pinned type regardless of the exporter now */
	mock_select_idx = 0;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_b);
	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);

	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_b);

	dma_buf_unpin(attach);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_b);

	dma_resv_unlock(dmabuf->resv);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);

	mock_type_a.map = NULL;
	mock_type_a.unmap = NULL;
	mock_type_b.map = NULL;
	mock_type_b.unmap = NULL;
}

/*
 * If the first map/pin fails it shouldn't pin the selection. However all
 * subsequent map/pin failure must not reset the selection state, otherwise a
 * retry could reselect without an invalidate.
 */
static void test_select_survives_failed_map(struct kunit *test)
{
	static const struct dma_buf_mapping_match offer_ab[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	MOCK_IMPORTER_OPS(importer_ops, imp);
	struct dma_buf_attachment *attach;
	struct dma_buf_mapping *mapping;
	struct dma_buf *dmabuf;
	struct device *dev;

	mock_type_a.map = mock_type_map;
	mock_type_a.unmap = mock_type_unmap;
	mock_type_b.map = mock_type_map;
	mock_type_b.unmap = mock_type_unmap;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_select_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	mock_exp_offer = offer_ab;
	mock_exp_offer_len = ARRAY_SIZE(offer_ab);
	attach = dma_buf_dynamic_attach(dmabuf, dev, &importer_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	dma_resv_lock(dmabuf->resv, NULL);

	/* A map that selected and then failed does not change the selected state */
	mock_select_idx = 1;
	mock_map_err = -EIO;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mapping), -EIO);
	KUNIT_EXPECT_FALSE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_a);
	mock_map_err = 0;

	/* Pin selects B and holds it */
	KUNIT_EXPECT_EQ(test, dma_buf_pin(attach), 0);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_b);
	KUNIT_EXPECT_EQ(test, mock_pin_count, 1);

	/* A map failing under that pin must not allow a different selection */
	mock_select_idx = 0;
	mock_map_err = -EIO;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mapping), -EIO);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_b);
	KUNIT_EXPECT_EQ(test, mock_pin_count, 1);
	mock_map_err = 0;

	/* The retry gets the pinned type, not a new selection */
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_b);
	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);

	dma_buf_unpin(attach);
	KUNIT_EXPECT_EQ(test, mock_pin_count, 0);

	dma_resv_unlock(dmabuf->resv);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);

	mock_type_a.map = NULL;
	mock_type_a.unmap = NULL;
	mock_type_b.map = NULL;
	mock_type_b.unmap = NULL;
}

/*
 * The same rule for a failing exporter pin. A pin that selected and then
 * failed resets the selection state, and one that failed after an existing map
 * does not.
 */
static void test_select_after_failed_pin(struct kunit *test)
{
	static const struct dma_buf_mapping_match offer_ab[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	MOCK_IMPORTER_OPS(importer_ops, imp);
	struct dma_buf_attachment *attach;
	struct dma_buf_mapping *mapping;
	struct dma_buf *dmabuf;
	struct device *dev;

	mock_type_a.map = mock_type_map;
	mock_type_a.unmap = mock_type_unmap;
	mock_type_b.map = mock_type_map;
	mock_type_b.unmap = mock_type_unmap;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_select_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	mock_exp_offer = offer_ab;
	mock_exp_offer_len = ARRAY_SIZE(offer_ab);
	attach = dma_buf_dynamic_attach(dmabuf, dev, &importer_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	dma_resv_lock(dmabuf->resv, NULL);

	/* First pin selects B but the exporter cannot pin: nothing is kept */
	mock_select_idx = 1;
	mock_pin_err = -ENOSPC;
	KUNIT_EXPECT_EQ(test, dma_buf_pin(attach), -ENOSPC);
	KUNIT_EXPECT_FALSE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_a);
	KUNIT_EXPECT_EQ(test, mock_pin_count, 0);
	mock_pin_err = 0;

	/* A live mapping selects A */
	mock_select_idx = 0;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_a);

	/* A pin failing under it leaves that selection in place */
	mock_select_idx = 1;
	mock_pin_err = -ENOSPC;
	KUNIT_EXPECT_EQ(test, dma_buf_pin(attach), -ENOSPC);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_a);
	mock_pin_err = 0;

	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);
	dma_resv_unlock(dmabuf->resv);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);

	mock_type_a.map = NULL;
	mock_type_a.unmap = NULL;
	mock_type_b.map = NULL;
	mock_type_b.unmap = NULL;
}

static const struct dma_exporter dma_exporter_params[] = {
	{ &mock_dmabuf_simple_sgt_ops, "simple_sgt" },
	{ &mock_dmabuf_two_exp_ops, "two_exp" },
};

KUNIT_ARRAY_PARAM_DESC(dma_exporter, dma_exporter_params, desc);

/*
 * After an invalidate the exporter may select another type for the next map
 * while a mapping made with the previous selection is still live. Since
 * importers don't have to unmap on invalidate the attachment selection may
 * have changed by the time unmap is called. Therefore every mapping must be
 * torn down with the type it was made with, and the new selection governs new
 * maps only not unmap.
 */
static void test_unmap_uses_own_type(struct kunit *test)
{
	static const struct dma_buf_mapping_match offer_ab[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	MOCK_IMPORTER_OPS(importer_ops, imp);
	struct dma_buf_mapping *mapping, *mapping2;
	struct dma_buf_attachment *attach;
	struct dma_buf *dmabuf;
	struct device *dev;

	mock_type_a.map = mock_type_map;
	mock_type_a.unmap = mock_type_unmap;
	mock_type_b.map = mock_type_map;
	mock_type_b.unmap = mock_type_unmap;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_select_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	mock_exp_offer = offer_ab;
	mock_exp_offer_len = ARRAY_SIZE(offer_ab);
	attach = dma_buf_dynamic_attach(dmabuf, dev, &importer_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	dma_resv_lock(dmabuf->resv, NULL);

	/* Mapped with the exporter's second entry */
	mock_select_idx = 1;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_b);
	KUNIT_EXPECT_PTR_EQ(test, mapping->match, &attach->matches[1]);
	KUNIT_EXPECT_EQ(test, mock_select_count, 1);

	/* Invalidated while that mapping is live: the next map reselects */
	dma_buf_invalidate_mappings(dmabuf);
	KUNIT_EXPECT_FALSE(test, attach->selected);
	mock_select_idx = 0;
	mapping2 = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping2);
	KUNIT_EXPECT_PTR_EQ(test, mapping2->type, &mock_type_a);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_EQ(test, mock_select_count, 2);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_a);

	/* The old mapping is still torn down as B, the new one as A */
	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);
	KUNIT_EXPECT_PTR_EQ(test, mock_unmap_type, &mock_type_b);
	KUNIT_EXPECT_PTR_EQ(test, mock_unmap_made_by, &mock_type_b);
	/* The selection itself is untouched by the unmap */
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_a);
	KUNIT_EXPECT_TRUE(test, attach->selected);

	dma_buf_mapping_unmap(attach, mapping2, DMA_BIDIRECTIONAL);
	KUNIT_EXPECT_PTR_EQ(test, mock_unmap_type, &mock_type_a);
	KUNIT_EXPECT_PTR_EQ(test, mock_unmap_made_by, &mock_type_a);

	/* Nothing live, but the selection stands until the next invalidate */
	mock_select_idx = 1;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_a);
	KUNIT_EXPECT_EQ(test, mock_select_count, 2);
	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);

	dma_resv_unlock(dmabuf->resv);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);

	mock_type_a.map = NULL;
	mock_type_a.unmap = NULL;
	mock_type_b.map = NULL;
	mock_type_b.unmap = NULL;
}

/*
 * An exporter that moves the buffer during map/pin after invalidating must not
 * leave the attachment in an unselected state after the map/pin succeeds. Once
 * a mapping succeeds it must lock in the selection made after the invalidate and
 * move and subsequent map/pin operations must get the same type.
 */
static void test_select_survives_own_invalidate(struct kunit *test)
{
	static const struct dma_buf_mapping_match offer_ab[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	struct dma_buf_mapping_match imp[] = {
		{ .type = &mock_type_a },
		{ .type = &mock_type_b },
	};
	MOCK_IMPORTER_OPS(importer_ops, imp);
	struct dma_buf_mapping *mapping, *mapping2;
	struct dma_buf_attachment *attach;
	struct dma_buf *dmabuf;
	struct device *dev;

	mock_type_a.map = mock_type_map;
	mock_type_a.unmap = mock_type_unmap;
	mock_type_b.map = mock_type_map;
	mock_type_b.unmap = mock_type_unmap;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_select_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	mock_exp_offer = offer_ab;
	mock_exp_offer_len = ARRAY_SIZE(offer_ab);
	attach = dma_buf_dynamic_attach(dmabuf, dev, &importer_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	mock_invalidate_in_callback = true;
	dma_resv_lock(dmabuf->resv, NULL);

	/* The map's move invalidates the attachment selection. */
	mock_select_idx = 1;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_mapping_type_of(mapping),
			    &mock_type_b);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_EQ(test, mock_select_count, 1);

	/* A second map does not reselect. */
	mock_select_idx = 0;
	mapping2 = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mapping2);
	KUNIT_EXPECT_PTR_EQ(test, mapping2->type, &mock_type_b);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_EQ(test, mock_select_count, 1);

	/* Nor does a pin taken under it */
	KUNIT_EXPECT_EQ(test, dma_buf_pin(attach), 0);
	KUNIT_EXPECT_TRUE(test, attach->selected);
	KUNIT_EXPECT_EQ(test, mock_select_count, 1);
	dma_buf_unpin(attach);

	dma_buf_mapping_unmap(attach, mapping2, DMA_BIDIRECTIONAL);
	dma_buf_mapping_unmap(attach, mapping, DMA_BIDIRECTIONAL);

	/* A map that invalidates and then fails establishes nothing */
	dma_buf_invalidate_mappings(dmabuf);
	mock_map_err = -EIO;
	mapping = dma_buf_mapping_map(attach, DMA_BIDIRECTIONAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mapping), -EIO);
	KUNIT_EXPECT_FALSE(test, attach->selected);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_attach_map_type(attach)->type,
			    &mock_type_a);
	mock_map_err = 0;

	dma_resv_unlock(dmabuf->resv);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);

	mock_type_a.map = NULL;
	mock_type_a.unmap = NULL;
	mock_type_b.map = NULL;
	mock_type_b.unmap = NULL;
}

static struct kunit_case dma_mapping_cases[] = {
	KUNIT_CASE(test_match_fail),
	KUNIT_CASE(test_match_no_match_callback),
	KUNIT_CASE(test_match_callback_returns),
	KUNIT_CASE(test_match_exporter_priority),
	KUNIT_CASE(test_match_importer_priority),
	KUNIT_CASE_PARAM(test_sgt_attach, dma_exporter_gen_params),
	KUNIT_CASE(test_mock_attach),
	KUNIT_CASE(test_match_set_is_final),
	KUNIT_CASE(test_select_at_map),
	KUNIT_CASE(test_select_at_pin),
	KUNIT_CASE(test_select_survives_failed_map),
	KUNIT_CASE(test_select_after_failed_pin),
	KUNIT_CASE(test_unmap_uses_own_type),
	KUNIT_CASE(test_select_survives_own_invalidate),
	{}
};

static struct kunit_suite dma_mapping_test_suite = {
	.name = "dma-buf-mapping",
	.init = reset_mock_state,
	.test_cases = dma_mapping_cases,
};

kunit_test_suite(dma_mapping_test_suite);

MODULE_IMPORT_NS("DMA_BUF");
