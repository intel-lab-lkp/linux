// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for dma_buf_match_mapping()
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/dma-buf.h>
#include <linux/dma-buf-mapping.h>
#include <linux/errno.h>

/* Mock tracking state -- reset before each test */
static bool mock_match_called;
static const struct dma_buf_mapping_match *mock_match_exp_arg;
static const struct dma_buf_mapping_match *mock_match_imp_arg;
static int mock_match_ret;

static bool mock_finish_called;
static const struct dma_buf_match_args *mock_finish_args_arg;
static const struct dma_buf_mapping_match *mock_finish_exp_arg;
static const struct dma_buf_mapping_match *mock_finish_imp_arg;

static int reset_mock_state(struct kunit *test)
{
	mock_match_called = false;
	mock_match_exp_arg = NULL;
	mock_match_imp_arg = NULL;
	mock_match_ret = 0;
	mock_finish_called = false;
	mock_finish_args_arg = NULL;
	mock_finish_exp_arg = NULL;
	mock_finish_imp_arg = NULL;
	return 0;
}

static int mock_match(struct dma_buf *dmabuf,
		      const struct dma_buf_mapping_match *exp,
		      const struct dma_buf_mapping_match *imp)
{
	mock_match_called = true;
	mock_match_exp_arg = exp;
	mock_match_imp_arg = imp;
	return mock_match_ret;
}

static void mock_finish_match(struct dma_buf_match_args *args,
			      const struct dma_buf_mapping_match *exp,
			      const struct dma_buf_mapping_match *imp)
{
	mock_finish_called = true;
	mock_finish_args_arg = args;
	mock_finish_exp_arg = exp;
	mock_finish_imp_arg = imp;

	/* Test doesn't always set attach */
	if (args->attach)
		args->attach->map_type = (struct dma_buf_mapping_match){
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
	struct dma_buf_match_args args = {
		.imp_matches = matches,
		.imp_len = ARRAY_SIZE(matches),
	};

	/* Zero-length exporter array returns -EINVAL */
	KUNIT_EXPECT_EQ(test, dma_buf_match_mapping(&args, NULL, 0), -EINVAL);
	KUNIT_EXPECT_FALSE(test, mock_match_called);
	KUNIT_EXPECT_FALSE(test, mock_finish_called);

	/* Zero-length importer array returns -EINVAL */
	args = (struct dma_buf_match_args){};
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, matches,
					      ARRAY_SIZE(matches)),
			-EINVAL);
	KUNIT_EXPECT_FALSE(test, mock_match_called);
	KUNIT_EXPECT_FALSE(test, mock_finish_called);

	/* Different types produce no match */
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, exp, ARRAY_SIZE(exp)),
			-EINVAL);
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
	struct dma_buf_match_args args = {
		.imp_matches = matches,
		.imp_len = ARRAY_SIZE(matches),
	};

	KUNIT_EXPECT_EQ(
		test,
		dma_buf_match_mapping(&args, matches, ARRAY_SIZE(matches)), 0);
	KUNIT_EXPECT_FALSE(test, mock_match_called);
	KUNIT_EXPECT_TRUE(test, mock_finish_called);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_args_arg, &args);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_exp_arg, &matches[0]);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_imp_arg, &matches[0]);
}

static void test_match_callback_returns(struct kunit *test)
{
	struct dma_buf_mapping_match matches[] = { { .type = &mock_type_a } };
	struct dma_buf_match_args args = {
		.imp_matches = matches,
		.imp_len = ARRAY_SIZE(matches),
	};

	/* type->match() returns -EOPNOTSUPP. Skips to next */
	mock_match_ret = -EOPNOTSUPP;
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, matches,
					      ARRAY_SIZE(matches)),
			-EINVAL);
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

/* Multiple importers. First exporter compatible type wins */
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
	struct dma_buf_match_args args = {
		.imp_matches = imp,
		.imp_len = ARRAY_SIZE(imp),
	};

	/* First matches */
	KUNIT_EXPECT_EQ(
		test, dma_buf_match_mapping(&args, exp1, ARRAY_SIZE(exp1)), 0);
	KUNIT_EXPECT_TRUE(test, mock_finish_called);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_exp_arg, &exp1[0]);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_imp_arg, &imp[0]);

	/* Second matches */
	KUNIT_EXPECT_EQ(
		test, dma_buf_match_mapping(&args, exp2, ARRAY_SIZE(exp2)), 0);
	KUNIT_EXPECT_TRUE(test, mock_finish_called);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_exp_arg, &exp2[0]);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_imp_arg, &imp[1]);
}

