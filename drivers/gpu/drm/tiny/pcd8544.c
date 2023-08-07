// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for Philips PCD8544 LCD controller/driver.
 * Compatible with Nokia 5110/3310 84x48 LCD displays.
 *
 * Copyright 2023 Viktar Simanenka <viteosen@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_rect.h>
#include <drm/drm_simple_kms_helper.h>

/*
 * The display is monochrome, every bit in buffer is a pixel.
 * Display RAM divided into 6 banks along y-axis, each bank 84 bytes along x-axis.
 * Driver uses horizontal addressing.
 */

#define PCD8544_FUNCTIONSET     0x20
#define PCD8544_DISPLAYCONTROL  0x08
#define PCD8544_SETYADDR        0x40
#define PCD8544_SETXADDR        0x80
#define PCD8544_SETBIAS         0x10
#define PCD8544_SETTEMPCOEF     0x04
#define PCD8544_SETVOP          0x80

#define PCD8544_EXTENDED_INSTRUCTION    0x01
#define PCD8544_VERTICAL_ADDRESSING     0x02
#define PCD8544_DISPLAYNORMAL       0x04
#define PCD8544_DISPLAYINVERTED     0x05

struct pcd8544_device {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct spi_device *spi;

	u32 width;
	u32 height;
	u8 *tx_buf; // Buffer used for transfer
	size_t tx_buflen;

	struct backlight_device *backlight;
	struct gpio_desc *reset;
	struct gpio_desc *dc;

	u32 inverted;
	u32 temperature_coeff;
	u32 bias;
	u32 voltage_op;
};

MODULE_PARM_DESC(inverted, "Invert display colors: 1 - enable, 0 - disable");
MODULE_PARM_DESC(voltage_op, "Vop[6:0] LCD voltage operation coefficient: 0-127 (default: 0)");
MODULE_PARM_DESC(temperature_coeff, "TC[1:0] Temperature compensation coefficient: 0-3 (default: 0)");
MODULE_PARM_DESC(bias, "BS[2:0] Bias system coefficient: 0-7 (default: 4)");

#define drm_to_dev(__dev) container_of(__dev, struct pcd8544_device, drm)

static int pcd8544_spi_transfer(struct spi_device *spi, const void *buf, size_t len)
{
	size_t max_chunk = spi_max_transfer_size(spi);
	struct spi_transfer tr = {
		.bits_per_word = 8,
		.speed_hz = 0,
	};
	struct spi_message m;
	size_t chunk;
	int ret;

	max_chunk = ALIGN_DOWN(max_chunk, 2);

	spi_message_init_with_transfers(&m, &tr, 1);

	while (len) {
		chunk = min(len, max_chunk);

		tr.tx_buf = buf;
		tr.len = chunk;
		buf += chunk;
		len -= chunk;

		ret = spi_sync(spi, &m);
		if (ret)
			return ret;
	}
	return 0;
}

static int pcd8544_spi_command(struct pcd8544_device *pcd8544_dev, u8 cmd)
{
	struct spi_device *spi = pcd8544_dev->spi;

	gpiod_set_value_cansleep(pcd8544_dev->dc, 0);

	return pcd8544_spi_transfer(spi, &cmd, 1);
}

static int pcd8544_spi_data(struct pcd8544_device *pcd8544_dev,
				const void *tx_buf, size_t len)
{
	struct spi_device *spi = pcd8544_dev->spi;

	gpiod_set_value_cansleep(pcd8544_dev->dc, 1);

	return pcd8544_spi_transfer(spi, tx_buf, len);
}

static void pcd8544_screen_update(struct pcd8544_device *pcd8544_dev,
				const u8 *buf, size_t len, u8 addr_x, u8 addr_y)
{
	pcd8544_spi_command(pcd8544_dev, PCD8544_SETYADDR | addr_y);
	pcd8544_spi_command(pcd8544_dev, PCD8544_SETXADDR | addr_x);
	pcd8544_spi_data(pcd8544_dev, buf, len);
}

