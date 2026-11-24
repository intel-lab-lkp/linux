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

#define MALI_C720_NUM_DECOMPANDER_LUT_ELEMENTS 33

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

/**
 * enum mali_c720_param_block_type - Mali-C720 parameter block targets
 *
 * @MALI_C720_PARAM_BLOCK_DIGITAL_GAIN_INP: Digital gain for the input pipeline.
 * @MALI_C720_PARAM_BLOCK_HISTOGRAM_INP: Histogram configuration for the input pipeline.
 * @MALI_C720_PARAM_BLOCK_AWB_GAINS_HV: Auto-white balance gains for the HV pipeline.
 * @MALI_C720_PARAM_BLOCK_HISTOGRAM_HV: HV histogram configuration (pre-Iridix).
 * @MALI_C720_PARAM_BLOCK_HISTOGRAM_HV_IRIDIX: HV histogram configuration (post-Iridix).
 * @MALI_C720_PARAM_BLOCK_COLOR_STATS_HV: Color stats configuration for the HV pipeline.
 * @MALI_C720_PARAM_BLOCK_BLACK_LEVEL_HV: Black level offsets for the HV pipeline.
 * @MALI_C720_PARAM_BLOCK_SQRT_INP: Square root configuration for the input pipeline.
 * @MALI_C720_PARAM_BLOCK_SQUARE_A_HV: Square A configuration for the HV pipeline.
 * @MALI_C720_PARAM_BLOCK_SQUARE_B_HV: Square B configuration for the HV pipeline.
 * @MALI_C720_PARAM_BLOCK_SQRT_HV: Square root configuration for the HV pipeline.
 * @MALI_C720_PARAM_BLOCK_DECOMPANDER_INP: Decompander configuration.
 *
 * Each identifier selects one logical module and instance. Applications can
 * query V4L2_CID_MALI_C720_PARAM_CAPABILITIES for the targets present on the
 * device. The control returns a read-only U16 array of supported identifiers.
 */
enum mali_c720_param_block_type {
	MALI_C720_PARAM_BLOCK_DIGITAL_GAIN_INP,
	MALI_C720_PARAM_BLOCK_HISTOGRAM_INP,
	MALI_C720_PARAM_BLOCK_AWB_GAINS_HV,
	MALI_C720_PARAM_BLOCK_HISTOGRAM_HV,
	MALI_C720_PARAM_BLOCK_HISTOGRAM_HV_IRIDIX,
	MALI_C720_PARAM_BLOCK_COLOR_STATS_HV,
	MALI_C720_PARAM_BLOCK_BLACK_LEVEL_HV,
	MALI_C720_PARAM_BLOCK_SQRT_INP,
	MALI_C720_PARAM_BLOCK_SQUARE_A_HV,
	MALI_C720_PARAM_BLOCK_SQUARE_B_HV,
	MALI_C720_PARAM_BLOCK_SQRT_HV,
	MALI_C720_PARAM_BLOCK_DECOMPANDER_INP,
	MALI_C720_PARAM_BLOCK_MAX,
};

#define V4L2_CID_MALI_C720_STATS_CAPABILITIES		(V4L2_CID_USER_MALI_C720_BASE + 0)
#define V4L2_CID_MALI_C720_PARAM_CAPABILITIES		(V4L2_CID_USER_MALI_C720_BASE + 1)

enum mali_c720_params_digital_gain_mask {
	MALI_C720_PARAMS_DIGITAL_GAIN_MASK_GAIN,
	MALI_C720_PARAMS_DIGITAL_GAIN_MASK_OFFSET,
};

/**
 * struct mali_c720_params_digital_gain - Digital gain settings
 *
 * This struct configures the digital gain and a common black level for all
 * color channels.
 *
 * Set header.type to the target from &enum mali_c720_param_block_type.
 * See &enum mali_c720_params_digital_gain_mask for the @updated_mask bit positions.
 * Undefined bits are reserved and must be set to zero.
 *
 * @header: The generic V4L2 ISP block header
 * @updated_mask: Mask of configuration fields to update
 * @gain: Input digital gain in unsigned Q12.8 format [0..1048575]; 256 = 1x
 * @offset: Input black level [0..16777215]
 */
