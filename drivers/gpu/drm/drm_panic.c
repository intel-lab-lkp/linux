// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright (c) 2023 Jocelyn Falempe <jfalempe@redhat.com>
 * inspired by the drm_log driver from David Herrmann <dh.herrmann@gmail.com>
 * Tux Ascii art taken from cowsay written by Tony Monroe
 */

#include <linux/font.h>
#include <linux/iosys-map.h>
#include <linux/kdebug.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/panic_notifier.h>
#include <linux/types.h>

#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_panic.h>
#include <drm/drm_print.h>


MODULE_AUTHOR("Jocelyn Falempe");
MODULE_DESCRIPTION("DRM PANIC");
MODULE_LICENSE("GPL");

/**
 * DOC: DRM Panic
 *
 * This module displays a user friendly message on screen when a kernel panic
 * occurs. It's useful when using a user-space console instead of fbcon.
 * It's intended for end-user, so have minimal technical/debug information.
 *
 * It will display only one frame, so just clear it, and draw white pixels for
 * the characters. Performance optimizations are low priority as the machine is
 * already in an unusable state.
 */

/*
 * List of active drm devices that can render a panic
 */
struct dpanic_drm_device {
	struct list_head head;
	struct drm_device *dev;
};

struct dpanic_line {
	u32 len;
	const char *txt;
};

#define PANIC_LINE(s) {.len = sizeof(s), .txt = s}

const struct dpanic_line panic_msg[] = {
	PANIC_LINE("KERNEL PANIC !"),
	PANIC_LINE(""),
	PANIC_LINE("Please reboot your computer."),
	PANIC_LINE(""),
};

const struct dpanic_line logo[] = {
	PANIC_LINE("     .--.        _"),
	PANIC_LINE("    |o_o |      | |"),
	PANIC_LINE("    |:_/ |      | |"),
	PANIC_LINE("   //   \\ \\     |_|"),
	PANIC_LINE("  (|     | )     _"),
	PANIC_LINE(" /'\\_   _/`\\    (_)"),
	PANIC_LINE(" \\___)=(___/"),
};

static LIST_HEAD(dpanic_devices);
static DEFINE_MUTEX(dpanic_lock);

#define IOSYS_WRITE8(offset, val) iosys_map_wr(screen_base, offset, u8, val)
/*
 * Only handle DRM_FORMAT_XRGB8888 for testing
 */
static inline void dpanic_draw_px(struct iosys_map *screen_base, size_t offset, u32 pixel_format,
				  u8 r, u8 g, u8 b)
{
	switch (pixel_format) {
	case DRM_FORMAT_XRGB8888:
		IOSYS_WRITE8(offset++, b);
		IOSYS_WRITE8(offset++, g);
		IOSYS_WRITE8(offset++, r);
		IOSYS_WRITE8(offset++, 0xff);
		break;
	default:
		pr_err("Format not supported\n");
		break;
	/* TODO other format */
	}
}

/* Draw a single character at given destination */
static void dpanic_draw_char(char ch, size_t x, size_t y,
			     struct drm_scanout_buffer *sb,
			     const struct font_desc *font)
{
	size_t src_width, src_height, src_stride, i, dst_off;
	const u8 *src;

	src_width = font->width;
	src_height = font->height;
	src_stride = DIV_ROUND_UP(src_width, 8);

	dst_off = x * font->width * sb->format->cpp[0] + y * font->height * sb->pitch;

	src = font->data;
	src += ch * src_height * src_stride;

	while (src_height--) {
		for (i = 0; i < src_width; ++i) {
			/* only draw white pixels */
			if (src[i / 8] & (0x80 >> (i % 8)))
				dpanic_draw_px(&sb->map, dst_off + i * sb->format->cpp[0],
					       sb->format->format, 0xff, 0xff, 0xff);
		}
		src += src_stride;
		dst_off += sb->pitch;
	}
}

