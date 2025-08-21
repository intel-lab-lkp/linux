// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright (c) 2025 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
 *
 * KUNIT tests for drm panic
 */

#include <drm/drm_fourcc.h>
#include <drm/drm_panic.h>

#include <kunit/test.h>

#include <linux/units.h>
#include <linux/vmalloc.h>

struct drm_test_mode {
	const int width;
	const int height;
	const u32 format;
	void (*draw_screen)(struct drm_scanout_buffer *sb);
	const char *fname;
};

/*
 * Run all tests for the 3 panic screens: user, kmsg and qr_code
 */
#define DRM_TEST_MODE_LIST(func) \
	DRM_PANIC_TEST_MODE(1024, 768, DRM_FORMAT_XRGB8888, func) \
	DRM_PANIC_TEST_MODE(300, 200, DRM_FORMAT_XRGB8888, func) \
	DRM_PANIC_TEST_MODE(1920, 1080, DRM_FORMAT_XRGB8888, func) \
	DRM_PANIC_TEST_MODE(1024, 768, DRM_FORMAT_RGB565, func) \
	DRM_PANIC_TEST_MODE(1024, 768, DRM_FORMAT_RGB888, func) \

#define DRM_PANIC_TEST_MODE(w, h, f, name) { \
	.width = w, \
	.height = h, \
	.format = f, \
	.draw_screen = draw_panic_screen_##name, \
	.fname = #name, \
	}, \

static const struct drm_test_mode drm_test_modes_cases[] = {
	DRM_TEST_MODE_LIST(user)
	DRM_TEST_MODE_LIST(kmsg)
	DRM_TEST_MODE_LIST(qr_code)
};
#undef DRM_PANIC_TEST_MODE

static int drm_test_panic_init(struct kunit *test)
{
	struct drm_scanout_buffer *priv;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	test->priv = priv;

	drm_panic_set_description("Kunit testing");

	return 0;
}

static void drm_test_panic_screen_user_map(struct kunit *test)
{
	struct drm_scanout_buffer *sb = test->priv;
	const struct drm_test_mode *params = test->param_value;
	void *fb;
	int fb_size;

	sb->format = drm_format_info(params->format);
	fb_size = params->width * params->height * sb->format->cpp[0];

	fb = vmalloc(fb_size);
	KUNIT_ASSERT_NOT_NULL(test, fb);

	iosys_map_set_vaddr(&sb->map[0], fb);
	sb->width = params->width;
	sb->height = params->height;
	sb->pitch[0] = params->width * sb->format->cpp[0];

	params->draw_screen(sb);
	vfree(fb);
}

static void drm_test_panic_screen_user_page(struct kunit *test)
{
	struct drm_scanout_buffer *sb = test->priv;
	const struct drm_test_mode *params = test->param_value;
	int fb_size;
	struct page **pages;
	int i;
	int npages;

	sb->format = drm_format_info(params->format);
	fb_size = params->width * params->height * sb->format->cpp[0];
	npages = DIV_ROUND_UP(fb_size, PAGE_SIZE);

	pages = kmalloc_array(npages, sizeof(struct page *), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pages);

	for (i = 0; i < npages; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}
	sb->pages = pages;
	sb->width = params->width;
	sb->height = params->height;
	sb->pitch[0] = params->width * sb->format->cpp[0];

	params->draw_screen(sb);

	for (i = 0; i < npages; i++)
		__free_page(pages[i]);
	kfree(pages);
}

#ifdef CONFIG_DRM_PANIC_KUNIT_TEST_DUMP
#include <linux/base64.h>
#include <linux/delay.h>
#include <linux/zlib.h>

#define LINE_LEN 128

#define COMPR_LEVEL 6
#define WINDOW_BITS 12
#define MEM_LEVEL 4