struct mali_c720_params_digital_gain {
	struct v4l2_isp_block_header header;
	__u64 updated_mask;
	__u32 gain;
	__u32 offset;
};

enum mali_c720_params_awb_gains_mask {
	MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN00,
	MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN01,
	MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN10,
	MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN11,
};

/**
 * struct mali_c720_params_awb_gains - Gain settings for auto white balance
 *
 * There are four gain settings corresponding to the Bayer color channels.
 * The ISP accounts for the input Bayer order, so the mapping to R, Gr, Gb
 * and B is independent of that order. A value of 256 represents a 1x gain.
 *
 * Set header.type to the target from &enum mali_c720_param_block_type.
 * See &enum mali_c720_params_awb_gains_mask for the @updated_mask bit positions.
 * Undefined bits are reserved and must be set to zero.
 *
 * @header: The generic V4L2 ISP block header
 * @updated_mask: Mask of configuration fields to update
 * @gain00: Multiplier for color channel 00 (R), unsigned Q4.8 [0..4095]
 * @gain01: Multiplier for color channel 01 (Gr), unsigned Q4.8 [0..4095]
 * @gain10: Multiplier for color channel 10 (Gb), unsigned Q4.8 [0..4095]
 * @gain11: Multiplier for color channel 11 (B), unsigned Q4.8 [0..4095]
 */
struct mali_c720_params_awb_gains {
	struct v4l2_isp_block_header header;
	__u64 updated_mask;
	__u16 gain00;
	__u16 gain01;
	__u16 gain10;
	__u16 gain11;
};

enum mali_c720_params_black_level_mask {
	MALI_C720_PARAMS_BLACK_LEVEL_MASK_CHAN00,
	MALI_C720_PARAMS_BLACK_LEVEL_MASK_CHAN01,
	MALI_C720_PARAMS_BLACK_LEVEL_MASK_CHAN10,
	MALI_C720_PARAMS_BLACK_LEVEL_MASK_CHAN11,
};

/**
 * struct mali_c720_params_black_level - Black level for each color channel
 *
 * This struct configures black level subtraction from each Bayer color channel.
 *
 * Set header.type to the target from &enum mali_c720_param_block_type.
 * See &enum mali_c720_params_black_level_mask for the @updated_mask bit positions.
 * Undefined bits are reserved and must be set to zero.
 *
 * @header: The generic V4L2 ISP block header
 * @updated_mask: Mask of configuration fields to update
 * @chan00: Offset from color channel 00 (R); HV range [0..65535]
 * @chan01: Offset from color channel 01 (Gr); HV range [0..65535]
 * @chan10: Offset from color channel 10 (Gb); HV range [0..65535]
 * @chan11: Offset from color channel 11 (B); HV range [0..65535]
 */
struct mali_c720_params_black_level {
	struct v4l2_isp_block_header header;
	__u64 updated_mask;
	__u32 chan00;
	__u32 chan01;
	__u32 chan10;
	__u32 chan11;
};

enum mali_c720_params_square_sqrt_mask {
	MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_IN,
	MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_OUT,
	MALI_C720_PARAMS_SQUARE_SQRT_MASK_NORMALIZATION,
};

/**
 * struct mali_c720_params_sqrt - Configuration for the square root module
 *
 * The square root module compands linear data into the square root domain,
 * with black level correction and normalization.
 *
 * Set header.type to the target from &enum mali_c720_param_block_type.
 * See &enum mali_c720_params_square_sqrt_mask for the @updated_mask bit positions.
 * Undefined bits are reserved and must be set to zero.
 *
 * @header: The generic V4L2 ISP block header
 * @updated_mask: Mask of configuration fields to update
 * @normalization: Normalization coefficient applied before the square root,
 *                 in Q1.16 format [0..131071]; 65536 = 1.0
 * @black_level_linear: Input black level in the linear domain [0..16777215]
 * @black_level_sqrt: Output black level in the square root domain [0..4095]
 */
struct mali_c720_params_sqrt {
	struct v4l2_isp_block_header header;
	__u64 updated_mask;
	__u32 normalization;
	__u32 black_level_linear;
	__u16 black_level_sqrt;
};