/* Multiple exporters. First exporter compatible type wins */
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
	struct dma_buf_match_args args = {
		.imp_matches = imp1,
		.imp_len = ARRAY_SIZE(imp1),
	};

	/* Single importer */
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, exp, ARRAY_SIZE(exp)), 0);
	KUNIT_EXPECT_TRUE(test, mock_finish_called);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_exp_arg, &exp[1]);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_imp_arg, &imp1[0]);

	/* Two importers, skipping the first */
	args = (struct dma_buf_match_args){
		.imp_matches = imp2,
		.imp_len = ARRAY_SIZE(imp2),
	};
	KUNIT_EXPECT_EQ(test,
			dma_buf_match_mapping(&args, exp, ARRAY_SIZE(exp)), 0);
	KUNIT_EXPECT_TRUE(test, mock_finish_called);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_exp_arg, &exp[0]);
	KUNIT_EXPECT_PTR_EQ(test, mock_finish_imp_arg, &imp2[1]);
}

static void mock_dmabuf_release(struct dma_buf *dmabuf)
{
}

static struct sg_table *mock_map_dma_buf(struct dma_buf_attachment *attach,
					 enum dma_data_direction dir)
{
	return ERR_PTR(-ENODEV);
}

static void mock_unmap_dma_buf(struct dma_buf_attachment *attach,
			       struct sg_table *sgt,
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
 * dma_buf_sgt_attach()
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

	attach = dma_buf_sgt_attach(dmabuf, dev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	KUNIT_EXPECT_PTR_EQ(test, attach->map_type.type,
			    &dma_buf_mapping_sgt_type);
	KUNIT_EXPECT_PTR_EQ(test, dma_buf_sgt_dma_device(attach), dev);
	KUNIT_EXPECT_FALSE(test, dma_buf_sgt_p2p_allowed(attach));

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);
}

static void mock_move_notify(struct dma_buf_attachment *attach)
{
}

static const struct dma_buf_attach_ops mock_importer_ops = {
	.move_notify = &mock_move_notify,
};

/* Check a dynamic attach with a non-sgt mapping type */
static void test_mock_attach(struct kunit *test)
{
	struct dma_buf_mapping_match imp[] = { { .type = &mock_type_a } };
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct device *dev;

	dev = kunit_device_register(test, "dma-buf-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	dmabuf = mock_dmabuf_export(&mock_dmabuf_two_exp_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmabuf);

	attach = dma_buf_mapping_attach(dmabuf, imp, ARRAY_SIZE(imp),
					&mock_importer_ops, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, attach);

	KUNIT_EXPECT_PTR_EQ(test, attach->map_type.type, &mock_type_a);

	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);
}

static const struct dma_exporter dma_exporter_params[] = {
	{ &mock_dmabuf_simple_sgt_ops, "simple_sgt" },
	{ &mock_dmabuf_two_exp_ops, "two_exp" },
};
KUNIT_ARRAY_PARAM_DESC(dma_exporter, dma_exporter_params, desc);

static struct kunit_case dma_mapping_cases[] = {
	KUNIT_CASE(test_match_fail),
	KUNIT_CASE(test_match_no_match_callback),
	KUNIT_CASE(test_match_callback_returns),
	KUNIT_CASE(test_match_exporter_priority),
	KUNIT_CASE(test_match_importer_priority),
	KUNIT_CASE_PARAM(test_sgt_attach, dma_exporter_gen_params),
	KUNIT_CASE(test_mock_attach),
	{}
};

static struct kunit_suite dma_mapping_test_suite = {
	.name = "dma-buf-mapping",
	.init = reset_mock_state,
	.test_cases = dma_mapping_cases,
};

kunit_test_suite(dma_mapping_test_suite);

MODULE_IMPORT_NS("DMA_BUF");