static void dpanic_draw_line_centered(const struct dpanic_line *line, size_t y,
				      struct drm_scanout_buffer *sb,
				      const struct font_desc *font)
{
	size_t chars_per_line = sb->width / font->width;
	size_t skip_left, x;

	if (line->len > chars_per_line)
		return;

	skip_left = (chars_per_line - line->len) / 2;

	for (x = 0; x < line->len; x++)
		dpanic_draw_char(line->txt[x], skip_left + x, y, sb, font);
}

/*
 * Draw the Tux logo at the upper left corner
 */
static void dpanic_draw_logo(struct drm_scanout_buffer *sb,
			     const struct font_desc *font)
{
	size_t chars_per_line = sb->width / font->width;
	size_t x, y;

	for (y = 0; y < ARRAY_SIZE(logo); y++)
		for (x = 0; x < logo[y].len && x < chars_per_line; x++)
			dpanic_draw_char(logo[y].txt[x], x, y, sb, font);
}

/*
 * Draw the panic message at the center of the screen
 */
static void dpanic_static_draw(struct drm_scanout_buffer *sb, const char *msg)
{
	size_t y, lines;
	int skip_top;
	size_t len = ARRAY_SIZE(panic_msg);
	struct dpanic_line reason;
	const struct font_desc *font = get_default_font(sb->width, sb->height, 0x8080, 0x8080);

	if (!font)
		return;

	reason.len = strlen(msg);
	reason.txt = msg;

	lines = sb->height / font->height;
	skip_top = (lines - len - 1) / 2;
	if (skip_top < 0)
		return;

	/* clear screen */
	iosys_map_memset(&sb->map, 0, 0, sb->height * sb->pitch);

	for (y = 0; y < len; y++)
		dpanic_draw_line_centered(&panic_msg[y], y + skip_top, sb, font);
	dpanic_draw_line_centered(&reason, y + skip_top, sb, font);

	if (skip_top >= ARRAY_SIZE(logo))
		dpanic_draw_logo(sb, font);
}

static void drm_panic_device(struct drm_device *dev, const char *msg)
{
	struct drm_scanout_buffer sb;

	if (dev->driver->get_scanout_buffer(dev, &sb))
		return;

	dpanic_static_draw(&sb, msg);
}

static int drm_panic(struct notifier_block *this, unsigned long event,
		     void *ptr)
{
	struct dpanic_drm_device *dpanic_device;

	list_for_each_entry(dpanic_device, &dpanic_devices, head) {
		drm_panic_device(dpanic_device->dev, ptr);
	}
	return NOTIFY_OK;
}

struct notifier_block drm_panic_notifier = {
	.notifier_call = drm_panic,
};

/**
 * drm_panic_register() - Initialize DRM panic for a device
 * @dev: the DRM device on which the panic screen will be displayed.
 */
void drm_panic_register(struct drm_device *dev)
{
	struct dpanic_drm_device *new;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return;

	new->dev = dev;
	list_add_tail(&new->head, &dpanic_devices);

	drm_info(dev, "Registered with drm panic\n");
}
EXPORT_SYMBOL(drm_panic_register);

/**
 * drm_panic_unregister()
 * @dev: the DRM device previously registered.
 */
void drm_panic_unregister(struct drm_device *dev)
{
	struct dpanic_drm_device *dpanic_device;
	struct dpanic_drm_device *found = NULL;

	list_for_each_entry(dpanic_device, &dpanic_devices, head) {
		if (dpanic_device->dev == dev)
			found = dpanic_device;
	}
	if (found) {
		list_del(&found->head);
		kfree(found);
		drm_info(dev, "Unregistered with drm panic\n");
	}
}
EXPORT_SYMBOL(drm_panic_unregister);

/**
 * drm_panic_init() - Initialize drm-panic subsystem
 *
 * register the panic notifier
 */
void drm_panic_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &drm_panic_notifier);
}

/**
 * drm_log_exit() - Shutdown drm-panic subsystem
 */
void drm_panic_exit(void)
{
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &drm_panic_notifier);
}
