/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2024 Collabora Ltd */
/* Copyright 2024 Arm ltd. */

#ifndef __PANTHOR_PERF_H__
#define __PANTHOR_PERF_H__

#include <linux/types.h>

struct drm_gem_object;
struct drm_panthor_perf_cmd_setup;
struct panthor_device;
struct panthor_file;
struct panthor_perf;

void panthor_perf_info_init(struct panthor_device *ptdev);

int panthor_perf_init(struct panthor_device *ptdev);
void panthor_perf_unplug(struct panthor_device *ptdev);

int panthor_perf_session_setup(struct panthor_device *ptdev, struct panthor_perf *perf,
		struct drm_panthor_perf_cmd_setup *setup_args,
		struct panthor_file *pfile);
int panthor_perf_session_teardown(struct panthor_file *pfile, struct panthor_perf *perf,
		u32 sid);
int panthor_perf_session_start(struct panthor_file *pfile, struct panthor_perf *perf,
		u32 sid, u64 user_data);
int panthor_perf_session_stop(struct panthor_file *pfile, struct panthor_perf *perf,
		u32 sid, u64 user_data);
int panthor_perf_session_sample(struct panthor_file *pfile, struct panthor_perf *perf,
		u32 sid, u64 user_data);
void panthor_perf_session_destroy(struct panthor_file *pfile, struct panthor_perf *perf);

#endif /* __PANTHOR_PERF_H__ */
