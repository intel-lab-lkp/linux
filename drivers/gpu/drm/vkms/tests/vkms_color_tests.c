/* SPDX-License-Identifier: GPL-2.0+ */

#include <kunit/test.h>

#include <drm/drm_fixed.h>

#define TEST_LUT_SIZE 16

static struct drm_color_lut test_linear_array[TEST_LUT_SIZE] = {
	{ 0x0, 0x0, 0x0, 0 },
	{ 0x1111, 0x1111, 0x1111, 0 },
	{ 0x2222, 0x2222, 0x2222, 0 },
	{ 0x3333, 0x3333, 0x3333, 0 },
	{ 0x4444, 0x4444, 0x4444, 0 },
	{ 0x5555, 0x5555, 0x5555, 0 },
	{ 0x6666, 0x6666, 0x6666, 0 },
	{ 0x7777, 0x7777, 0x7777, 0 },
	{ 0x8888, 0x8888, 0x8888, 0 },
	{ 0x9999, 0x9999, 0x9999, 0 },
	{ 0xaaaa, 0xaaaa, 0xaaaa, 0 },
	{ 0xbbbb, 0xbbbb, 0xbbbb, 0 },
	{ 0xcccc, 0xcccc, 0xcccc, 0 },
	{ 0xdddd, 0xdddd, 0xdddd, 0 },
	{ 0xeeee, 0xeeee, 0xeeee, 0 },
	{ 0xffff, 0xffff, 0xffff, 0 },
};

const struct vkms_color_lut test_linear_lut = {
	.base = test_linear_array,
	.lut_length = TEST_LUT_SIZE,
	.channel_value2index_ratio = 0xf000fll
};


static void vkms_color_test_get_lut_index(struct kunit *test)
{
	int i;

	KUNIT_EXPECT_EQ(test, drm_fixp2int(get_lut_index(&test_linear_lut, test_linear_array[0].red)), 0);

	for (i = 0; i < TEST_LUT_SIZE; i++)
		KUNIT_EXPECT_EQ(test, drm_fixp2int_ceil(get_lut_index(&test_linear_lut, test_linear_array[i].red)), i);
}

static void vkms_color_test_lerp(struct kunit *test)
{
	/*** half-way round down ***/
	s64 t = 0x80000000 - 1;
	KUNIT_EXPECT_EQ(test, lerp_u16(0x0, 0x10, t), 0x8);

	/* odd a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0x10, t), 0x8);

	/* odd b */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0xf, t), 0x8);

	/* b = a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x10, t), 0x10);

	/* b = a + 1 */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x11, t), 0x10);


	/*** half-way round up ***/
	t = 0x80000000;
	KUNIT_EXPECT_EQ(test, lerp_u16(0x0, 0x10, t), 0x8);

	/* odd a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0x10, t), 0x9);

	/* odd b */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0xf, t), 0x8);

	/* b = a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x10, t), 0x10);

	/* b = a + 1 */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x11, t), 0x11);

	/*** t = 0.0 ***/
	t = 0x0;
	KUNIT_EXPECT_EQ(test, lerp_u16(0x0, 0x10, t), 0x0);

	/* odd a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0x10, t), 0x1);

	/* odd b */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0xf, t), 0x1);

	/* b = a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x10, t), 0x10);

	/* b = a + 1 */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x11, t), 0x10);

	/*** t = 1.0 ***/
	t = 0x100000000;
	KUNIT_EXPECT_EQ(test, lerp_u16(0x0, 0x10, t), 0x10);

	/* odd a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0x10, t), 0x10);

	/* odd b */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0xf, t), 0xf);

	/* b = a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x10, t), 0x10);

	/* b = a + 1 */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x11, t), 0x11);


	/*** t = 0.0 + 1 ***/
	t = 0x0 + 1;
	KUNIT_EXPECT_EQ(test, lerp_u16(0x0, 0x10, t), 0x0);

	/* odd a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0x10, t), 0x1);

	/* odd b */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0xf, t), 0x1);

	/* b = a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x10, t), 0x10);

	/* b = a + 1 */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x11, t), 0x10);

	/*** t = 1.0 - 1 ***/
	t = 0x100000000 - 1;
	KUNIT_EXPECT_EQ(test, lerp_u16(0x0, 0x10, t), 0x10);

	/* odd a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0x10, t), 0x10);

	/* odd b */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x1, 0xf, t), 0xf);

	/* b = a */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x10, t), 0x10);

	/* b = a + 1 */
	KUNIT_EXPECT_EQ(test, lerp_u16(0x10, 0x11, t), 0x11);


	/*** t chosen to verify the flipping point of result a (or b) to a+1 (or b-1) ***/
	KUNIT_EXPECT_EQ(test, lerp_u16(0x0, 0x1, 0x80000000 - 1), 0x0);
	KUNIT_EXPECT_EQ(test, lerp_u16(0x0, 0x1, 0x80000000), 0x1);
}

static struct kunit_case vkms_color_test_cases[] = {
	KUNIT_CASE(vkms_color_test_get_lut_index),
	KUNIT_CASE(vkms_color_test_lerp),
	{}
};

static struct kunit_suite vkms_color_test_suite = {
	.name = "vkms-color",
	.test_cases = vkms_color_test_cases,
};
kunit_test_suite(vkms_color_test_suite);

MODULE_LICENSE("GPL");