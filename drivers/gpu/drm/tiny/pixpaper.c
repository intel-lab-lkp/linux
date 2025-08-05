// SPDX-License-Identifier: GPL-2.0
/*
 * DRM driver for PIXPAPER e-ink panel
 *
 * Author: LiangCheng Wang <zaq14760@gmail.com>,
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/spi/spi.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>

#include "pixpaper-regs.h"

MODULE_IMPORT_NS("DMA_BUF");

/*
 * The panel has a visible resolution of 122x250.
 * However, the controller requires the horizontal resolution to be aligned to 128 pixels.
 * No porch or sync timing values are provided in the datasheet, so we define minimal
 * placeholder values to satisfy the DRM framework.
 */

/* Panel visible resolution */
#define PIXPAPER_WIDTH           122
#define PIXPAPER_HEIGHT          250

/* Controller requires 128 horizontal pixels total (for memory alignment) */
#define PIXPAPER_HTOTAL          128
#define PIXPAPER_HFP             2
#define PIXPAPER_HSYNC           2
#define PIXPAPER_HBP             (PIXPAPER_HTOTAL - PIXPAPER_WIDTH - PIXPAPER_HFP - PIXPAPER_HSYNC)

/*
 * According to the datasheet, the total vertical blanking must be 55 lines,
 * regardless of how the vertical back porch is set.
 * Here we allocate VFP=2, VSYNC=2, and VBP=51 to sum up to 55 lines.
 * Total vertical lines = 250 (visible) + 55 (blanking) = 305.
 */
#define PIXPAPER_VTOTAL  (250 + 55)
#define PIXPAPER_VFP     2
#define PIXPAPER_VSYNC   2
#define PIXPAPER_VBP     (55 - PIXPAPER_VFP - PIXPAPER_VSYNC)

/*
 * Pixel clock calculation:
 * pixel_clock = htotal * vtotal * refresh_rate
 *             = 128 * 305 * 50
 *             = 1,952,000 Hz = 1952 kHz
 */
#define PIXPAPER_PIXEL_CLOCK     1952

#define PIXPAPER_WIDTH_MM        24    /* approximate from 23.7046mm */
#define PIXPAPER_HEIGHT_MM       49    /* approximate from 48.55mm */

#define PIXPAPER_SPI_BITS_PER_WORD	8
#define PIXPAPER_SPI_SPEED_DEFAULT      1000000

#define PIXPAPER_PANEL_BUFFER_WIDTH	128
#define PIXPAPER_PANEL_BUFFER_TWO_BYTES_PER_ROW (PIXPAPER_PANEL_BUFFER_WIDTH / 4)

#define PIXPAPER_COLOR_THRESHOLD_LOW_CHANNEL		60
#define PIXPAPER_COLOR_THRESHOLD_HIGH_CHANNEL		200
#define PIXPAPER_COLOR_THRESHOLD_YELLOW_MIN_GREEN	180

struct pixpaper_error_ctx {
	int errno_code;
};

struct pixpaper_panel {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct spi_device *spi;
	struct gpio_desc *reset;
	struct gpio_desc *busy;
	struct gpio_desc *dc;
};

static inline struct pixpaper_panel *to_pixpaper_panel(struct drm_device *drm)
{
	return container_of(drm, struct pixpaper_panel, drm);
}

static void pixpaper_wait_busy(struct pixpaper_panel *panel)
{
	unsigned int timeout_ms = 10000;
	unsigned long timeout_jiffies = jiffies + msecs_to_jiffies(timeout_ms);

	usleep_range(1000, 1500);
	while (gpiod_get_value_cansleep(panel->busy) != 1) {
		if (time_after(jiffies, timeout_jiffies)) {
			drm_warn(&panel->drm, "Busy wait timed out\n");
			return;
		}
		usleep_range(100, 200);
	}
}

static void pixpaper_spi_sync(struct spi_device *spi, struct spi_message *msg,
			      struct pixpaper_error_ctx *err)
{
	if (err->errno_code)
		return;

