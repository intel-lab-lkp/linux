// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright (c) 2025-2026 Francesco Valla <francesco@valla.it>
 *
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/efi-bgrt.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/iosys-map.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/types.h>

#include <drm/drm_client.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>

#include "drm_client_internal.h"
#include "drm_draw_internal.h"
#include "drm_internal.h"

/**
 * DOC: overview
 *
 * This is a simple graphic bootsplash, able to display either a plain color or
 * a static image.
 */

static unsigned int splash_color = CONFIG_DRM_CLIENT_SPLASH_BACKGROUND_COLOR;
module_param(splash_color, uint, 0400);
MODULE_PARM_DESC(splash_color, "Splash background color (RGB888)");

#if IS_ENABLED(CONFIG_DRM_CLIENT_SPLASH_SRC_BMP)
#define DEFAULT_SPLASH_BMP "drm_splash.bmp"
static char *splash_bmp = DEFAULT_SPLASH_BMP;
module_param(splash_bmp, charp, 0400);
MODULE_PARM_DESC(splash_bmp, "Name of splash image (default: \"" DEFAULT_SPLASH_BMP "\")");
#endif // CONFIG_DRM_CLIENT_SPLASH_SRC_BMP

#if IS_ENABLED(CONFIG_DRM_CLIENT_SPLASH_BMP_SUPPORT)
#define BMP_FILE_MAGIC_ID 0x4d42

/* BMP header structures copied from drivers/video/fbdev/efifb.c */
struct bmp_file_header {
	u16 id;
	u32 file_size;
	u32 reserved;
	u32 bitmap_offset;
} __packed;

struct bmp_dib_header {
	u32 dib_header_size;
	s32 width;
	s32 height;
	u16 planes;
	u16 bpp;
	u32 compression;
	u32 bitmap_size;
	u32 horz_resolution;
	u32 vert_resolution;
	u32 colors_used;
	u32 colors_important;
} __packed;
#endif // CONFIG_DRM_CLIENT_SPLASH_BMP_SUPPORT

typedef int (*drm_splash_data_get_func_t)(void *priv, const u8 **data, size_t *size);
typedef void (*drm_splash_data_release_func_t)(void *priv);

struct drm_splash_scanout {
	int id;
	u32 format;
	unsigned int width;
	unsigned int height;
	struct drm_client_buffer *buffer;
	bool bg_drawn;
};

struct drm_splash {
	struct drm_client_dev client;
	u32 preferred_format;
	struct device dev;

	struct task_struct *thread;
	atomic_t pending;

	struct mutex hotplug_lock;
	bool initialized;

	u32 n_scanout;
	struct drm_splash_scanout *scanout;

	spinlock_t data_lock;
	drm_splash_data_get_func_t data_get;
	drm_splash_data_release_func_t data_release;
	void *data_priv;
};

static struct drm_splash *client_to_drm_splash(struct drm_client_dev *client)
{
	return container_of_const(client, struct drm_splash, client);
}

static void __maybe_unused
drm_splash_data_source_push(struct drm_splash *splash,
			    drm_splash_data_get_func_t get,
			    drm_splash_data_release_func_t release,
			    void *priv)
{
	guard(spinlock)(&splash->data_lock);

	/* Release previous data */
	if (splash->data_release)
		splash->data_release(splash->data_priv);

	splash->data_get = get;
	splash->data_release = release;
	splash->data_priv = priv;
}

static void drm_splash_data_source_pop(struct drm_splash *splash,
				       drm_splash_data_get_func_t *get,
				       drm_splash_data_release_func_t *release,
				       void **priv)
{
	guard(spinlock)(&splash->data_lock);

	*get = splash->data_get;
	splash->data_get = NULL;

	*release = splash->data_release;
	splash->data_release = NULL;

	*priv = splash->data_priv;
	splash->data_priv = NULL;
}

