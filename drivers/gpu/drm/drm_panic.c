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
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_panic.h>
#include <drm/drm_print.h>


MODULE_AUTHOR("Jocelyn Falempe");
MODULE_DESCRIPTION("DRM panic handler");
MODULE_LICENSE("GPL");

/**
 * DOC: DRM Panic
 *
 * This module displays a user friendly message on screen when a kernel panic
 * occurs. This is conflicting with fbcon, so you can only enable it when fbcon
 * is disabled.
 * It's intended for end-user, so have minimal technical/debug information.
 */

/*
 * Implementation details:
 *
 * It is a panic handler, so it can't take lock, allocate memory, run tasks/irq,
 * or attempt to sleep. It's a best effort, and it may not be able to display
 * the message in all situations (like if the panic occurs in the middle of a
 * modesetting).
 * It will display only one static frame, so performance optimizations are low
 * priority as the machine is already in an unusable state.
 * The frame is drawn line by line, to allow easier conversion to the fb format.
 */

/*
 * List of active drm devices that can render a panic
 */
struct drm_panic_device {
	struct list_head head;
	struct drm_device *dev;
};

struct drm_panic_line {
	u32 len;
	const char *txt;
};

#define PANIC_LINE(s) {.len = sizeof(s) - 1, .txt = s}

struct drm_panic_line panic_msg[] = {
	PANIC_LINE("KERNEL PANIC !"),
	PANIC_LINE(""),
	PANIC_LINE("Please reboot your computer."),
	PANIC_LINE(""),
	PANIC_LINE(""), /* overwritten with panic reason */
};

const struct drm_panic_line logo[] = {
	PANIC_LINE("     .--.        _"),
	PANIC_LINE("    |o_o |      | |"),
	PANIC_LINE("    |:_/ |      | |"),
	PANIC_LINE("   //   \\ \\     |_|"),
	PANIC_LINE("  (|     | )     _"),
	PANIC_LINE(" /'\\_   _/`\\    (_)"),
	PANIC_LINE(" \\___)=(___/"),
};

static LIST_HEAD(drm_panic_devices);
static DEFINE_MUTEX(drm_panic_lock);

/*
 * This buffer is used to convert xrgb8888 to the scanout buffer format.
 * It is allocated when the first client register, and freed when the last client
 * unregisters.
 * There is no need for mutex, as the panic garantee that only 1 CPU is still
 * running, and preemption is disabled.
 */
#define DRM_PANIC_MAX_WIDTH 8096
static u32 *drm_panic_line_buf;

static u32 fg_color;
static u32 bg_color;

static void draw_line(const struct drm_panic_line *msg, size_t left_margin,
		      size_t l, size_t width, const struct font_desc *font)
{
	size_t x, i;
	const u8 *src;
	size_t src_stride = DIV_ROUND_UP(font->width, 8);
	u32 *dst = drm_panic_line_buf;

	for (x = 0; x < left_margin * font->width; x++)
		*dst++ = bg_color;

	for (i = 0; i < msg->len; i++) {
		src = font->data + (msg->txt[i] * font->height + l) * src_stride;
		drm_fb_r1_to_32bit_line(dst, src, font->width, fg_color, bg_color);
		dst += font->width;
	}
	for (x = (left_margin + msg->len) * font->width; x < width ; x++)
		*dst++ = bg_color;
}

static void draw_txt_line(const struct drm_panic_line *msg, size_t left_margin, size_t y,
			  struct drm_scanout_buffer *sb,
			  const struct font_desc *font,
			  void (*convert)(void *dbuf, const void *sbuf, unsigned int npixels))
{
	size_t dst_off;
	size_t l;

	for (l = 0; l < font->height; l++) {
		draw_line(msg, left_margin, l, sb->width, font);
		if (convert)
			convert(drm_panic_line_buf, drm_panic_line_buf, sb->width);

		dst_off = (y * font->height + l) * sb->pitch;
		iosys_map_memcpy_to(&sb->map, dst_off, drm_panic_line_buf,
				    sb->width * sb->format->cpp[0]);
	}
}