/**
 * struct mali_c720_params_square - Configuration for the square module
 *
 * The square module converts square root data back into the linear domain,
 * with black level correction and normalization.
 *
 * Set header.type to the target from &enum mali_c720_param_block_type.
 * See &enum mali_c720_params_square_sqrt_mask for the @updated_mask bit positions.
 * Undefined bits are reserved and must be set to zero.
 *
 * @header: The generic V4L2 ISP block header
 * @updated_mask: Mask of configuration fields to update
 * @black_level_linear: Output black level, [0..4095] for Square-A or
 *                      [0..16777215] for Square-B
 * @black_level_sqrt: Input black level, [0..16777215] for Square-A or
 *                    [0..4095] for Square-B
 * @normalization: Normalization coefficient applied after squaring,
 *                 in Q1.16 format [0..131071]; 65536 = 1.0
 */
struct mali_c720_params_square {
	struct v4l2_isp_block_header header;
	__u64 updated_mask;
	__u32 black_level_linear;
	__u32 black_level_sqrt;
	__u32 normalization;
};

enum mali_c720_params_decompander_mask {
	MALI_C720_PARAMS_DECOMPANDER_MASK_ENABLE,
	MALI_C720_PARAMS_DECOMPANDER_MASK_LUT_X,
	MALI_C720_PARAMS_DECOMPANDER_MASK_LUT_Y,
};

/**
 * struct mali_c720_params_decompander - Configuration for the decompander
 *
 * The input decompander expands compressed sensor data using a piecewise
 * linear curve defined by 33 points. Each X/Y pair maps an input pixel
 * intensity to an expanded output intensity, with linear interpolation
 * between points.
 *
 * The X coordinates must be in nondecreasing order and cover the input range.
 * A nonzero difference between adjacent X coordinates must be at least 8.
 * Repeated X coordinates must have equal Y coordinates.
 * Both lut_x and lut_y fields must be provided together, with their respective
 * updated_mask bits set.
 *
 * Y coordinates may exceed the 24-bit output range or be negative.
 * Interpolation uses these extended values before clamping the final pixel
 * output to the 24-bit range [0..16777215]. Nonnegative Y coordinates
 * [0..0x17fffff] are stored directly; encodings [0x1800000..0x1ffffff]
 * represent negative values, decoded by subtracting 0x2000000.
 *
 * Set header.type to the target from &enum mali_c720_param_block_type.
 * See &enum mali_c720_params_decompander_mask for the @updated_mask bit positions.
 * Undefined bits are reserved and must be set to zero.
 *
 * @header: The generic V4L2 ISP block header
 * @updated_mask: Mask of configuration fields to update
 * @enable: Enable decompanding: 0 = bypass, 1 = enabled
 * @reserved: Reserved; set to zero
 * @lut_x: 33 input pixel intensities, unsigned 24-bit [0..16777215]
 * @lut_y: 33 corresponding output pixel intensities, using the encoding above
 */
struct mali_c720_params_decompander {
	struct v4l2_isp_block_header header;
	__u64 updated_mask;
	__u8 enable;
	__u8 reserved[3];
	__u32 lut_x[MALI_C720_NUM_DECOMPANDER_LUT_ELEMENTS];
	__u32 lut_y[MALI_C720_NUM_DECOMPANDER_LUT_ELEMENTS];
};

/**
 * enum mali_c720_awb_stats_mode - Statistics mode for auto white balance
 * @MALI_C720_AWB_MODE_GRGB: Green/Red and Green/Blue ratios
 * @MALI_C720_AWB_MODE_RGBG: Red/Green and Blue/Green ratios
 */
enum mali_c720_awb_stats_mode {
	MALI_C720_AWB_MODE_GRGB = 0,
	MALI_C720_AWB_MODE_RGBG = 1,
};

enum mali_c720_params_color_stats_mask {
	MALI_C720_PARAMS_COLOR_STATS_MASK_NODES_USED_HORIZ,
	MALI_C720_PARAMS_COLOR_STATS_MASK_NODES_USED_VERT,
	MALI_C720_PARAMS_COLOR_STATS_MASK_AWB_STATS_MODE,
	MALI_C720_PARAMS_COLOR_STATS_MASK_WHITE_LEVEL_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_BLACK_LEVEL_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_CR_REF_MAX_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_CR_REF_MIN_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_CR_REF_HIGH_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_CR_REF_LOW_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_CB_REF_MAX_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_CB_REF_MIN_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_CB_REF_HIGH_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_CB_REF_LOW_AWB,
	MALI_C720_PARAMS_COLOR_STATS_MASK_WEIGHTS,
};

