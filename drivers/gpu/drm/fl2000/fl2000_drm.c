// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include <linux/math64.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_connector.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank_helper.h>

#include "fl2000.h"
#include "fl2000_it66121.h"

#define DRM_DRIVER_NAME "fl2000"
#define DRM_DRIVER_DESC "Fresco Logic FL2000 USB display"

#define DRM_DRIVER_MAJOR      0
#define DRM_DRIVER_MINOR      0
#define DRM_DRIVER_PATCHLEVEL 1

/* Maximum supported resolution, no documented hardware limit */
#define FL2000_MAX_WIDTH  4000
#define FL2000_MAX_HEIGHT 4000

/*
 * 32-bit input. The 10-bit formats are accepted because the compositor hands
 * us its scanout buffer as it is: Mutter renders ARGB2101010 and, when the
 * secondary output cannot take that format, falls back to a copy path that
 * has leaked GPU memory until the desktop stalled.
 */
#define FL2000_FB_BPP 32
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_ARGB2101010,
};

/* The cursor sprite is blended into the outgoing frame by the CPU */
static const u32 fl2000_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

/*
 * Maximum pixel clock set to 500MHz. It is hard to get more or less precise PLL configuration for
 * higher clock.
 */
#define FL2000_MAX_PIXCLOCK 500000000

/* PLL computing precision is 6 digits after the decimal point */
#define FL2000_PLL_PRECISION 1000000

/* Input xtal clock, Hz */
#define FL2000_XTAL 10000000 /* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN 62500000 /* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX 1000000000 /* 1 GHz */

/*
 * Maximum acceptable ppm error of the pixel clock. Digital sinks get the exact clock or nothing;
 * an analog monitor locks to the line rate of whatever it is sent, so a clock off by half a
 * percent goes unnoticed, while changing the length of a line (htotal) throws its sampling off.
 */
#define FL2000_PPM_ERR_MAX	500
#define FL2000_PPM_ERR_MAX_VGA	5000

/*
 * Bulk payload the link sustains, bytes per second. High speed carries at most 13 packets of 512
 * bytes per 125 us microframe (53 MB/s); SuperSpeed 500 MB/s after its 8b/10b encoding. Packet and
 * link overhead are taken as 10 %. On SuperSpeed 1920x1080 at 60 Hz in RGB888 (373 MB/s) streams
 * fine, and the vendor driver offers 1920x1200 at 60 Hz in RGB888 (415 MB/s). The FL2000 is a
 * 5 Gb/s device: it never runs at SuperSpeed Plus.
 */
#define FL2000_BULK_BW_HIGH_SPEED  (53248000ull * 90 / 100)
#define FL2000_BULK_BW_SUPER_SPEED (500000000ull * 90 / 100)

/*
 * Bytes per pixel the USB link can carry for @mode: 3 (RGB888), 2 (RGB565) or 0 for neither. The
 * FL2000 has a line buffer and no frame buffer: it takes the active pixels of exactly one frame per
 * refresh, so the stream needs the rate of the active area, not the pixel clock.
 */
static unsigned int fl2000_get_bytes_pix(enum usb_device_speed speed,
					 const struct drm_display_mode *mode)
{
	u64 max_bw, rate;
	u32 total = (u32)mode->htotal * mode->vtotal;
	u64 bytes_pix;

	switch (speed) {
	case USB_SPEED_HIGH:
		max_bw = FL2000_BULK_BW_HIGH_SPEED;
		break;
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		max_bw = FL2000_BULK_BW_SUPER_SPEED;
		break;
	default:
		return 0;
	}

	if (!total)
		return 0;

	/* Active pixels per second; at most 500 MHz x 4000 x 4000, so no overflow */
	rate = div_u64((u64)mode->clock * 1000 * mode->hdisplay * mode->vdisplay, total);
	if (!rate)
		return 0;

	bytes_pix = div64_u64(max_bw, rate);
	if (bytes_pix >= 3)
		return 3; /* RGB888 */
	if (bytes_pix == 2)
		return 2; /* RGB565 */

	/* RGB332 is not supported */
	return 0;
}