static struct drm_splash_scanout *
get_scanout_from_tile_group(struct drm_splash *splash, int id)
{
	int j;

	for (j = 0; j < splash->n_scanout; j++)
		if (splash->scanout[j].id == id)
			return &splash->scanout[j];

	return NULL;
}

static u32 drm_splash_find_usable_format(struct drm_plane *plane,
					 u32 preferred_format)
{
	int i;

	/* Check if the preferred format can be used */
	for (i = 0; i < plane->format_count; i++)
		if (plane->format_types[i] == preferred_format)
			return preferred_format;

	/* Otherwise, find the first format that can be converted from XRGB8888 */
	for (i = 0; i < plane->format_count; i++)
		if (drm_draw_color_from_xrgb8888(0xffffffff, plane->format_types[i]) != 0)
			return plane->format_types[i];

	return DRM_FORMAT_INVALID;
}

static void drm_splash_fill(struct iosys_map *map, unsigned int dst_pitch,
			    unsigned int height, unsigned int width,
			    u32 px_width, u32 color)
{
	switch (px_width) {
	case 2:
		drm_draw_fill16(map, dst_pitch, height, width, color);
		break;
	case 3:
		drm_draw_fill24(map, dst_pitch, height, width, color);
		break;
	case 4:
		drm_draw_fill32(map, dst_pitch, height, width, color);
		break;
	default:
		WARN_ONCE(1, "Can't fill with pixel width %d", px_width);
	}
}

static int drm_splash_fill_solid_color(struct drm_client_buffer *buffer, u32 color)
{
	struct drm_client_dev *client = buffer->client;
	struct drm_framebuffer *fb = buffer->fb;
	struct drm_rect r = DRM_RECT_INIT(0, 0, fb->width, fb->height);
	u32 px_width = fb->format->cpp[0];
	struct iosys_map map;
	int ret;

	ret = drm_client_buffer_vmap_local(buffer, &map);
	if (ret) {
		drm_err(client->dev, "splash: cannot vmap buffer: %d", ret);
		return ret;
	}

	drm_splash_fill(&map, fb->pitches[0], drm_rect_height(&r),
			drm_rect_width(&r), px_width, color);

	drm_client_buffer_vunmap_local(buffer);

	return drm_client_buffer_flush(buffer, &r);
}

#if IS_ENABLED(CONFIG_DRM_CLIENT_SPLASH_BMP_SUPPORT)
static void drm_splash_blit_pix16(struct iosys_map *map, unsigned int dpitch,
				  unsigned int x_pad, unsigned int y_pad,
				  const u8 *sbuf8, unsigned int spitch,
				  unsigned int width, unsigned int height,
				  bool invert_y, u32 format)
{
	unsigned int x, y, src_offset, dst_offset;
	u32 scolor, dcolor, wr_off;

	for (y = 0; y < height; y++) {
		src_offset = (invert_y ? (height - y - 1) : y) * spitch;
		dst_offset = (y_pad + y) * dpitch;

		for (x = 0; x < width; x++) {
			scolor = *(const u32 *)(&sbuf8[src_offset + 3 * x]);
			dcolor = drm_draw_color_from_xrgb8888(scolor, format);
			wr_off = dst_offset + (x_pad + x) * sizeof(u16);

			iosys_map_wr(map, wr_off, u16, dcolor);
		}
	}
}

static void drm_splash_blit_pix24(struct iosys_map *map, unsigned int dpitch,
				  unsigned int x_pad, unsigned int y_pad,
				  const u8 *sbuf8, unsigned int spitch,
				  unsigned int width, unsigned int height,
				  bool invert_y, u32 format)
{
	unsigned int x, y, src_offset, dst_offset;
	u32 scolor, dcolor, wr_off;

	for (y = 0; y < height; y++) {
		src_offset = (invert_y ? (height - y - 1) : y) * spitch;
		dst_offset = (y_pad + y) * dpitch;

		for (x = 0; x < width; x++) {
			scolor = *(const u32 *)(&sbuf8[src_offset + 3 * x]);
			dcolor = drm_draw_color_from_xrgb8888(scolor, format);
			wr_off = dst_offset + (x_pad + x) * 3;

			iosys_map_wr(map, wr_off, u8, (dcolor & 0x000000FF) >> 0);
			iosys_map_wr(map, wr_off + 1, u8, (dcolor & 0x0000FF00) >> 8);
			iosys_map_wr(map, wr_off + 2, u8, (dcolor & 0x00FF0000) >> 16);
		}
	}
}

