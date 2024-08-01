// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright (c) 2024 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
 */

#include <linux/circ_buf.h>
#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/iosys-map.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <drm/drm_client.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>

#include "drm_draw.h"
#include "drm_log.h"

MODULE_AUTHOR("Jocelyn Falempe");
MODULE_DESCRIPTION("DRM boot logger");
MODULE_LICENSE("GPL");

/**
 * DOC: overview
 *
 * This is a simple graphic logger, to print the kernel message on screen, until
 * a userspace application is able to take over.
 * It is only for debugging purpose.
 */

struct drm_log_scanout {
	struct drm_client_buffer *buffer;
	const struct font_desc *font;
	struct iosys_map map;
	u32 rows;
	u32 columns;
	u32 line;
	u32 format;
	u32 px_width;
	u32 front_color;
};

struct drm_log_client {
	struct list_head head;
	struct drm_client_dev client;
	u32 n_scanout;
	struct drm_log_scanout *scanout;
};

static LIST_HEAD(drm_log_clients);
static DEFINE_MUTEX(drm_log_lock);

/*
 * A circular buffer, with the last kmsg logs to print.
 * 8K is more than what can be drawn on most monitors.
 */
#define CIRC_BUF_SIZE	(1 << 13)
#define CIRC_BUF_MASK	(CIRC_BUF_SIZE - 1)
struct circ_buf drm_log_buf;
static DEFINE_SPINLOCK(drm_log_writer_lock);
static DEFINE_SPINLOCK(drm_log_reader_lock);

/*
 * Circular buffer functions
 */

/* Read one line of kmsg, and update the tail pointer */
static unsigned int drm_log_buf_read(char *s, unsigned int size)
{
	spin_lock(&drm_log_reader_lock);
	/* Read index before reading contents at that index. */
	unsigned long head = smp_load_acquire(&drm_log_buf.head);
	unsigned long tail = drm_log_buf.tail;
	unsigned int len = 0;
	unsigned int i;

	if (CIRC_CNT(head, tail, CIRC_BUF_SIZE) >= 1) {
		/* take one line of text */
		for (i = 0; ((i + tail) & CIRC_BUF_MASK) != head && len < size; i++) {
			char c = drm_log_buf.buf[(i + tail) & CIRC_BUF_MASK];

			if (c == '\n') {
				i++;
				break;
			}
			s[i] = c;
			len++;
		}
		/* Finish reading descriptor before incrementing tail. */
		smp_store_release(&drm_log_buf.tail,
				  (tail + i) & CIRC_BUF_MASK);
	}
	spin_unlock(&drm_log_reader_lock);
	return len;
}

/* Move the tail pointer at least len bytes, to make room */
static void drm_log_buf_drop(unsigned int len)
{
	spin_lock(&drm_log_reader_lock);
	/* Read index before reading contents at that index. */
	unsigned long head = smp_load_acquire(&drm_log_buf.head);
	unsigned long tail = drm_log_buf.tail;
	unsigned long count = CIRC_CNT(head, tail, CIRC_BUF_SIZE);
	int i;

	/* Drop the oldest logs, stop at line boundary if possible */
	for (i = len; i < count; i++) {
		char c = drm_log_buf.buf[(i + tail) & CIRC_BUF_MASK];

		if (c == '\n') {
			i++;
			break;
		}
	}
	/* Finish reading descriptor before incrementing tail. */
	smp_store_release(&drm_log_buf.tail,
			  (tail + i) & CIRC_BUF_MASK);

	spin_unlock(&drm_log_reader_lock);
}

static bool drm_log_buf_write(const char *s, unsigned int len)
{
	bool written = false;

	spin_lock(&drm_log_writer_lock);
	unsigned long head = drm_log_buf.head;
	/* The spin_unlock() and next spin_lock() provide needed ordering. */
	unsigned long tail = READ_ONCE(drm_log_buf.tail);

	if (CIRC_SPACE(head, tail, CIRC_BUF_SIZE) >= len) {
		int i;

		for (i = 0; i < len; i++)
			drm_log_buf.buf[(head + i) & CIRC_BUF_MASK] = s[i];

		/* Update the head pointer, and make sure it is visible before releasing the lock */
		smp_store_release(&drm_log_buf.head,
				  (head + len) & CIRC_BUF_MASK);
		written = true;
	}
	spin_unlock(&drm_log_writer_lock);
	return written;
}

static void drm_log_clear_line(struct drm_log_scanout *scanout, u32 line)
{
	struct drm_framebuffer *fb = scanout->buffer->fb;
	unsigned long height = scanout->font->height;
	struct drm_rect r = DRM_RECT_INIT(0, line * height, fb->width, height);

	iosys_map_memset(&scanout->map, r.y1 * fb->pitches[0], 0, height * fb->pitches[0]);
	drm_client_framebuffer_flush(scanout->buffer, &r);
}