static void draw_empty_lines(struct drm_scanout_buffer *sb,
			     size_t start_line,
			     size_t len,
			     void (*convert)(void *dbuf, const void *sbuf, unsigned int npixels))
{
	size_t i, dst_off;
	u32 *dst = drm_panic_line_buf;

	for (i = 0; i < sb->width; i++)
		*dst++ = bg_color;

	if (convert)
		convert(drm_panic_line_buf, drm_panic_line_buf, sb->width);

	for (i = 0; i < len; i++) {
		dst_off = (start_line + i) * sb->pitch;
		iosys_map_memcpy_to(&sb->map, dst_off, drm_panic_line_buf,
				    sb->width * sb->format->cpp[0]);
	}
}

static size_t panic_msg_needed_lines(size_t chars_per_line)
{
	size_t msg_len = ARRAY_SIZE(panic_msg);
	size_t lines = 0;
	size_t i;

	for (i = 0; i < msg_len; i++)
		lines += panic_msg[i].len ? DIV_ROUND_UP(panic_msg[i].len, chars_per_line) : 1;
	return lines;
}

static bool can_draw_logo(size_t chars_per_line, size_t lines, size_t msg_lines)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(logo); i++) {
		if (logo[i].len > chars_per_line)
			return false;
	}
	if (lines < msg_lines + ARRAY_SIZE(logo))
		return false;
	return true;
}

static size_t get_start_line(size_t lines, size_t msg_lines, bool draw_logo)
{
	size_t remaining;
	size_t logo_len = ARRAY_SIZE(logo);

	if (lines < msg_lines)
		return 0;
	remaining = lines - msg_lines;
	if (draw_logo && remaining / 2 <= logo_len)
		return logo_len + (remaining - logo_len) / 4;
	return remaining / 2;
}

/*
 * Return the function to convert xrgb8888 to the scanout buffer format
 * NULL if no conversion is needed, and ERR_PTR if format can't be converted.
 * Also set fg_color and bg_color accordingly.
 */
static void (*get_convert_func(const struct drm_format_info *format))
	    (void *, const void *, unsigned int)
{
	/* reset colors, this are le32 value as it's what drm_fb_xxxx_to_xxx() expect */
	fg_color = CONFIG_DRM_PANIC_FOREGROUND_COLOR;
	bg_color = CONFIG_DRM_PANIC_BACKGROUND_COLOR;

	switch (format->format) {
	/* 16 bit format */
	case DRM_FORMAT_RGB565:
		return drm_fb_xrgb8888_to_rgb565_line;
	case DRM_FORMAT_RGBA5551:
		return drm_fb_xrgb8888_to_rgba5551_line;
	case DRM_FORMAT_XRGB1555:
		return drm_fb_xrgb8888_to_xrgb1555_line;
	case DRM_FORMAT_ARGB1555:
		return drm_fb_xrgb8888_to_argb1555_line;
	/* 24 bit format */
	case DRM_FORMAT_RGB888:
		return drm_fb_xrgb8888_to_rgb888_line;
	/* 32 bit format: convert the fg_color and bg_color */
	case DRM_FORMAT_XRGB8888:
		return NULL;
	case DRM_FORMAT_ARGB8888:
		fg_color = fg_color | 0xff000000;
		bg_color = bg_color | 0xff000000;
		return NULL;
	case DRM_FORMAT_XBGR8888:
		fg_color = swab32(fg_color) >> 8;
		bg_color = swab32(bg_color) >> 8;
		return NULL;
	case DRM_FORMAT_ABGR8888:
		fg_color = (swab32(fg_color) >> 8) | 0xff000000;
		bg_color = (swab32(bg_color) >> 8) | 0xff000000;
		return NULL;
	case DRM_FORMAT_XRGB2101010:
		drm_fb_xrgb8888_to_xrgb2101010_line(&fg_color, &fg_color, 1);
		drm_fb_xrgb8888_to_xrgb2101010_line(&bg_color, &bg_color, 1);
		return NULL;
	case DRM_FORMAT_ARGB2101010:
		drm_fb_xrgb8888_to_argb2101010_line(&fg_color, &fg_color, 1);
		drm_fb_xrgb8888_to_argb2101010_line(&bg_color, &bg_color, 1);
		return NULL;
	default:
		return ERR_PTR(EINVAL);
	}
}

/*
 * Draw the panic message at the center of the screen
 */