static void drm_splash_blit_pix32(struct iosys_map *map, unsigned int dpitch,
				  unsigned int x_pad, unsigned int y_pad,
				  const u8 *sbuf8, unsigned int spitch,
				  unsigned int width, unsigned int height,
				  bool invert_y, u32 format)
{
	unsigned int x, y, src_offset, dst_offset;
	u32 scolor, dcolor, wr_off;

	for (y = 0; y < height; y++) {
		src_offset = (invert_y ? (height - y - 1) : y) * spitch;
		dst_offset = (y_pad + y) * dpitch;

		for (x = 0; x < width; x++) {
			scolor = *(const u32 *)(&sbuf8[src_offset + 3 * x]);
			dcolor = drm_draw_color_from_xrgb8888(scolor, format);
			wr_off = dst_offset + (x_pad + x) * sizeof(u32);

			iosys_map_wr(map, wr_off, u32, dcolor);
		}
	}
}

static void drm_splash_blit_rgb888(struct iosys_map *map, unsigned int dpitch,
				   unsigned int x_pad, unsigned int y_pad,
				   const u8 *sbuf8, unsigned int spitch,
				   unsigned int width, unsigned int height,
				   bool invert_y)
{
	unsigned int y, src_offset, dst_offset;

	for (y = 0; y < height; y++) {
		src_offset = (invert_y ? (height - y - 1) : y) * spitch;
		dst_offset = (y_pad + y) * dpitch + x_pad * 3;

		iosys_map_memcpy_to(map, dst_offset, &sbuf8[src_offset], width * 3);
	}
}

static int drm_splash_bmp_to_scanout(struct drm_splash_scanout *scanout,
				     const u8 *data, size_t data_len)

