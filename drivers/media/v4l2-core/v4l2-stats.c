// SPDX-License-Identifier: GPL-2.0-only
/*
 * v4l2-stats.c
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

#include <linux/types.h>
#include <linux/seq_file.h>
#include <linux/clk.h>
#include <media/v4l2-stats.h>

static const char * const dev_type_name[] = {
	[MEDIA_DEV_TYPE_V4L2] = "media",
	[MEDIA_DEV_TYPE_V4L2_STATELESS_ENCODER] = "encoder",
	[MEDIA_DEV_TYPE_V4L2_STATELESS_DECODER] = "decoder",
};

void v4l2_stats_init(struct v4l2_stats *stats)
{
	stats->hw_usage_time = 0;
	stats->media_dev_type = MEDIA_DEV_TYPE_V4L2;
}

void v4l2_stats_exit(struct v4l2_stats *stats)
{
}

void v4l2_stats_update_hw_usage(struct v4l2_stats *stats, u64 usage_time)
{
	stats->hw_usage_time += usage_time;
}
EXPORT_SYMBOL_GPL(v4l2_stats_update_hw_usage);

void v4l2_stats_set_media_dev_type(struct v4l2_stats *stats, enum v4l2_media_dev_type type)
{
	if (type >= MEDIA_DEV_TYPE_COUNT)
		return;

	stats->media_dev_type = type;
}
EXPORT_SYMBOL_GPL(v4l2_stats_set_media_dev_type);

void v4l2_stats_show(struct v4l2_stats *stats, struct seq_file *m)
{
	seq_printf(m, "media-type:\t%s\n", dev_type_name[stats->media_dev_type]);
	seq_printf(m, "media-engine-usage:\t%llu ns\n", stats->hw_usage_time);
}
EXPORT_SYMBOL_GPL(v4l2_stats_show);

void v4l2_stats_show_clock(struct seq_file *m, struct clk *clk)
{
	seq_printf(m, "media-maxfreq:\t%lu Hz\n",
		   clk_get_rate(clk));
	seq_printf(m, "media-curfreq:\t%lu Hz\n",
		   clk_get_rate(clk));
}
EXPORT_SYMBOL_GPL(v4l2_stats_show_clock);