/**
 * struct mali_c720_params_color_stats - Color statistics configuration
 *
 * This struct configures the statistics generated for auto white balance.
 * Pixel intensity limits exclude overly bright or dark pixels from the
 * statistics. Color ratio minima and maxima bound a region in R/G and B/G
 * space, while the high and low ratios trim its upper-right and lower-left
 * corners. Only pixels whose ratios fall inside the region marked "A" are
 * counted, independently of @awb_stats_mode::
 *
 *     B/G
 *      ^
 *  max |  +-----------------+  (high, max)
 *      |  |                  \
 *  high|  |                   + (max, high)
 *      |  |         A         |
 *  low |  +                   |
 *      |   \                  |
 *  min |    +-----------------+
 *      +--------------------------> R/G
 *         min low          high max
 *
 * Setting low to min and high to max selects the full rectangle.
 *
 * The product of @nodes_used_horiz and @nodes_used_vert must not exceed
 * MALI_C720_MAX_ZONES (1024). Exceeding this limit produces invalid statistics
 * that cannot be parsed reliably.
 * When updating zone dimensions, both horizontal and vertical directions must
 * be provided with their respective updated_mask bits set.
 *
 * Zone weights control each zone's contribution to the global statistics.
 * A weight of 0 excludes the zone; values 1..14 represent weights of
 * 1/16..14/16 and 15 applies the full weight. Per-zone results are not weighted.
 *
 * Set header.type to the target from &enum mali_c720_param_block_type.
 * See &enum mali_c720_params_color_stats_mask for the @updated_mask bit positions.
 * Undefined bits are reserved and must be set to zero.
 *
 * @header: The generic V4L2 ISP block header
 * @updated_mask: Mask of configuration fields to update
 * @nodes_used_horiz: Number of horizontal zones [3..32]
 * @nodes_used_vert: Number of vertical zones [3..255], subject to the total
 *                   zone limit above
 * @awb_stats_mode: Ratio mode, see &enum mali_c720_awb_stats_mode
 * @reserved: Reserved; set to zero
 * @white_level_awb: Upper pixel intensity limit (exclusive), 14-bit [0..16383]
 * @black_level_awb: Lower pixel intensity limit (exclusive), 14-bit [0..16383]
 * @cr_ref_max_awb: Maximum R/G ratio, unsigned Q4.8 [0..4095]
 * @cr_ref_min_awb: Minimum R/G ratio, unsigned Q4.8 [0..4095]
 * @cr_ref_high_awb: R/G ratio trim high, unsigned Q4.8 [0..4095]
 * @cr_ref_low_awb: R/G ratio trim low, unsigned Q4.8 [0..4095]
 * @cb_ref_max_awb: Maximum B/G ratio, unsigned Q4.8 [0..4095]
 * @cb_ref_min_awb: Minimum B/G ratio, unsigned Q4.8 [0..4095]
 * @cb_ref_high_awb: B/G ratio trim high, unsigned Q4.8 [0..4095]
 * @cb_ref_low_awb: B/G ratio trim low, unsigned Q4.8 [0..4095]
 * @weights: Zone weights in row-major order from the top left, one byte per
 *           zone [0..15]. Index is y * nodes_used_horiz + x.
 */
struct mali_c720_params_color_stats {
	struct v4l2_isp_block_header header;
	__u64 updated_mask;
	__u8 nodes_used_horiz;
	__u8 nodes_used_vert;
	__u8 awb_stats_mode;
	__u8 reserved;
	__u16 white_level_awb;
	__u16 black_level_awb;
	__u16 cr_ref_max_awb;
	__u16 cr_ref_min_awb;
	__u16 cr_ref_high_awb;
	__u16 cr_ref_low_awb;
	__u16 cb_ref_max_awb;
	__u16 cb_ref_min_awb;
	__u16 cb_ref_high_awb;
	__u16 cb_ref_low_awb;
	__u8 weights[MALI_C720_MAX_ZONES];
};

