/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef __UAPI_MALI_C720_CONFIG_H
#define __UAPI_MALI_C720_CONFIG_H

#include <linux/types.h>
#include <linux/v4l2-controls.h>
#include <linux/media/v4l2-isp.h>

#define MALI_C720_MAX_ZONES 1024

/**
 * struct mali_c720_awb_stats_nodes_used - Color statistics zone dimensions
 * @reserved1: Undefined buffer space
 * @nodes_used_horiz: Number of horizontal zones used for this frame
 * @nodes_used_vert: Number of vertical zones used for this frame
 * @reserved2: Undefined buffer space
 *
 * The product of the dimensions gives the number of valid zone records in
 * &struct mali_c720_awb_statistics, up to MALI_C720_MAX_ZONES.
 */
struct mali_c720_awb_stats_nodes_used {
	__u8 reserved1[8];
	__u8 nodes_used_horiz;
	__u8 nodes_used_vert;
	__u8 reserved2[6];
} __attribute__((aligned(16)));

/**
 * struct mali_c720_awb_statistics_tail - Global color statistics
 * @global_rg: Global R/G or G/R ratio, in Q4.8 format
 * @global_bg: Global B/G or G/B ratio, in Q4.8 format
 * @global_num_pixels: Number of accepted pixels after zone weighting
 * @frame_count: Frame counter
 * @crc16: Statistics CRC
 * @reserved: Undefined buffer space
 *
 * This structure contains the global color ratios and pixel count. It follows
 * the active zone records and can be located using mali_c720_awb_stats_tail().
 * The ratio mode is selected by awb_stats_mode in
 * &struct mali_c720_params_color_stats. See &enum mali_c720_awb_stats_mode.
 */
struct mali_c720_awb_statistics_tail {
	__le16 global_rg;
	__le16 global_bg;
	__le32 global_num_pixels;
	__le32 frame_count;
	__le16 crc16;
	__u8 reserved[2];
};

/**
 * struct mali_c720_awb_statistics - Per-zone and global color statistics
 * @zones: Zone records in row-major order, starting at the top left
 * @zones.avg_rg: R/G or G/R ratio, in Q4.8 format
 * @zones.avg_bg: B/G or G/B ratio, in Q4.8 format
 * @zones.num_pixels: Number of accepted pixels in the zone before weighting
 * @tail_reserved: Undefined buffer space
 * @nodes_used: Zone dimensions used for this frame
 *
 * The ISP collects average color ratios and pixel counts for each zone. The
 * global statistics follow the last active zone and can be located using
 * mali_c720_awb_stats_tail(). The ratio mode is selected by awb_stats_mode in
 * &struct mali_c720_params_color_stats. See &enum mali_c720_awb_stats_mode.
 */
struct mali_c720_awb_statistics {
	struct {
		__le16 avg_rg;
		__le16 avg_bg;
		__le32 num_pixels;
	} zones[MALI_C720_MAX_ZONES];

	__u8 tail_reserved[sizeof(struct mali_c720_awb_statistics_tail)];
	struct mali_c720_awb_stats_nodes_used nodes_used;
} __attribute__((aligned(16)));

static inline struct mali_c720_awb_statistics_tail *
mali_c720_awb_stats_tail(struct mali_c720_awb_statistics *stats)
{
	return (struct mali_c720_awb_statistics_tail *)
		(stats->zones + stats->nodes_used.nodes_used_horiz *
		 stats->nodes_used.nodes_used_vert);
}

/**
 * struct mali_c720_ae_1024bin_hist - Histogram statistics
 * @bins: 1024 compressed, weighted pixel counts
 * @total_pixels: Number of sampled pixels before weighting
 * @counted_pixels: Sum of weights accumulated in all histogram banks
 * @plane_total: Sampled pixel counts for each bank before weighting
 * @plane_counted: Sum of weights accumulated in each bank
 * @frame_count: Frame counter
 * @crc16: Statistics CRC
 * @reserved: Undefined buffer space
 *
 * The histogram collects a zone-weighted distribution of pixel intensities.
 * The configured plane mode selects one 1024-bin histogram, four 256-bin
 * histograms or two 512-bin histograms, stored consecutively in @bins. The
 * bank counters follow the same order.
 *
 * Each bin contains a four-bit exponent followed by a twelve-bit mantissa.
 * An approximate weighted pixel count can be decoded as::
 *
 *     e = bin >> 12;
 *     m = bin & 0xfff;
 *     count = (e ? (m + 4096) << (e - 1) : m) * 4;
 */
struct mali_c720_ae_1024bin_hist {
	__le16 bins[1024];
	__le32 total_pixels;
	__le32 counted_pixels;
	__le32 plane_total[4];
	__le32 plane_counted[4];
	__le32 frame_count;
	__le16 crc16;
	__u8 reserved[2];
} __attribute__((aligned(16)));

/**
 * enum mali_c720_stats_type - Mali-C720 statistics block targets
 * @MALI_C720_STATS_NOOP: No data; skip the block using its size.
 * @MALI_C720_STATS_HISTOGRAM_INP: Input histogram statistics.
 * @MALI_C720_STATS_HISTOGRAM_HV: HV histogram statistics (pre-Iridix).
 * @MALI_C720_STATS_HISTOGRAM_HV_IRIDIX: HV histogram statistics (post-Iridix).
 * @MALI_C720_STATS_COLOR_HV: HV color statistics.
 */
enum mali_c720_stats_type {
	MALI_C720_STATS_NOOP,
	MALI_C720_STATS_HISTOGRAM_INP,
	MALI_C720_STATS_HISTOGRAM_HV,
	MALI_C720_STATS_HISTOGRAM_HV_IRIDIX,
	MALI_C720_STATS_COLOR_HV,
	MALI_C720_STATS_MAX,
};

/**
 * struct mali_c720_stats_color - Color statistics block
 * @header: Generic V4L2 ISP block header; type identifies the color statistics
 *          target from &enum mali_c720_stats_type
 * @reserved: Undefined buffer space
 * @data: Color statistics payload
 */
struct mali_c720_stats_color {
	struct v4l2_isp_block_header header;
	__u8 reserved[8];
	struct mali_c720_awb_statistics data;
};

/**
 * struct mali_c720_stats_histogram - Histogram statistics block
 * @header: Generic V4L2 ISP block header; type identifies the histogram target
 *          from &enum mali_c720_stats_type
 * @reserved: Undefined buffer space
 * @data: Histogram payload
 */
struct mali_c720_stats_histogram {
	struct v4l2_isp_block_header header;
	__u8 reserved[8];
	struct mali_c720_ae_1024bin_hist data;
};

/**
 * MALI_C720_STATS_MAX_SIZE - Maximum serialized statistics block size
 *
 * Maximum size of all statistics blocks in a full hardware configuration,
 * including the initial NOOP block used for alignment, but excluding the
 * struct v4l2_isp_buffer header. The actual buffer size depends on the
 * discovered statistics blocks and is reported through the metadata format.
 */
#define MALI_C720_STATS_MAX_SIZE				\
	(sizeof(struct v4l2_isp_block_header) +			\
	 sizeof(struct mali_c720_stats_color) +			\
	 sizeof(struct mali_c720_stats_histogram) +		\
	 sizeof(struct mali_c720_stats_histogram) +		\
	 sizeof(struct mali_c720_stats_histogram))

#define V4L2_CID_MALI_C720_STATS_CAPABILITIES		(V4L2_CID_USER_MALI_C720_BASE + 0)

#endif
