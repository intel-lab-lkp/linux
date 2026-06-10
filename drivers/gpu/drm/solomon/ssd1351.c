// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for the Solomon SSD1351 RGB565 color OLED controller
 *
 * The SSD1351 drives up to a 128x128 65k-color OLED panel over a 4-wire
 * SPI bus (clock, data, chip-select and a Data/Command GPIO). Unlike the
 * monochrome and grayscale SSD13xx parts handled by the ssd130x driver,
 * the SSD1351 has a native 16-bit RGB565 frame format, so this driver
 * uploads pixels as RGB565 rather than down-converting to RGB332.
 *
 * The command set (column/row range 0x15/0x75, write-RAM 0x5c) differs
 * from MIPI DCS, so the generic mipi_dbi helpers cannot be reused for the
 * pixel path; a small SPI command/data layer is implemented here instead.
 *
 * Author: Amit Barzilai <amit.barzilai22@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_drv.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#define SSD1351_WIDTH 128
#define SSD1351_HEIGHT 128

/* Commands */
#define SSD1351_SET_COL_ADDRESS 0x15
#define SSD1351_WRITE_RAM 0x5c
#define SSD1351_SET_ROW_ADDRESS 0x75
#define SSD1351_SET_REMAP 0xa0
#define SSD1351_SET_START_LINE 0xa1
#define SSD1351_SET_DISPLAY_OFFSET 0xa2
#define SSD1351_SET_DISPLAY_NORMAL 0xa6
#define SSD1351_SET_FUNCTION 0xab
#define SSD1351_SET_DISPLAY_OFF 0xae
#define SSD1351_SET_DISPLAY_ON 0xaf
#define SSD1351_SET_PHASE_LENGTH 0xb1
#define SSD1351_SET_CLOCK_DIV 0xb3
#define SSD1351_SET_VSL 0xb4
#define SSD1351_SET_GPIO 0xb5
#define SSD1351_SET_PRECHARGE2 0xb6
#define SSD1351_SET_PRECHARGE 0xbb
#define SSD1351_SET_VCOMH 0xbe
#define SSD1351_SET_CONTRAST 0xc1
#define SSD1351_SET_CONTRAST_MASTER 0xc7
#define SSD1351_SET_MUX_RATIO 0xca
#define SSD1351_SET_COMMAND_LOCK 0xfd

/* Re-map / Color Depth (command 0xa0) bits */
#define SSD1351_REMAP_COLUMN BIT(1) /* reverse column (SEG) order */
#define SSD1351_REMAP_COLOR_BGR BIT(2) /* swap sub-pixel color order */
#define SSD1351_REMAP_COM_SCAN BIT(4) /* reverse COM scan direction */
#define SSD1351_REMAP_COM_SPLIT BIT(5) /* odd/even COM split */
#define SSD1351_REMAP_65K BIT(6) /* 65k (RGB565) color depth */

struct ssd1351_device {
	struct drm_device drm;
	struct spi_device *spi;
	struct gpio_desc *reset;
	struct gpio_desc *dc;
	u32 rotation;

