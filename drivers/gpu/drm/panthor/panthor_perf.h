/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2025 Collabora Ltd */
/* Copyright 2025 Arm ltd. */

#ifndef __PANTHOR_PERF_H__
#define __PANTHOR_PERF_H__

#include <linux/types.h>

struct drm_file;
struct drm_panthor_perf_session_create;
struct panthor_device;
struct panthor_file;
struct panthor_perf;
struct panthor_perf_pool;

int panthor_perf_pool_create(struct panthor_file *pfile);
void panthor_perf_pool_destroy(struct panthor_file *pfile);

int panthor_perf_init(struct panthor_device *ptdev);
void panthor_perf_unplug(struct panthor_device *ptdev);

int panthor_perf_session_create(struct drm_file *file,
				struct drm_panthor_perf_session_create *setup_args,
				unsigned long *sid);
int panthor_perf_session_destroy(struct panthor_file *pfile, unsigned int handle);
int panthor_perf_session_start(struct panthor_file *pfile, u32 handle, u64 user_data);
int panthor_perf_session_stop(struct panthor_file *pfile, unsigned int sid, u64 user_data);
int panthor_perf_session_sample(struct panthor_file *pfile, unsigned int handle, u64 user_data);

#endif /* __PANTHOR_PERF_H__ */

