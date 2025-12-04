// SPDX-License-Identifier: GPL-2.0-or-later

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>
#include <linux/delay.h>

#include "yhgch_drm_drv.h"
#include "yhgch_drm_regs.h"

struct yhgch_dislay_pll_config {
	u64 hdisplay;
	u64 vdisplay;
	u32 pll1_config_value;
	u32 pll2_config_value;
};

static const struct yhgch_dislay_pll_config yhgch_pll_table[] = {
	{ 640, 480, CRT_PLL1_NS_25MHZ, CRT_PLL2_NS_25MHZ },
	{ 800, 600, CRT_PLL1_NS_40MHZ, CRT_PLL2_NS_40MHZ },
	{ 1024, 768, CRT_PLL1_NS_65MHZ, CRT_PLL2_NS_65MHZ },
	{ 1280, 1024, CRT_PLL1_NS_108MHZ, CRT_PLL2_NS_108MHZ },
	{ 1920, 1080, CRT_PLL1_NS_148MHZ, CRT_PLL2_NS_148MHZ },
};

static int yhgch_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct drm_crtc_state *new_crtc_state = NULL;

	if (new_plane_state->crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_plane_state->crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state, new_crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, true);
}

static void yhgch_handle_damage(void __iomem *addr_base, struct iosys_map *src,
				struct drm_framebuffer *fb,
				struct drm_rect *clip)
{
	struct iosys_map dst;

	iosys_map_set_vaddr_iomem(&dst, addr_base);
	iosys_map_incr(&dst, drm_fb_clip_offset(fb->pitches[0], fb->format, clip));
	drm_fb_memcpy(&dst, fb->pitches, src, fb, clip);
}

static void yhgch_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct yhgch_drm_private *priv = to_yhgch_drm_private(plane->dev);
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage;
	u32 reg;
	s64 gpu_addr = 0;
	u32 line_l;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE) == 0) {
		drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
		drm_atomic_for_each_plane_damage(&iter, &damage) {
			yhgch_handle_damage(priv->vram_base, shadow_plane_state->data, fb, &damage);
		}
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	}

	writel(gpu_addr, priv->mmio + YHGCH_CRT_FB_ADDRESS);

	reg = drm_format_info_min_pitch(fb->format, 0, fb->width);

	line_l = fb->pitches[0];
	writel(FIELD_PREP(YHGCH_CRT_FB_WIDTH_WIDTH_MASK, reg) |
	       FIELD_PREP(YHGCH_CRT_FB_WIDTH_OFFS_MASK, line_l),
	       priv->mmio + YHGCH_CRT_FB_WIDTH);

	/* SET PIXEL FORMAT */
	reg = readl(priv->mmio + YHGCH_CRT_DISP_CTL);
	reg &= ~YHGCH_CRT_DISP_CTL_FORMAT_MASK;
	reg |= FIELD_PREP(YHGCH_CRT_DISP_CTL_FORMAT_MASK,
			   fb->format->cpp[0] * 8 / 16);
	writel(reg, priv->mmio + YHGCH_CRT_DISP_CTL);
}

static const u32 channel_formats1[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB888,
};

static struct drm_plane_funcs yhgch_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const struct drm_plane_helper_funcs yhgch_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = yhgch_plane_atomic_check,
	.atomic_update = yhgch_plane_atomic_update,
};

static void yhgch_crtc_dpms(struct drm_crtc *crtc, u32 dpms)
{
	struct yhgch_drm_private *priv = to_yhgch_drm_private(crtc->dev);
	u32 reg;

	reg = readl(priv->mmio + YHGCH_CRT_DISP_CTL);
	reg &= ~YHGCH_CRT_DISP_CTL_DPMS_MASK;
	reg |= FIELD_PREP(YHGCH_CRT_DISP_CTL_DPMS_MASK, dpms);
	reg &= ~YHGCH_CRT_DISP_CTL_TIMING_MASK;
	if (dpms == YHGCH_CRT_DPMS_ON)
		reg |= YHGCH_CRT_DISP_CTL_TIMING(1);
	writel(reg, priv->mmio + YHGCH_CRT_DISP_CTL);
}

static void yhgch_crtc_atomic_enable(struct drm_crtc *crtc,
				     struct drm_atomic_state *old_state)
{
	u32 reg;
	struct yhgch_drm_private *priv = to_yhgch_drm_private(crtc->dev);

	yhgch_set_power_mode(priv, YHGCH_PW_MODE_CTL_MODE_MODE0);

	/* Enable display power gate & LOCALMEM power gate */
	reg = readl(priv->mmio + YHGCH_CURRENT_GATE);
	reg &= ~YHGCH_CURR_GATE_LOCALMEM_MASK;
	reg &= ~YHGCH_CURR_GATE_DISPLAY_MASK;
	reg |= YHGCH_CURR_GATE_LOCALMEM(1);
	reg |= YHGCH_CURR_GATE_DISPLAY(1);
	yhgch_set_current_gate(priv, reg);
	yhgch_crtc_dpms(crtc, YHGCH_CRT_DPMS_ON);
}

