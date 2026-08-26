// SPDX-License-Identifier: GPL-2.0
/*
 * Test cases for the drm_format functions
 *
 * Copyright (c) 2022 Maíra Canal <mairacanal@riseup.net>
 */

#include <kunit/test.h>

#include <drm/drm_fourcc.h>

/*
 * drm_format_info_block_width() must return 0 for invalid arguments:
 * a NULL format info, a negative plane index, or a plane index
 * beyond the number of planes, instead of crashing or returning
 * garbage.
 */
static void drm_test_format_block_width_invalid(struct kunit *test)
{
	const struct drm_format_info *info = NULL;

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 0);
}

/*
 * Check the block width of each plane of a single-plane, non-tiled
 * format (XRGB4444): plane 0 is made of 1x1 blocks, and plane indexes
 * beyond the number of planes report a width of 0.
 */
static void drm_test_format_block_width_one_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_XRGB4444);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
}

/*
 * Check the block width of each plane of a two-plane, non-tiled
 * format (NV12): both planes are made of 1x1 blocks, and plane
 * indexes beyond the number of planes report a width of 0.
 */
static void drm_test_format_block_width_two_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_NV12);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 2), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
}

/*
 * Check the block width of each plane of a three-plane, non-tiled
 * format (YUV422): all three planes are made of 1x1 blocks, and plane
 * indexes beyond the number of planes report a width of 0.
 */
static void drm_test_format_block_width_three_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_YUV422);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 2), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 3), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
}

/*
 * Check the block width of a tiled format (X0L0), whose pixels are
 * stored in 2x2 blocks: plane 0 reports a width of 2, and plane
 * indexes beyond the number of planes report a width of 0.
 */
static void drm_test_format_block_width_tiled(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_X0L0);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
}

/*
 * drm_format_info_block_height() must return 0 for invalid arguments:
 * a NULL format info, a negative plane index, or a plane index
 * beyond the number of planes, instead of crashing or returning
 * garbage.
 */
static void drm_test_format_block_height_invalid(struct kunit *test)
{
	const struct drm_format_info *info = NULL;

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 0);
}

/*
 * Check the block height of each plane of a single-plane, non-tiled
 * format (XRGB4444): plane 0 is made of 1x1 blocks, and plane indexes
 * beyond the number of planes report a height of 0.
 */
static void drm_test_format_block_height_one_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_XRGB4444);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 0);
}

/*
 * Check the block height of each plane of a two-plane, non-tiled
 * format (NV12): both planes are made of 1x1 blocks, and plane
 * indexes beyond the number of planes report a height of 0.
 */
static void drm_test_format_block_height_two_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_NV12);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 2), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
}

/*
 * Check the block height of each plane of a three-plane, non-tiled
 * format (YUV422): all three planes are made of 1x1 blocks, and plane
 * indexes beyond the number of planes report a height of 0.
 */
static void drm_test_format_block_height_three_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_YUV422);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 2), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 3), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
}

/*
 * Check the block height of a tiled format (X0L0), whose pixels are
 * stored in 2x2 blocks: plane 0 reports a height of 2, and plane
 * indexes beyond the number of planes report a height of 0.
 */
static void drm_test_format_block_height_tiled(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_X0L0);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
}

/*
 * drm_format_info_min_pitch() must return 0 for invalid arguments:
 * a NULL format info, a negative plane index, or a plane index
 * beyond the number of planes, instead of crashing or returning
 * garbage.
 */
static void drm_test_format_min_pitch_invalid(struct kunit *test)
{
	const struct drm_format_info *info = NULL;

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);
}

/*
 * Check the minimum pitch of the single 8-bits-per-pixel plane of
 * RGB332: one byte per pixel for any width, including odd widths
 * which must not be rounded, up to the UINT_MAX boundary.
 */
static void drm_test_format_min_pitch_one_plane_8bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_RGB332);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 640);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 1024);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 671);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, (UINT_MAX - 1)),
			(uint64_t)(UINT_MAX - 1));
}

/*
 * Check the minimum pitch of the single 16-bits-per-pixel plane of
 * XRGB4444: two bytes per pixel for any width, including odd widths
 * which must not be rounded, up to the UINT_MAX boundary.
 */
static void drm_test_format_min_pitch_one_plane_16bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_XRGB4444);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 4);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 1280);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 2048);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 3840);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 8192);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 1342);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX * 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, (UINT_MAX - 1)),
			(uint64_t)(UINT_MAX - 1) * 2);
}

/*
 * Check the minimum pitch of the single 24-bits-per-pixel plane of
 * RGB888: three bytes per pixel for any width, including odd widths
 * which must not be rounded, up to the UINT_MAX boundary.
 */