{
	struct drm_client_buffer *buffer = scanout->buffer;
	struct drm_client_dev *client = buffer->client;
	struct drm_framebuffer *fb = buffer->fb;
	u32 px_width = fb->format->cpp[0];
	const struct bmp_file_header *file_header;
	const struct bmp_dib_header *dib_header;
	u32 bmp_width, bmp_height, bmp_pitch;
	bool bmp_invert_y;
	unsigned int x_pad, y_pad;
	const u8 *image_data;
	struct iosys_map map;
	struct drm_rect r;
	int ret;

	if (data_len < (sizeof(*file_header) + sizeof(*dib_header))) {
		drm_err(client->dev, "splash: BMP file too short");
		return -EINVAL;
	}

	file_header = (const struct bmp_file_header *)data;
	if (file_header->id != BMP_FILE_MAGIC_ID) {
		drm_err(client->dev, "splash: invalid BMP magic 0x%04X",
			file_header->id);
		return -EINVAL;
	}

	dib_header = (const struct bmp_dib_header *)(data + sizeof(*file_header));

	/* Restrict supported format to uncompressed, 24bit RGB888 */
	if (dib_header->dib_header_size != 40 || dib_header->width < 0 ||
	    dib_header->planes != 1 || dib_header->compression != 0 ||
	    dib_header->bpp != 24) {
		drm_err(client->dev, "splash: invalid BMP format");
		return -EINVAL;
	}

	bmp_width = dib_header->width;
	bmp_height = abs(dib_header->height);
	bmp_pitch = round_up(3 * bmp_width, 4);
	bmp_invert_y = (dib_header->height > 0);

	if ((file_header->bitmap_offset + bmp_pitch * bmp_height) > data_len) {
		drm_err(client->dev, "splash: invalid BMP size");
		return -EINVAL;
	}

	if (bmp_width > scanout->width || bmp_height > scanout->height) {
		drm_err(client->dev, "splash: BMP image is too big for the screen");
		return -EINVAL;
	}

	image_data = data + file_header->bitmap_offset;

	ret = drm_client_buffer_vmap_local(buffer, &map);
	if (ret) {
		drm_err(client->dev, "splash: cannot vmap buffer: %d", ret);
		return ret;
	}

	/* Center X and Y */
	x_pad = (scanout->width - bmp_width) / 2;
	y_pad = (scanout->height - bmp_height) / 2;
	r = DRM_RECT_INIT(x_pad, y_pad, bmp_width, bmp_height);

	/* In case the target format is RGB888, source data can be copied to
	 * the video buffer line by line, avoiding some overhead.
	 */
	if (scanout->format == DRM_FORMAT_RGB888) {
		drm_splash_blit_rgb888(&map, fb->pitches[0], x_pad, y_pad,
				       image_data, bmp_pitch, bmp_width,
				       bmp_height, bmp_invert_y);
	} else {
		switch (px_width) {
		case 2:
			drm_splash_blit_pix16(&map, fb->pitches[0], x_pad,
					      y_pad, image_data, bmp_pitch,
					      bmp_width, bmp_height,
					      bmp_invert_y, scanout->format);
			break;
		case 3:
			drm_splash_blit_pix24(&map, fb->pitches[0], x_pad,
					      y_pad, image_data, bmp_pitch,
					      bmp_width, bmp_height,
					      bmp_invert_y, scanout->format);
			break;
		case 4:
			drm_splash_blit_pix32(&map, fb->pitches[0], x_pad,
					      y_pad, image_data, bmp_pitch,
					      bmp_width, bmp_height,
					      bmp_invert_y, scanout->format);
			break;
		default:
			drm_warn_once(client->dev,
				      "splash: can't blit with pixel width %d",
				      px_width);
		}
	}

	drm_client_buffer_vunmap_local(buffer);

	return drm_client_buffer_flush(buffer, &r);
}
#else
static inline int drm_splash_bmp_to_scanout(struct drm_splash_scanout *scanout,
					    const u8 *data, size_t data_len)
{
	return -EOPNOTSUPP;
}
#endif

static int drm_splash_draw_scanout(struct drm_splash *splash,
				   struct drm_splash_scanout *scanout,
				   const u8 *data, size_t data_len)
{
	if (!scanout->buffer)
		return -ENODEV;

	if (!scanout->bg_drawn) {
		u32 color = drm_draw_color_from_xrgb8888(splash_color,
							 scanout->format);
		drm_splash_fill_solid_color(scanout->buffer, color);
		scanout->bg_drawn = true;
	}

	if (data != NULL) {
		/* Ignore the return value, since the solid color has already
		 * been drawn to screen.
		 */
		drm_splash_bmp_to_scanout(scanout, data, data_len);
	}

	return 0;
}

static int drm_splash_render_thread(void *data)
{
	struct drm_splash *splash = data;
	struct drm_client_dev *client = &splash->client;

	while (!kthread_should_stop()) {
		unsigned int draw_count = 0;
		drm_splash_data_get_func_t get_fn = NULL;
		drm_splash_data_release_func_t release_fn = NULL;
		void *priv = NULL;
		const u8 *img_data = NULL;
		size_t img_data_len = 0;
		int i, ret;

		drm_splash_data_source_pop(splash, &get_fn, &release_fn, &priv);

		if (get_fn) {
			ret = get_fn(priv, &img_data, &img_data_len);
			if (ret) {
				drm_err(client->dev,
					"splash: failed to get image data: %d",
					ret);
			}
		}

		for (i = 0; i < splash->n_scanout; i++) {
			ret = drm_splash_draw_scanout(splash,
						      &splash->scanout[i],
						      img_data, img_data_len);
			if (ret) {
				drm_err(client->dev,
					"splash: failed to fill scanout %d: %d",
					i, ret);
				continue;
			}

			draw_count++;
		}

		if (release_fn)
			release_fn(priv);

		if (draw_count > 0) {
			ret = drm_client_modeset_commit(client);
			/* If commit returns EBUSY, another master showed up.
			 * This means that the splash is no more required.
			 */
			if (ret == -EBUSY) {
				drm_info(client->dev,
					"splash: not master anymore, exiting");
				break;
			}
		}

		/* If no changes arrived in the mean time, wait to be awaken,
		 * e.g.by a firmware callback.
		 */
		if (atomic_xchg(&splash->pending, 0) == 0)
			set_current_state(TASK_UNINTERRUPTIBLE);

		schedule();
	}

	return 0;
}