	/* Scratch buffer holding one frame of RGB565 pixels for SPI upload */
	u8 *tx_buf;

	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static struct ssd1351_device *drm_to_ssd1351(struct drm_device *drm)
{
	return container_of(drm, struct ssd1351_device, drm);
}

/*
 * SPI access. The D/C GPIO selects whether the bytes shifted out are
 * interpreted as a command (low) or as data (high).
 */
static int ssd1351_write_cmd(struct ssd1351_device *ssd1351, u8 cmd,
			     const u8 *params, size_t num)
{
	int ret;

	gpiod_set_value_cansleep(ssd1351->dc, 0);
	ret = spi_write(ssd1351->spi, &cmd, 1);
	if (ret)
		return ret;

	if (num) {
		gpiod_set_value_cansleep(ssd1351->dc, 1);
		ret = spi_write(ssd1351->spi, params, num);
		if (ret)
			return ret;
	}

	return 0;
}

/* Send a command followed by a fixed list of single-byte parameters. */
#define ssd1351_command(ssd1351, cmd, ...)                   \
	({                                                   \
		static const u8 _params[] = { __VA_ARGS__ }; \
		ssd1351_write_cmd((ssd1351), (cmd), _params, \
				  ARRAY_SIZE(_params));      \
	})

static void ssd1351_reset(struct ssd1351_device *ssd1351)
{
	if (!ssd1351->reset)
		return;

	/*
	 * Work in logical levels: 1 asserts reset, 0 releases it. The DT's
	 * GPIO_ACTIVE_LOW flag handles the physical inversion, so this pulse is
	 * correct regardless of how the board wires the RES# line.
	 */
	gpiod_set_value_cansleep(ssd1351->reset, 1);
	usleep_range(20, 1000);
	gpiod_set_value_cansleep(ssd1351->reset, 0);
	msleep(120);
}

static int ssd1351_init_display(struct ssd1351_device *ssd1351)
{
	u8 remap = SSD1351_REMAP_65K | SSD1351_REMAP_COM_SPLIT |
		   SSD1351_REMAP_COLOR_BGR;
	int ret;

	ssd1351_reset(ssd1351);

	/* Unlock the controller and allow access to all command registers */
	ret = ssd1351_command(ssd1351, SSD1351_SET_COMMAND_LOCK, 0x12);
	if (ret)
		return ret;
	ret = ssd1351_command(ssd1351, SSD1351_SET_COMMAND_LOCK, 0xb1);
	if (ret)
		return ret;

	ret = ssd1351_command(ssd1351, SSD1351_SET_DISPLAY_OFF);
	if (ret)
		return ret;

	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_CLOCK_DIV, 0xf1);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_MUX_RATIO, 0x7f);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_COL_ADDRESS, 0x00, 0x7f);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_ROW_ADDRESS, 0x00, 0x7f);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_START_LINE, 0x00);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_DISPLAY_OFFSET, 0x00);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_GPIO, 0x00);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_FUNCTION, 0x01);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_PHASE_LENGTH, 0x32);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_VSL, 0xa0, 0xb5, 0x55);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_PRECHARGE, 0x17);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_VCOMH, 0x05);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_CONTRAST, 0xc8, 0x80, 0xc8);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_CONTRAST_MASTER, 0x0f);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_PRECHARGE2, 0x01);
	ret = ret ?: ssd1351_command(ssd1351, SSD1351_SET_DISPLAY_NORMAL);
	if (ret)
		return ret;

	/*
	 * Select 65k (RGB565) color depth and the orientation. 0 and 180 degrees
	 * are reached with the column/COM-scan mirror bits while keeping the
	 * controller in horizontal address-increment mode, which matches the
	 * row-major pixel data produced below. The hardware can also rotate by
	 * 90/270 via the vertical address-increment bit, but that transposes the
	 * upload and would require a transposing blit + window remap that this
	 * driver does not implement; such rotations are rejected at probe.
	 */
	if (ssd1351->rotation == 180)
		remap |= SSD1351_REMAP_COLUMN;
	else
		remap |= SSD1351_REMAP_COM_SCAN;

	ret = ssd1351_write_cmd(ssd1351, SSD1351_SET_REMAP, &remap, 1);
	if (ret)
		return ret;

	return ssd1351_command(ssd1351, SSD1351_SET_DISPLAY_ON);
}

static void ssd1351_fb_blit_rect(struct ssd1351_device *ssd1351,
				 struct drm_framebuffer *fb,
				 const struct iosys_map *vmap,
				 struct drm_rect *rect,
				 struct drm_format_conv_state *fmtcnv_state)
{
	unsigned int width = drm_rect_width(rect);
	unsigned int height = drm_rect_height(rect);
	unsigned int dst_pitch = width * sizeof(u16);
	struct iosys_map dst;
	u8 range[2];