static void draw_panic_static(struct drm_scanout_buffer *sb, const char *msg)
{
	size_t lines, msg_lines, l, msg_start_line, msgi;
	size_t center, chars_per_line;
	bool draw_logo;
	struct drm_panic_line panic_line;
	size_t msg_len = ARRAY_SIZE(panic_msg);
	size_t logo_len = ARRAY_SIZE(logo);
	void (*convert)(void *dbuf, const void *sbuf, unsigned int npixels);
	const struct font_desc *font = get_default_font(sb->width, sb->height, 0x8080, 0x8080);

	if (!font)
		return;

	/* Set the panic reason */
	panic_msg[msg_len - 1].len = strlen(msg);
	panic_msg[msg_len - 1].txt = msg;

	lines = sb->height / font->height;
	chars_per_line = sb->width / font->width;

	msg_lines = panic_msg_needed_lines(chars_per_line);
	draw_logo = can_draw_logo(chars_per_line, lines, msg_lines);
	msg_start_line = get_start_line(lines, msg_lines, draw_logo);

	convert = get_convert_func(sb->format);
	if (IS_ERR(convert))
		return;

	msgi = 0;
	panic_line.len = 0;
	for (l = 0; l < lines; l++) {
		if (draw_logo && l < logo_len)
			draw_txt_line(&logo[l], 0, l, sb, font, convert);
		else if (l >= msg_start_line && msgi < msg_len) {
			if (!panic_line.len) {
				panic_line.txt = panic_msg[msgi].txt;
				panic_line.len = panic_msg[msgi].len;
			}
			if (!panic_line.len)
				draw_empty_lines(sb, l * font->height, font->height, convert);
			else {
				center = panic_line.len > chars_per_line ?
					 0 : (chars_per_line - panic_line.len) / 2;
				draw_txt_line(&panic_line, center, l, sb, font, convert);
			}
			if (panic_line.len > chars_per_line) {
				panic_line.len -= chars_per_line;
				panic_line.txt += chars_per_line;
			} else {
				panic_line.len = 0;
				msgi++;
			}
		} else {
			draw_empty_lines(sb, l * font->height, font->height, convert);
		}
	}
	/* Fill the bottom of the screen, if sb->height is not a multiple of font->height */
	draw_empty_lines(sb, lines * font->height, sb->height - lines * font->height, convert);
}

static void draw_panic_device(struct drm_device *dev, const char *msg)
{
	struct drm_scanout_buffer sb;

	if (dev->driver->get_scanout_buffer(dev, &sb))
		return;
	if (!drm_panic_line_buf)
		return;

	/* to avoid buffer overflow on drm_panic_line_buf */
	if (sb.width > DRM_PANIC_MAX_WIDTH)
		sb.width = DRM_PANIC_MAX_WIDTH;

	draw_panic_static(&sb, msg);
}

static int drm_panic(struct notifier_block *this, unsigned long event,
		     void *ptr)
{
	struct drm_panic_device *drm_panic_device;

	list_for_each_entry(drm_panic_device, &drm_panic_devices, head) {
		draw_panic_device(drm_panic_device->dev, ptr);
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
	struct drm_panic_device *new;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return;

	new->dev = dev;
	mutex_lock(&drm_panic_lock);
	if (!drm_panic_line_buf)
		drm_panic_line_buf = kmalloc(DRM_PANIC_MAX_WIDTH * sizeof(u32), GFP_KERNEL);
	if (!drm_panic_line_buf)
		goto unlock;
	list_add_tail(&new->head, &drm_panic_devices);
	drm_info(dev, "Registered with drm panic\n");
unlock:
	mutex_unlock(&drm_panic_lock);
}
EXPORT_SYMBOL(drm_panic_register);

/**
 * drm_panic_unregister()
 * @dev: the DRM device previously registered.
 */
void drm_panic_unregister(struct drm_device *dev)
{
	struct drm_panic_device *drm_panic_device;
	struct drm_panic_device *found = NULL;

	mutex_lock(&drm_panic_lock);
	list_for_each_entry(drm_panic_device, &drm_panic_devices, head) {
		if (drm_panic_device->dev == dev)
			found = drm_panic_device;
	}
	if (found) {
		list_del(&found->head);
		kfree(found);
		drm_info(dev, "Unregistered with drm panic\n");
	}
	if (drm_panic_line_buf && list_empty(&drm_panic_devices)) {
		kfree(drm_panic_line_buf);
		drm_panic_line_buf = NULL;
	}
	mutex_unlock(&drm_panic_lock);
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
 * drm_panic_exit() - Shutdown drm-panic subsystem
 */
void drm_panic_exit(void)
{
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &drm_panic_notifier);
}