struct fl2000_drm_if {
	struct usb_device *usb_dev;
	struct drm_device drm;
	struct drm_plane plane;
	/*
	 * Without a cursor plane of its own the compositor keeps the pointer on
	 * the primary GPU's hardware cursor, where it never reaches this output.
	 */
	struct drm_plane cursor;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct fl2000_vga vga;
	struct fl2000_stream *stream;
	struct fl2000_intr *intr;
	/* Set once userspace can see the device: events before that have nobody to tell */
	bool registered;
};

static inline struct fl2000_drm_if *to_fl2000_drm_if(struct drm_device *drm)
{
	return container_of(drm, struct fl2000_drm_if, drm);
}

DEFINE_DRM_GEM_FOPS(fl2000_drm_driver_fops);

/*
 * Mode config cleanup is managed by drmm_mode_config_init(), and the display
 * pipeline is shut down on unbind, so no .release callback is needed.
 */
static struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &fl2000_drm_driver_fops,

	/*
	 * Paged shmem buffers: the framebuffer is only ever read by the CPU
	 * (fl2000_stream_compress), never DMA'd to the device, so it does not
	 * need to be physically contiguous.
	 */
	.dumb_create = drm_gem_shmem_dumb_create,
	/*
	 * Import with a mapping, not DRM_GEM_SHMEM_DRIVER_OPS' no-map import.
	 * Mapping the attachment makes the exporter pin the buffer where this
	 * device can reach it, system memory for amdgpu. Left unmapped, amdgpu
	 * keeps it in VRAM and vmaps it as one linear range from where it
	 * thinks it starts, which for a buffer split over several VRAM blocks
	 * is right for the last block only: at 1360x768 and 1366x768 all but
	 * the bottom ~10 % of the desktop came out black or garbled.
	 */
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Integer division compute of ppm error */
static u64 fl2000_pll_ppm_err(u64 clock_mil, u32 vco_clk, u32 divisor)
{
	u64 pll_clk_mil = div_u64((u64)vco_clk * FL2000_PLL_PRECISION, divisor);
	u64 pll_clk_err;

	/* Not using abs() here to avoid a possible overflow */
	if (pll_clk_mil > clock_mil)
		pll_clk_err = pll_clk_mil - clock_mil;
	else
		pll_clk_err = clock_mil - pll_clk_mil;

	return div64_u64(pll_clk_err, div_u64(clock_mil, FL2000_PLL_PRECISION));
}

