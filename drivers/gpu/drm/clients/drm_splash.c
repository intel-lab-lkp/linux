// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright (c) 2025 Francesco Valla <francesco@valla.it>
 *
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/font.h>
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

// TODO: determine proper size for max message length
#define DRM_SPLASH_MAX_MSG_LEN 128

static char *message;
module_param(message, charp, 0400);
MODULE_PARM_DESC(message, "Initial message, up to " __stringify(DRM_SPLASH_MAX_MSG_LEN) " chars");

#ifdef CONFIG_DRM_CLIENT_SPLASH_LOAD_AS_FW
static bool skip_image;
module_param(skip_image, bool, 0400);
MODULE_PARM_DESC(skip_image, "Do not try to load splash image (default: false)");
#endif

/**
 * DOC: overview
 *
 * This is a simple graphic bootsplash.
 * Images to be shown are loaded as firmware.
 */

struct drm_splash_scanout {
	int id;
	u32 format;
	unsigned int width;
	unsigned int height;
	struct drm_client_buffer *buffer;

	struct mutex lock;
	const struct font_desc *font;
	bool bg_drawn;
	bool message_drawn;

#ifdef CONFIG_DRM_CLIENT_SPLASH_LOAD_AS_FW
	const struct firmware *fw;
#endif
};

struct drm_splash {
	struct drm_client_dev client;
	u32 preferred_format;
	struct device dev;

	struct mutex lock;
	struct task_struct *thread;
	atomic_t pending;
	bool initialized;

	char message[DRM_SPLASH_MAX_MSG_LEN];
	u8 progress;

	u32 n_scanout;
	struct drm_splash_scanout *scanout;
};

static struct drm_splash *client_to_drm_splash(struct drm_client_dev *client)
{
	return container_of_const(client, struct drm_splash, client);
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