static inline void drm_splash_wake_render_thread(struct drm_splash *splash)
{
	atomic_set(&splash->pending, 1);
	wake_up_process(splash->thread);
}

#if IS_ENABLED(CONFIG_DRM_CLIENT_SPLASH_SRC_BMP)
static int drm_splash_fw_get(void *priv, const u8 **data, size_t *size)
{
	const struct firmware *fw = priv;

	if (!fw)
		return -ENODATA;

	*data = fw->data;
	*size = fw->size;

	return 0;
}

static void drm_splash_fw_release(void *priv)
{
	const struct firmware *fw = priv;

	if (fw)
		release_firmware(fw);
}

static void drm_splash_fw_callback(const struct firmware *fw, void *context)
{
	struct drm_splash *splash = context;
	struct drm_client_dev *client = &splash->client;

	if (!fw || !fw->data) {
		drm_err(client->dev, "splash: no firmware");
		return;
	}

	drm_splash_data_source_push(splash, drm_splash_fw_get,
				    drm_splash_fw_release, (void *)fw);

	/* Wake the render thread */
	drm_dbg(client->dev, "splash: firmware loaded, wake up drawing thread");
	drm_splash_wake_render_thread(splash);
}

static int drm_splash_fw_request_bmp(struct drm_splash *splash)
{
	struct drm_client_dev *client = &splash->client;

	drm_info(client->dev, "splash: request %s as firmware", splash_bmp);

	return request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT,
				       splash_bmp, client->dev->dev, GFP_KERNEL,
				       splash, drm_splash_fw_callback);
}
#else
static inline int drm_splash_fw_request_bmp(struct drm_splash *splash)
{
	return -EOPNOTSUPP;
}
#endif // CONFIG_DRM_CLIENT_SPLASH_SRC_BMP

#if IS_ENABLED(CONFIG_DRM_CLIENT_SPLASH_SRC_BGRT)
static int drm_splash_bgrt_get_data(void *priv, const u8 **data, size_t *size)
{
	void *bgrt_image = priv;

	*data = bgrt_image;
	*size = bgrt_image_size;

	return 0;
}

static void drm_splash_bgrt_release(void *priv)
{
	void *bgrt_image = priv;

	if (bgrt_image)
		memunmap(bgrt_image);
}

static int drm_splash_bgrt_load(struct drm_splash *splash)
{
	struct drm_client_dev *client = &splash->client;
	void *bgrt_image = NULL;

	drm_info(client->dev, "splash: using EFI BGRT");

	if (!bgrt_tab.image_address) {
		drm_info(client->dev, "splash: no BGRT found");
		return -ENOENT;
	}

	if (bgrt_tab.status & 0x06) {
		drm_info(client->dev, "splash: BGRT rotation bits set, skipping");
		return -EOPNOTSUPP;
	}

	drm_dbg(client->dev, "splash: BGRT image is at 0x%016llx, size=%zX",
		bgrt_tab.image_address, bgrt_image_size);

	bgrt_image = memremap(bgrt_tab.image_address, bgrt_image_size,
			      MEMREMAP_WB);
	if (!bgrt_image) {
		drm_warn(client->dev, "splash: failed to map BGRT image memory");
		return -ENOMEM;
	}

	drm_splash_data_source_push(splash, drm_splash_bgrt_get_data,
				    drm_splash_bgrt_release, bgrt_image);

	drm_splash_wake_render_thread(splash);

	return 0;
}
#else
static inline int drm_splash_bgrt_load(struct drm_splash *splash)
{
	return -EOPNOTSUPP;
}
#endif // CONFIG_DRM_CLIENT_SPLASH_SRC_BGRT

