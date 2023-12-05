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

static struct drm_panic_line panic_msg[] = {
	PANIC_LINE("KERNEL PANIC !"),
	PANIC_LINE(""),
	PANIC_LINE("Please reboot your computer."),
	PANIC_LINE(""),
	PANIC_LINE(""), /* overwritten with panic reason */
};

static const struct drm_panic_line logo[] = {
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

static void draw_empty_line_map(struct drm_scanout_buffer *sb, size_t top, size_t height, u32 color)
{
	struct iosys_map dst = sb->map;

	iosys_map_incr(&dst, top * sb->pitch);
	drm_fb_fill(&dst, sb->pitch, height, sb->width, color, sb->format->cpp[0]);
}

static void draw_txt_line_map(const struct drm_panic_line *msg, size_t left, size_t top,
			      struct drm_scanout_buffer *sb, u32 fg_color, u32 bg_color,
			      const struct font_desc *font)
{
	size_t i;
	const u8 *src;
	size_t src_stride = DIV_ROUND_UP(font->width, 8);
	struct iosys_map dst = sb->map;
	size_t end_text;
	unsigned int px_width = sb->format->cpp[0];

	iosys_map_incr(&dst, top * sb->pitch);
	drm_fb_fill(&dst, sb->pitch, font->height, left, bg_color, px_width);
	iosys_map_incr(&dst, left * px_width);
	for (i = 0; i < msg->len; i++) {
		src = font->data + (msg->txt[i] * font->height) * src_stride;
		drm_fb_blit_from_r1(&dst, sb->pitch, src, src_stride, font->height, font->width,
				    fg_color, bg_color, px_width);
		iosys_map_incr(&dst, font->width * px_width);
	}
	end_text = (msg->len * font->width) + left;
	if (sb->width > end_text)
		drm_fb_fill(&dst, sb->pitch, font->height, sb->width - end_text,
			    bg_color, px_width);
}

static void draw_empty_line_px(struct drm_scanout_buffer *sb, size_t top, size_t height, u32 color)
{
	unsigned int x, y;

	for (y = 0; y < height; y++)
		for (x = 0; x < sb->width; x++)
			sb->draw_pixel_xy(x, y + top, color, sb->private);
}

static void draw_txt_line_px(const struct drm_panic_line *msg, size_t left, size_t top,
			     struct drm_scanout_buffer *sb, u32 fg_color, u32 bg_color,
			     const struct font_desc *font)
{
	unsigned int x, y, i;
	const u8 *src;
	u32 color;
	size_t src_stride = DIV_ROUND_UP(font->width, 8);
	size_t end_text = msg->len * font->width + left;
	size_t right = sb->width > end_text ? sb->width - end_text : 0;

	for (y = 0; y < font->height; y++) {
		for (x = 0; x < left; x++)
			sb->draw_pixel_xy(x, y + top, bg_color, sb->private);

		for (i = 0; i < msg->len; i++) {
			src = font->data + (msg->txt[i] * font->height + y) * src_stride;
			for (x = 0; x < font->width; x++) {
				color = (src[x / 8] & (0x80 >> (x % 8))) ? fg_color : bg_color;
				sb->draw_pixel_xy(x + left + font->width * i, y + top, color,
						  sb->private);
			}
		}

		for (x = 0; x < right; x++)
			sb->draw_pixel_xy(x + end_text, y + top, bg_color, sb->private);
	}
}

static void draw_empty_line(struct drm_scanout_buffer *sb, size_t top, size_t height, u32 color)
{
	if (sb->draw_pixel_xy)
		draw_empty_line_px(sb, top, height, color);
	else
		draw_empty_line_map(sb, top, height, color);
}

static void draw_txt_line(const struct drm_panic_line *msg, size_t left, size_t top,
			  struct drm_scanout_buffer *sb, u32 fg_color, u32 bg_color,
			  const struct font_desc *font)
{
	if (sb->draw_pixel_xy)
		draw_txt_line_px(msg, left, top, sb, fg_color, bg_color, font);
	else
		draw_txt_line_map(msg, left, top, sb, fg_color, bg_color, font);
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
 * Draw the panic message at the center of the screen
 */
static void draw_panic_static(struct drm_scanout_buffer *sb, const char *msg)
{
	size_t lines, msg_lines, l, msg_start_line, remaining, msgi;
	size_t chars_per_line;
	bool draw_logo;
	struct drm_panic_line panic_line;
	size_t msg_len = ARRAY_SIZE(panic_msg);
	size_t logo_len = ARRAY_SIZE(logo);
	u32 fg_color = CONFIG_DRM_PANIC_FOREGROUND_COLOR;
	u32 bg_color = CONFIG_DRM_PANIC_BACKGROUND_COLOR;
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

	fg_color = drm_fb_convert_from_xrgb8888(fg_color, sb->format->format);
	bg_color = drm_fb_convert_from_xrgb8888(bg_color, sb->format->format);

	msgi = 0;
	panic_line.len = 0;
	for (l = 0; l < lines; l++) {
		if (draw_logo && l < logo_len)
			draw_txt_line(&logo[l], 0, l * font->height, sb, fg_color, bg_color, font);
		else if (l >= msg_start_line && msgi < msg_len) {
			if (!panic_line.len) {
				panic_line.txt = panic_msg[msgi].txt;
				panic_line.len = panic_msg[msgi].len;
			}
			if (!panic_line.len) {
				draw_empty_line(sb, l * font->height, font->height, bg_color);
				msgi++;
			} else if (panic_line.len > chars_per_line) {
				remaining = panic_line.len - chars_per_line;
				panic_line.len = chars_per_line;
				draw_txt_line(&panic_line, 0, l * font->height, sb, fg_color,
					      bg_color, font);
				panic_line.txt += chars_per_line;
				panic_line.len = remaining;
			} else {
				draw_txt_line(&panic_line,
					      font->width * (chars_per_line - panic_line.len) / 2,
					      l * font->height, sb, fg_color, bg_color, font);
				panic_line.len = 0;
				msgi++;
			}
		} else {
			draw_empty_line(sb, l * font->height, font->height, bg_color);
		}
	}
	/* Fill the bottom of the screen, if sb->height is not a multiple of font->height */
	if (sb->height % font->height)
		draw_empty_line(sb, l * font->height, sb->height - l * font->height, bg_color);
}

static void draw_panic_device(struct drm_device *dev, const char *msg)
{
	struct drm_scanout_buffer sb = {0};

	if (dev->driver->get_scanout_buffer(dev, &sb))
		return;
	draw_panic_static(&sb, msg);
	if (sb.flush)
		sb.flush(sb.private);
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

static struct notifier_block drm_panic_notifier = {
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
	list_add_tail(&new->head, &drm_panic_devices);
	mutex_unlock(&drm_panic_lock);

	drm_info(dev, "Registered with drm panic\n");
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