static void yhgch_crtc_atomic_disable(struct drm_crtc *crtc,
				      struct drm_atomic_state *old_state)
{
	u32 reg;
	struct yhgch_drm_private *priv = to_yhgch_drm_private(crtc->dev);

	yhgch_crtc_dpms(crtc, YHGCH_CRT_DPMS_OFF);

	yhgch_set_power_mode(priv, YHGCH_PW_MODE_CTL_MODE_SLEEP);

	/* Enable display power gate & LOCALMEM power gate */
	reg = readl(priv->mmio + YHGCH_CURRENT_GATE);
	reg &= ~YHGCH_CURR_GATE_LOCALMEM_MASK;
	reg &= ~YHGCH_CURR_GATE_DISPLAY_MASK;
	reg |= YHGCH_CURR_GATE_LOCALMEM(0);
	reg |= YHGCH_CURR_GATE_DISPLAY(0);
	yhgch_set_current_gate(priv, reg);
}

static enum drm_mode_status
yhgch_crtc_mode_valid(struct drm_crtc *crtc,
		      const struct drm_display_mode *mode)
{
	size_t i = 0;
	int vrefresh = drm_mode_vrefresh(mode);

	if (vrefresh < 59 || vrefresh > 61)
		return MODE_NOCLOCK;

	for (i = 0; i < ARRAY_SIZE(yhgch_pll_table); i++) {
		if (yhgch_pll_table[i].hdisplay == mode->hdisplay &&
		    yhgch_pll_table[i].vdisplay == mode->vdisplay)
			return MODE_OK;
	}

	return MODE_BAD;
}

static void set_vclock_yhgch(struct drm_device *dev, u64 pll)
{
	u32 val;
	struct yhgch_drm_private *priv = to_yhgch_drm_private(dev);

	val = readl(priv->mmio + CRT_PLL1_NS);
	val &= ~(CRT_PLL1_NS_OUTER_BYPASS(1));
	writel(val, priv->mmio + CRT_PLL1_NS);

	val = CRT_PLL1_NS_INTER_BYPASS(1) | CRT_PLL1_NS_POWERON(1);
	writel(val, priv->mmio + CRT_PLL1_NS);

	writel(pll, priv->mmio + CRT_PLL1_NS);

	usleep_range(1000, 2000);

	val = pll & ~(CRT_PLL1_NS_POWERON(1));
	writel(val, priv->mmio + CRT_PLL1_NS);

	usleep_range(1000, 2000);

	val &= ~(CRT_PLL1_NS_INTER_BYPASS(1));
	writel(val, priv->mmio + CRT_PLL1_NS);

	usleep_range(1000, 2000);

	val |= CRT_PLL1_NS_OUTER_BYPASS(1);
	writel(val, priv->mmio + CRT_PLL1_NS);
}

static void get_pll_config(u64 x, u64 y, u32 *pll1, u32 *pll2)
{
	size_t i;
	size_t count = ARRAY_SIZE(yhgch_pll_table);

	for (i = 0; i < count; i++) {
		if (yhgch_pll_table[i].hdisplay == x &&
		    yhgch_pll_table[i].vdisplay == y) {
			*pll1 = yhgch_pll_table[i].pll1_config_value;
			*pll2 = yhgch_pll_table[i].pll2_config_value;
			return;
		}
	}
}

/*
 * This function takes care the extra registers and bit fields required to
 * setup a mode in board.
 * Explanation about Display Control register:
 * FPGA only supports 7 predefined pixel clocks, and clock select is
 * in bit 4:0 of new register 0x802a8.
 */
static u32 display_ctrl_adjust(struct drm_device *dev,
			       struct drm_display_mode *mode,
			       u32 ctrl)
{
	u64 w, h;
	u32 pll1;		/* bit[31:0] of PLL */
	u32 pll2;		/* bit[63:32] of PLL */
	struct yhgch_drm_private *priv = to_yhgch_drm_private(dev);

	w = mode->hdisplay;
	h = mode->vdisplay;

	get_pll_config(w, h, &pll1, &pll2);
	writel(pll2, priv->mmio + CRT_PLL2_NS);
	set_vclock_yhgch(dev, pll1);

	/*
	 * yhgch has to set up the top-left and bottom-right
	 * registers as well.
	 * Note that normal chip only use those two register for
	 * auto-centering mode.
	 */
	writel(FIELD_PREP(YHGCH_CRT_AUTO_CENTERING_TL_TOP_MASK, 0) |
	       FIELD_PREP(YHGCH_CRT_AUTO_CENTERING_TL_LEFT_MASK, 0),
	       priv->mmio + YHGCH_CRT_AUTO_CENTERING_TL);

	writel(FIELD_PREP(YHGCH_CRT_AUTO_CENTERING_BR_BOTTOM_MASK, h - 1) |
	       FIELD_PREP(YHGCH_CRT_AUTO_CENTERING_BR_RIGHT_MASK, w - 1),
	       priv->mmio + YHGCH_CRT_AUTO_CENTERING_BR);

	/*
	 * Assume common fields in ctrl have been properly set before
	 * calling this function.
	 * This function only sets the extra fields in ctrl.
	 */

	/* Set bit 25 of display controller: Select CRT or VGA clock */
	ctrl &= ~YHGCH_CRT_DISP_CTL_CRTSELECT_MASK;
	ctrl &= ~YHGCH_CRT_DISP_CTL_CLOCK_PHASE_MASK;

	ctrl |= YHGCH_CRT_DISP_CTL_CRTSELECT(YHGCH_CRTSELECT_CRT);

	/* clock_phase_polarity is 0 */
	ctrl |= YHGCH_CRT_DISP_CTL_CLOCK_PHASE(0);
	ctrl |= FIELD_PREP(YHGCH_CRT_DISP_CTL_FORMAT_MASK, 2);

	writel(ctrl, priv->mmio + YHGCH_CRT_DISP_CTL);

	return ctrl;
}