static int drm_splash_init_client(struct drm_splash *splash)
{
	struct drm_client_dev *client = &splash->client;
	struct drm_mode_set *modeset;
	unsigned int modeset_mask = 0;
	unsigned int fb_count = 0;
	int j;

	if (drm_client_modeset_probe(client, 0, 0))
		return -1;

	j = 0;
	drm_client_for_each_modeset(modeset, client) {
		struct drm_splash_scanout *tmp;
		struct drm_splash_scanout *scanout;
		u32 format;
		int id = -1;

		/* Skip modesets without a mode */
		if (!modeset->mode)
			continue;

		if (modeset->connectors[0]->has_tile) {
			struct drm_splash_scanout *tiled;
			int new_id = modeset->connectors[0]->tile_group->id;

			/* Tiled modesets contribute to a single framebuffer,
			 * check if this tiled group has already been seen.
			 */
			tiled = get_scanout_from_tile_group(splash, new_id);
			if (tiled != NULL) {
				if (!modeset->x)
					tiled->width += modeset->mode->vdisplay;
				if (!modeset->y)
					tiled->height += modeset->mode->hdisplay;
				modeset->fb = tiled->buffer->fb;
				continue;
			}

			/* New tile group, save its ID for later */
			id = new_id;
		}

		format = drm_splash_find_usable_format(modeset->crtc->primary,
						       splash->preferred_format);
		if (format == DRM_FORMAT_INVALID) {
			drm_warn(client->dev,
				 "splash: can't find a usable format for modeset");
			continue;
		}

		tmp = krealloc(splash->scanout,
			       (splash->n_scanout + 1) * sizeof(*splash->scanout),
			       GFP_KERNEL);
		if (!tmp) {
			drm_warn(client->dev,
				 "splash: can't reallocate the scanout array");
			break;
		}

		splash->scanout = tmp;
		scanout = &splash->scanout[splash->n_scanout];
		splash->n_scanout++;

		memset(scanout, 0, sizeof(*scanout));
		scanout->id = id;
		scanout->format = format;
		scanout->width = modeset->mode->hdisplay;
		scanout->height = modeset->mode->vdisplay;

		modeset_mask |= BIT(j);
		j++;
	}

	/* Now that all sensible modesets have been collected, allocate buffers */
	j = 0;
	drm_client_for_each_modeset(modeset, client) {
		struct drm_splash_scanout *scanout;

		if (!(modeset_mask & BIT(j)))
			continue;

		scanout = &splash->scanout[j];
		j++;

		scanout->buffer = drm_client_buffer_create_dumb(client,
								scanout->width,
								scanout->height,
								scanout->format);
		if (IS_ERR(scanout->buffer)) {
			drm_warn(client->dev,
				 "splash: can't create dumb buffer %d %d %p4cc",
				 scanout->width, scanout->height, &scanout->format);
			continue;
		}

		drm_info(client->dev, "splash: created dumb buffer %d %d %p4cc",
			 scanout->width, scanout->height, &scanout->format);

		modeset->fb = scanout->buffer->fb;
		fb_count++;
	}

	return (fb_count == 0) ? -ENODEV : 0;
}

static void drm_splash_free_scanout(struct drm_client_dev *client)
{
	struct drm_splash *splash = client_to_drm_splash(client);
	int i;

	if (splash->n_scanout) {
		for (i = 0; i < splash->n_scanout; i++)
			drm_client_buffer_delete(splash->scanout[i].buffer);

		splash->n_scanout = 0;
		kfree(splash->scanout);
		splash->scanout = NULL;
	}
}

