// SPDX-License-Identifier: GPL-2.0
/*
 * Test cases for the drm_framebuffer functions
 *
 * Copyright (c) 2022 Maíra Canal <mairacanal@riseup.net>
 */

#include <kunit/test.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_mode.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_print.h>

#include "../drm_crtc_internal.h"

#define MIN_WIDTH 4
#define MAX_WIDTH 4096
#define MIN_HEIGHT 4
#define MAX_HEIGHT 4096

#define DRM_MODE_FB_INVALID BIT(2)

struct drm_framebuffer_test {
	int buffer_created;
	struct drm_mode_fb_cmd2 cmd;
	const char *name;
};

static const struct drm_framebuffer_test drm_framebuffer_create_cases[] = {
{ .buffer_created = 1, .name = "ABGR8888 normal sizes",
	.cmd = { .width = 600, .height = 600, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .pitches = { 4 * 600, 0, 0 },
	}
},
{ .buffer_created = 1, .name = "ABGR8888 max sizes",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .pitches = { 4 * MAX_WIDTH, 0, 0 },
	}
},
{ .buffer_created = 1, .name = "ABGR8888 pitch greater than min required",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .pitches = { 4 * MAX_WIDTH + 1, 0, 0 },
	}
},
{ .buffer_created = 0, .name = "ABGR8888 pitch less than min required",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .pitches = { 4 * MAX_WIDTH - 1, 0, 0 },
	}
},
{ .buffer_created = 0, .name = "ABGR8888 Invalid width",
	.cmd = { .width = MAX_WIDTH + 1, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .pitches = { 4 * (MAX_WIDTH + 1), 0, 0 },
	}
},
{ .buffer_created = 0, .name = "ABGR8888 Invalid buffer handle",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 0, 0, 0 }, .pitches = { 4 * MAX_WIDTH, 0, 0 },
	}
},
{ .buffer_created = 0, .name = "No pixel format",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = 0,
		 .handles = { 1, 0, 0 }, .pitches = { 4 * MAX_WIDTH, 0, 0 },
	}
},
{ .buffer_created = 0, .name = "ABGR8888 Width 0",
	.cmd = { .width = 0, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .pitches = { 4 * MAX_WIDTH, 0, 0 },
	}
},
{ .buffer_created = 0, .name = "ABGR8888 Height 0",
	.cmd = { .width = MAX_WIDTH, .height = 0, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .pitches = { 4 * MAX_WIDTH, 0, 0 },
	}
},
{ .buffer_created = 0, .name = "ABGR8888 Out of bound height * pitch combination",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .offsets = { UINT_MAX - 1, 0, 0 },
		 .pitches = { 4 * MAX_WIDTH, 0, 0 },
	}
},
{ .buffer_created = 1, .name = "ABGR8888 Large buffer offset",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .offsets = { UINT_MAX / 2, 0, 0 },
		 .pitches = { 4 * MAX_WIDTH, 0, 0 },
	}
},
{ .buffer_created = 0, .name = "ABGR8888 Non-zero buffer offset for unused plane",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .offsets = { UINT_MAX / 2, UINT_MAX / 2, 0 },
		 .pitches = { 4 * MAX_WIDTH, 0, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
	}
},
{ .buffer_created = 0, .name = "ABGR8888 Invalid flag",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .offsets = { UINT_MAX / 2, 0, 0 },
		 .pitches = { 4 * MAX_WIDTH, 0, 0 }, .flags = DRM_MODE_FB_INVALID,
	}
},
{ .buffer_created = 1, .name = "ABGR8888 Set DRM_MODE_FB_MODIFIERS without modifiers",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .offsets = { UINT_MAX / 2, 0, 0 },
		 .pitches = { 4 * MAX_WIDTH, 0, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
	}
},
{ .buffer_created = 1, .name = "ABGR8888 Valid buffer modifier",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .offsets = { UINT_MAX / 2, 0, 0 },
		 .pitches = { 4 * MAX_WIDTH, 0, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { AFBC_FORMAT_MOD_YTR, 0, 0 },
	}
},
{ .buffer_created = 0,
	.name = "ABGR8888 Invalid buffer modifier(DRM_FORMAT_MOD_SAMSUNG_64_32_TILE)",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .offsets = { UINT_MAX / 2, 0, 0 },
		 .pitches = { 4 * MAX_WIDTH, 0, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { DRM_FORMAT_MOD_SAMSUNG_64_32_TILE, 0, 0 },
	}
},
{ .buffer_created = 1, .name = "ABGR8888 Extra pitches without DRM_MODE_FB_MODIFIERS",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .offsets = { UINT_MAX / 2, 0, 0 },
		 .pitches = { 4 * MAX_WIDTH, 4 * MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 0, .name = "ABGR8888 Extra pitches with DRM_MODE_FB_MODIFIERS",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_ABGR8888,
		 .handles = { 1, 0, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .pitches = { 4 * MAX_WIDTH, 4 * MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 1, .name = "NV12 Normal sizes",
	.cmd = { .width = 600, .height = 600, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 0 }, .pitches = { 600, 600, 0 },
	}
},
{ .buffer_created = 1, .name = "NV12 Max sizes",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 0 }, .pitches = { MAX_WIDTH, MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 0, .name = "NV12 Invalid pitch",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 0 }, .pitches = { MAX_WIDTH, MAX_WIDTH - 1, 0 },
	}
},
{ .buffer_created = 0, .name = "NV12 Invalid modifier/missing DRM_MODE_FB_MODIFIERS flag",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 0 }, .modifier = { DRM_FORMAT_MOD_SAMSUNG_64_32_TILE, 0, 0 },
		 .pitches = { MAX_WIDTH, MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 0, .name = "NV12 different  modifier per-plane",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { DRM_FORMAT_MOD_SAMSUNG_64_32_TILE, 0, 0 },
		 .pitches = { MAX_WIDTH, MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 1, .name = "NV12 with DRM_FORMAT_MOD_SAMSUNG_64_32_TILE",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { DRM_FORMAT_MOD_SAMSUNG_64_32_TILE,
			 DRM_FORMAT_MOD_SAMSUNG_64_32_TILE, 0 },
		 .pitches = { MAX_WIDTH, MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 0, .name = "NV12 Valid modifiers without DRM_MODE_FB_MODIFIERS",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 0 }, .modifier = { DRM_FORMAT_MOD_SAMSUNG_64_32_TILE,
						       DRM_FORMAT_MOD_SAMSUNG_64_32_TILE, 0 },
		 .pitches = { MAX_WIDTH, MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 0, .name = "NV12 Modifier for inexistent plane",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { DRM_FORMAT_MOD_SAMSUNG_64_32_TILE, DRM_FORMAT_MOD_SAMSUNG_64_32_TILE,
			       DRM_FORMAT_MOD_SAMSUNG_64_32_TILE },
		 .pitches = { MAX_WIDTH, MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 0, .name = "NV12 Handle for inexistent plane",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 1 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .pitches = { MAX_WIDTH, MAX_WIDTH, 0 },
	}
},
{ .buffer_created = 1, .name = "NV12 Handle for inexistent plane without DRM_MODE_FB_MODIFIERS",
	.cmd = { .width = 600, .height = 600, .pixel_format = DRM_FORMAT_NV12,
		 .handles = { 1, 1, 1 }, .pitches = { 600, 600, 600 },
	}
},
{ .buffer_created = 1, .name = "YVU420 DRM_MODE_FB_MODIFIERS set without modifier",
	.cmd = { .width = 600, .height = 600, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .pitches = { 600, 300, 300 },
	}
},
{ .buffer_created = 1, .name = "YVU420 Normal sizes",
	.cmd = { .width = 600, .height = 600, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .pitches = { 600, 300, 300 },
	}
},
{ .buffer_created = 1, .name = "YVU420 Max sizes",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2),
						      DIV_ROUND_UP(MAX_WIDTH, 2) },
	}
},
{ .buffer_created = 0, .name = "YVU420 Invalid pitch",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2) - 1,
						      DIV_ROUND_UP(MAX_WIDTH, 2) },
	}
},
{ .buffer_created = 1, .name = "YVU420 Different pitches",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2) + 1,
						      DIV_ROUND_UP(MAX_WIDTH, 2) + 7 },
	}
},
{ .buffer_created = 1, .name = "YVU420 Different buffer offsets/pitches",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .offsets = { MAX_WIDTH, MAX_WIDTH  +
			 MAX_WIDTH * MAX_HEIGHT, MAX_WIDTH  + 2 * MAX_WIDTH * MAX_HEIGHT },
		 .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2) + 1,
			 DIV_ROUND_UP(MAX_WIDTH, 2) + 7 },
	}
},
{ .buffer_created = 0,
	.name = "YVU420 Modifier set just for plane 0, without DRM_MODE_FB_MODIFIERS",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .modifier = { AFBC_FORMAT_MOD_SPARSE, 0, 0 },
		 .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2), DIV_ROUND_UP(MAX_WIDTH, 2) },
	}
},
{ .buffer_created = 0,
	.name = "YVU420 Modifier set just for planes 0, 1, without DRM_MODE_FB_MODIFIERS",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 },
		 .modifier = { AFBC_FORMAT_MOD_SPARSE, AFBC_FORMAT_MOD_SPARSE, 0 },
		 .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2), DIV_ROUND_UP(MAX_WIDTH, 2) },
	}
},
{ .buffer_created = 0,
	.name = "YVU420 Modifier set just for plane 0, 1, with DRM_MODE_FB_MODIFIERS",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { AFBC_FORMAT_MOD_SPARSE, AFBC_FORMAT_MOD_SPARSE, 0 },
		 .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2), DIV_ROUND_UP(MAX_WIDTH, 2) },
	}
},
{ .buffer_created = 1, .name = "YVU420 Valid modifier",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { AFBC_FORMAT_MOD_SPARSE, AFBC_FORMAT_MOD_SPARSE,
			 AFBC_FORMAT_MOD_SPARSE },
		 .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2), DIV_ROUND_UP(MAX_WIDTH, 2) },
	}
},
{ .buffer_created = 0, .name = "YVU420 Different modifiers per plane",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { AFBC_FORMAT_MOD_SPARSE, AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_YTR,
			       AFBC_FORMAT_MOD_SPARSE },
		 .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2), DIV_ROUND_UP(MAX_WIDTH, 2) },
	}
},
{ .buffer_created = 0, .name = "YVU420 Modifier for inexistent plane",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YVU420,
		 .handles = { 1, 1, 1 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { AFBC_FORMAT_MOD_SPARSE, AFBC_FORMAT_MOD_SPARSE,
			 AFBC_FORMAT_MOD_SPARSE, AFBC_FORMAT_MOD_SPARSE },
		 .pitches = { MAX_WIDTH, DIV_ROUND_UP(MAX_WIDTH, 2), DIV_ROUND_UP(MAX_WIDTH, 2) },
	}
},
{ .buffer_created = 0, .name = "YUV420_10BIT Invalid modifier(DRM_FORMAT_MOD_LINEAR)",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_YUV420_10BIT,
		 .handles = { 1, 0, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .modifier = { DRM_FORMAT_MOD_LINEAR, 0, 0 },
		 .pitches = { MAX_WIDTH, 0, 0 },
	}
},
{ .buffer_created = 1, .name = "X0L2 Normal sizes",
	.cmd = { .width = 600, .height = 600, .pixel_format = DRM_FORMAT_X0L2,
		 .handles = { 1, 0, 0 }, .pitches = { 1200, 0, 0 }
	}
},
{ .buffer_created = 1, .name = "X0L2 Max sizes",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_X0L2,
		 .handles = { 1, 0, 0 }, .pitches = { 2 * MAX_WIDTH, 0, 0 }
	}
},
{ .buffer_created = 0, .name = "X0L2 Invalid pitch",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_X0L2,
		 .handles = { 1, 0, 0 }, .pitches = { 2 * MAX_WIDTH - 1, 0, 0 }
	}
},
{ .buffer_created = 1, .name = "X0L2 Pitch greater than minimum required",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_X0L2,
		 .handles = { 1, 0, 0 }, .pitches = { 2 * MAX_WIDTH + 1, 0, 0 }
	}
},
{ .buffer_created = 0, .name = "X0L2 Handle for inexistent plane",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_X0L2,
		 .handles = { 1, 1, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
		 .pitches = { 2 * MAX_WIDTH + 1, 0, 0 }
	}
},
{ .buffer_created = 1,
	.name = "X0L2 Offset for inexistent plane, without DRM_MODE_FB_MODIFIERS set",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_X0L2,
		 .handles = { 1, 0, 0 }, .offsets = { 0, 0, 3 },
		 .pitches = { 2 * MAX_WIDTH + 1, 0, 0 }
	}
},
{ .buffer_created = 0, .name = "X0L2 Modifier without DRM_MODE_FB_MODIFIERS set",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_X0L2,
		 .handles = { 1, 0, 0 }, .pitches = { 2 * MAX_WIDTH + 1, 0, 0 },
		 .modifier = { AFBC_FORMAT_MOD_SPARSE, 0, 0 },
	}
},
{ .buffer_created = 1, .name = "X0L2 Valid modifier",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT, .pixel_format = DRM_FORMAT_X0L2,
		 .handles = { 1, 0, 0 }, .pitches = { 2 * MAX_WIDTH + 1, 0, 0 },
		 .modifier = { AFBC_FORMAT_MOD_SPARSE, 0, 0 }, .flags = DRM_MODE_FB_MODIFIERS,
	}
},
{ .buffer_created = 0, .name = "X0L2 Modifier for inexistent plane",
	.cmd = { .width = MAX_WIDTH, .height = MAX_HEIGHT,
		 .pixel_format = DRM_FORMAT_X0L2, .handles = { 1, 0, 0 },
		 .pitches = { 2 * MAX_WIDTH + 1, 0, 0 },
		 .modifier = { AFBC_FORMAT_MOD_SPARSE, AFBC_FORMAT_MOD_SPARSE, 0 },
		 .flags = DRM_MODE_FB_MODIFIERS,
	}
},
};