static int pcd8544_fb_xrgb8888_to_mono(u8 *dst, struct drm_framebuffer *fb,
					struct drm_rect *clip)
{
	struct iosys_map fb_map[DRM_FORMAT_MAX_PLANES];
	struct iosys_map fb_data[DRM_FORMAT_MAX_PLANES];
	struct iosys_map buf_map;
	u8 *buf;
	unsigned int width = drm_rect_width(clip);
	unsigned int height = drm_rect_height(clip);
	unsigned int line_length = DIV_ROUND_UP(width, 8);
	unsigned int bank_max = DIV_ROUND_UP(height, 8);
	unsigned int x, y, bank;
	int ret = 0;

	buf = kcalloc(line_length, height, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf_map = (struct iosys_map)IOSYS_MAP_INIT_VADDR(buf);

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		goto out_free;

	ret = drm_gem_fb_vmap(fb, fb_map, fb_data);
	if (ret)
		goto out_end_cpu_access;

	drm_fb_xrgb8888_to_mono(&buf_map, &line_length, fb_data, fb, clip);

	drm_gem_fb_vunmap(fb, fb_map);

	for (bank = 0; bank < bank_max; bank++) {
		for (x = 0; x < width; x++) {
			u8 data = 0;

			for (y = 0; y < 8; y++) {
				u8 byte = buf[(8 * bank + y) * line_length + x / 8];
				u8 bit = (byte >> (x % 8)) & 1;

				data |= bit << y;
			}
			*dst++ = data;
		}
	}

out_end_cpu_access:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
out_free:
	kfree(buf);

	return ret;
}

static void pcd8544_fb_dirty(struct drm_framebuffer *fb, struct drm_rect *rect)
{
	struct pcd8544_device *pcd8544_dev = drm_to_dev(fb->dev);
	int idx, ret = 0;

	if (!drm_dev_enter(fb->dev, &idx))
		return;

	/* Align rect y to display bank boundaries */
	rect->y1 = round_down(rect->y1, 8);
	rect->y2 = min_t(unsigned int, round_up(rect->y2, 8), pcd8544_dev->width);

	ret = pcd8544_fb_xrgb8888_to_mono(pcd8544_dev->tx_buf, fb, rect);
	if (ret)
		goto err_msg;

	/* if full update */
	if (rect->x1 == 0 && rect->x2 == pcd8544_dev->width &&
		rect->y1 == 0 && rect->y2 == pcd8544_dev->height) {
		pcd8544_screen_update(pcd8544_dev, pcd8544_dev->tx_buf, pcd8544_dev->tx_buflen, 0, 0);
	} else {
		u32 rect_width = drm_rect_width(rect);
		u32 bank_min = rect->y1 / 8;
		u32 bank_max = rect->y2 / 8;

		for (u32 bank = bank_min; bank <= bank_max; bank++)
			pcd8544_screen_update(pcd8544_dev, &pcd8544_dev->tx_buf[rect_width * (bank - bank_min)],
						rect_width, rect->x1, bank);
	}

err_msg:
	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n", ret);

	drm_dev_exit(idx);
}

static enum drm_mode_status pcd8544_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
							const struct drm_display_mode *mode)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct pcd8544_device *pcd8544_dev = drm_to_dev(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &pcd8544_dev->mode);
}