static inline u32 fl2000_pll_get_divisor(u64 clock_mil, u32 vco_clk, u64 *min_ppm_err)
{
	static const u32 divisor_arr[] = {
		2,   4,	  6,   7,   8,	 9,   10,  11,	12,  13,  14,  15,  16,	 17,  18,  19,
		20,  21,  22,  23,  24,	 25,  26,  27,	28,  29,  30,  31,  32,	 33,  34,  35,
		36,  37,  38,  39,  40,	 41,  42,  43,	44,  45,  46,  47,  48,	 49,  50,  51,
		52,  53,  54,  55,  56,	 57,  58,  59,	60,  61,  62,  63,  64,	 65,  66,  67,
		68,  69,  70,  71,  72,	 73,  74,  75,	76,  77,  78,  79,  80,	 81,  82,  83,
		84,  85,  86,  87,  88,	 89,  90,  91,	92,  93,  94,  95,  96,	 97,  98,  99,
		100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
		116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128
	};
	u32 best_divisor = 0;

	for (int i = 0; i < ARRAY_SIZE(divisor_arr); i++) {
		u32 divisor = divisor_arr[i];
		u64 ppm_err = fl2000_pll_ppm_err(clock_mil, vco_clk, divisor);

		if (ppm_err < *min_ppm_err) {
			*min_ppm_err = ppm_err;
			best_divisor = divisor;
		}
	}

	return best_divisor;
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll, u32 *clock_calculated)
{
	static const u32 prescaler_max = 2;
	static const u32 multiplier_max = 128;
	u64 min_ppm_err = (u64)(-1);

	for (u32 prescaler = 1; prescaler <= prescaler_max; prescaler++)
		for (u32 multiplier = 1; multiplier <= multiplier_max; multiplier++) {
			/* Do not need precision here yet, no 10^6 multiply */
			u32 vco_clk = FL2000_XTAL / prescaler * multiplier;
			u32 divisor;

			if (vco_clk < FL2000_VCOCLOCK_MIN || vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			divisor = fl2000_pll_get_divisor(clock_mil, vco_clk, &min_ppm_err);
			if (divisor == 0)
				continue;

			pll->prescaler = prescaler;
			pll->multiplier = multiplier;
			pll->divisor = divisor;
			if (vco_clk < 125000000)
				pll->function = 0;
			else if (vco_clk < 250000000)
				pll->function = 1;
			else if (vco_clk < 500000000)
				pll->function = 2;
			else
				pll->function = 3;
			*clock_calculated = vco_clk / divisor;
		}

	return min_ppm_err;
}

/*
 * Find the PLL setting for a mode, accepting a pixel clock off by up to @max_ppm parts per
 * million. The timings of the mode are never touched, only the clock the PLL can make for it.
 */
static int fl2000_mode_calc(const struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode, struct fl2000_pll *pll,
			    u64 max_ppm)
{
	u32 clock_calculated;
	u64 ppm_err;

	if (mode->clock * 1000 > FL2000_MAX_PIXCLOCK)
		return -EINVAL;

	/* To keep precision use clock multiplied by 10^6 */
	ppm_err = fl2000_pll_calc((u64)mode->clock * 1000 * FL2000_PLL_PRECISION, pll,
				  &clock_calculated);
	if (ppm_err >= max_ppm)
		return -EINVAL;

	if (adjusted_mode) {
		drm_mode_copy(adjusted_mode, mode);
		adjusted_mode->clock = clock_calculated / 1000;
	}

	return 0;
}

static enum drm_mode_status fl2000_crtc_mode_valid(struct drm_crtc *crtc,
						   const struct drm_display_mode *mode)
{
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(crtc->dev);
	struct drm_display_mode adjusted_mode;
	struct fl2000_pll pll;

	/* Get PLL configuration and check if mode adjustments are needed */
	if (fl2000_mode_calc(mode, &adjusted_mode, &pll, FL2000_PPM_ERR_MAX_VGA))
		return MODE_BAD;

	if (fl2000_get_bytes_pix(drm_if->usb_dev->speed, &adjusted_mode) == 0)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

/*
 * Primary plane
 */

static const u64 fl2000_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static int fl2000_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *new_crtc_state = NULL;
	bool is_cursor = plane->type == DRM_PLANE_TYPE_CURSOR;

	/* Clipping a larger sprite would put a truncated pointer on screen */
	if (is_cursor && new_plane_state->fb &&
	    (new_plane_state->fb->width > FL2000_CURSOR_MAX ||
	     new_plane_state->fb->height > FL2000_CURSOR_MAX))
		return -EINVAL;

	if (new_plane_state->crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_plane_state->crtc);

	/* The cursor may be placed anywhere; the primary plane is the frame */
	return drm_atomic_helper_check_plane_state(new_plane_state, new_crtc_state,
						   DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						   is_cursor, false);
}

/*
 * Only the damaged rectangle is converted, into the stream's shadow frame,
 * which is the one persistent copy every outgoing buffer is built from.
 */
static void fl2000_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(plane->dev);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_rect damage;
	ktime_t start;
	int idx;

	if (!fb)
		return; /* plane is disabled */

	if (!drm_atomic_helper_damage_merged(old_plane_state, plane_state, &damage))
		return;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return;

	if (drm_dev_enter(plane->dev, &idx)) {
		start = ktime_get();
		fl2000_stream_compress(drm_if->stream, &shadow_plane_state->data[0],
				       drm_gem_is_imported(fb->obj[0]), fb->format->format,
				       fb->width, fb->height, fb->pitches[0], &damage);
		fl2000_stream_note_render(drm_if->stream,
					  (unsigned int)ktime_us_delta(ktime_get(), start),
					  drm_rect_width(&damage) * drm_rect_height(&damage));
		drm_dev_exit(idx);
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

/*
 * Hand the cursor sprite to the stream, which blends it into every outgoing
 * frame. Moving the pointer keeps the same framebuffer and skips the copy.
 */
static void fl2000_cursor_atomic_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(plane->dev);
	struct drm_framebuffer *fb = plane_state->fb;
	unsigned int w, h;
	u32 *sprite;
	int idx;

	if (!fb || !plane_state->visible) {
		fl2000_stream_set_cursor(drm_if->stream, NULL, 0, 0, 0, 0);
		return;
	}

	if (fb == old_plane_state->fb && old_plane_state->visible) {
		fl2000_stream_move_cursor(drm_if->stream, plane_state->crtc_x, plane_state->crtc_y);
		return;
	}

	/* Size is bounded by the atomic check */
	w = fb->width;
	h = fb->height;

	/* Repack into a tightly packed sprite: the framebuffer has its own pitch */
	sprite = kmalloc_objs(*sprite, (size_t)w * h);
	if (!sprite)
		return;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto out_free;

	if (drm_dev_enter(plane->dev, &idx)) {
		for (unsigned int y = 0; y < h; y++)
			iosys_map_memcpy_from(sprite + (size_t)y * w, &shadow_plane_state->data[0],
					      (size_t)y * fb->pitches[0], (size_t)w * sizeof(u32));

		fl2000_stream_set_cursor(drm_if->stream, sprite, w, h, plane_state->crtc_x,
					 plane_state->crtc_y);
		drm_dev_exit(idx);
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
out_free:
	kfree(sprite);
}

static const struct drm_plane_helper_funcs fl2000_cursor_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = fl2000_plane_atomic_check,
	.atomic_update = fl2000_cursor_atomic_update,
};

static const struct drm_plane_helper_funcs fl2000_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = fl2000_plane_atomic_check,
	.atomic_update = fl2000_plane_atomic_update,
};