struct drm_framebuffer_test_priv {
	struct drm_device dev;
	void *private;
};

static struct drm_framebuffer *fb_create_mock(struct drm_device *dev,
					      struct drm_file *file_priv,
					      const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_framebuffer_test_priv *priv = container_of(dev, typeof(*priv), dev);
	int *buffer_created = priv->private;
	*buffer_created = 1;
	return ERR_PTR(-EINVAL);
}

static struct drm_mode_config_funcs mock_config_funcs = {
	.fb_create = fb_create_mock,
};

static int drm_framebuffer_test_init(struct kunit *test)
{
	struct device *parent;
	struct drm_framebuffer_test_priv *priv;
	struct drm_device *dev;

	parent = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent);

	priv = drm_kunit_helper_alloc_drm_device(test, parent, typeof(*priv),
						 dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);
	dev = &priv->dev;

	dev->mode_config.min_width = MIN_WIDTH;
	dev->mode_config.max_width = MAX_WIDTH;
	dev->mode_config.min_height = MIN_HEIGHT;
	dev->mode_config.max_height = MAX_HEIGHT;
	dev->mode_config.funcs = &mock_config_funcs;

	test->priv = priv;
	return 0;
}

static void drm_test_framebuffer_create(struct kunit *test)
{
	const struct drm_framebuffer_test *params = test->param_value;
	struct drm_framebuffer_test_priv *priv = test->priv;
	struct drm_device *dev = &priv->dev;
	int buffer_created = 0;

	priv->private = &buffer_created;
	drm_internal_framebuffer_create(dev, &params->cmd, NULL);
	KUNIT_EXPECT_EQ(test, params->buffer_created, buffer_created);
}