static int compress_image(u8 *src, int size, u8 *dst)
{
	struct z_stream_s stream;

	stream.workspace = kmalloc(zlib_deflate_workspacesize(WINDOW_BITS, MEM_LEVEL),
				   GFP_KERNEL);

	if (zlib_deflateInit2(&stream, COMPR_LEVEL, Z_DEFLATED, WINDOW_BITS,
			      MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK)
		return -EINVAL;

	stream.next_in = src;
	stream.avail_in = size;
	stream.total_in = 0;
	stream.next_out = dst;
	stream.avail_out = size;
	stream.total_out = 0;

	if (zlib_deflate(&stream, Z_FINISH) != Z_STREAM_END)
		return -EINVAL;

	if (zlib_deflateEnd(&stream) != Z_OK)
		return -EINVAL;

	kfree(stream.workspace);

	return stream.total_out;
}

static void dump_image(u8 *fb, unsigned int width, unsigned int height)
{
	int len = 0;
	char *dst;
	char *compressed;
	int sent = 0;
	int stride = DIV_ROUND_UP(width, 8);
	int size = stride * height;

	compressed = vzalloc(size);
	if (!compressed)
		return;
	len = compress_image(fb, size, compressed);
	if (len < 0) {
		pr_err("Compression failed %d", len);
		return;
	}

	dst = vzalloc(4 * DIV_ROUND_UP(len, 3) + 1);
	if (!dst)
		return;

	len = base64_encode(compressed, len, dst);

	pr_info("KUNIT PANIC IMAGE DUMP START %dx%d", width, height);
	while (len > 0) {
		char save = dst[sent + LINE_LEN];

		dst[sent + LINE_LEN] = 0;
		pr_info("%s", dst + sent);
		dst[sent + LINE_LEN] = save;
		sent += LINE_LEN;
		len -= LINE_LEN;
	}
	pr_info("KUNIT PANIC IMAGE DUMP END");
	vfree(compressed);
	vfree(dst);

}

// Ignore pixel format, use 1bit per pixel in monochrome.
static void drm_test_panic_set_pixel(struct drm_scanout_buffer *sb,
				     unsigned int x,
				     unsigned int y,
				     u32 color)
{
	int stride = DIV_ROUND_UP(sb->width, 8);
	size_t off = x / 8 + y * stride;
	u8 shift = 7 - (x % 8);
	u8 *fb = (u8 *) sb->private;

	if (color)
		fb[off] |= 1 << shift;
	else
		fb[off] &= ~(1 << shift);
}

#else
static void dump_image(u8 *fb, unsigned int width, unsigned int height) {}
static void drm_test_panic_set_pixel(struct drm_scanout_buffer *sb,
				     unsigned int x,
				     unsigned int y,
				     u32 color)
{
}
#endif

static void drm_test_panic_screen_user_set_pixel(struct kunit *test)
{
	struct drm_scanout_buffer *sb = test->priv;
	const struct drm_test_mode *params = test->param_value;
	int fb_size;
	u8 *fb;

	sb->format = drm_format_info(params->format);
	fb_size = DIV_ROUND_UP(params->width, 8) * params->height;

	fb = vzalloc(fb_size);
	KUNIT_ASSERT_NOT_NULL(test, fb);
	sb->private = fb;
	sb->set_pixel = drm_test_panic_set_pixel;
	sb->width = params->width;
	sb->height = params->height;

	params->draw_screen(sb);
	if (params->format == DRM_FORMAT_XRGB8888)
		dump_image(fb, sb->width, sb->height);

	vfree(fb);
}

static void drm_test_panic_desc(const struct drm_test_mode *t, char *desc)
{
	sprintf(desc, "Panic screen %s, mode: %d x %d \t%p4cc",
		t->fname, t->width, t->height, &t->format);
}

KUNIT_ARRAY_PARAM(drm_test_panic_screen_user_map, drm_test_modes_cases, drm_test_panic_desc);
KUNIT_ARRAY_PARAM(drm_test_panic_screen_user_page, drm_test_modes_cases, drm_test_panic_desc);
KUNIT_ARRAY_PARAM(drm_test_panic_screen_user_set_pixel, drm_test_modes_cases, drm_test_panic_desc);

static struct kunit_case drm_panic_screen_user_test[] = {
	KUNIT_CASE_PARAM(drm_test_panic_screen_user_map,
			 drm_test_panic_screen_user_map_gen_params),
	KUNIT_CASE_PARAM(drm_test_panic_screen_user_page,
			 drm_test_panic_screen_user_page_gen_params),
	KUNIT_CASE_PARAM(drm_test_panic_screen_user_set_pixel,
			 drm_test_panic_screen_user_set_pixel_gen_params),
	{ }
};

static struct kunit_suite drm_panic_suite = {
	.name = "drm_panic",
	.init = drm_test_panic_init,
	.test_cases = drm_panic_screen_user_test,
};

kunit_test_suite(drm_panic_suite);
