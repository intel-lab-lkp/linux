/* SPDX-License-Identifier: MIT */

#ifndef DRM_CLIENT_SETUP_H
#define DRM_CLIENT_SETUP_H

struct drm_device;
struct drm_format_info;

void drm_client_setup(struct drm_device *dev, const struct drm_format_info *format);
void drm_client_setup_with_color_mode(struct drm_device *dev, unsigned int color_mode);

#endif