static void pcd8544_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct pcd8544_device *pcd8544_dev = drm_to_dev(pipe->crtc.dev);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_rect rect = {
		.x1 = 0,
		.x2 = fb->width,
		.y1 = 0,
		.y2 = fb->height,
	};
	int idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	drm_dbg(pipe->crtc.dev, "\n");

	/* Reset display */
	gpiod_set_value_cansleep(pcd8544_dev->reset, 0);
	usleep_range(20, 1000);
	gpiod_set_value_cansleep(pcd8544_dev->reset, 1);
	msleep(120);

	/* Init */
	pcd8544_spi_command(pcd8544_dev, PCD8544_FUNCTIONSET | PCD8544_EXTENDED_INSTRUCTION);
	pcd8544_spi_command(pcd8544_dev, PCD8544_SETBIAS | (pcd8544_dev->bias & 0x7));
	pcd8544_spi_command(pcd8544_dev, PCD8544_SETTEMPCOEF | (pcd8544_dev->temperature_coeff & 0x3));
	pcd8544_spi_command(pcd8544_dev, PCD8544_SETVOP | (pcd8544_dev->voltage_op & 0x7F));
	pcd8544_spi_command(pcd8544_dev, PCD8544_FUNCTIONSET);
	if (pcd8544_dev->inverted)
		pcd8544_spi_command(pcd8544_dev, PCD8544_DISPLAYCONTROL | PCD8544_DISPLAYINVERTED);
	else
		pcd8544_spi_command(pcd8544_dev, PCD8544_DISPLAYCONTROL | PCD8544_DISPLAYNORMAL);

	pcd8544_fb_dirty(fb, &rect);

	drm_dev_exit(idx);
}

static void pcd8544_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct pcd8544_device *pcd8544_dev = drm_to_dev(pipe->crtc.dev);

	drm_dbg(pipe->crtc.dev, "\n");

	/* Clear screen */
	memset(pcd8544_dev->tx_buf, 0, pcd8544_dev->tx_buflen);
	pcd8544_screen_update(pcd8544_dev, pcd8544_dev->tx_buf, pcd8544_dev->tx_buflen, 0, 0);

	if (pcd8544_dev->backlight)
		backlight_disable(pcd8544_dev->backlight);
}

static void pcd8544_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect rect;

	if (!pipe->crtc.state->active)
		return;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		pcd8544_fb_dirty(state->fb, &rect);
}

static const struct drm_simple_display_pipe_funcs pcd8544_pipe_funcs = {
	.mode_valid = pcd8544_pipe_mode_valid,
	.enable		= pcd8544_pipe_enable,
	.disable	= pcd8544_pipe_disable,
	.update		= pcd8544_pipe_update,
};

DEFINE_DRM_GEM_DMA_FOPS(pcd8544_fops);

static int pcd8544_connector_get_modes(struct drm_connector *connector)
{
	struct pcd8544_device *pcd8544_dev = drm_to_dev(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &pcd8544_dev->mode);
}

static const struct drm_connector_helper_funcs pcd8544_connector_hfuncs = {
	.get_modes = pcd8544_connector_get_modes,
};

static const struct drm_connector_funcs pcd8544_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs pcd8544_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int __maybe_unused pcd8544_pm_suspend(struct device *dev)
{
	return drm_mode_config_helper_suspend(dev_get_drvdata(dev));
}

static int __maybe_unused pcd8544_pm_resume(struct device *dev)
{
	drm_mode_config_helper_resume(dev_get_drvdata(dev));

	return 0;
}

static const struct dev_pm_ops pcd8544_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pcd8544_pm_suspend, pcd8544_pm_resume)
};

static const struct drm_driver pcd8544_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &pcd8544_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.name			= "pcd8544",
	.desc			= "Philips PCD8544",
	.date			= "20230701",
	.major			= 1,
	.minor			= 0,
};

static const uint32_t pcd8544_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static struct drm_display_mode pcd8544_mode = {
	DRM_SIMPLE_MODE(84, 48, 35, 28),
};

static const struct of_device_id pcd8544_of_match[] = {
	{ .compatible = "nxp,pcd8544" },
	{ }
};
MODULE_DEVICE_TABLE(of, pcd8544_of_match);