static void drm_log_blit(struct iosys_map *dst, unsigned int dst_pitch,
			 const u8 *src, unsigned int src_pitch,
			 u32 height, u32 width, u32 scale, u32 px_width, u32 color)
{
	switch (px_width) {
	case 2:
		drm_draw_blit16(dst, dst_pitch, src, src_pitch, height, width, scale, color);
		break;
	case 3:
		drm_draw_blit24(dst, dst_pitch, src, src_pitch, height, width, scale, color);
		break;
	case 4:
		drm_draw_blit32(dst, dst_pitch, src, src_pitch, height, width, scale, color);
		break;
	default:
		WARN_ONCE(1, "Can't blit with pixel width %d\n", px_width);
	}
}

static void drm_log_draw_line(struct drm_log_scanout *scanout, const char *s, unsigned int len)
{
	struct drm_framebuffer *fb = scanout->buffer->fb;
	struct iosys_map map = scanout->map;
	const struct font_desc *font = scanout->font;
	size_t font_pitch = DIV_ROUND_UP(font->width, 8);
	const u8 *src;
	u32 px_width = fb->format->cpp[0];
	struct drm_rect r = DRM_RECT_INIT(0, scanout->line * font->height,
					  fb->width, (scanout->line + 1) * font->height);
	u32 i;

	iosys_map_incr(&map, r.y1 * fb->pitches[0]);
	for (i = 0; i < len && i < scanout->columns; i++) {
		src = drm_draw_get_char_bitmap(font, s[i], font_pitch);
		drm_log_blit(&map, fb->pitches[0], src, font_pitch, font->height, font->width,
			     1, px_width, scanout->front_color);
		iosys_map_incr(&map, font->width * px_width);
	}
	drm_client_framebuffer_flush(scanout->buffer, &r);

	scanout->line++;
	if (scanout->line >= scanout->rows)
		scanout->line = 0;
}

static void drm_log_draw_new_line(struct drm_log_scanout *scanout,
				  const char *s, unsigned int len)
{
	if (scanout->line == 0) {
		drm_log_clear_line(scanout, 0);
		drm_log_clear_line(scanout, 1);
		drm_log_clear_line(scanout, 2);
	} else if (scanout->line + 2 < scanout->rows)
		drm_log_clear_line(scanout, scanout->line + 2);

	drm_log_draw_line(scanout, s, len);
}

static void drm_log_draw_kmsg_record(struct drm_log_scanout *scanout,
				     const char *s, unsigned int len)
{
	while (len > scanout->columns) {
		drm_log_draw_new_line(scanout, s, scanout->columns);
		s += scanout->columns;
		len -= scanout->columns;
	}
	if (len)
		drm_log_draw_new_line(scanout, s, len);
}

static void drm_log_draw_all(const char *s, unsigned int len)
{
	struct drm_log_client *dclient;
	int i;

	mutex_lock(&drm_log_lock);
	list_for_each_entry(dclient, &drm_log_clients, head)
		for (i = 0; i < dclient->n_scanout; i++)
			drm_log_draw_kmsg_record(&dclient->scanout[i], s, len);
	mutex_unlock(&drm_log_lock);
}

static bool drm_log_active_output(void)
{
	struct drm_log_client *dclient;
	bool active = false;

	mutex_lock(&drm_log_lock);
	list_for_each_entry(dclient, &drm_log_clients, head) {
		if (dclient->n_scanout)
			active = true;
	}
	mutex_unlock(&drm_log_lock);
	return active;
}

static bool drm_log_work_draw(void)
{
	unsigned int len;
	char buf[512];

	len = drm_log_buf_read(buf, sizeof(buf));
	if (len)
		drm_log_draw_all(buf, len);
	return len != 0;
}

static void drm_log_work_fn(struct work_struct *work);
DECLARE_WORK(drm_log_work, drm_log_work_fn);

static void drm_log_work_fn(struct work_struct *work)
{
	if (!drm_log_active_output())
		return;

	if (drm_log_work_draw())
		schedule_work(&drm_log_work);
}

static u32 drm_log_find_usable_format(struct drm_plane *plane)
{
	int i;

	for (i = 0; i < plane->format_count; i++)
		if (drm_draw_color_from_xrgb8888(0xffffff, plane->format_types[i]) != 0)
			return plane->format_types[i];
	return DRM_FORMAT_INVALID;
}

static int drm_log_setup_modeset(struct drm_client_dev *client,
				 struct drm_mode_set *mode_set,
				 struct drm_log_scanout *scanout)
{
	struct drm_crtc *crtc = mode_set->crtc;
	u32 width = mode_set->mode->hdisplay;
	u32 height = mode_set->mode->vdisplay;
	u32 format;
	int ret;

	scanout->font = get_default_font(width, height, NULL, NULL);
	if (!scanout->font)
		return -ENOENT;

	format = drm_log_find_usable_format(crtc->primary);
	if (format == DRM_FORMAT_INVALID)
		return -EINVAL;

