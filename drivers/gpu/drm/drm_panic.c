// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright (c) 2023 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
 * inspired by the drm_log driver from David Herrmann <dh.herrmann@gmail.com>
 * Tux Ascii art taken from cowsay written by Tony Monroe
 */

#include <linux/font.h>
#include <linux/iosys-map.h>
#include <linux/kdebug.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/panic_notifier.h>
#include <linux/types.h>

#include <drm/drm_drv.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_panic.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>


MODULE_AUTHOR("Jocelyn Falempe");
MODULE_DESCRIPTION("DRM panic handler");
MODULE_LICENSE("GPL");

/**
 * DOC: overview
 *
 * To enable DRM panic for a driver, you should register the primary plane
 * with drm_panic_register(). Then when a scanout buffer is set for this plane,
 * call drm_panic_set_buffer(), so if a panic occurs, it will draw to this.
 * Make sure to update it when the scanout buffer changes. Also you should call
 * drm_panic_unset_buffer() when the plane is disabled, or when the scanout
 * buffer is no more accessible.
 */

/*
 * This module displays a user friendly message on screen when a kernel panic
 * occurs. This is conflicting with fbcon, so you can only enable it when fbcon
 * is disabled.
 * It's intended for end-user, so have minimal technical/debug information.
 *
 * Implementation details:
 *
 * It is a panic handler, so it can't take lock, allocate memory, run tasks/irq,
 * or attempt to sleep. It's a best effort, and it may not be able to display
 * the message in all situations (like if the panic occurs in the middle of a
 * modesetting).
 * It will display only one static frame, so performance optimizations are low
 * priority as the machine is already in an unusable state.
 */

/**
 * struct drm_scanout_buffer - DRM scanout buffer
 *
 * This structure holds the information necessary for drm_panic to draw the
 * panic screen, and display it.
 */
struct drm_scanout_buffer {
	/**
	 * @lock:
	 *
	 * a raw spinlock to make sure that when the panic handler is running
	 * the data in this struct is valid.
	 */
	struct raw_spinlock lock;
	/**
	 * @format:
	 *
	 * drm format of the scanout buffer.
	 */
	const struct drm_format_info *format;
	/**
	 * @map:
	 *
	 * Virtual address of the scanout buffer, either in memory or iomem.
	 * The scanout buffer should be in linear format, and can be directly
	 * sent to the display hardware. Tearing is not an issue for the panic
	 * screen.
	 */
	struct iosys_map map;
	/**
	 * @width: Width of the scanout buffer, in pixels.
	 */
	unsigned int width;
	/**
	 * @height: Height of the scanout buffer, in pixels.
	 */
	unsigned int height;
	/**
	 * @pitch: Length in bytes between the start of two consecutive lines.
	 */
	unsigned int pitch;
};

static inline struct drm_plane *to_drm_plane(struct notifier_block *nb)
{
	return container_of(nb, struct drm_plane, panic_notifier);
}

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

static void draw_empty_line(struct drm_scanout_buffer *sb, size_t top, size_t height, u32 color)
{
	struct iosys_map dst = sb->map;

	iosys_map_incr(&dst, top * sb->pitch);
	drm_fb_fill(&dst, sb->pitch, height, sb->width, color, sb->format->cpp[0]);
}

static void draw_txt_line(const struct drm_panic_line *msg, size_t left, size_t top,
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
	const struct font_desc *font = get_default_font(sb->width, sb->height,
							0x80808080, 0x80808080);

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

/*
 * drm_panic_is_format_supported()
 * @format: a fourcc color code
 * Returns: true if supported, false otherwise.
 *
 * Check if drm_panic will be able to use this color format.
 */
static bool drm_panic_is_format_supported(u32 format)
{
	return drm_fb_convert_from_xrgb8888(0xffffff, format) != 0;
}

static void draw_panic_plane(struct drm_plane *plane, const char *msg)
{
	struct drm_scanout_buffer *sb = plane->panic_scanout;

	if (!sb)
		return;

	if (!raw_spin_trylock(&sb->lock))
		return;

	if (!iosys_map_is_null(&sb->map) && drm_panic_is_format_supported(sb->format->format)) {
		draw_panic_static(sb, msg);
		if (plane->helper_private->panic_flush)
			plane->helper_private->panic_flush(plane);
	}
	raw_spin_unlock(&sb->lock);
}

static int drm_panic(struct notifier_block *this, unsigned long event,
		     void *ptr)
{
	struct drm_plane *plane = to_drm_plane(this);

	draw_panic_plane(plane, ptr);

	return NOTIFY_OK;
}
/**
 * drm_panic_set_buffer()
 *
 * @sb: The scanout_buffer to set.
 * @fb: The current drm_framebuffer struct (only format, height, width and
 *      pitches[0]) is used.
 * @map: The iosys_map pointing to the current scanout buffer.
 *
 * Set the scanout buffer, that will be used if a panic occurs.
 * Make sure to update it before the iosysmap is no longer valid.
 */
void drm_panic_set_buffer(struct drm_scanout_buffer *sb,
			  struct drm_framebuffer *fb,
			  struct iosys_map *map)
{
	if (!sb)
		return;

	raw_spin_lock(&sb->lock);
	sb->map = *map;
	sb->format = fb->format;
	sb->height = fb->height;
	sb->width = fb->width;
	sb->pitch = fb->pitches[0];
	raw_spin_unlock(&sb->lock);
}
EXPORT_SYMBOL(drm_panic_set_buffer);

/**
 * drm_panic_unset_buffer()
 *
 * Unset the scanout buffer before it is no longer accessible.
 * @sb: the scanout_buffer to be cleared.
 *
 * After calling this function, if a panic occurs, it won't be displayed on this
 * plane.
 */
void drm_panic_unset_buffer(struct drm_scanout_buffer *sb)
{
	if (!sb)
		return;

	raw_spin_lock(&sb->lock);
	iosys_map_clear(&sb->map);
	sb->format = NULL;
	raw_spin_unlock(&sb->lock);
}
EXPORT_SYMBOL(drm_panic_unset_buffer);

/**
 * drm_panic_register() - Initialize DRM panic for a primary plane
 * @plane: the plane on which the panic screen will be displayed.
 */
void drm_panic_register(struct drm_plane *plane)
{
	struct drm_scanout_buffer *sb;

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return;

	raw_spin_lock_init(&sb->lock);
	plane->panic_scanout = sb;
	plane->panic_notifier.notifier_call = drm_panic;
	plane->panic_notifier.priority = -5;
	if (atomic_notifier_chain_register(&panic_notifier_list,
					   &plane->panic_notifier)) {
		drm_warn(plane->dev, "Failed to register panic handler\n");
		plane->panic_scanout = NULL;
		kfree(sb);
	} else
		drm_info(plane->dev, "Registered with drm panic\n");
}
EXPORT_SYMBOL(drm_panic_register);

/**
 * drm_panic_unregister()
 * @plane: the plane previously registered.
 */
void drm_panic_unregister(struct drm_plane *plane)
{
	if (plane->panic_scanout) {
		atomic_notifier_chain_unregister(&panic_notifier_list,
						 &plane->panic_notifier);
		kfree(plane->panic_scanout);
		plane->panic_scanout = NULL;
	}
}
EXPORT_SYMBOL(drm_panic_unregister);

