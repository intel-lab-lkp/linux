/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef __DRM_PANIC_H__
#define __DRM_PANIC_H__

/*
 * Copyright (c) 2023 Jocelyn Falempe <jfalempe@redhat.com>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/iosys-map.h>

#ifdef CONFIG_DRM_PANIC

void drm_panic_init(void);
void drm_panic_exit(void);

void drm_panic_init_client(struct drm_device *dev);
#else

static inline void drm_panic_init(void) {}
static inline void drm_panic_exit(void) {}
static inline void drm_panic_init_client(struct drm_device *dev) {}
#endif

#endif /* __DRM_LOG_H__ */