static void drm_test_format_min_pitch_one_plane_24bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_RGB888);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 3);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 6);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 3072);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 5760);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 12288);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 2013);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX * 3);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX - 1),
			(uint64_t)(UINT_MAX - 1) * 3);
}

/*
 * Check the minimum pitch of the single 32-bits-per-pixel plane of
 * ABGR8888: four bytes per pixel for any width, including odd widths
 * which must not be rounded, up to the UINT_MAX boundary.
 */
static void drm_test_format_min_pitch_one_plane_32bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_ABGR8888);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 4);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 8);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 2560);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 7680);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 16384);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 2684);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX * 4);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX - 1),
			(uint64_t)(UINT_MAX - 1) * 4);
}

/*
 * Check the minimum pitch of both planes of NV12: the first plane
 * stores one byte per pixel, the second stores two bytes per pixel,
 * odd widths must not be rounded, and chroma subsampling is not
 * taken into account.
 */
static void drm_test_format_min_pitch_two_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_NV12);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 1), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 1), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 640);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 320), 640);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 1024);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 512), 1024);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 960), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 2048), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 671);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 336), 672);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, UINT_MAX / 2 + 1),
			(uint64_t)UINT_MAX + 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, (UINT_MAX - 1)),
			(uint64_t)(UINT_MAX - 1));
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, (UINT_MAX - 1) /  2),
			(uint64_t)(UINT_MAX - 1));
}

/*
 * Check the minimum pitch of all three planes of YUV422, where each
 * plane stores one byte per pixel: odd widths must not be rounded and
 * chroma subsampling is not taken into account.
 */
static void drm_test_format_min_pitch_three_plane_8bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_YUV422);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 3, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 640);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 320), 320);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 320), 320);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 1024);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 512), 512);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 512), 512);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 960), 960);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 960), 960);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 2048), 2048);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 2048), 2048);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 671);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 336), 336);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 336), 336);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, UINT_MAX / 2 + 1),
			(uint64_t)UINT_MAX / 2 + 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, UINT_MAX / 2 + 1),
			(uint64_t)UINT_MAX / 2 + 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, (UINT_MAX - 1) / 2),
			(uint64_t)(UINT_MAX - 1) / 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, (UINT_MAX - 1) / 2),
			(uint64_t)(UINT_MAX - 1) / 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, (UINT_MAX - 1) / 2),
			(uint64_t)(UINT_MAX - 1) / 2);
}

/*
 * Check the minimum pitch of a tiled format (X0L2), which stores
 * 8 bytes in 2x2 pixel blocks: two bytes per pixel for any width,
 * including odd widths which must not be rounded, up to the UINT_MAX
 * boundary.
 */
static void drm_test_format_min_pitch_tiled(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_X0L2);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 4);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 1280);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 2048);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 3840);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 8192);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 1342);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX * 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX - 1),
			(uint64_t)(UINT_MAX - 1) * 2);
}

static struct kunit_case drm_format_tests[] = {
	KUNIT_CASE(drm_test_format_block_width_invalid),
	KUNIT_CASE(drm_test_format_block_width_one_plane),
	KUNIT_CASE(drm_test_format_block_width_two_plane),
	KUNIT_CASE(drm_test_format_block_width_three_plane),
	KUNIT_CASE(drm_test_format_block_width_tiled),
	KUNIT_CASE(drm_test_format_block_height_invalid),
	KUNIT_CASE(drm_test_format_block_height_one_plane),
	KUNIT_CASE(drm_test_format_block_height_two_plane),
	KUNIT_CASE(drm_test_format_block_height_three_plane),
	KUNIT_CASE(drm_test_format_block_height_tiled),
	KUNIT_CASE(drm_test_format_min_pitch_invalid),
	KUNIT_CASE(drm_test_format_min_pitch_one_plane_8bpp),
	KUNIT_CASE(drm_test_format_min_pitch_one_plane_16bpp),
	KUNIT_CASE(drm_test_format_min_pitch_one_plane_24bpp),
	KUNIT_CASE(drm_test_format_min_pitch_one_plane_32bpp),
	KUNIT_CASE(drm_test_format_min_pitch_two_plane),
	KUNIT_CASE(drm_test_format_min_pitch_three_plane_8bpp),
	KUNIT_CASE(drm_test_format_min_pitch_tiled),
	{}
};

static struct kunit_suite drm_format_test_suite = {
	.name = "drm_format",
	.test_cases = drm_format_tests,
};

kunit_test_suite(drm_format_test_suite);

MODULE_DESCRIPTION("Test cases for the drm_format functions");
MODULE_LICENSE("GPL");