	iosys_map_set_vaddr(&dst, ssd1351->tx_buf);

	/*
	 * The panel expects RGB565 most-significant byte first; the big-endian
	 * conversion produces exactly that byte stream for the 8-bit SPI words.
	 */
	drm_fb_xrgb8888_to_rgb565be(&dst, &dst_pitch, vmap, fb, rect,
				    fmtcnv_state);

	range[0] = rect->x1;
	range[1] = rect->x2 - 1;
	if (ssd1351_write_cmd(ssd1351, SSD1351_SET_COL_ADDRESS, range,
			      sizeof(range)))
		return;

	range[0] = rect->y1;
	range[1] = rect->y2 - 1;
	if (ssd1351_write_cmd(ssd1351, SSD1351_SET_ROW_ADDRESS, range,
			      sizeof(range)))
		return;

	ssd1351_write_cmd(ssd1351, SSD1351_WRITE_RAM, ssd1351->tx_buf,
			  width * height * sizeof(u16));
}

/*
 * Plane
 */

static int ssd1351_plane_atomic_check(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *crtc = plane_state->crtc;
	struct drm_crtc_state *crtc_state = NULL;

	if (crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	return drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING, false,
						   false);
}

static void ssd1351_plane_atomic_update(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_plane_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(plane_state);
	struct ssd1351_device *ssd1351 = drm_to_ssd1351(plane->dev);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage;
	struct drm_rect dst_clip;
	int idx;

	if (!fb)
		return;

	if (!drm_dev_enter(&ssd1351->drm, &idx))
		return;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto out_drm_dev_exit;

	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		dst_clip = plane_state->dst;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;

		ssd1351_fb_blit_rect(ssd1351, fb, &shadow_plane_state->data[0],
				     &dst_clip,
				     &shadow_plane_state->fmtcnv_state);
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

out_drm_dev_exit:
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs ssd1351_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = ssd1351_plane_atomic_check,
	.atomic_update = ssd1351_plane_atomic_update,
};

static const struct drm_plane_funcs ssd1351_plane_funcs = {
	DRM_GEM_SHADOW_PLANE_FUNCS,
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
};

static const u32 ssd1351_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/*
 * CRTC
 */

static void ssd1351_crtc_atomic_enable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	struct ssd1351_device *ssd1351 = drm_to_ssd1351(crtc->dev);
	int idx, ret;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	ret = ssd1351_init_display(ssd1351);
	if (ret)
		drm_err(crtc->dev, "Failed to initialize display: %d\n", ret);

	drm_dev_exit(idx);
}

static void ssd1351_crtc_atomic_disable(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	struct ssd1351_device *ssd1351 = drm_to_ssd1351(crtc->dev);
	int idx;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	ssd1351_command(ssd1351, SSD1351_SET_DISPLAY_OFF);

	drm_dev_exit(idx);
}

static const struct drm_crtc_helper_funcs ssd1351_crtc_helper_funcs = {
	.atomic_check = drm_crtc_helper_atomic_check,
	.atomic_enable = ssd1351_crtc_atomic_enable,
	.atomic_disable = ssd1351_crtc_atomic_disable,
};

static const struct drm_crtc_funcs ssd1351_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

/*
 * Encoder
 */

static const struct drm_encoder_funcs ssd1351_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/*
 * Connector
 */

static const struct drm_display_mode ssd1351_mode = {
	DRM_SIMPLE_MODE(SSD1351_WIDTH, SSD1351_HEIGHT, 24, 24),
};

static int ssd1351_connector_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &ssd1351_mode);
}

static const struct drm_connector_helper_funcs ssd1351_connector_helper_funcs = {
	.get_modes = ssd1351_connector_get_modes,
};

static const struct drm_connector_funcs ssd1351_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs ssd1351_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * Driver
 */

