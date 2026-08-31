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

#include <linux/list.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct backlight_device;
struct drm_backlight;
struct drm_connector;
struct drm_device;

/**
 * struct drm_backlight_funcs - backend operations for a DRM backlight
 *
 * A DRM backlight is backend-agnostic: the core forwards luminance requests to
 * whatever backend a driver has linked. Today the only backend is the backlight
 * subsystem (&struct backlight_device), but the same core can drive other
 * backends (DDC/CI, MIPI-DCS, ...) by providing a different set of these
 * operations without any change to the core.
 *
 * All callbacks are invoked from process context (a workqueue), never from an
 * atomic commit tail or while holding a spinlock, so they are allowed to sleep.
 */
struct drm_backlight_funcs {
	/**
	 * @set_luminance:
	 *
	 * Push @value (already clamped to the backend range) to the hardware.
	 * Returns 0 on success or a negative error code.
	 */
	int (*set_luminance)(struct drm_backlight *b, unsigned int value);

	/**
	 * @get_luminance:
	 *
	 * Read the current hardware luminance into @value. Returns 0 on success
	 * or a negative error code. May be NULL if the backend cannot be read
	 * back.
	 */
	int (*get_luminance)(struct drm_backlight *b, unsigned int *value);

	/**
	 * @get_range:
	 *
	 * Report the backend's maximum luminance in @max and whether the
	 * backend can turn the panel off at luminance 0 in @can_disable.
	 */
	void (*get_range)(struct drm_backlight *b, unsigned int *max,
			  bool *can_disable);
};

/**
 * struct drm_backlight - per-connector backlight state
 *
 * This structure is embedded in &struct drm_connector and initialized by the
 * DRM core; drivers never allocate it. It becomes active once a driver links a
 * backend with drm_backlight_link().
 */
struct drm_backlight {
	/** @connector: connector this backlight belongs to */
	struct drm_connector *connector;
	/** @funcs: backend operations, or NULL while no backend is linked */
	const struct drm_backlight_funcs *funcs;
	/** @backend: backend private pointer (e.g. the &backlight_device) */
	void *backend;
	/** @list: entry on the global list of linked DRM backlights */
	struct list_head list;
	/** @work: deferred hardware update and uevent */
	struct work_struct work;
	/** @set_value: luminance value pending application by @work */
	unsigned int set_value;
	/**
	 * @luminance_clients: number of luminance-aware DRM clients that have
	 * taken this backlight over. While > 0, legacy sysfs writes to the
	 * linked backend return -EBUSY.
	 */
	unsigned int luminance_clients;
	/** @changed: a uevent is pending for @work to emit */
	bool changed : 1;
};

#if IS_ENABLED(CONFIG_DRM_BACKLIGHT)

int drm_backlight_init(void);
void drm_backlight_exit(void);

void drm_backlight_connector_init(struct drm_connector *connector);
void drm_backlight_connector_cleanup(struct drm_connector *connector);
void drm_backlight_unregister(struct drm_connector *connector);

int drm_backlight_link(struct drm_connector *connector,
		       struct backlight_device *bd);
struct backlight_device *drm_backlight_get_device(struct drm_connector *connector);

void drm_backlight_inhibit_legacy(struct drm_connector *connector);
void drm_backlight_uninhibit_legacy(struct drm_connector *connector);
void drm_backlight_inhibit_legacy_all(struct drm_device *dev);
void drm_backlight_uninhibit_legacy_all(struct drm_device *dev);

void drm_backlight_set_luminance(struct drm_connector *connector,
				 unsigned int value);

#else /* CONFIG_DRM_BACKLIGHT */

static inline int drm_backlight_init(void) { return 0; }
static inline void drm_backlight_exit(void) {}

static inline void drm_backlight_connector_init(struct drm_connector *connector) {}
static inline void drm_backlight_connector_cleanup(struct drm_connector *connector) {}
static inline void drm_backlight_unregister(struct drm_connector *connector) {}

static inline int drm_backlight_link(struct drm_connector *connector,
				     struct backlight_device *bd)
{
	return 0;
}

static inline struct backlight_device *
drm_backlight_get_device(struct drm_connector *connector)
{
	return NULL;
}

static inline void drm_backlight_inhibit_legacy(struct drm_connector *connector) {}
static inline void drm_backlight_uninhibit_legacy(struct drm_connector *connector) {}
static inline void drm_backlight_inhibit_legacy_all(struct drm_device *dev) {}
static inline void drm_backlight_uninhibit_legacy_all(struct drm_device *dev) {}

static inline void drm_backlight_set_luminance(struct drm_connector *connector,
					       unsigned int value) {}

#endif /* CONFIG_DRM_BACKLIGHT */

#endif /* __DRM_BACKLIGHT_H__ */
