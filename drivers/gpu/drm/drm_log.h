/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef __DRM_LOG_H__
#define __DRM_LOG_H__

#ifdef CONFIG_DRM_LOG
void drm_log_register(struct drm_device *dev);
#else
static inline void drm_log_register(struct drm_device *dev) {}
#endif

#endif