	int ret = spi_sync(spi, msg);

	if (ret < 0)
		err->errno_code = ret;
}

static void pixpaper_send_cmd(struct pixpaper_panel *panel, u8 cmd,
			      struct pixpaper_error_ctx *err)
{
	if (err->errno_code)
		return;

	struct spi_transfer xfer = {
		.tx_buf = &cmd,
		.len = 1,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	gpiod_set_value_cansleep(panel->dc, 0);
	usleep_range(1, 5);
	pixpaper_spi_sync(panel->spi, &msg, err);
}

static void pixpaper_send_data(struct pixpaper_panel *panel, u8 data,
			       struct pixpaper_error_ctx *err)
{
	if (err->errno_code)
		return;

	struct spi_transfer xfer = {
		.tx_buf = &data,
		.len = 1,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	gpiod_set_value_cansleep(panel->dc, 1);
	usleep_range(1, 5);
	pixpaper_spi_sync(panel->spi, &msg, err);
}

static int pixpaper_panel_hw_init(struct pixpaper_panel *panel)
{
	struct pixpaper_error_ctx err = { .errno_code = 0 };

	gpiod_set_value_cansleep(panel->reset, 0);
	msleep(50);
	gpiod_set_value_cansleep(panel->reset, 1);
	msleep(50);

	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_UNKNOWN_4D, &err);
	pixpaper_send_data(panel, PIXPAPER_UNKNOWN_4D_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_PANEL_SETTING, &err);
	pixpaper_send_data(panel, PIXPAPER_PSR_CONFIG, &err);
	pixpaper_send_data(panel, PIXPAPER_PSR_CONFIG2, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_POWER_SETTING, &err);
	pixpaper_send_data(panel, PIXPAPER_PWR_CONFIG1, &err);
	pixpaper_send_data(panel, PIXPAPER_PWR_CONFIG2, &err);
	pixpaper_send_data(panel, PIXPAPER_PWR_VSP_8_2V, &err);
	pixpaper_send_data(panel, PIXPAPER_PWR_VSPL_15V, &err);
	pixpaper_send_data(panel, PIXPAPER_PWR_VSN_4V, &err);
	pixpaper_send_data(panel, PIXPAPER_PWR_VSP_8_2V, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_POWER_OFF_SEQUENCE, &err);
	pixpaper_send_data(panel, PIXPAPER_PFS_CONFIG1, &err);
	pixpaper_send_data(panel, PIXPAPER_PFS_CONFIG2, &err);
	pixpaper_send_data(panel, PIXPAPER_PFS_CONFIG3, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_BOOSTER_SOFT_START, &err);
	pixpaper_send_data(panel, PIXPAPER_BTST_CONFIG1, &err);
	pixpaper_send_data(panel, PIXPAPER_BTST_CONFIG2, &err);
	pixpaper_send_data(panel, PIXPAPER_BTST_CONFIG3, &err);
	pixpaper_send_data(panel, PIXPAPER_BTST_CONFIG4, &err);
	pixpaper_send_data(panel, PIXPAPER_BTST_CONFIG5, &err);
	pixpaper_send_data(panel, PIXPAPER_BTST_CONFIG6, &err);
	pixpaper_send_data(panel, PIXPAPER_BTST_CONFIG7, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_PLL_CONTROL, &err);
	pixpaper_send_data(panel, PIXPAPER_PLL_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_TEMP_SENSOR_CALIB, &err);
	pixpaper_send_data(panel, PIXPAPER_TSE_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_VCOM_INTERVAL, &err);
	pixpaper_send_data(panel, PIXPAPER_CDI_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_UNKNOWN_60, &err);
	pixpaper_send_data(panel, PIXPAPER_UNKNOWN_60_CONFIG1, &err);
	pixpaper_send_data(panel, PIXPAPER_UNKNOWN_60_CONFIG2, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_RESOLUTION_SETTING, &err);
	pixpaper_send_data(panel, PIXPAPER_TRES_HRES_H, &err);
	pixpaper_send_data(panel, PIXPAPER_TRES_HRES_L, &err);
	pixpaper_send_data(panel, PIXPAPER_TRES_VRES_H, &err);
	pixpaper_send_data(panel, PIXPAPER_TRES_VRES_L, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_GATE_SOURCE_START, &err);
	pixpaper_send_data(panel, PIXPAPER_GSST_S_START, &err);
	pixpaper_send_data(panel, PIXPAPER_GSST_RESERVED, &err);
	pixpaper_send_data(panel, PIXPAPER_GSST_G_START_H, &err);
	pixpaper_send_data(panel, PIXPAPER_GSST_G_START_L, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_UNKNOWN_E7, &err);
	pixpaper_send_data(panel, PIXPAPER_UNKNOWN_E7_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_POWER_SAVING, &err);
	pixpaper_send_data(panel, PIXPAPER_PWS_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_UNKNOWN_E0, &err);
	pixpaper_send_data(panel, PIXPAPER_UNKNOWN_E0_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_UNKNOWN_B4, &err);
	pixpaper_send_data(panel, PIXPAPER_UNKNOWN_B4_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_UNKNOWN_B5, &err);
	pixpaper_send_data(panel, PIXPAPER_UNKNOWN_B5_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_UNKNOWN_E9, &err);
	pixpaper_send_data(panel, PIXPAPER_UNKNOWN_E9_CONFIG, &err);
	if (err.errno_code)
		goto init_fail;
	pixpaper_wait_busy(panel);