	/* If preferred format is not set, use RGB888 (which offers full colors
	 * with minimal occupation).
	 */
	if (preferred_format == 0)
		preferred_format = DRM_FORMAT_RGB888;

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

static void drm_splash_blit(struct iosys_map *dst, unsigned int dst_pitch,
			    const u8 *src, unsigned int src_pitch,
			    u32 height, u32 width, u32 px_width, u32 color)
{
	switch (px_width) {
	case 2:
		drm_draw_blit16(dst, dst_pitch, src, src_pitch, height, width, 1, color);
		break;
	case 3:
		drm_draw_blit24(dst, dst_pitch, src, src_pitch, height, width, 1, color);
		break;
	case 4:
		drm_draw_blit32(dst, dst_pitch, src, src_pitch, height, width, 1, color);
		break;
	default:
		WARN_ONCE(1, "Can't blit with pixel width %d\n", px_width);
	}
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
		WARN_ONCE(1, "Can't fill with pixel width %d\n", px_width);
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

	return drm_client_framebuffer_flush(buffer, &r);
}

#ifdef CONFIG_DRM_CLIENT_SPLASH_LOAD_AS_FW
static int drm_splash_fill_from_data(struct drm_client_buffer *buffer,
				     const u8 *data, size_t data_len)
{
	struct drm_client_dev *client = buffer->client;
	struct drm_framebuffer *fb = buffer->fb;
	struct drm_rect r = DRM_RECT_INIT(0, 0, fb->width, fb->height);
	struct iosys_map map;
	size_t buffer_size;
	int ret;

	buffer_size = fb->width * fb->height * fb->format->cpp[0];
	if (data_len != buffer_size) {
		drm_err(client->dev,
			"splash: data size mismatch (expected %zu, got %zu)",
			data_len, buffer_size);
		return -ENODATA;
	}

	ret = drm_client_buffer_vmap_local(buffer, &map);
	if (ret) {
		drm_err(client->dev, "splash: cannot vmap buffer: %d", ret);
		return ret;
	}

	iosys_map_memcpy_to(&map, 0, data, data_len);

	drm_client_buffer_vunmap_local(buffer);

	return drm_client_framebuffer_flush(buffer, &r);
}
#endif

static int drm_splash_draw_bar_message(struct drm_splash_scanout *scanout,
				       const char *msg,
				       unsigned int progress,
				       u32 bg_color,
				       u32 fg_color)
{
	struct drm_framebuffer *fb = scanout->buffer->fb;
	const struct font_desc *font = scanout->font;
	size_t font_pitch = DIV_ROUND_UP(font->width, 8);
	u32 px_width = fb->format->cpp[0];
	unsigned int y_padding = 2;
	struct drm_rect r = DRM_RECT_INIT(0, fb->height * 3 / 4 - y_padding,
					  fb->width, font->height + y_padding);
	unsigned int fill_width = drm_rect_width(&r) * progress / 100;
	struct iosys_map map;
	const u8 *src;
	size_t i, len;

	/* Clamp len if required */
	len = min(strlen(msg), drm_rect_width(&r) / font->width);

	if (drm_client_buffer_vmap_local(scanout->buffer, &map))
		return -1;

	/* Draw progress bar */
	iosys_map_incr(&map, r.y1 * fb->pitches[0]);
	drm_splash_fill(&map, fb->pitches[0], drm_rect_height(&r),
			drm_rect_width(&r), px_width, bg_color);
	drm_splash_fill(&map, fb->pitches[0], drm_rect_height(&r),
			fill_width, px_width, fg_color);

	/* Center the message horizontally */
	iosys_map_incr(&map, y_padding * fb->pitches[0]);
	iosys_map_incr(&map, (drm_rect_width(&r) - (font->width * len)) * px_width / 2);

	/* Write message */
	for (i = 0; i < len; i++) {
		unsigned int ch_x;

		src = drm_draw_get_char_bitmap(font, msg[i], font_pitch);

		/* Use background color over fill bar, foreground otherwise */
		ch_x = (drm_rect_width(&r) - font->width * len) / 2 + i * font->width;
		drm_splash_blit(&map, fb->pitches[0], src, font_pitch,
				font->height, font->width, px_width,
				(fill_width > ch_x) ? bg_color : fg_color);
		iosys_map_incr(&map, font->width * px_width);
	}

	drm_client_buffer_vunmap_local(scanout->buffer);
	drm_client_framebuffer_flush(scanout->buffer, &r);

	return 0;
}

static int drm_splash_draw_scanout(struct drm_splash_scanout *scanout,
				   const char *msg, unsigned int progress)
{
	u32 bg_color = drm_draw_color_from_xrgb8888(CONFIG_DRM_CLIENT_SPLASH_BACKGROUND_COLOR,
						    scanout->format);
	u32 fg_color = drm_draw_color_from_xrgb8888(CONFIG_DRM_CLIENT_SPLASH_FOREGROUND_COLOR,
						    scanout->format);
	int ret = -ENOENT;

	if (!scanout->buffer)
		return -ENODEV;

#ifdef CONFIG_DRM_CLIENT_SPLASH_LOAD_AS_FW
	if (!skip_image) {
		const struct firmware *fw = NULL;

		scoped_guard(mutex, &scanout->lock) {
			fw = scanout->fw;
			scanout->fw = NULL;
		}

		if (fw) {
			ret = drm_splash_fill_from_data(scanout->buffer,
							fw->data, fw->size);
			release_firmware(fw);

			if (ret == 0)
				scanout->bg_drawn = true;
		}
	}
#endif

	/* If no firmware has been used to fill the screen (either by choice of
	 * because it's unavailable) fill the screen with the background color.
	 *
	 */
	if (!scanout->bg_drawn) {
		drm_splash_fill_solid_color(scanout->buffer, bg_color);
		scanout->bg_drawn = true;
	}

	/* If message is empty and no previous message was shown, there is
	 * nothing to do
	 */
	if (scanout->message_drawn || strlen(msg) != 0 || progress != 0) {
		ret = drm_splash_draw_bar_message(scanout, msg, progress,
						  bg_color, fg_color);
		if (ret)
			return ret;

		scanout->message_drawn = true;
	}

	return 0;
}

static int drm_splash_render_thread(void *data)
{
	struct drm_splash *splash = data;
	struct drm_client_dev *client = &splash->client;
	char buf[sizeof(splash->message)];
	unsigned int progress;

	while (!kthread_should_stop()) {
		unsigned int draw_count = 0;
		int j, ret;

		/* Copy message and progress to be drawn, to avoid locking for
		 * too much time and/or showing different contents on different
		 * screens.
		 */
		scoped_guard(mutex, &splash->lock) {
			strscpy(buf, splash->message);
			progress = splash->progress;
		}

		for (j = 0; j < splash->n_scanout; j++) {
			ret = drm_splash_draw_scanout(&splash->scanout[j], buf,
						      progress);
			if (ret) {
				drm_err(client->dev,
					"splash: failed to fill scanout %d: %d",
					j, ret);
				continue;
			}

			draw_count++;
		}

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

		/* If no changes arrived in the mean time, wait to be awaken by
		 * a sysfs write, a firmware callback or a stop command.
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
		mutex_init(&scanout->lock);

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

		scanout->buffer = drm_client_framebuffer_create(client,
								scanout->width,
								scanout->height,
								scanout->format);
		if (IS_ERR(scanout->buffer)) {
			drm_warn(client->dev,
				 "splash: can't create framebuffer %d %d %p4cc",
				 scanout->width, scanout->height, &scanout->format);
			continue;
		}

		drm_info(client->dev, "splash: created framebuffer %d %d %p4cc",
			 scanout->width, scanout->height, &scanout->format);

		scanout->font = get_default_font(scanout->width, scanout->height,
						 NULL, NULL);
		if (!scanout->font) {
			drm_warn(client->dev,
				 "splash: failed to get default font");
		}

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
		for (i = 0; i < splash->n_scanout; i++) {
			drm_client_framebuffer_delete(splash->scanout[i].buffer);
#ifdef CONFIG_DRM_CLIENT_SPLASH_LOAD_AS_FW
			if (splash->scanout[i].fw)
				release_firmware(splash->scanout[i].fw);
#endif
			mutex_destroy(&splash->scanout[i].lock);
		}
		splash->n_scanout = 0;
		kfree(splash->scanout);
		splash->scanout = NULL;
	}
}

#ifdef CONFIG_DRM_CLIENT_SPLASH_LOAD_AS_FW
static void drm_splash_fw_callback(const struct firmware *fw, void *context)
{
	struct drm_splash_scanout *scanout = context;
	struct drm_client_dev *client = scanout->buffer->client;
	struct drm_splash *splash = client_to_drm_splash(client);

	if (!fw || !fw->data) {
		drm_err(client->dev, "splash: no firmware");
		return;
	}

	/* Assign new firmware to the scanout */
	scoped_guard(mutex, &scanout->lock) {
		if (scanout->fw)
			release_firmware(scanout->fw);
		scanout->fw = fw;
	}

	/* Wake the render thread */
	drm_dbg(client->dev, "splash: firmware loaded, wake up drawing thread");
	drm_splash_wake_render_thread(splash);
}

static int drm_splash_kick_fw_load(struct drm_splash *splash,
				   struct task_struct *thread)
{
	struct drm_client_dev *client = &splash->client;
	int j;

	for (j = 0; j < splash->n_scanout; j++) {
		struct drm_splash_scanout *scanout = &splash->scanout[j];
		char *fw_name = kasprintf(GFP_KERNEL,
					  "drm_splash_%ux%u_%.4s.raw",
					  scanout->width, scanout->height,
					  (const char *)&scanout->format);
		if (!fw_name)
			return -ENOMEM;

		drm_dbg(client->dev, "splash: request firmware %s", fw_name);
		request_firmware_nowait(THIS_MODULE, FW_ACTION_NOUEVENT, fw_name,
					&splash->dev, GFP_KERNEL,
					scanout, drm_splash_fw_callback);
		kfree(fw_name);
	}

	return 0;
}
#endif /* CONFIG_DRM_CLIENT_SPLASH_LOAD_AS_FW */

static int drm_splash_client_hotplug(struct drm_client_dev *client)
{
	struct drm_splash *splash = client_to_drm_splash(client);
	int ret;

	guard(mutex)(&splash->lock);

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

#ifdef CONFIG_DRM_CLIENT_SPLASH_LOAD_AS_FW
	if (!skip_image) {
		ret = drm_splash_kick_fw_load(splash, splash->thread);
		if (ret) {
			drm_err(client->dev, "splash: failed to kick fw load: %d", ret);
			kthread_stop(splash->thread);
			drm_splash_free_scanout(client);
			return ret;
		}
	}
#endif

	/* Wake the render thread to show initial contents */
	drm_splash_wake_render_thread(splash);

	splash->initialized = true;

	return 0;
}

static void drm_splash_client_unregister(struct drm_client_dev *client)
{
	struct drm_splash *splash = client_to_drm_splash(client);
	struct drm_device *dev = client->dev;

	kthread_stop(splash->thread);
	device_del(&splash->dev);
	drm_splash_free_scanout(client);
	drm_client_release(client);
	put_device(&splash->dev);
	kfree(splash);
	drm_dbg(dev, "Unregistered with drm splash");
}

static const struct drm_client_funcs drm_splash_client_funcs = {
	.owner		= THIS_MODULE,
	.hotplug	= drm_splash_client_hotplug,
	.unregister	= drm_splash_client_unregister,
};

static ssize_t progress_store(struct device *device,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	struct drm_splash *splash = dev_get_drvdata(device);
	u8 progress;
	int ret;

	ret = kstrtou8(buf, 0, &progress);
	if (ret)
		return ret;

	if (ret > 100)
		return -ERANGE;

	scoped_guard(mutex, &splash->lock)
		splash->progress = progress;

	drm_splash_wake_render_thread(splash);

	return count;
}
DEVICE_ATTR_WO(progress);

static ssize_t message_store(struct device *device,
			     struct device_attribute *attr,
			     const char *buf,
			     size_t count)
{
	struct drm_splash *splash = dev_get_drvdata(device);
	size_t len = min(count, sizeof(splash->message));

	scoped_guard(mutex, &splash->lock)
		strscpy(splash->message, buf, len);

	drm_splash_wake_render_thread(splash);

	return count;
}
DEVICE_ATTR_WO(message);

static ssize_t stop_store(struct device *device,
			  struct device_attribute *attr,
			  const char *buf,
			  size_t count)
{
	struct drm_splash *splash = dev_get_drvdata(device);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	if (val != 0)
		kthread_stop(splash->thread);

	return count;
}
DEVICE_ATTR_WO(stop);

static struct attribute *drm_splash_attrs[] = {
	&dev_attr_message.attr,
	&dev_attr_progress.attr,
	&dev_attr_stop.attr,
	NULL
};
ATTRIBUTE_GROUPS(drm_splash);

/**
 * drm_splash_register() - Register a drm device to drm_splash
 * @dev: the drm device to register.
 * @format: drm device preferred format.
 */
void drm_splash_register(struct drm_device *dev,
			 const struct drm_format_info *format)
{
	struct drm_splash *splash;
	int ret;

	splash = kzalloc(sizeof(*splash), GFP_KERNEL);
	if (!splash)
		goto err_warn;

	mutex_init(&splash->lock);
	if (format && format->num_planes == 1)
		splash->preferred_format = format->format;

	if (message)
		strscpy(splash->message, message);

	if (drm_client_init(dev, &splash->client, "drm_splash",
			    &drm_splash_client_funcs))
		goto err_free;

	device_initialize(&splash->dev);
	splash->dev.parent = dev->dev;
	splash->dev.groups = drm_splash_groups;
	dev_set_name(&splash->dev, "drm_splash");
	dev_set_drvdata(&splash->dev, splash);
	ret = device_add(&splash->dev);
	if (ret)
		goto err_free;

	drm_client_register(&splash->client);
	drm_dbg(dev, "Registered with drm splash");

	return;

err_free:
	kfree(splash);
err_warn:
	drm_warn(dev, "Failed to register with drm splash");
}