static void drm_framebuffer_test_to_desc(const struct drm_framebuffer_test *t, char *desc)
{
	strscpy(desc, t->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(drm_framebuffer_create, drm_framebuffer_create_cases,
		  drm_framebuffer_test_to_desc);

/*
 * This test is very similar to drm_test_framebuffer_create, except that it
 * set dev->mode_config.fb_modifiers_not_supported member to 1, covering
 * the case of trying to create a framebuffer with modifiers without the
 * device really supporting it.
 */
static void drm_test_framebuffer_modifiers_not_supported(struct kunit *test)
{
	struct drm_framebuffer_test_priv *priv = test->priv;
	struct drm_device *dev = &priv->dev;
	int buffer_created = 0;

	/* A valid cmd with modifier */
	struct drm_mode_fb_cmd2 cmd = {
		.width = MAX_WIDTH, .height = MAX_HEIGHT,
		.pixel_format = DRM_FORMAT_ABGR8888, .handles = { 1, 0, 0 },
		.offsets = { UINT_MAX / 2, 0, 0 }, .pitches = { 4 * MAX_WIDTH, 0, 0 },
		.flags = DRM_MODE_FB_MODIFIERS,
	};

	priv->private = &buffer_created;
	dev->mode_config.fb_modifiers_not_supported = 1;

	drm_internal_framebuffer_create(dev, &cmd, NULL);
	KUNIT_EXPECT_EQ(test, 0, buffer_created);
}

/* Parameters for testing drm_framebuffer_check_src_coords function */
struct drm_framebuffer_check_src_coords_case {
	const char *name;
	const int expect;
	const unsigned int fb_size;
	const uint32_t src_x;
	const uint32_t src_y;

	/* Deltas to be applied on source */
	const uint32_t dsrc_w;
	const uint32_t dsrc_h;
};

static const struct drm_framebuffer_check_src_coords_case
drm_framebuffer_check_src_coords_cases[] = {
	{ .name = "Success: source fits into fb",
	  .expect = 0,
	},
	{ .name = "Fail: overflowing fb with x-axis coordinate",
	  .expect = -ENOSPC, .src_x = 1, .fb_size = UINT_MAX,
	},
	{ .name = "Fail: overflowing fb with y-axis coordinate",
	  .expect = -ENOSPC, .src_y = 1, .fb_size = UINT_MAX,
	},
	{ .name = "Fail: overflowing fb with source width",
	  .expect = -ENOSPC, .dsrc_w = 1, .fb_size = UINT_MAX - 1,
	},
	{ .name = "Fail: overflowing fb with source height",
	  .expect = -ENOSPC, .dsrc_h = 1, .fb_size = UINT_MAX - 1,
	},
};

static void drm_test_framebuffer_check_src_coords(struct kunit *test)
{
	const struct drm_framebuffer_check_src_coords_case *params = test->param_value;
	const uint32_t src_x = params->src_x;
	const uint32_t src_y = params->src_y;
	const uint32_t src_w = (params->fb_size << 16) + params->dsrc_w;
	const uint32_t src_h = (params->fb_size << 16) + params->dsrc_h;
	const struct drm_framebuffer fb = {
		.width  = params->fb_size,
		.height = params->fb_size
	};
	int ret;

	ret = drm_framebuffer_check_src_coords(src_x, src_y, src_w, src_h, &fb);
	KUNIT_EXPECT_EQ(test, ret, params->expect);
}

static void
check_src_coords_test_to_desc(const struct drm_framebuffer_check_src_coords_case *t,
			      char *desc)
{
	strscpy(desc, t->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(check_src_coords, drm_framebuffer_check_src_coords_cases,
		  check_src_coords_test_to_desc);

static void drm_test_framebuffer_cleanup(struct kunit *test)
{
	struct drm_framebuffer_test_priv *priv = test->priv;
	struct drm_device *dev = &priv->dev;
	struct list_head *fb_list = &dev->mode_config.fb_list;
	struct drm_framebuffer fb1 = { .dev = dev };
	struct drm_framebuffer fb2 = { .dev = dev };

	/* This must result on [fb_list] -> fb1 -> fb2 */
	list_add_tail(&fb1.head, fb_list);
	list_add_tail(&fb2.head, fb_list);
	dev->mode_config.num_fb = 2;

	KUNIT_ASSERT_PTR_EQ(test, fb_list->prev, &fb2.head);
	KUNIT_ASSERT_PTR_EQ(test, fb_list->next, &fb1.head);
	KUNIT_ASSERT_PTR_EQ(test, fb1.head.prev, fb_list);
	KUNIT_ASSERT_PTR_EQ(test, fb1.head.next, &fb2.head);
	KUNIT_ASSERT_PTR_EQ(test, fb2.head.prev, &fb1.head);
	KUNIT_ASSERT_PTR_EQ(test, fb2.head.next, fb_list);

	drm_framebuffer_cleanup(&fb1);

	/* Now [fb_list] -> fb2 */
	KUNIT_ASSERT_PTR_EQ(test, fb_list->prev, &fb2.head);
	KUNIT_ASSERT_PTR_EQ(test, fb_list->next, &fb2.head);
	KUNIT_ASSERT_PTR_EQ(test, fb2.head.prev, fb_list);
	KUNIT_ASSERT_PTR_EQ(test, fb2.head.next, fb_list);
	KUNIT_ASSERT_EQ(test, dev->mode_config.num_fb, 1);

	drm_framebuffer_cleanup(&fb2);

	/* Now fb_list is empty */
	KUNIT_ASSERT_TRUE(test, list_empty(fb_list));
	KUNIT_ASSERT_EQ(test, dev->mode_config.num_fb, 0);
}

static void drm_test_framebuffer_lookup(struct kunit *test)
{
	struct drm_framebuffer_test_priv *priv = test->priv;
	struct drm_device *dev = &priv->dev;
	struct drm_format_info format = { };
	struct drm_framebuffer fb1 = { .dev = dev, .format = &format };
	struct drm_framebuffer *fb2;
	uint32_t id = 0;
	int ret;

	ret = drm_framebuffer_init(dev, &fb1, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	id = fb1.base.id;

	/* Looking for fb1 */
	fb2 = drm_framebuffer_lookup(dev, NULL, id);
	KUNIT_EXPECT_PTR_EQ(test, fb2, &fb1);

	/* Looking for an inexistent framebuffer */
	fb2 = drm_framebuffer_lookup(dev, NULL, id + 1);
	KUNIT_EXPECT_NULL(test, fb2);

	drm_framebuffer_cleanup(&fb1);
}

static void drm_test_framebuffer_init(struct kunit *test)
{
	struct drm_framebuffer_test_priv *priv = test->priv;
	struct drm_device *dev = &priv->dev;
	struct drm_format_info format = { };
	struct drm_framebuffer fb1 = { .format = &format };
	struct drm_framebuffer *fb2;
	struct drm_framebuffer_funcs funcs = { };
	int ret;

	/* Fails if fb->dev doesn't point to the drm_device passed on first arg */
	ret = drm_framebuffer_init(dev, &fb1, &funcs);
	KUNIT_ASSERT_EQ(test, ret, -EINVAL);
	fb1.dev = dev;

	/* Fails if fb.format isn't set */
	fb1.format = NULL;
	ret = drm_framebuffer_init(dev, &fb1, &funcs);
	KUNIT_ASSERT_EQ(test, ret, -EINVAL);
	fb1.format = &format;

	ret = drm_framebuffer_init(dev, &fb1, &funcs);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * Check if fb->funcs is actually set to the drm_framebuffer_funcs
	 * passed to it
	 */
	KUNIT_EXPECT_PTR_EQ(test, fb1.funcs, &funcs);

	/* The fb->comm must be set to the current running process */
	KUNIT_EXPECT_STREQ(test, fb1.comm, current->comm);

	/* The fb->base must be successfully initialized */
	KUNIT_EXPECT_NE(test, fb1.base.id, 0);
	KUNIT_EXPECT_EQ(test, fb1.base.type, DRM_MODE_OBJECT_FB);
	KUNIT_EXPECT_EQ(test, kref_read(&fb1.base.refcount), 1);
	KUNIT_EXPECT_PTR_EQ(test, fb1.base.free_cb, &drm_framebuffer_free);

	/* Checks if the fb is really published and findable */
	fb2 = drm_framebuffer_lookup(dev, NULL, fb1.base.id);
	KUNIT_EXPECT_PTR_EQ(test, fb2, &fb1);

	/* There must be just that one fb initialized */
	KUNIT_EXPECT_EQ(test, dev->mode_config.num_fb, 1);
	KUNIT_EXPECT_PTR_EQ(test, dev->mode_config.fb_list.prev, &fb1.head);
	KUNIT_EXPECT_PTR_EQ(test, dev->mode_config.fb_list.next, &fb1.head);

	drm_framebuffer_cleanup(&fb1);
}

static void destroy_free_mock(struct drm_framebuffer *fb)
{
	struct drm_framebuffer_test_priv *priv = container_of(fb->dev, typeof(*priv), dev);
	int *buffer_freed = priv->private;
	*buffer_freed = 1;
}

static struct drm_framebuffer_funcs framebuffer_funcs_free_mock = {
	.destroy = destroy_free_mock,
};

static void drm_test_framebuffer_free(struct kunit *test)
{
	struct drm_framebuffer_test_priv *priv = test->priv;
	struct drm_device *dev = &priv->dev;
	struct drm_mode_object *obj;
	struct drm_framebuffer fb = {
		.dev = dev,
		.funcs = &framebuffer_funcs_free_mock,
	};
	int buffer_freed = 0;
	int id, ret;

	priv->private = &buffer_freed;

	/*
	 * Case where the fb isn't registered. Just test if
	 * drm_framebuffer_free calls fb->funcs->destroy
	 */
	drm_framebuffer_free(&fb.base.refcount);
	KUNIT_EXPECT_EQ(test, buffer_freed, 1);

	buffer_freed = 0;

	ret = drm_mode_object_add(dev, &fb.base, DRM_MODE_OBJECT_FB);
	KUNIT_ASSERT_EQ(test, ret, 0);
	id = fb.base.id;

	/* Now, test with the fb registered, that must end unregistered */
	drm_framebuffer_free(&fb.base.refcount);
	KUNIT_EXPECT_EQ(test, fb.base.id, 0);
	KUNIT_EXPECT_EQ(test, buffer_freed, 1);

	/* Test if the old id of the fb was really removed from the idr pool */
	obj = drm_mode_object_find(dev, NULL, id, DRM_MODE_OBJECT_FB);
	KUNIT_EXPECT_PTR_EQ(test, obj, NULL);
}

static struct kunit_case drm_framebuffer_tests[] = {
	KUNIT_CASE_PARAM(drm_test_framebuffer_check_src_coords, check_src_coords_gen_params),
	KUNIT_CASE(drm_test_framebuffer_cleanup),
	KUNIT_CASE_PARAM(drm_test_framebuffer_create, drm_framebuffer_create_gen_params),
	KUNIT_CASE(drm_test_framebuffer_free),
	KUNIT_CASE(drm_test_framebuffer_init),
	KUNIT_CASE(drm_test_framebuffer_lookup),
	KUNIT_CASE(drm_test_framebuffer_modifiers_not_supported),
	{ }
};

static struct kunit_suite drm_framebuffer_test_suite = {
	.name = "drm_framebuffer",
	.init = drm_framebuffer_test_init,
	.test_cases = drm_framebuffer_tests,
};

kunit_test_suite(drm_framebuffer_test_suite);

MODULE_LICENSE("GPL");