static const struct spi_device_id pcd8544_id[] = {
	{ "pcd8544", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, pcd8544_id);

static int pcd8544_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct pcd8544_device *pcd8544_dev;
	struct drm_device *drm;
	int ret;
	static const uint64_t modifiers[] = {
		DRM_FORMAT_MOD_LINEAR,
		DRM_FORMAT_MOD_INVALID
	};

	pcd8544_dev = devm_drm_dev_alloc(dev, &pcd8544_driver, struct pcd8544_device, drm);
	if (IS_ERR(pcd8544_dev))
		return PTR_ERR(pcd8544_dev);

	pcd8544_dev->spi = spi;

	pcd8544_dev->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pcd8544_dev->reset))
		return dev_err_probe(dev, PTR_ERR(pcd8544_dev->reset), "Failed to get GPIO 'reset'\n");

	pcd8544_dev->dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(pcd8544_dev->dc))
		return dev_err_probe(dev, PTR_ERR(pcd8544_dev->dc), "Failed to get GPIO 'dc'\n");

	pcd8544_dev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(pcd8544_dev->backlight))
		return PTR_ERR(pcd8544_dev->backlight);

	pcd8544_dev->inverted = device_property_read_bool(dev, "nxp,inverted");
	if (device_property_read_u32(dev, "nxp,temperature-coeff", &pcd8544_dev->temperature_coeff))
		pcd8544_dev->temperature_coeff = 0;
	if (device_property_read_u32(dev, "nxp,bias", &pcd8544_dev->bias))
		pcd8544_dev->bias = 4;
	if (device_property_read_u32(dev, "nxp,voltage-op", &pcd8544_dev->voltage_op))
		pcd8544_dev->voltage_op = 0;

	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	drm_mode_copy(&pcd8544_dev->mode, &pcd8544_mode);
	pcd8544_dev->width = pcd8544_mode.hdisplay;
	pcd8544_dev->height = pcd8544_mode.vdisplay;
	pcd8544_dev->tx_buflen = pcd8544_dev->width * DIV_ROUND_UP(pcd8544_dev->height, 8);
	pcd8544_dev->tx_buf = devm_kzalloc(dev, pcd8544_dev->tx_buflen, GFP_KERNEL);
	if (!pcd8544_dev->tx_buf)
		return -ENOMEM;

	drm = &pcd8544_dev->drm;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm_connector_helper_add(&pcd8544_dev->connector, &pcd8544_connector_hfuncs);
	ret = drm_connector_init(drm, &pcd8544_dev->connector, &pcd8544_connector_funcs, DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;

	drm->mode_config.funcs = &pcd8544_mode_config_funcs;
	drm->mode_config.min_width = pcd8544_dev->mode.hdisplay;
	drm->mode_config.max_width = pcd8544_dev->mode.hdisplay;
	drm->mode_config.min_height = pcd8544_dev->mode.vdisplay;
	drm->mode_config.max_height = pcd8544_dev->mode.vdisplay;

	ret = drm_simple_display_pipe_init(drm, &pcd8544_dev->pipe, &pcd8544_pipe_funcs,
					pcd8544_formats, ARRAY_SIZE(pcd8544_formats),
					modifiers, &pcd8544_dev->connector);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&pcd8544_dev->pipe.plane);

	spi_set_drvdata(spi, drm);

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_dbg(drm, "SPI speed: %uMHz\n", spi->max_speed_hz / 1000000);

	drm_fbdev_generic_setup(drm, 0);

	return 0;
}

static void pcd8544_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void pcd8544_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver pcd8544_spi_driver = {
	.driver = {
		.name = "pcd8544",
		.owner = THIS_MODULE,
		.of_match_table = pcd8544_of_match,
		.pm = &pcd8544_pm_ops,
	},
	.id_table = pcd8544_id,
	.probe = pcd8544_probe,
	.remove = pcd8544_remove,
	.shutdown = pcd8544_shutdown,
};
module_spi_driver(pcd8544_spi_driver);

MODULE_DESCRIPTION("Philips PCD8544 DRM driver");
MODULE_AUTHOR("Viktar Simanenka <viteosen@gmail.com>");
MODULE_LICENSE("GPL");
