/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2019 Collabora ltd. */
/* Copyright 2026 NXP */

#ifndef __PANTHOR_DEVFREQ_H__
#define __PANTHOR_DEVFREQ_H__

struct devfreq;
struct thermal_cooling_device;

struct panthor_device;
struct panthor_devfreq;

int panthor_devfreq_init(struct panthor_device *ptdev);

void panthor_devfreq_resume(struct panthor_device *ptdev);
void panthor_devfreq_suspend(struct panthor_device *ptdev);

void panthor_devfreq_record_busy(struct panthor_device *ptdev);
void panthor_devfreq_record_idle(struct panthor_device *ptdev);

unsigned long panthor_devfreq_get_freq(struct panthor_device *ptdev);

#ifdef CONFIG_DEBUG_FS
struct drm_minor;
void panthor_devfreq_debugfs_init(struct drm_minor *minor);
#endif

#endif /* __PANTHOR_DEVFREQ_H__ */
