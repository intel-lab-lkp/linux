// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author:Mark Yao <mark.yao@rock-chips.com>
 */

#include <linux/kernel.h>

#include <drm/drm.h>
#include <drm/drm_atomic.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_fb.h"
#include "rockchip_drm_gem.h"

static const struct drm_framebuffer_funcs rockchip_drm_fb_funcs = {
	.destroy       = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
	.dirty	       = drm_atomic_helper_dirtyfb,
};

/*
 * This commit tail explicitly copies and changes the behaviour of
 * the related core DRM atomic helper instead of trying to make
 * the core helpers overly generic.
 */
static void rockchip_drm_atomic_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;

	/*
	 * Variant of drm_atomic_helper_commit_modeset_disables()
	 * that will disable and post-disable all bridges BEFORE
	 * disabling the CRTC.
	 */
	drm_atomic_helper_commit_encoder_bridge_disable(dev, state);
	drm_atomic_helper_commit_encoder_bridge_post_disable(dev, state);
	drm_atomic_helper_commit_crtc_disable(dev, state);
	drm_atomic_helper_update_legacy_modeset_state(dev, state);
	drm_atomic_helper_calc_timestamping_constants(state);
	drm_atomic_helper_commit_crtc_set_mode(dev, state);

	/*
	 * Variant of drm_atomic_helper_commit_modeset_enables()
	 * that will enable the CRTC BEFORE pre-enabling and
	 * enabling the bridges.
	 */
	drm_atomic_helper_commit_crtc_enable(dev, state);
	drm_atomic_helper_commit_encoder_bridge_pre_enable(dev, state);
	drm_atomic_helper_commit_encoder_bridge_enable(dev, state);
	drm_atomic_helper_commit_writebacks(dev, state);

	drm_atomic_helper_commit_planes(dev, state,
					DRM_PLANE_COMMIT_ACTIVE_ONLY);

	drm_atomic_helper_fake_vblank(state);

	drm_atomic_helper_commit_hw_done(state);

	drm_atomic_helper_wait_for_vblanks(dev, state);

	drm_atomic_helper_cleanup_planes(dev, state);
}

static const struct drm_mode_config_helper_funcs rockchip_mode_config_helpers = {
	/*
	 * Using this function is necessary to commit atomic updates
	 * that need the CRTC to be enabled before a commit, as is
	 * the case with e.g. DSI displays.
	 */
	.atomic_commit_tail = rockchip_drm_atomic_commit_tail,
};

static struct drm_framebuffer *
rockchip_fb_create(struct drm_device *dev, struct drm_file *file,
		   const struct drm_format_info *info,
		   const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_afbc_framebuffer *afbc_fb;
	int ret;

	afbc_fb = kzalloc(sizeof(*afbc_fb), GFP_KERNEL);
	if (!afbc_fb)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_fb_init_with_funcs(dev, &afbc_fb->base,
					 file, info, mode_cmd,
					 &rockchip_drm_fb_funcs);
	if (ret) {
		kfree(afbc_fb);
		return ERR_PTR(ret);
	}

	if (drm_is_afbc(mode_cmd->modifier[0])) {
		ret = drm_gem_fb_afbc_init(dev, info, mode_cmd, afbc_fb);
		if (ret) {
			drm_framebuffer_put(&afbc_fb->base);
			return ERR_PTR(ret);
		}
	}

	return &afbc_fb->base;
}

static const struct drm_mode_config_funcs rockchip_drm_mode_config_funcs = {
	.fb_create = rockchip_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

void rockchip_drm_mode_config_init(struct drm_device *dev)
{
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;

	/*
	 * set max width and height as default value(4096x4096).
	 * this value would be used to check framebuffer size limitation
	 * at drm_mode_addfb().
	 */
	dev->mode_config.max_width = 4096;
	dev->mode_config.max_height = 4096;

	dev->mode_config.funcs = &rockchip_drm_mode_config_funcs;
	dev->mode_config.helper_private = &rockchip_mode_config_helpers;

	dev->mode_config.normalize_zpos = true;
}
