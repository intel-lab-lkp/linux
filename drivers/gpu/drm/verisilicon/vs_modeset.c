// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) VeriSilicon Holdings Co., Ltd.
 */
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "vs_modeset.h"

static const struct drm_mode_config_funcs vs_mode_config_funcs = {
	.fb_create			 = drm_gem_fb_create,
	.atomic_check		 = drm_atomic_helper_check,
	.atomic_commit		 = drm_atomic_helper_commit,
};

static struct drm_mode_config_helper_funcs vs_mode_config_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

void vs_mode_config_init(struct drm_device *dev)
{
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return;

	dev->mode_config.funcs = &vs_mode_config_funcs;
	dev->mode_config.helper_private = &vs_mode_config_helpers;
}