static const struct drm_plane_funcs fl2000_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/*
 * CRTC
 */

static int fl2000_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(crtc->dev);
	struct fl2000_pll pll;
	int ret;

	ret = drm_crtc_helper_atomic_check(crtc, state);
	if (ret)
		return ret;

	if (!crtc_state->enable || !drm_atomic_crtc_needs_modeset(crtc_state))
		return 0;

	/* PLL configuration, with htotal and clock adjusted to what the PLL can produce */
	ret = fl2000_mode_calc(&crtc_state->mode, &crtc_state->adjusted_mode, &pll,
			       FL2000_PPM_ERR_MAX_VGA);
	if (ret)
		return ret;

	/* The USB link must be able to carry the adjusted pixel clock */
	if (!fl2000_get_bytes_pix(drm_if->usb_dev->speed, &crtc_state->adjusted_mode))
		return -EINVAL;

	return 0;
}

/*
 * HDMI has always run with the monitor sense forced, so it stays so whenever HDMI is driven.
 * Driving VGA alone leaves it to the chip, which then powers the DAC by itself and notices the
 * cable being unplugged.
 */
static void fl2000_crtc_force_connect(struct fl2000_drm_if *drm_if,
				      const struct drm_crtc_state *crtc_state)
{
	fl2000_force_connect(drm_if->usb_dev,
			     crtc_state->encoder_mask & drm_encoder_mask(&drm_if->encoder));
}