static void yhgch_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	u32 val;
	struct drm_display_mode *mode = &crtc->state->mode;
	struct drm_device *dev = crtc->dev;
	struct yhgch_drm_private *priv = to_yhgch_drm_private(dev);
	u32 width = mode->hsync_end - mode->hsync_start;
	u32 height = mode->vsync_end - mode->vsync_start;

	//writel(format_pll_reg(), priv->mmio + YHGCH_CRT_PLL_CTRL);
	writel(FIELD_PREP(YHGCH_CRT_HORZ_TOTAL_TOTAL_MASK, mode->htotal - 1) |
	       FIELD_PREP(YHGCH_CRT_HORZ_TOTAL_DISP_END_MASK, mode->hdisplay - 1),
	       priv->mmio + YHGCH_CRT_HORZ_TOTAL);

	writel(FIELD_PREP(YHGCH_CRT_HORZ_SYNC_WIDTH_MASK, width) |
	       FIELD_PREP(YHGCH_CRT_HORZ_SYNC_START_MASK, mode->hsync_start - 1),
	       priv->mmio + YHGCH_CRT_HORZ_SYNC);

	writel(FIELD_PREP(YHGCH_CRT_VERT_TOTAL_TOTAL_MASK, mode->vtotal - 1) |
	       FIELD_PREP(YHGCH_CRT_VERT_TOTAL_DISP_END_MASK, mode->vdisplay - 1),
	       priv->mmio + YHGCH_CRT_VERT_TOTAL);

	writel(FIELD_PREP(YHGCH_CRT_VERT_SYNC_HEIGHT_MASK, height) |
	       FIELD_PREP(YHGCH_CRT_VERT_SYNC_START_MASK, mode->vsync_start - 1),
	       priv->mmio + YHGCH_CRT_VERT_SYNC);

	val = FIELD_PREP(YHGCH_CRT_DISP_CTL_VSYNC_PHASE_MASK, 0);
	val |= FIELD_PREP(YHGCH_CRT_DISP_CTL_HSYNC_PHASE_MASK, 0);
	val |= YHGCH_CRT_DISP_CTL_TIMING(1);
	val |= YHGCH_CRT_DISP_CTL_PLANE(1);

	display_ctrl_adjust(dev, mode, val);
}

static const struct drm_crtc_funcs yhgch_crtc_funcs = {
	.page_flip = drm_atomic_helper_page_flip,
	.set_config = drm_atomic_helper_set_config,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs yhgch_crtc_helper_funcs = {
	.mode_set_nofb = yhgch_crtc_mode_set_nofb,
	.atomic_enable = yhgch_crtc_atomic_enable,
	.atomic_disable = yhgch_crtc_atomic_disable,
	.mode_valid = yhgch_crtc_mode_valid,
};

int yhgch_de_init(struct yhgch_drm_private *priv)
{
	struct drm_device *dev = &priv->dev;
	struct drm_crtc *crtc = &priv->crtc;
	struct drm_plane *plane = &priv->primary_plane;
	int ret;

	ret = drm_universal_plane_init(dev, plane, 1, &yhgch_plane_funcs,
				       channel_formats1,
				       ARRAY_SIZE(channel_formats1),
				       NULL,
				       DRM_PLANE_TYPE_PRIMARY,
				       NULL);
	if (ret) {
		drm_err(dev, "failed to init plane: %d\n", ret);
		return ret;
	}

	drm_plane_helper_add(plane, &yhgch_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(plane);

	ret = drm_crtc_init_with_planes(dev, crtc, plane,
					NULL, &yhgch_crtc_funcs, NULL);
	if (ret) {
		drm_err(dev, "failed to init crtc: %d\n", ret);
		return ret;
	}

	drm_crtc_helper_add(crtc, &yhgch_crtc_helper_funcs);

	return 0;
}