DEFINE_DRM_GEM_FOPS(ssd1351_fops);

static const struct drm_driver ssd1351_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &ssd1351_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
	.name = "ssd1351",
	.desc = "Solomon SSD1351",
	.major = 1,
	.minor = 0,
};

static int ssd1351_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ssd1351_device *ssd1351;
	struct drm_device *drm;
	int ret;

	ssd1351 = devm_drm_dev_alloc(dev, &ssd1351_driver,
				     struct ssd1351_device, drm);
	if (IS_ERR(ssd1351))
		return PTR_ERR(ssd1351);

	drm = &ssd1351->drm;
	ssd1351->spi = spi;
	spi_set_drvdata(spi, drm);

	ssd1351->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ssd1351->reset))
		return dev_err_probe(dev, PTR_ERR(ssd1351->reset),
				     "Failed to get reset GPIO\n");

	ssd1351->dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(ssd1351->dc))
		return dev_err_probe(dev, PTR_ERR(ssd1351->dc),
				     "Failed to get D/C GPIO\n");

	device_property_read_u32(dev, "rotation", &ssd1351->rotation);
	if (ssd1351->rotation != 0 && ssd1351->rotation != 180)
		return dev_err_probe(dev, -EINVAL,
				     "Unsupported rotation %u; only 0 and 180 are supported\n",
				     ssd1351->rotation);

	ssd1351->tx_buf = devm_kmalloc(dev,
				       SSD1351_WIDTH * SSD1351_HEIGHT * sizeof(u16),
				       GFP_KERNEL);
	if (!ssd1351->tx_buf)
		return -ENOMEM;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = SSD1351_WIDTH;
	drm->mode_config.max_width = SSD1351_WIDTH;
	drm->mode_config.min_height = SSD1351_HEIGHT;
	drm->mode_config.max_height = SSD1351_HEIGHT;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &ssd1351_mode_config_funcs;

	ret = drm_universal_plane_init(drm, &ssd1351->plane, 0,
				       &ssd1351_plane_funcs, ssd1351_formats,
				       ARRAY_SIZE(ssd1351_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&ssd1351->plane, &ssd1351_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&ssd1351->plane);

	ret = drm_crtc_init_with_planes(drm, &ssd1351->crtc, &ssd1351->plane,
					NULL, &ssd1351_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&ssd1351->crtc, &ssd1351_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &ssd1351->encoder, &ssd1351_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	ssd1351->encoder.possible_crtcs = drm_crtc_mask(&ssd1351->crtc);

	ret = drm_connector_init(drm, &ssd1351->connector,
				 &ssd1351_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;
	drm_connector_helper_add(&ssd1351->connector,
				 &ssd1351_connector_helper_funcs);

	ret = drm_connector_attach_encoder(&ssd1351->connector,
					   &ssd1351->encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);

	return 0;
}

static void ssd1351_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void ssd1351_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static const struct of_device_id ssd1351_of_match[] = {
	{ .compatible = "solomon,ssd1351" },
	{}
};
MODULE_DEVICE_TABLE(of, ssd1351_of_match);

static const struct spi_device_id ssd1351_id[] = { { "ssd1351", 0 }, {} };
MODULE_DEVICE_TABLE(spi, ssd1351_id);

static struct spi_driver ssd1351_spi_driver = {
	.driver = {
		.name = "ssd1351",
		.of_match_table = ssd1351_of_match,
	},
	.id_table = ssd1351_id,
	.probe = ssd1351_probe,
	.remove = ssd1351_remove,
	.shutdown = ssd1351_shutdown,
};
module_spi_driver(ssd1351_spi_driver);

MODULE_DESCRIPTION("Solomon SSD1351 DRM driver");
MODULE_AUTHOR("Amit Barzilai <amit.barzilai22@gmail.com>");
MODULE_LICENSE("GPL");
