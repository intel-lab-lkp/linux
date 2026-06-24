/* SPDX-License-Identifier: MIT */
#ifndef __DRM_BACKLIGHT_H__
#define __DRM_BACKLIGHT_H__

/*
 * Copyright (c) 2014 David Herrmann <dh.herrmann at gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/types.h>

struct backlight_device;
struct drm_backlight;
struct drm_connector;
struct drm_device;
struct drm_mode_object;

int drm_backlight_init(void);
void drm_backlight_exit(void);

int drm_backlight_alloc(struct drm_connector *connector);
void drm_backlight_free(struct drm_connector *connector);
void drm_backlight_register(struct drm_backlight *b);
void drm_backlight_unregister(struct drm_backlight *b);

void drm_backlight_link(struct drm_backlight *b, struct backlight_device *bd);
struct backlight_device *drm_backlight_get_device(struct drm_backlight *b);
void drm_backlight_inhibit_legacy(struct drm_backlight *b);
void drm_backlight_uninhibit_legacy(struct drm_backlight *b);
void drm_backlight_inhibit_legacy_all(struct drm_device *dev);
void drm_backlight_uninhibit_legacy_all(struct drm_device *dev);
void drm_backlight_set_luminance(struct drm_backlight *b, unsigned int value);
#endif /* __DRM_BACKLIGHT_H__ */