/**
 * enum mali_c720_histogram_input - Input for pre-Iridix histograms
 * @MALI_C720_HISTOGRAM_INPUT_RAW: Raw Bayer data
 * @MALI_C720_HISTOGRAM_INPUT_LUMA: Luminance data
 */
enum mali_c720_histogram_input {
	MALI_C720_HISTOGRAM_INPUT_RAW = 0,
	MALI_C720_HISTOGRAM_INPUT_LUMA = 1,
};

/**
 * enum mali_c720_histogram_skip_x - Horizontal pixel skipping
 * @MALI_C720_HISTOGRAM_SKIP_X_EVERY_2ND: Collect every 2nd pixel horizontally
 * @MALI_C720_HISTOGRAM_SKIP_X_EVERY_3RD: Collect every 3rd pixel horizontally
 * @MALI_C720_HISTOGRAM_SKIP_X_EVERY_4TH: Collect every 4th pixel horizontally
 * @MALI_C720_HISTOGRAM_SKIP_X_EVERY_5TH: Collect every 5th pixel horizontally
 * @MALI_C720_HISTOGRAM_SKIP_X_EVERY_8TH: Collect every 8th pixel horizontally
 * @MALI_C720_HISTOGRAM_SKIP_X_EVERY_9TH: Collect every 9th pixel horizontally
 */
enum mali_c720_histogram_skip_x {
	MALI_C720_HISTOGRAM_SKIP_X_EVERY_2ND = 0,
	MALI_C720_HISTOGRAM_SKIP_X_EVERY_3RD = 1,
	MALI_C720_HISTOGRAM_SKIP_X_EVERY_4TH = 2,
	MALI_C720_HISTOGRAM_SKIP_X_EVERY_5TH = 3,
	MALI_C720_HISTOGRAM_SKIP_X_EVERY_8TH = 4,
	MALI_C720_HISTOGRAM_SKIP_X_EVERY_9TH = 5,
};

/**
 * enum mali_c720_histogram_skip_y - Vertical pixel skipping
 * @MALI_C720_HISTOGRAM_SKIP_Y_ALL: Collect every row
 * @MALI_C720_HISTOGRAM_SKIP_Y_EVERY_2ND: Collect every 2nd row
 * @MALI_C720_HISTOGRAM_SKIP_Y_EVERY_3RD: Collect every 3rd row
 * @MALI_C720_HISTOGRAM_SKIP_Y_EVERY_4TH: Collect every 4th row
 * @MALI_C720_HISTOGRAM_SKIP_Y_EVERY_5TH: Collect every 5th row
 * @MALI_C720_HISTOGRAM_SKIP_Y_EVERY_8TH: Collect every 8th row
 * @MALI_C720_HISTOGRAM_SKIP_Y_EVERY_9TH: Collect every 9th row
 */
enum mali_c720_histogram_skip_y {
	MALI_C720_HISTOGRAM_SKIP_Y_ALL = 0,
	MALI_C720_HISTOGRAM_SKIP_Y_EVERY_2ND = 1,
	MALI_C720_HISTOGRAM_SKIP_Y_EVERY_3RD = 2,
	MALI_C720_HISTOGRAM_SKIP_Y_EVERY_4TH = 3,
	MALI_C720_HISTOGRAM_SKIP_Y_EVERY_5TH = 4,
	MALI_C720_HISTOGRAM_SKIP_Y_EVERY_8TH = 5,
	MALI_C720_HISTOGRAM_SKIP_Y_EVERY_9TH = 6,
};

/**
 * enum mali_c720_histogram_row_column_offset - Starting row or column
 * @MALI_C720_HISTOGRAM_FIRST_ROW_OR_COL: Start from the first row or column
 * @MALI_C720_HISTOGRAM_SECOND_ROW_OR_COL: Start from the second row or column
 */
enum mali_c720_histogram_row_column_offset {
	MALI_C720_HISTOGRAM_FIRST_ROW_OR_COL = 0,
	MALI_C720_HISTOGRAM_SECOND_ROW_OR_COL = 1,
};