/* Runs before the bridge is enabled: the IT66121 is programmed with the FL2000 clocking it */
static void fl2000_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct drm_device *drm = crtc->dev;
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(drm);
	struct usb_device *usb_dev = drm_if->usb_dev;
	const struct drm_display_mode *mode = &crtc->state->mode;
	const struct drm_display_mode *adjusted_mode = &crtc->state->adjusted_mode;
	struct fl2000_timings timings;
	struct fl2000_pll pll;
	unsigned int bytes_pix;

	/*
	 * Same computation as in atomic_check, which already validated the mode
	 * and stored the adjusted htotal and clock in adjusted_mode.
	 */
	if (WARN_ON(fl2000_mode_calc(mode, NULL, &pll, FL2000_PPM_ERR_MAX_VGA)))
		return;
	bytes_pix = fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode);

	drm_dbg_kms(drm, "Mode requested:  " DRM_MODE_FMT "\n", DRM_MODE_ARG(mode));
	drm_dbg_kms(drm, "Mode configured: " DRM_MODE_FMT "\n", DRM_MODE_ARG(adjusted_mode));

	/* Prepare timing configuration */
	timings.hactive = adjusted_mode->hdisplay;
	timings.htotal = adjusted_mode->htotal;
	timings.hsync_width = adjusted_mode->hsync_end - adjusted_mode->hsync_start;
	timings.hstart = adjusted_mode->htotal - adjusted_mode->hsync_start + 1;
	timings.vactive = adjusted_mode->vdisplay;
	timings.vtotal = adjusted_mode->vtotal;
	timings.vsync_width = adjusted_mode->vsync_end - adjusted_mode->vsync_start;
	timings.vstart = adjusted_mode->vtotal - adjusted_mode->vsync_start + 1;

	/* Set PLL settings */
	fl2000_set_pll(usb_dev, &pll);

	fl2000_crtc_force_connect(drm_if, crtc->state);

	/* Reset FL2000 and confirm PLL settings */
	fl2000_reset(usb_dev);

	/* Set timings settings */
	fl2000_set_timings(usb_dev, &timings);

	/* Pixel format according to number of bytes per pixel */
	fl2000_set_pixfmt(usb_dev, bytes_pix);

	/* Configure frame transfers */
	fl2000_set_transfers(usb_dev);

	/* Enable interrupts */
	fl2000_enable_interrupts(usb_dev);

	fl2000_afe_magic(usb_dev);

	fl2000_stream_mode_set(drm_if->stream, mode->hdisplay, mode->vdisplay, bytes_pix);
}

static void fl2000_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(crtc->dev);

	/* mode_set_nofb does not run when only the connector changed, and disable cleared it */
	fl2000_crtc_force_connect(drm_if, drm_atomic_get_new_crtc_state(state, crtc));

	fl2000_stream_enable(drm_if->stream);

	drm_crtc_vblank_atomic_enable(crtc, state);
}

static void fl2000_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(crtc->dev);

	fl2000_stream_disable(drm_if->stream);

	/* Nothing is shown any more: let the chip's own sense report the monitors again */
	fl2000_force_connect(drm_if->usb_dev, false);

	drm_crtc_vblank_atomic_disable(crtc, state);
}

static const struct drm_crtc_helper_funcs fl2000_crtc_helper_funcs = {
	.mode_valid = fl2000_crtc_mode_valid,
	.atomic_check = fl2000_crtc_atomic_check,
	.mode_set_nofb = fl2000_crtc_mode_set_nofb,
	.atomic_flush = drm_crtc_vblank_atomic_flush,
	.atomic_enable = fl2000_crtc_atomic_enable,
	.atomic_disable = fl2000_crtc_atomic_disable,
};

