// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

/*
 * KUnit coverage for the QAT live migration remote-import parser. The cases
 * drive the real adf_mstate_mgr_init_from_remote() on a buffer sized as the
 * GEN4 VFIO migration backend allocates it (4096 bytes), including the
 * preh_len == buffer-size boundary case. Included from adf_mstate_mgr.c to
 * reach the file-local preamble and section types.
 */

#include <kunit/test.h>

#define ADF_MSTATE_TEST_BUF_SIZE 4096

static void qat_mstate_remote_run(struct kunit *test, u16 preh_len,
				  u16 n_sects, u32 sect0_size, int expect)
{
	struct adf_mstate_mgr mgr;
	struct adf_mstate_preh *pre;
	u8 *buf;
	int ret;

	buf = kzalloc(ADF_MSTATE_TEST_BUF_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	pre = (struct adf_mstate_preh *)buf;
	pre->magic = ADF_MSTATE_MAGIC;
	pre->version = ADF_MSTATE_VERSION;
	pre->preh_len = preh_len;
	pre->n_sects = n_sects;
	pre->size = 0;

	/* Place an in-bounds section header when there is room for one. */
	if (n_sects &&
	    (u32)preh_len + sizeof(struct adf_mstate_sect_h) <= ADF_MSTATE_TEST_BUF_SIZE) {
		struct adf_mstate_sect_h *s =
			(struct adf_mstate_sect_h *)(buf + preh_len);

		s->size = sect0_size;
		s->sub_sects = 0;
	}

	ret = adf_mstate_mgr_init_from_remote(&mgr, buf, ADF_MSTATE_TEST_BUF_SIZE,
					      NULL, NULL);
	KUNIT_EXPECT_EQ(test, ret, expect);

	kfree(buf);
}

/* Valid empty preamble: the validation loop never runs. */
static void qat_mstate_remote_empty(struct kunit *test)
{
	qat_mstate_remote_run(test, sizeof(struct adf_mstate_preh), 0, 0, 0);
}

/* Valid in-bounds section header: same parser path, no out-of-bounds read. */
static void qat_mstate_remote_inbounds_sect(struct kunit *test)
{
	qat_mstate_remote_run(test, sizeof(struct adf_mstate_preh), 1, 0, 0);
}

/* preh_len == buffer size puts the cursor past the allocation; expect -EINVAL. */
static void qat_mstate_remote_oob_header(struct kunit *test)
{
	qat_mstate_remote_run(test, ADF_MSTATE_TEST_BUF_SIZE, 1, 0, -EINVAL);
}

static struct kunit_case qat_mstate_remote_cases[] = {
	KUNIT_CASE(qat_mstate_remote_empty),
	KUNIT_CASE(qat_mstate_remote_inbounds_sect),
	KUNIT_CASE(qat_mstate_remote_oob_header),
	{}
};

static struct kunit_suite qat_mstate_remote_suite = {
	.name = "qat_mstate_remote",
	.test_cases = qat_mstate_remote_cases,
};

kunit_test_suite(qat_mstate_remote_suite);
