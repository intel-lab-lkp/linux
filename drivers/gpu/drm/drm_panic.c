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
#include <linux/slab.h>

#include <drm/drm_client.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>

MODULE_AUTHOR("Jocelyn Falempe");
MODULE_DESCRIPTION("DRM PANIC");
MODULE_LICENSE("GPL");

/**
 * This module displays a user friendly message on screen when a kernel panic
 * occurs.
 * It will display only one frame, so just clear it, and draw white pixels for
 * the characters. Performance optimizations are low priority as the machine is
 * already in an unusable state.
 */

/**
 * List of drm clients
 */
struct dpanic_drm_client {
	struct list_head head;
	struct drm_client_dev client;
};

struct dpanic_line {
	u32 len;
	const char *txt;
};

#define PANIC_LINE(s) {.len = sizeof(s), .txt = s}

const struct dpanic_line panic_msg[] = {
	PANIC_LINE("KERNEL PANIC !"),
	PANIC_LINE(""),
	PANIC_LINE("Please reboot your computer.")
};

const struct dpanic_line logo[] = {
	PANIC_LINE("     .--."),
	PANIC_LINE("    |o_o |"),
	PANIC_LINE("    |:_/ |"),
	PANIC_LINE("   //   \\ \\"),
	PANIC_LINE("  (|     | )"),
	PANIC_LINE(" /'\\_   _/`\\"),
	PANIC_LINE(" \\___)=(___/"),
};

static LIST_HEAD(dpanic_clients);
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
			     struct drm_framebuffer *fb,
			     struct iosys_map *map,
			     const struct font_desc *font)
{
	size_t src_width, src_height, src_stride, i, dst_off;
	const u8 *src;

	src_width = font->width;
	src_height = font->height;
	src_stride = DIV_ROUND_UP(src_width, 8);

	dst_off = x * font->width * fb->format->cpp[0] + y * font->height * fb->pitches[0];

	src = font->data;
	src += ch * src_height * src_stride;

	while (src_height--) {
		for (i = 0; i < src_width; ++i) {
			/* only draw white pixels */
			if (src[i / 8] & (0x80 >> (i % 8)))
				dpanic_draw_px(map, dst_off + i * fb->format->cpp[0],
					       fb->format->format, 0xff, 0xff, 0xff);
		}
		src += src_stride;
		dst_off += fb->pitches[0];
	}
}

static void dpanic_draw_line_centered(const struct dpanic_line *line, size_t y,
				      struct drm_framebuffer *fb,
				      struct iosys_map *map,
				      const struct font_desc *font)
{
	size_t chars_per_line = fb->width / font->width;
	size_t skip_left, x;

	if (line->len > chars_per_line)
		return;

	skip_left = (chars_per_line - line->len) / 2;

	for (x = 0; x < line->len; x++)
		dpanic_draw_char(line->txt[x], skip_left + x, y, fb, map, font);
}

/*
 * Draw the Tux logo at the upper left corner
 */
static void dpanic_draw_logo(struct drm_framebuffer *fb,
			     struct iosys_map *map,
			     const struct font_desc *font)
{
	size_t chars_per_line = fb->width / font->width;
	size_t x, y;

	for (y = 0; y < ARRAY_SIZE(logo); y++)
		for (x = 0; x < logo[y].len && x < chars_per_line; x++)
			dpanic_draw_char(logo[y].txt[x], x, y, fb, map, font);
}

/*
 * Draw the panic message at the center of the screen
 */
static void dpanic_static_draw(struct drm_client_buffer *buffer)
{
	size_t y, lines, skip_top;
	size_t len = ARRAY_SIZE(panic_msg);
	struct iosys_map map;
	struct drm_framebuffer *fb = buffer->fb;
	const struct font_desc *font = get_default_font(fb->width, fb->height, 0x8080, 0x8080);

	if (!font)
		return;

	if (drm_client_buffer_vmap(buffer, &map)) {
		pr_err("VMAP failed\n");
		return;
	}
	lines = fb->height / font->height;
	skip_top = (lines - len) / 2;

	/* clear screen */
	iosys_map_memset(&map, 0, 0, fb->height * fb->pitches[0]);

	for (y = 0; y < len; y++)
		dpanic_draw_line_centered(&panic_msg[y], y + skip_top, fb, &map, font);

	if (skip_top >= ARRAY_SIZE(logo))
		dpanic_draw_logo(fb, &map, font);

	drm_client_framebuffer_flush(buffer, NULL);
}

#define MAX_MODESET 8

static void drm_panic_client(struct drm_client_dev *client)
{
	struct drm_client_buffer *buffer[MAX_MODESET];
	int ret, n_modeset, i;
	struct drm_mode_set *mode_set;

	ret = drm_client_modeset_probe(client, 0, 0);
	if (ret)
		return;

	n_modeset = 0;
	drm_client_for_each_modeset(mode_set, client) {
		struct drm_plane *primary = mode_set->crtc->primary;
		struct drm_framebuffer *fb;

		if (primary->state && primary->state->fb)
			fb = primary->state->fb;
		else if (primary->fb)
			fb = primary->fb;
		else
			continue;

		pr_debug("DRM Panic, FB width %d, height %d\n", fb->width, fb->height);
		buffer[n_modeset] = drm_client_framebuffer_create(client, fb->width,
								  fb->height,
								  fb->format->format);

		if (IS_ERR(buffer[n_modeset])) {
			pr_err("DRM Panic can't allocate buffer\n");
			continue;
		}
		mode_set->fb = buffer[n_modeset]->fb;
		n_modeset++;
		if (n_modeset == MAX_MODESET)
			break;
	}
	ret = drm_client_modeset_commit_locked(client);
	if (ret)
		return;

	for (i = 0; i < n_modeset; i++)
		dpanic_static_draw(buffer[i]);
}

static int drm_panic(struct notifier_block *this, unsigned long event,
		     void *ptr)
{
	struct dpanic_drm_client *dpanic_client;

	list_for_each_entry(dpanic_client, &dpanic_clients, head) {
		drm_panic_client(&dpanic_client->client);
	}
	return NOTIFY_OK;
}

struct notifier_block drm_panic_notifier = {
	.notifier_call = drm_panic,
};

void drm_panic_init_client(struct drm_device *dev)
{
	struct dpanic_drm_client *new;
	int ret;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return;

	ret = drm_client_init(dev, &new->client, "drm_panic", NULL);
	if (ret < 0) {
		kfree(new);
		return;
	}
	drm_client_register(&new->client);
	list_add_tail(&new->head, &dpanic_clients);

	drm_info(dev, "Registered with drm panic\n");
}
EXPORT_SYMBOL(drm_panic_init_client);

/**
 * drm_panic_init() - Initialize drm-panic subsystem
 *
 * register the panic notifier
 */
void drm_panic_init(void)
{
	/* register panic handler */
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