	return 0;

init_fail:
	drm_err(&panel->drm, "Hardware initialization failed (err=%d)\n",
		err.errno_code);
	return err.errno_code;
}

/*
 * Convert framebuffer pixels to 2-bit e-paper format:
 *   00 - White
 *   01 - Black
 *   10 - Yellow
 *   11 - Red
 */
static u8 pack_pixels_to_byte(u32 *src_pixels, int i, int j,
			      struct drm_framebuffer *fb)
{
	u8 packed_byte = 0;
	int k;

	for (k = 0; k < 4; k++) {
		int current_pixel_x = j * 4 + k;
		u8 two_bit_val;

		if (current_pixel_x < PIXPAPER_WIDTH) {
			u32 pixel_offset =
				(i * (fb->pitches[0] / 4)) + current_pixel_x;
			u32 pixel = src_pixels[pixel_offset];
			u32 r = (pixel >> 16) & 0xFF;
			u32 g = (pixel >> 8) & 0xFF;
			u32 b = pixel & 0xFF;

			if (r < PIXPAPER_COLOR_THRESHOLD_LOW_CHANNEL &&
			    g < PIXPAPER_COLOR_THRESHOLD_LOW_CHANNEL &&
			    b < PIXPAPER_COLOR_THRESHOLD_LOW_CHANNEL) {
				two_bit_val = 0b00;
			} else if (r > PIXPAPER_COLOR_THRESHOLD_HIGH_CHANNEL &&
				   g > PIXPAPER_COLOR_THRESHOLD_HIGH_CHANNEL &&
				   b > PIXPAPER_COLOR_THRESHOLD_HIGH_CHANNEL) {
				two_bit_val = 0b01;
			} else if (r > PIXPAPER_COLOR_THRESHOLD_HIGH_CHANNEL &&
				   g < PIXPAPER_COLOR_THRESHOLD_LOW_CHANNEL &&
				   b < PIXPAPER_COLOR_THRESHOLD_LOW_CHANNEL) {
				two_bit_val = 0b11;
			} else if (r > PIXPAPER_COLOR_THRESHOLD_HIGH_CHANNEL &&
				   g > PIXPAPER_COLOR_THRESHOLD_YELLOW_MIN_GREEN &&
				   b < PIXPAPER_COLOR_THRESHOLD_LOW_CHANNEL) {
				two_bit_val = 0b10;
			} else {
				two_bit_val = 0b01;
			}
		} else {
			two_bit_val = 0b01;
		}

		packed_byte |= two_bit_val << ((3 - k) * 2);
	}

