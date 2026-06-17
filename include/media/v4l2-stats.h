/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * v4l2-stats.h
 *
 * V4L2 statistics management.
 *
 * Maintain a per-file handle list of statistics about the hardware and handle
 * exposing it in the fdinfo.
 *
 * Copyright (C) 2026 Collabora.
 *
 * Contact: Detlev Casanova <detlev.casanova@collabora.com>
 */
#ifndef V4L2_STATS_H
#define V4L2_STATS_H

#include <linux/types.h>

struct clk;
struct seq_file;

enum v4l2_media_dev_type {
	MEDIA_DEV_TYPE_V4L2 = 0,
	MEDIA_DEV_TYPE_V4L2_STATELESS_ENCODER,
	MEDIA_DEV_TYPE_V4L2_STATELESS_DECODER,

	MEDIA_DEV_TYPE_COUNT,
};

struct v4l2_stats {
	u64 hw_usage_time;
	enum v4l2_media_dev_type media_dev_type;
};

void v4l2_stats_init(struct v4l2_stats *stats);
void v4l2_stats_exit(struct v4l2_stats *stats);

void v4l2_stats_update_hw_usage(struct v4l2_stats *stats, u64 usage_time);
void v4l2_stats_set_media_dev_type(struct v4l2_stats *stats, enum v4l2_media_dev_type type);

void v4l2_stats_show(struct v4l2_stats *stats, struct seq_file *m);
void v4l2_stats_show_clock(struct seq_file *m, struct clk *clk);

#endif /* V4L2_STATS_H */