/**
 * enum mali_c720_histogram_scale - Intensity range scaling
 * @MALI_C720_HISTOGRAM_SCALE_1X: No scaling
 * @MALI_C720_HISTOGRAM_SCALE_2X: Scale by 2
 * @MALI_C720_HISTOGRAM_SCALE_4X: Scale by 4
 * @MALI_C720_HISTOGRAM_SCALE_8X: Scale by 8
 * @MALI_C720_HISTOGRAM_SCALE_16X: Scale by 16
 */
enum mali_c720_histogram_scale {
	MALI_C720_HISTOGRAM_SCALE_1X = 0,
	MALI_C720_HISTOGRAM_SCALE_2X = 1,
	MALI_C720_HISTOGRAM_SCALE_4X = 2,
	MALI_C720_HISTOGRAM_SCALE_8X = 3,
	MALI_C720_HISTOGRAM_SCALE_16X = 4,
};

/**
 * enum mali_c720_histogram_plane_mode - Mode for histogram collection
 * @MALI_C720_HISTOGRAM_COMBINED: All color planes in one 1024-bin histogram
 *                              with a bin width of 16384
 * @MALI_C720_HISTOGRAM_SEPARATE: Each color plane in one 256-bin histogram
 *                             with a bin width of 65536
 * @MALI_C720_HISTOGRAM_SEPARATE_ALL: As %MALI_C720_HISTOGRAM_SEPARATE, without
 *                                 pixel skipping; ignores skip and offset fields
 * @MALI_C720_HISTOGRAM_FOCUS_00: Top left plane in the first bank, rest in second bank
 * @MALI_C720_HISTOGRAM_FOCUS_01: Top right plane in the first bank, rest in second bank
 * @MALI_C720_HISTOGRAM_FOCUS_10: Bottom left plane in the first bank, rest in second bank
 * @MALI_C720_HISTOGRAM_FOCUS_11: Bottom right plane in the first bank, rest in second bank
 *
 * In the "focus" modes statistics are collected into two 512-bin histograms
 * with a bin width of 32768. One color plane is in the first histogram with
 * the remainder combined into the second. The four options represent which
 * of the four positions in a Bayer pattern is the focused plane.
 *
 * Bin widths are expressed in 24-bit input levels with intensity scaling set
 * to %MALI_C720_HISTOGRAM_SCALE_1X. Value 3 is reserved.
 */
enum mali_c720_histogram_plane_mode {
	MALI_C720_HISTOGRAM_COMBINED = 0,
	MALI_C720_HISTOGRAM_SEPARATE = 1,
	MALI_C720_HISTOGRAM_SEPARATE_ALL = 2,
	MALI_C720_HISTOGRAM_FOCUS_00 = 4,
	MALI_C720_HISTOGRAM_FOCUS_01 = 5,
	MALI_C720_HISTOGRAM_FOCUS_10 = 6,
	MALI_C720_HISTOGRAM_FOCUS_11 = 7,
};

enum mali_c720_params_histogram_mask {
	MALI_C720_PARAMS_HISTOGRAM_MASK_STREAM_MUX_SEL,
	MALI_C720_PARAMS_HISTOGRAM_MASK_NODES_USED_HORIZ,
	MALI_C720_PARAMS_HISTOGRAM_MASK_NODES_USED_VERT,
	MALI_C720_PARAMS_HISTOGRAM_MASK_SKIP_X,
	MALI_C720_PARAMS_HISTOGRAM_MASK_SKIP_Y,
	MALI_C720_PARAMS_HISTOGRAM_MASK_OFFSET_X,
	MALI_C720_PARAMS_HISTOGRAM_MASK_OFFSET_Y,
	MALI_C720_PARAMS_HISTOGRAM_MASK_SCALE_BOTTOM,
	MALI_C720_PARAMS_HISTOGRAM_MASK_SCALE_TOP,
	MALI_C720_PARAMS_HISTOGRAM_MASK_PLANE_MODE,
	MALI_C720_PARAMS_HISTOGRAM_MASK_WEIGHTS,
};