	return packed_byte;
}

static int pixpaper_plane_helper_atomic_check(struct drm_plane *plane,
					      struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *new_crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state = NULL;
	int ret;

	if (new_crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state,
						  new_crtc_state, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	if (ret)
		return ret;
	else if (!new_plane_state->visible)
		return 0;

	return 0;
}

static int pixpaper_crtc_helper_atomic_check(struct drm_crtc *crtc,
					     struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);

	if (!crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(crtc_state);
}

static void pixpaper_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	struct pixpaper_panel *panel = to_pixpaper_panel(crtc->dev);
	struct drm_device *drm = &panel->drm;
	int idx;
	struct pixpaper_error_ctx err = { .errno_code = 0 };

	if (!drm_dev_enter(drm, &idx))
		return;

	pixpaper_send_cmd(panel, PIXPAPER_CMD_POWER_ON, &err);
	if (err.errno_code) {
		drm_err(drm, "Failed to send PON command: %d\n",
			err.errno_code);
		goto exit_drm_dev;
	}

	pixpaper_wait_busy(panel);

	drm_dbg(drm, "Panel enabled and powered on\n");

exit_drm_dev:
	drm_dev_exit(idx);
}

static void pixpaper_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_state *state)
{
	struct pixpaper_panel *panel = to_pixpaper_panel(crtc->dev);
	struct drm_device *drm = &panel->drm;
	struct pixpaper_error_ctx err = { .errno_code = 0 };
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;

	pixpaper_send_cmd(panel, PIXPAPER_CMD_POWER_OFF, &err);
	if (err.errno_code) {
		drm_err(drm, "Failed to send POF command: %d\n",
			err.errno_code);
		goto exit_drm_dev;
	}
	pixpaper_wait_busy(panel);

	drm_dbg(drm, "Panel disabled\n");

exit_drm_dev:
	drm_dev_exit(idx);
}

static void pixpaper_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(plane_state);
	struct drm_crtc *crtc = plane_state->crtc;
	struct pixpaper_panel *panel = to_pixpaper_panel(crtc->dev);

	struct drm_device *drm = &panel->drm;
	struct drm_framebuffer *fb = plane_state->fb;
	struct iosys_map map = shadow_plane_state->data[0];
	void *vaddr = map.vaddr;
	int i, j, idx;
	u32 *src_pixels = NULL;
	struct pixpaper_error_ctx err = { .errno_code = 0 };

	if (!drm_dev_enter(drm, &idx))
		return;

	drm_dbg(drm, "Starting frame update (phys=%dx%d, buf_w=%d)\n",
		PIXPAPER_WIDTH, PIXPAPER_HEIGHT, PIXPAPER_PANEL_BUFFER_WIDTH);

	if (!fb || !plane_state->visible) {
		drm_err(drm,
			"No framebuffer or plane not visible, skipping update\n");
		goto update_cleanup;
	}

	src_pixels = (u32 *)vaddr;

	pixpaper_send_cmd(panel, PIXPAPER_CMD_DATA_START_TRANSMISSION, &err);
	if (err.errno_code)
		goto update_cleanup;

	pixpaper_wait_busy(panel);

	for (i = 0; i < PIXPAPER_HEIGHT; i++) {
		for (j = 0; j < PIXPAPER_PANEL_BUFFER_TWO_BYTES_PER_ROW; j++) {
			u8 packed_byte =
				pack_pixels_to_byte(src_pixels, i, j, fb);

			pixpaper_wait_busy(panel);
			pixpaper_send_data(panel, packed_byte, &err);
		}
	}
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_POWER_ON, &err);
	if (err.errno_code) {
		drm_err(drm, "Failed to send PON command: %d\n",
			err.errno_code);
		goto update_cleanup;
	}
	pixpaper_wait_busy(panel);

	pixpaper_send_cmd(panel, PIXPAPER_CMD_DISPLAY_REFRESH, &err);
	pixpaper_send_data(panel, PIXPAPER_DRF_VCOM_AC, &err);
	if (err.errno_code) {
		drm_err(drm, "Failed sending data after DRF: %d\n",
			err.errno_code);
		goto update_cleanup;
	}
	pixpaper_wait_busy(panel);