	scanout->buffer = drm_client_framebuffer_create(client, width, height, format);
	if (IS_ERR(scanout->buffer)) {
		drm_warn(client->dev, "drm_log can't create framebuffer %d %d %p4cc\n",
			 width, height, &format);
		return -ENOMEM;
	}
	ret = drm_client_buffer_vmap(scanout->buffer, &scanout->map);
	if (ret) {
		drm_warn(client->dev, "drm_log, unable to vmap framebuffer\n");
		drm_client_framebuffer_delete(scanout->buffer);
		return -ENOMEM;
	}
	mode_set->fb = scanout->buffer->fb;
	scanout->rows = height / scanout->font->height;
	scanout->columns = width / scanout->font->width;
	scanout->front_color = drm_draw_color_from_xrgb8888(0xffffff, format);
	return 0;
}

static void drm_log_init_client(struct drm_log_client *dclient)
{
	struct drm_client_dev *client = &dclient->client;
	int i, n_modeset;
	struct drm_mode_set *mode_set;

	if (drm_client_modeset_probe(client, 0, 0))
		return;

	n_modeset = 0;
	drm_client_for_each_modeset(mode_set, client)
		n_modeset++;

	dclient->scanout = kcalloc(n_modeset, sizeof(*dclient->scanout), GFP_KERNEL);
	n_modeset = 0;
	drm_client_for_each_modeset(mode_set, client) {
		if (drm_log_setup_modeset(client, mode_set, &dclient->scanout[n_modeset]))
			continue;
		n_modeset++;
	}
	if (n_modeset == 0)
		goto err_nomodeset;

	if (drm_client_modeset_commit(client))
		goto err_failed_commit;

	dclient->n_scanout = n_modeset;
	return;

err_failed_commit:
	for (i = 0; i < n_modeset; i++) {
		drm_client_buffer_vunmap(dclient->scanout[i].buffer);
		drm_client_framebuffer_delete(dclient->scanout[i].buffer);
	}
err_nomodeset:
	kfree(dclient->scanout);
	dclient->scanout = NULL;
}

static struct drm_log_client *to_drm_log_client(struct drm_client_dev *client)
{
	return container_of(client, struct drm_log_client, client);
}

static void drm_log_client_unregister(struct drm_client_dev *client)
{
	struct drm_log_client *dclient = to_drm_log_client(client);
	int i;

	mutex_lock(&drm_log_lock);
	if (dclient->n_scanout) {
		for (i = 0; i < dclient->n_scanout; i++) {
			drm_client_buffer_vunmap(dclient->scanout[i].buffer);
			drm_client_framebuffer_delete(dclient->scanout[i].buffer);
		}
		dclient->n_scanout = 0;
	}
	mutex_unlock(&drm_log_lock);
	drm_client_release(client);
}

static int drm_log_client_hotplug(struct drm_client_dev *client)
{
	struct drm_log_client *dclient = to_drm_log_client(client);

	/* TODO handle hotplug event when it's already initialized */
	if (!dclient->n_scanout)
		drm_log_init_client(dclient);
	return 0;
}

static const struct drm_client_funcs drm_log_client_funcs = {
	.owner		= THIS_MODULE,
	.unregister	= drm_log_client_unregister,
	.hotplug	= drm_log_client_hotplug,
};

/**
 * drm_log_register() - Register a drm device to drm_log
 * @dev: the drm device to register.
 */
void drm_log_register(struct drm_device *dev)
{
	struct drm_log_client *new;

	new = kzalloc(sizeof(*new), GFP_KERNEL);

	if (drm_client_init(dev, &new->client, "drm_log", &drm_log_client_funcs)) {
		kfree(new);
		return;
	}
	drm_client_register(&new->client);
	mutex_lock(&drm_log_lock);
	list_add(&new->head, &drm_log_clients);
	mutex_unlock(&drm_log_lock);
	drm_info(dev, "Registered with drm log\n");
	/* Kick the worker thread, as the circular buffer can already contain some messages */
	schedule_work(&drm_log_work);
}

/**
 * drm_log_unregister()
 * @dev: the drm device previously registered.
 */
void drm_log_unregister(struct drm_device *dev)
{
	struct drm_log_client *dclient, *found;

	mutex_lock(&drm_log_lock);
	list_for_each_entry(dclient, &drm_log_clients, head) {
		if (dclient->client.dev == dev)
			found = dclient;
	}

	if (found) {
		list_del(&found->head);
		kfree(found->scanout);
		kfree(found);
		drm_info(dev, "Unregistered with drm log\n");
	}
	mutex_unlock(&drm_log_lock);
}

static void drm_log_con_write(struct console *con, const char *s, unsigned int len)
{
	while (!drm_log_buf_write(s, len))
		drm_log_buf_drop(len);
	schedule_work(&drm_log_work);
}

static struct console drm_log_console = {
	.name		= "drm_log",
	.write		= drm_log_con_write,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
};

static __init int drm_log_init(void)
{
	drm_log_buf.buf = kmalloc(CIRC_BUF_SIZE, GFP_KERNEL);
	if (!drm_log_buf.buf)
		return -ENOMEM;

	register_console(&drm_log_console);
	return 0;
}

module_init(drm_log_init);