static const struct drm_crtc_funcs fl2000_crtc_funcs = {
	.atomic_create_state = drm_atomic_helper_crtc_create_state,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	/*
	 * The FL2000 has no vblank interrupt: vblank events come from a timer
	 * running at the mode's refresh rate.
	 */
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

/*
 * Digital sinks are only offered the modes whose pixel clock the PLL hits closely: they expect the
 * exact clock of the mode they advertised. The VGA output accepts a larger error.
 */
static enum drm_mode_status fl2000_hdmi_encoder_mode_valid(struct drm_encoder *encoder,
							   const struct drm_display_mode *mode)
{
	struct fl2000_pll pll;

	return fl2000_mode_calc(mode, NULL, &pll, FL2000_PPM_ERR_MAX) ? MODE_NOCLOCK : MODE_OK;
}

static const struct drm_encoder_helper_funcs fl2000_hdmi_encoder_helper_funcs = {
	.mode_valid = fl2000_hdmi_encoder_mode_valid,
};

/**
 * fl2000_drm_hotplug() - tell userspace that a monitor came, went or changed
 * @drm: DRM device
 * @events: the sink event bits of the status register that were seen
 *
 * Called from the interrupt work.
 */
void fl2000_drm_hotplug(struct drm_device *drm, u32 events)
{
	struct fl2000_drm_if *drm_if = to_fl2000_drm_if(drm);
	int idx;

	/* The device sends uevents only once it has been registered */
	if (!smp_load_acquire(&drm_if->registered) || !drm_dev_enter(drm, &idx))
		return;

	if (events & (FL2000_VGA_STATUS_MONITOR_EVENT | FL2000_VGA_STATUS_EDID_EVENT))
		fl2000_vga_sink_event(&drm_if->vga);

	drm_kms_helper_hotplug_event(drm);

	drm_dev_exit(idx);
}

static void fl2000_drm_if_release(struct device *dev, void *res)
{
	struct fl2000_drm_if *drm_if = *(struct fl2000_drm_if **)res;
	struct drm_device *drm = &drm_if->drm;
	struct usb_device *usb_dev = drm_if->usb_dev;

	dev_dbg(dev, "Unbinding FL2000 master\n");

	/* No hotplug events for a node that is about to disappear */
	fl2000_intr_quiesce(drm_if->intr);
	drm_kms_helper_poll_fini(drm);

	/*
	 * Block userspace access and shut down the display pipeline while all
	 * the components are still bound.
	 */
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	/*
	 * Detach bridge component. This release function runs with the global
	 * component mutex held (component_master_del() -> master unbind ->
	 * devres_release()), so it must never call component_del() - that would
	 * self-deadlock. The IT66121 is destroyed by a usb_device devres instead.
	 */
	component_unbind_all(dev, drm);

	/* Stop streaming interface */
	fl2000_stream_destroy(usb_dev);

	/* Stop interrupts interface */
	fl2000_intr_destroy(usb_dev);

	/*
	 * Drop the driver's reference. The DRM device is freed when the last
	 * userspace file is closed; it only pins the USB device, never the I2C
	 * adapter, so the disconnect does not wait for userspace.
	 */
	drm_dev_put(drm);
}

/* The DRM device while the master is bound, NULL otherwise */
static struct fl2000_drm_if *fl2000_drm_if_of(struct device *master)
{
	struct fl2000_drm_if **holder = devres_find(master, fl2000_drm_if_release, NULL, NULL);

	return holder ? *holder : NULL;
}

/**
 * fl2000_drm_suspend() - stop the display for system sleep
 * @master: the component master
 *
 * Called once for the device, before any of its interfaces is suspended.
 *
 * Return: 0 on success, a negative error code otherwise, which aborts the sleep
 */
int fl2000_drm_suspend(struct device *master)
{
	struct fl2000_drm_if *drm_if = fl2000_drm_if_of(master);
	struct drm_bridge *bridge;
	int ret;

	if (!drm_if)
		return 0;

	/* No hotplug events while going to sleep */
	fl2000_intr_suspend(drm_if->intr);

	/* Saves the state, turns the outputs off and stops the stream */
	ret = drm_mode_config_helper_suspend(&drm_if->drm);
	if (ret) {
		fl2000_intr_resume(drm_if->intr);
		return ret;
	}

	bridge = drm_bridge_chain_get_first_bridge(&drm_if->encoder);
	if (bridge) {
		it66121_suspend(bridge);
		drm_bridge_put(bridge);
	}

	return 0;
}

/**
 * fl2000_drm_resume() - bring the display back after system sleep
 * @master: the component master
 *
 * Called once for the device, when all of it is awake again, also after a reset-resume. The chips
 * may have lost power: they are set up as at bind time before the saved state is restored.
 *
 * Return: 0 on success, a negative error code otherwise
 */
int fl2000_drm_resume(struct device *master)
{
	struct fl2000_drm_if *drm_if = fl2000_drm_if_of(master);
	struct usb_device *usb_dev;
	struct drm_bridge *bridge;
	int ret;

	if (!drm_if)
		return 0;
	usb_dev = drm_if->usb_dev;

	/* First, as the IT66121 is reached through the FL2000 I2C engine */
	fl2000_regmap_drop_cache(usb_dev);

	bridge = drm_bridge_chain_get_first_bridge(&drm_if->encoder);
	if (bridge) {
		it66121_resume(bridge);
		drm_bridge_put(bridge);
	}

	/* The same order as in fl2000_drm_bind() */
	fl2000_reset(usb_dev);
	fl2000_usb_magic(usb_dev);

	/* Without interrupts the connectors are still polled */
	fl2000_intr_resume(drm_if->intr);

	/* Another monitor may have been plugged in meanwhile */
	fl2000_vga_sink_event(&drm_if->vga);

	ret = drm_mode_config_helper_resume(&drm_if->drm);
	if (ret)
		drm_err(&drm_if->drm, "Cannot restore the display after sleep (%d)\n", ret);

	drm_kms_helper_hotplug_event(&drm_if->drm);

	return ret;
}

int fl2000_drm_bind(struct device *master)
{
	int ret = 0;
	struct usb_device *usb_dev = to_usb_device(master->parent);
	struct fl2000_drm_if *drm_if;
	struct fl2000_drm_if **holder;
	struct drm_device *drm;
	struct drm_mode_config *mode_config;
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector;

	dev_dbg(master, "Binding FL2000 master\n");

	/*
	 * The DRM device is a child of the USB device, not of the I2C adapter
	 * that is the component master. It holds a reference on its parent, and
	 * i2c_del_adapter() (run when the USB device is unbound) waits for every
	 * reference on the adapter: parenting it there deadlocks the hub worker
	 * on unplug and no device enumerates again. The put is explicit, in
	 * fl2000_drm_if_release().
	 */
	drm_if = __drm_dev_alloc(&usb_dev->dev, &fl2000_drm_driver, sizeof(*drm_if),
				 offsetof(struct fl2000_drm_if, drm));
	if (IS_ERR(drm_if)) {
		dev_err(master, "Cannot allocate DRM structure (%pe)\n", drm_if);
		return PTR_ERR(drm_if);
	}
	drm = &drm_if->drm;
	drm_if->usb_dev = usb_dev;

	/* A USB device cannot DMA by itself: dma-buf imports go through its host controller */
	drm_dev_set_dma_dev(drm, usb_dev->bus->sysdev);

	/*
	 * Devres that fl2000_drm_unbind() releases to tear everything down,
	 * registered once binding succeeded.
	 */
	holder = devres_alloc(fl2000_drm_if_release, sizeof(*holder), GFP_KERNEL);
	if (!holder) {
		drm_dev_put(drm);
		return -ENOMEM;
	}

	ret = drmm_mode_config_init(drm);
	if (ret) {
		drm_err(drm, "Cannot initialize DRM mode (%d)\n", ret);
		goto err_free_holder;
	}

	mode_config = &drm->mode_config;
	mode_config->funcs = &fl2000_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = FL2000_MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = FL2000_MAX_HEIGHT;
	/* Userspace sizes its sprite to this; a larger one is rejected */
	mode_config->cursor_width = FL2000_CURSOR_MAX;
	mode_config->cursor_height = FL2000_CURSOR_MAX;

	ret = drm_universal_plane_init(drm, &drm_if->plane, 0, &fl2000_plane_funcs,
				       fl2000_pixel_formats, ARRAY_SIZE(fl2000_pixel_formats),
				       fl2000_plane_format_modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		drm_err(drm, "Cannot initialize plane (%d)\n", ret);
		goto err_free_holder;
	}
	drm_plane_helper_add(&drm_if->plane, &fl2000_plane_helper_funcs);

	/*
	 * Enable the damage-clips property the update path relies on
	 * (drm_atomic_helper_damage_merged()).
	 */
	drm_plane_enable_fb_damage_clips(&drm_if->plane);

	ret = drm_universal_plane_init(drm, &drm_if->cursor, 0, &fl2000_plane_funcs,
				       fl2000_cursor_formats, ARRAY_SIZE(fl2000_cursor_formats),
				       fl2000_plane_format_modifiers, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret) {
		drm_err(drm, "Cannot initialize cursor plane (%d)\n", ret);
		goto err_free_holder;
	}
	drm_plane_helper_add(&drm_if->cursor, &fl2000_cursor_helper_funcs);

	ret = drmm_crtc_init_with_planes(drm, &drm_if->crtc, &drm_if->plane, &drm_if->cursor,
					 &fl2000_crtc_funcs, NULL);
	if (ret) {
		drm_err(drm, "Cannot initialize CRTC (%d)\n", ret);
		goto err_free_holder;
	}
	drm_crtc_helper_add(&drm_if->crtc, &fl2000_crtc_helper_funcs);
	drm_if->plane.possible_crtcs = drm_crtc_mask(&drm_if->crtc);
	drm_if->cursor.possible_crtcs = drm_crtc_mask(&drm_if->crtc);

	/* The FL2000 is programmed from the CRTC; the IT66121 bridge hangs off this encoder */
	ret = drmm_encoder_init(drm, &drm_if->encoder, NULL, DRM_MODE_ENCODER_NONE, NULL);
	if (ret) {
		drm_err(drm, "Cannot initialize encoder (%d)\n", ret);
		goto err_free_holder;
	}
	drm_if->encoder.possible_crtcs = drm_crtc_mask(&drm_if->crtc);
	drm_encoder_helper_add(&drm_if->encoder, &fl2000_hdmi_encoder_helper_funcs);

	/* Start streaming interface */
	drm_if->stream = fl2000_stream_create(usb_dev);
	if (IS_ERR(drm_if->stream)) {
		ret = PTR_ERR(drm_if->stream);
		drm_err(drm, "Cannot create stream (%d)\n", ret);
		goto err_free_holder;
	}

	/* Start interrupts interface */
	drm_if->intr = fl2000_intr_create(usb_dev, drm);
	if (IS_ERR(drm_if->intr)) {
		ret = PTR_ERR(drm_if->intr);
		drm_err(drm, "Cannot start interrupt polling (%d)\n", ret);
		goto err_destroy_stream;
	}

	/* Attach bridge */
	ret = component_bind_all(master, drm);
	if (ret) {
		drm_err(drm, "Cannot attach bridge (%d)\n", ret);
		goto err_destroy_intr;
	}

	/*
	 * The HDMI connector is the generic one of the bridge chain: the IT66121 attached without
	 * a connector of its own and reports the hot plug line and the EDID.
	 */
	connector = drm_bridge_connector_init(drm, &drm_if->encoder);
	if (IS_ERR(connector)) {
		ret = PTR_ERR(connector);
		drm_err(drm, "Cannot initialize HDMI connector (%d)\n", ret);
		goto err_unbind;
	}

	/*
	 * The VGA output goes in after the bridge: the IT66121 attaches to the first encoder of
	 * the device, which has to be the HDMI one.
	 */
	ret = fl2000_vga_init(drm, &drm_if->vga, &drm_if->crtc, &drm_if->encoder,
			      i2c_verify_adapter(master), drm_if->intr);
	if (ret) {
		drm_err(drm, "Cannot initialize VGA output (%d)\n", ret);
		goto err_unbind;
	}

	drm_mode_config_reset(drm);

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret) {
		drm_err(drm, "Failed to initialize %d VBLANK(s) (%d)\n",
			drm->mode_config.num_crtc, ret);
		goto err_unbind;
	}

	fl2000_reset(usb_dev);
	fl2000_usb_magic(usb_dev);

	/*
	 * Probe the connector before userspace can see the device. A compositor
	 * that finds a new GPU reads the cached connector state without probing
	 * it, and does not look again when the hotplug event follows; with an
	 * unprobed connector it never enables the output until something else
	 * makes it reload.
	 */
	mutex_lock(&drm->mode_config.mutex);
	drm_connector_list_iter_begin(drm, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter)
		drm_helper_probe_single_connector_modes(connector, FL2000_MAX_WIDTH,
							FL2000_MAX_HEIGHT);
	drm_connector_list_iter_end(&conn_iter);
	mutex_unlock(&drm->mode_config.mutex);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		drm_err(drm, "Cannot register DRM device (%d)\n", ret);
		goto err_unbind;
	}

	*holder = drm_if;
	devres_add(master, holder);

	/* From here on the interrupt work may tell userspace about monitors */
	smp_store_release(&drm_if->registered, true);

	/* Polls both connectors: events can be missed, and HDMI has no hot plug of its own */
	drm_kms_helper_poll_init(drm);

	/*
	 * fbdev emulation, so the console can use the display. Last, because it
	 * may set a mode right away and the reset above would undo it.
	 */
	drm_client_setup(drm, NULL);

	return 0;

err_unbind:
	component_unbind_all(master, drm);
err_destroy_intr:
	fl2000_intr_destroy(usb_dev);
err_destroy_stream:
	fl2000_stream_destroy(usb_dev);
err_free_holder:
	devres_free(holder);
	drm_dev_put(drm);
	return ret;
}

void fl2000_drm_unbind(struct device *master)
{
	devres_release(master, fl2000_drm_if_release, NULL, NULL);
}