static int drm_splash_client_hotplug(struct drm_client_dev *client)
{
	struct drm_splash *splash = client_to_drm_splash(client);
	int ret;

	guard(mutex)(&splash->hotplug_lock);

	/* The modesets that get a splash are defined at first hotplug event */
	if (splash->initialized)
		return 0;

	ret = drm_splash_init_client(splash);
	if (ret == -ENODEV) {
		drm_info(client->dev, "splash: no modeset found");
		return 0;
	} else if (ret) {
		drm_err(client->dev,
			"splash: failed to init client: %d", ret);
		return ret;
	}

	/* Create the render thread, waken later */
	splash->thread = kthread_create(drm_splash_render_thread,
					splash, "drm_splash_%s",
					client->dev->unique);
	if (IS_ERR(splash->thread)) {
		ret = PTR_ERR(splash->thread);
		drm_err(client->dev, "splash: failed to create render thread: %d", ret);
		drm_splash_free_scanout(client);
		return ret;
	}

	if (IS_ENABLED(CONFIG_DRM_CLIENT_SPLASH_SRC_BMP))
		ret = drm_splash_fw_request_bmp(splash);
	else if (IS_ENABLED(CONFIG_DRM_CLIENT_SPLASH_SRC_BGRT))
		ret = drm_splash_bgrt_load(splash);
	else
		ret = 0;

	if (ret) {
		drm_err(client->dev, "splash: failed to kick image load: %d", ret);
		kthread_stop(splash->thread);
		drm_splash_free_scanout(client);
		return ret;
	}

	/* Wake the render thread to show initial contents */
	drm_splash_wake_render_thread(splash);

	splash->initialized = true;

	return 0;
}

static int drm_splash_client_restore(struct drm_client_dev *client, bool force)
{
	int ret;

	if (force)
		ret = drm_client_modeset_commit_locked(client);
	else
		ret = drm_client_modeset_commit(client);

	return ret;
}

static void drm_splash_client_unregister(struct drm_client_dev *client)
{
	struct drm_splash *splash = client_to_drm_splash(client);

	kthread_stop(splash->thread);
	drm_splash_free_scanout(client);
	drm_client_release(client);

	if (splash->data_release)
		splash->data_release(splash->data_priv);
}

static void drm_splash_client_free(struct drm_client_dev *client)
{
	struct drm_splash *splash = client_to_drm_splash(client);
	struct drm_device *dev = client->dev;

	mutex_destroy(&splash->hotplug_lock);
	kfree(splash);

	drm_dbg(dev, "Unregistered with drm splash");
}

static const struct drm_client_funcs drm_splash_client_funcs = {
	.owner		= THIS_MODULE,
	.hotplug	= drm_splash_client_hotplug,
	.restore	= drm_splash_client_restore,
	.unregister	= drm_splash_client_unregister,
	.free		= drm_splash_client_free,
};

/**
 * drm_splash_register() - Register a drm device to drm_splash
 * @dev: the drm device to register.
 * @format: drm device preferred format.
 */
void drm_splash_register(struct drm_device *dev,
			 const struct drm_format_info *format)
{
	struct drm_splash *splash;

	splash = kzalloc(sizeof(*splash), GFP_KERNEL);
	if (!splash)
		goto err_warn;

	mutex_init(&splash->hotplug_lock);
	spin_lock_init(&splash->data_lock);

	if (format && format->num_planes == 1)
		splash->preferred_format = format->format;
	else
		splash->preferred_format = DRM_FORMAT_RGB888;

	if (drm_client_init(dev, &splash->client, "drm_splash",
			    &drm_splash_client_funcs))
		goto err_free;

	drm_client_register(&splash->client);
	drm_dbg(dev, "Registered with drm splash");

	return;

err_free:
	kfree(splash);
err_warn:
	drm_warn(dev, "Failed to register with drm splash");
}
