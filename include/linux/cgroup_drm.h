/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Valve Corporation */

#ifndef _CGROUP_DRM_H
#define _CGROUP_DRM_H

#include <drm/drm_file.h>

#if IS_ENABLED(CONFIG_CGROUP_DRM)
void drmcgroup_client_open(struct drm_file *file_priv);
void drmcgroup_client_close(struct drm_file *file_priv);
void drmcgroup_client_migrate(struct drm_file *file_priv);
#else
static inline void drmcgroup_client_open(struct drm_file *file_priv)
{
}

static inline void drmcgroup_client_close(struct drm_file *file_priv)
{
}

static void drmcgroup_client_migrate(struct drm_file *file_priv)
{
}
#endif

#endif	/* _CGROUP_DRM_H */