update_cleanup:
	if (err.errno_code && err.errno_code != -ETIMEDOUT)
		drm_err(drm, "Frame update function failed with error %d\n",
			err.errno_code);

	drm_dev_exit(idx);
}

static int pixpaper_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	drm_dbg(connector->dev, "CALLED for connector %s (id: %d)\n",
		connector->name, connector->base.id);

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		drm_err(connector->dev,
			"Failed to create mode for connector %s\n",
			connector->name);
		return 0;
	}

	mode->hdisplay     = PIXPAPER_WIDTH;
	mode->hsync_start  = PIXPAPER_WIDTH + PIXPAPER_HFP;
	mode->hsync_end    = mode->hsync_start + PIXPAPER_HSYNC;
	mode->htotal       = PIXPAPER_HTOTAL;

	mode->vdisplay     = PIXPAPER_HEIGHT;
	mode->vsync_start  = PIXPAPER_HEIGHT + PIXPAPER_VFP;
	mode->vsync_end    = mode->vsync_start + PIXPAPER_VSYNC;
	mode->vtotal       = PIXPAPER_VTOTAL;

	mode->clock        = PIXPAPER_PIXEL_CLOCK;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);

	if (drm_mode_validate_size(mode, connector->dev->mode_config.max_width,
				   connector->dev->mode_config.max_height) !=
	    MODE_OK) {
		drm_warn(connector->dev,
			 "Mode %s (%dx%d) failed size validation against max %dx%d\n",
			 mode->name, mode->hdisplay, mode->vdisplay,
			 connector->dev->mode_config.max_width,
			 connector->dev->mode_config.max_height);
		drm_mode_destroy(connector->dev, mode);
		return 0;
	}

	drm_mode_probed_add(connector, mode);
	drm_dbg(connector->dev, "Added mode '%s' (%dx%d@%d) to connector %s\n",
		mode->name, mode->hdisplay, mode->vdisplay,
		drm_mode_vrefresh(mode), connector->name);

	connector->display_info.width_mm  = PIXPAPER_WIDTH_MM;
	connector->display_info.height_mm = PIXPAPER_HEIGHT_MM;

	return 1;
}

static const struct drm_plane_funcs pixpaper_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const struct drm_plane_helper_funcs pixpaper_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = pixpaper_plane_helper_atomic_check,
	.atomic_update = pixpaper_plane_atomic_update,
};

static const struct drm_crtc_funcs pixpaper_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs pixpaper_crtc_helper_funcs = {
	.atomic_check = pixpaper_crtc_helper_atomic_check,
	.atomic_enable = pixpaper_crtc_atomic_enable,
	.atomic_disable = pixpaper_crtc_atomic_disable,
};

static const struct drm_encoder_funcs pixpaper_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_connector_funcs pixpaper_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs pixpaper_connector_helper_funcs = {
	.get_modes = pixpaper_connector_get_modes,
};

DEFINE_DRM_GEM_FOPS(pixpaper_fops);

static struct drm_driver pixpaper_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &pixpaper_fops,
	.name = "pixpaper",
	.desc = "DRM driver for PIXPAPER e-ink",
	.major = 1,
	.minor = 0,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static int pixpaper_mode_valid(struct drm_device *dev,
			       const struct drm_display_mode *mode)
{
	if (mode->hdisplay == PIXPAPER_WIDTH &&
	    mode->vdisplay == PIXPAPER_HEIGHT) {
		return MODE_OK;
	}
	return MODE_BAD;
}