/**
 * struct mali_c720_params_histogram - Histogram configuration
 *
 * This struct configures the 1024-bin histograms. The parameters allow
 * particular regions of the image to be masked and different kinds of
 * histogram to be selected.
 *
 * The @skip_x, @offset_x, @skip_y and @offset_y fields select pixels by their
 * position relative to the top left of the image. These fields reduce the
 * number of pixels counted, but some combinations exclude entire Bayer color
 * components.
 *
 * The zone dimensions define the spatial weighting grid, whose total number
 * of zones must not exceed MALI_C720_MAX_ZONES (1024). A zero weight
 * excludes the zone from the histogram.
 * When updating zone dimensions, both horizontal and vertical directions must
 * be provided with their respective updated_mask bits set.
 *
 * @scale_bottom and @scale_top increase bin resolution at the dark and bright
 * ends of the input range respectively. Samples outside the selected ranges
 * do not contribute to the histogram.
 *
 * Set header.type to the target from &enum mali_c720_param_block_type.
 * See &enum mali_c720_params_histogram_mask for the @updated_mask bit positions.
 * Undefined bits are reserved and must be set to zero.
 *
 * @header: The generic V4L2 ISP block header
 * @updated_mask: Mask of configuration fields to update
 * @stream_mux_select: Input selection for pre-Iridix histograms. See
 *                     &enum mali_c720_histogram_input. Ignored for other targets.
 * @nodes_used_horiz: Number of active zones horizontally [1..255], subject to the total
 *                    zone limit above
 * @nodes_used_vert: Number of active zones vertically [1..255], subject to the total
 *                   zone limit above
 * @skip_x: Horizontal decimation. See &enum mali_c720_histogram_skip_x
 * @skip_y: Vertical decimation. See &enum mali_c720_histogram_skip_y
 * @offset_x: Skip the first column, or not. See &enum mali_c720_histogram_row_column_offset
 * @offset_y: Skip the first row, or not. See &enum mali_c720_histogram_row_column_offset.
 *            With @skip_y set to %MALI_C720_HISTOGRAM_SKIP_Y_ALL, this selects
 *            the starting column on alternate rows instead.
 * @scale_bottom: Scale pixels in the bottom half of the intensity range.
 *                See &enum mali_c720_histogram_scale
 * @scale_top: Scale pixels in the top half of the intensity range.
 *             See &enum mali_c720_histogram_scale
 * @plane_mode: Plane separation mode. See &enum mali_c720_histogram_plane_mode
 * @reserved: Reserved; set to zero
 * @weights: Zone weighting, one byte per zone, starting at the top left and
 *           continuing in raster order. Each zone can be weighted in [0..15].
 *           The grid has @nodes_used_horiz columns and @nodes_used_vert rows.
 */
struct mali_c720_params_histogram {
	struct v4l2_isp_block_header header;
	__u64 updated_mask;
	__u8 stream_mux_select;
	__u8 nodes_used_horiz;
	__u8 nodes_used_vert;
	__u8 skip_x;
	__u8 skip_y;
	__u8 offset_x;
	__u8 offset_y;
	__u8 scale_bottom;
	__u8 scale_top;
	__u8 plane_mode;
	__u8 reserved[2];
	__u8 weights[MALI_C720_MAX_ZONES];
};

/**
 * MALI_C720_PARAMS_MAX_SIZE - Maximum size of all parameters for Mali-C720
 * in a full hardware configuration.
 *
 * Though the parameters for the Mali-C720 are passed as optional blocks, the
 * driver still needs to know the absolute maximum size so that it can allocate
 * a buffer sized appropriately to accommodate userspace attempting to set all
 * possible parameters in a single frame.
 *
 * Some structs are in this list multiple times. Where that's the case, it just
 * reflects the fact that several block identifiers share the same payload
 * structure.
 */
#define MALI_C720_PARAMS_MAX_SIZE				\
	(sizeof(struct mali_c720_params_digital_gain) +		\
	 sizeof(struct mali_c720_params_histogram) +		\
	 sizeof(struct mali_c720_params_awb_gains) +		\
	 sizeof(struct mali_c720_params_histogram) +		\
	 sizeof(struct mali_c720_params_histogram) +		\
	 sizeof(struct mali_c720_params_color_stats) +		\
	 sizeof(struct mali_c720_params_black_level) +		\
	 sizeof(struct mali_c720_params_sqrt) +			\
	 sizeof(struct mali_c720_params_square) +		\
	 sizeof(struct mali_c720_params_square) +		\
	 sizeof(struct mali_c720_params_sqrt) +			\
	 sizeof(struct mali_c720_params_decompander))

#endif
