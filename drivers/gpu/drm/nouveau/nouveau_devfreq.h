/* SPDX-License-Identifier: MIT */
#ifndef __NOUVEAU_DEVFREQ_H__
#define __NOUVEAU_DEVFREQ_H__

struct nouveau_drm;

#if IS_ENABLED(CONFIG_PM_DEVFREQ)
void nouveau_devfreq_init(struct nouveau_drm *drm);
void nouveau_devfreq_fini(struct nouveau_drm *drm);
void nouveau_devfreq_suspend(struct nouveau_drm *drm);
void nouveau_devfreq_resume(struct nouveau_drm *drm);
#else
static inline void nouveau_devfreq_init(struct nouveau_drm *drm) {}
static inline void nouveau_devfreq_fini(struct nouveau_drm *drm) {}
static inline void nouveau_devfreq_suspend(struct nouveau_drm *drm) {}
static inline void nouveau_devfreq_resume(struct nouveau_drm *drm) {}
#endif

#endif