static const struct drm_mode_config_funcs pixpaper_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.mode_valid = pixpaper_mode_valid,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int pixpaper_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct pixpaper_panel *panel;
	struct drm_device *drm;
	int ret;

	panel = devm_drm_dev_alloc(dev, &pixpaper_drm_driver,
				   struct pixpaper_panel, drm);
	if (IS_ERR(panel))
		return PTR_ERR(panel);

	drm = &panel->drm;
	panel->spi = spi;
	spi_set_drvdata(spi, panel);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = PIXPAPER_SPI_BITS_PER_WORD;

	if (!spi->max_speed_hz) {
		drm_warn(drm,
			 "spi-max-frequency not specified in DT, using default %u Hz\n",
			 PIXPAPER_SPI_SPEED_DEFAULT);
		spi->max_speed_hz = PIXPAPER_SPI_SPEED_DEFAULT;
	}

	ret = spi_setup(spi);
	if (ret < 0) {
		drm_err(drm, "SPI setup failed: %d\n", ret);
		return ret;
	}

	if (!dev->dma_mask)
		dev->dma_mask = &dev->coherent_dma_mask;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		drm_err(drm, "Failed to set DMA mask: %d\n", ret);
		return ret;
	}

	panel->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->reset))
		return PTR_ERR(panel->reset);

	panel->busy = devm_gpiod_get(dev, "busy", GPIOD_IN);
	if (IS_ERR(panel->busy))
		return PTR_ERR(panel->busy);

	panel->dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->dc))
		return PTR_ERR(panel->dc);

	ret = pixpaper_panel_hw_init(panel);
	if (ret) {
		drm_err(drm, "Panel hardware initialization failed: %d\n", ret);
		return ret;
	}

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &pixpaper_mode_config_funcs;
	drm->mode_config.min_width = PIXPAPER_WIDTH;
	drm->mode_config.max_width = PIXPAPER_WIDTH;
	drm->mode_config.min_height = PIXPAPER_HEIGHT;
	drm->mode_config.max_height = PIXPAPER_HEIGHT;

	ret = drm_universal_plane_init(drm, &panel->plane, 1,
				       &pixpaper_plane_funcs,
				       (const uint32_t[]){ DRM_FORMAT_XRGB8888 },
				       1, NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&panel->plane, &pixpaper_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &panel->crtc, &panel->plane, NULL,
					&pixpaper_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&panel->crtc, &pixpaper_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &panel->encoder, &pixpaper_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	panel->encoder.possible_crtcs = drm_crtc_mask(&panel->crtc);

	ret = drm_connector_init(drm, &panel->connector,
				 &pixpaper_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;

	drm_connector_helper_add(&panel->connector,
				 &pixpaper_connector_helper_funcs);
	drm_connector_attach_encoder(&panel->connector, &panel->encoder);

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);

	return 0;
}

static void pixpaper_remove(struct spi_device *spi)
{
	struct pixpaper_panel *panel = spi_get_drvdata(spi);

	if (!panel)
		return;

	drm_dev_unplug(&panel->drm);
	drm_atomic_helper_shutdown(&panel->drm);
}

static const struct spi_device_id pixpaper_ids[] = { { "pixpaper", 0 }, {} };
MODULE_DEVICE_TABLE(spi, pixpaper_ids);

static const struct of_device_id pixpaper_dt_ids[] = {
	{ .compatible = "mayqueen,pixpaper" },
	{}
};
MODULE_DEVICE_TABLE(of, pixpaper_dt_ids);

static struct spi_driver pixpaper_spi_driver = {
	.driver = {
		.name = "pixpaper",
		.of_match_table = pixpaper_dt_ids,
	},
	.id_table = pixpaper_ids,
	.probe = pixpaper_probe,
	.remove = pixpaper_remove,
};

module_spi_driver(pixpaper_spi_driver);

MODULE_AUTHOR("LiangCheng Wang");
MODULE_DESCRIPTION("DRM SPI driver for PIXPAPER e-ink panel");
MODULE_LICENSE("GPL");
