// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/crc-itu-t.h>
#include <linux/dev_printk.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mali-c720-common.h"

#define CAPABILITY_DISCOVERY_HEADER_OFFSET		0x0

#define MALI_C720_DISC_V1_FIELD_ID_MASK			GENMASK(7, 0)
#define MALI_C720_DISC_V1_LIST_VERSION_MASK		GENMASK(11, 8)
#define MALI_C720_DISC_V1_LIST_DESC_COUNT_MASK		GENMASK(31, 16)
#define MALI_C720_DISC_V1_SUBPIPE_INSTANCE_MASK		GENMASK(10, 8)
#define MALI_C720_DISC_V1_SUBPIPE_TYPE_MASK		GENMASK(15, 11)
#define MALI_C720_DISC_V1_SUBPIPE_DESC_COUNT_MASK	GENMASK(31, 16)
#define MALI_C720_DISC_V1_MODULE_INSTANCE_MASK		GENMASK(10, 8)
#define MALI_C720_DISC_V1_MODULE_REMOVED_MASK		BIT(14)
#define MALI_C720_DISC_V1_MODULE_OFFSET_MASK		GENMASK(31, 16)
#define MALI_C720_DISC_V1_MEMORY_TABLE_ID_MASK		GENMASK(9, 8)
#define MALI_C720_DISC_V1_MEMORY_SIZE_MASK		GENMASK(13, 10)
#define MALI_C720_DISC_V1_MEMORY_SCALAR_SUFFIX_MASK	BIT(14)
#define MALI_C720_DISC_V1_MEMORY_OFFSET_MASK		GENMASK(31, 16)
#define MALI_C720_DISC_V1_PARAM_ID_MASK			GENMASK(15, 8)
#define MALI_C720_DISC_V1_PARAM_VALUE_MASK		GENMASK(31, 16)
#define MALI_C720_DISC_V1_VERSION_PATCH_MASK		GENMASK(15, 8)
#define MALI_C720_DISC_V1_VERSION_MINOR_MASK		GENMASK(23, 16)
#define MALI_C720_DISC_V1_VERSION_MAJOR_MASK		GENMASK(31, 24)
#define MALI_C720_DISC_V1_FOOTER_CRC_MASK		GENMASK(31, 16)

#define MALI_C720_DISC_V1_FIELD_LIST_HEADER		0xf0
#define MALI_C720_DISC_V1_FIELD_SUBPIPE_HEADER		0xf1
#define MALI_C720_DISC_V1_FIELD_SUBPIPE_FOOTER		0xf2
#define MALI_C720_DISC_V1_FIELD_EXT_MEMORY		0xf8
#define MALI_C720_DISC_V1_FIELD_EXT_VERSION		0xf9
#define MALI_C720_DISC_V1_FIELD_EXT_PARAMETER		0xfa
#define MALI_C720_DISC_V1_FIELD_LIST_FOOTER		0xff

enum mali_c720_disc_v1_subpipe_id {
	MALI_C720_DISC_V1_SUBPIPE_ID_INPUT,
	MALI_C720_DISC_V1_SUBPIPE_ID_HV,
	MALI_C720_DISC_V1_SUBPIPE_ID_CV,
	MALI_C720_DISC_V1_SUBPIPE_ID_RAW_OUTPUT,
	MALI_C720_DISC_V1_SUBPIPE_ID_FR_OUTPUT,
	MALI_C720_DISC_V1_SUBPIPE_ID_PYR_OUTPUT,
	MALI_C720_DISC_V1_SUBPIPE_ID_FOV_OUTPUT,
	MALI_C720_DISC_V1_SUBPIPE_ID_STATS,
	MALI_C720_DISC_V1_SUBPIPE_ID_TPG,
	MALI_C720_DISC_V1_SUBPIPE_ID_MAX,
};

enum mali_c720_disc_v1_module_id {
	MALI_C720_DISC_V1_MODULE_ID_CM_FULL = 0x00,
	MALI_C720_DISC_V1_MODULE_ID_GLOBAL_ISP_CONTROL = 0x01,
	MALI_C720_DISC_V1_MODULE_ID_INTERRUPT_CONTROLLER = 0x02,
	MALI_C720_DISC_V1_MODULE_ID_FAULT_COLLECTOR = 0x03,
	MALI_C720_DISC_V1_MODULE_ID_MODULE_BIST_CONTROL = 0x04,
	MALI_C720_DISC_V1_MODULE_ID_MODULE_CRC_CONTROL = 0x05,
	MALI_C720_DISC_V1_MODULE_ID_MODULE_CG_CONTROL = 0x06,
	MALI_C720_DISC_V1_MODULE_ID_CM_CONTROL = 0x0f,
	MALI_C720_DISC_V1_MODULE_ID_CM_SLOT_LOADABLE = 0x10,
	MALI_C720_DISC_V1_MODULE_ID_PIPELINE_GLOBAL_CONTROL = 0x12,
	MALI_C720_DISC_V1_MODULE_ID_DIGITAL_GAIN = 0x18,
	MALI_C720_DISC_V1_MODULE_ID_GAIN_AND_OFFSET = 0x19,
	MALI_C720_DISC_V1_MODULE_ID_BLACK_LEVEL = 0x1c,
	MALI_C720_DISC_V1_MODULE_ID_WB_GAIN = 0x1d,
	MALI_C720_DISC_V1_MODULE_ID_OFFSET = 0x1e,
	MALI_C720_DISC_V1_MODULE_ID_SQUARE_ROOT_RAW = 0x20,
	MALI_C720_DISC_V1_MODULE_ID_SQUARE_RAW = 0x21,
	MALI_C720_DISC_V1_MODULE_ID_SQUARE_ROOT_RGB = 0x24,
	MALI_C720_DISC_V1_MODULE_ID_SQUARE_RGB = 0x25,
	MALI_C720_DISC_V1_MODULE_ID_DECOMPANDER = 0x28,
	MALI_C720_DISC_V1_MODULE_ID_FRAME_STITCHING = 0x29,
	MALI_C720_DISC_V1_MODULE_ID_DEFECT_PIXEL = 0x30,
	MALI_C720_DISC_V1_MODULE_ID_RGBIR_REMOSAIC = 0x31,
	MALI_C720_DISC_V1_MODULE_ID_CA_CORRECTION = 0x32,
	MALI_C720_DISC_V1_MODULE_ID_TEMPER = 0x33,
	MALI_C720_DISC_V1_MODULE_ID_SINTER = 0x34,
	MALI_C720_DISC_V1_MODULE_ID_SMART_SHADING = 0x40,
	MALI_C720_DISC_V1_MODULE_ID_RAW_TO_INTENSITY = 0x41,
	MALI_C720_DISC_V1_MODULE_ID_DEHAZE = 0x42,
	MALI_C720_DISC_V1_MODULE_ID_IRIDIX = 0x43,
	MALI_C720_DISC_V1_MODULE_ID_DEMOSAIC_STD = 0x44,
	MALI_C720_DISC_V1_MODULE_ID_DEMOSAIC_DFD = 0x45,
	MALI_C720_DISC_V1_MODULE_ID_PURPLE_FRINGE = 0x46,
	MALI_C720_DISC_V1_MODULE_ID_CCM = 0x47,
	MALI_C720_DISC_V1_MODULE_ID_CNR = 0x48,
	MALI_C720_DISC_V1_MODULE_ID_SCALER_RGB = 0x50,
	MALI_C720_DISC_V1_MODULE_ID_SCALER_RAW = 0x51,
	MALI_C720_DISC_V1_MODULE_ID_CROP = 0x54,
	MALI_C720_DISC_V1_MODULE_ID_SHARPENING_RGB = 0x58,
	MALI_C720_DISC_V1_MODULE_ID_SHARPENING_RAW = 0x59,
	MALI_C720_DISC_V1_MODULE_ID_GAMMA = 0x5c,
	MALI_C720_DISC_V1_MODULE_ID_TC_LUT_RGB = 0x5d,
	MALI_C720_DISC_V1_MODULE_ID_TC_LUT_RAW = 0x5e,
	MALI_C720_DISC_V1_MODULE_ID_OUTPUT_FORMATTER = 0x60,
	MALI_C720_DISC_V1_MODULE_ID_OUTPUT_MULTIPLEXER = 0x61,
	MALI_C720_DISC_V1_MODULE_ID_GAUSSIAN_PYRAMID = 0x62,
	MALI_C720_DISC_V1_MODULE_ID_RAW_STREAM_PACKER = 0x63,
	MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_MULTI = 0x67,
	MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_SINGLE = 0x68,
	MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_RGB_SINGLE = 0x69,
	MALI_C720_DISC_V1_MODULE_ID_FGM = 0x6a,
	MALI_C720_DISC_V1_MODULE_ID_FBIST_BRIDGE = 0x6b,
	MALI_C720_DISC_V1_MODULE_ID_PIPELINE_CRC = 0x6c,
	MALI_C720_DISC_V1_MODULE_ID_INPUT_CRC = 0x6d,
	MALI_C720_DISC_V1_MODULE_ID_INTENSITY_STATS = 0x70,
	MALI_C720_DISC_V1_MODULE_ID_HISTOGRAM_STATS = 0x71,
	MALI_C720_DISC_V1_MODULE_ID_COLOR_STATS = 0x72,
	MALI_C720_DISC_V1_MODULE_ID_AUTO_FOCUS_STATS = 0x73,
};

enum mali_c720_disc_v1_table_ca_correction_id {
	MALI_C720_DISC_V1_TABLE_CA_CORRECTION_COORD_MEM_ID = 0,
	MALI_C720_DISC_V1_TABLE_CA_CORRECTION_FILTER_MEM_ID,
	MALI_C720_DISC_V1_TABLE_CA_CORRECTION_MAX_ID,
};

enum mali_c720_disc_v1_table_smart_shading_id {
	MALI_C720_DISC_V1_TABLE_SMART_SHADING_MEM_ID = 0,
	MALI_C720_DISC_V1_TABLE_SMART_SHADING_MAX_ID,
};

enum mali_c720_disc_v1_table_dehaze_id {
	MALI_C720_DISC_V1_TABLE_DEHAZE_MEM_ID = 0,
	MALI_C720_DISC_V1_TABLE_DEHAZE_MAX_ID,
};

enum mali_c720_disc_v1_table_scaler_rgb_id {
	MALI_C720_DISC_V1_TABLE_SCALER_RGB_H_MEM_ID = 0,
	MALI_C720_DISC_V1_TABLE_SCALER_RGB_V_MEM_ID,
	MALI_C720_DISC_V1_TABLE_SCALER_RGB_MAX_ID,
};

struct mali_c720_disc_v1_subpipe_header {
	u16 desc_count;
	u8 type;
	u8 instance_id;
};

static u8 mali_c720_discovery_field_id(u32 record)
{
	return (u8)FIELD_GET(MALI_C720_DISC_V1_FIELD_ID_MASK, record);
}

static bool mali_c720_discovery_is_module_id(u8 field_id)
{
	switch (field_id) {
	case MALI_C720_DISC_V1_MODULE_ID_CM_FULL:
	case MALI_C720_DISC_V1_MODULE_ID_GLOBAL_ISP_CONTROL:
	case MALI_C720_DISC_V1_MODULE_ID_INTERRUPT_CONTROLLER:
	case MALI_C720_DISC_V1_MODULE_ID_FAULT_COLLECTOR:
	case MALI_C720_DISC_V1_MODULE_ID_MODULE_BIST_CONTROL:
	case MALI_C720_DISC_V1_MODULE_ID_MODULE_CRC_CONTROL:
	case MALI_C720_DISC_V1_MODULE_ID_MODULE_CG_CONTROL:
	case MALI_C720_DISC_V1_MODULE_ID_CM_CONTROL:
	case MALI_C720_DISC_V1_MODULE_ID_CM_SLOT_LOADABLE:
	case MALI_C720_DISC_V1_MODULE_ID_PIPELINE_GLOBAL_CONTROL:
	case MALI_C720_DISC_V1_MODULE_ID_DIGITAL_GAIN:
	case MALI_C720_DISC_V1_MODULE_ID_GAIN_AND_OFFSET:
	case MALI_C720_DISC_V1_MODULE_ID_BLACK_LEVEL:
	case MALI_C720_DISC_V1_MODULE_ID_WB_GAIN:
	case MALI_C720_DISC_V1_MODULE_ID_OFFSET:
	case MALI_C720_DISC_V1_MODULE_ID_SQUARE_ROOT_RAW:
	case MALI_C720_DISC_V1_MODULE_ID_SQUARE_RAW:
	case MALI_C720_DISC_V1_MODULE_ID_SQUARE_ROOT_RGB:
	case MALI_C720_DISC_V1_MODULE_ID_SQUARE_RGB:
	case MALI_C720_DISC_V1_MODULE_ID_DECOMPANDER:
	case MALI_C720_DISC_V1_MODULE_ID_FRAME_STITCHING:
	case MALI_C720_DISC_V1_MODULE_ID_DEFECT_PIXEL:
	case MALI_C720_DISC_V1_MODULE_ID_RGBIR_REMOSAIC:
	case MALI_C720_DISC_V1_MODULE_ID_CA_CORRECTION:
	case MALI_C720_DISC_V1_MODULE_ID_TEMPER:
	case MALI_C720_DISC_V1_MODULE_ID_SINTER:
	case MALI_C720_DISC_V1_MODULE_ID_SMART_SHADING:
	case MALI_C720_DISC_V1_MODULE_ID_RAW_TO_INTENSITY:
	case MALI_C720_DISC_V1_MODULE_ID_DEHAZE:
	case MALI_C720_DISC_V1_MODULE_ID_IRIDIX:
	case MALI_C720_DISC_V1_MODULE_ID_DEMOSAIC_STD:
	case MALI_C720_DISC_V1_MODULE_ID_DEMOSAIC_DFD:
	case MALI_C720_DISC_V1_MODULE_ID_PURPLE_FRINGE:
	case MALI_C720_DISC_V1_MODULE_ID_CCM:
	case MALI_C720_DISC_V1_MODULE_ID_CNR:
	case MALI_C720_DISC_V1_MODULE_ID_SCALER_RGB:
	case MALI_C720_DISC_V1_MODULE_ID_SCALER_RAW:
	case MALI_C720_DISC_V1_MODULE_ID_CROP:
	case MALI_C720_DISC_V1_MODULE_ID_SHARPENING_RGB:
	case MALI_C720_DISC_V1_MODULE_ID_SHARPENING_RAW:
	case MALI_C720_DISC_V1_MODULE_ID_GAMMA:
	case MALI_C720_DISC_V1_MODULE_ID_TC_LUT_RGB:
	case MALI_C720_DISC_V1_MODULE_ID_TC_LUT_RAW:
	case MALI_C720_DISC_V1_MODULE_ID_OUTPUT_FORMATTER:
	case MALI_C720_DISC_V1_MODULE_ID_OUTPUT_MULTIPLEXER:
	case MALI_C720_DISC_V1_MODULE_ID_GAUSSIAN_PYRAMID:
	case MALI_C720_DISC_V1_MODULE_ID_RAW_STREAM_PACKER:
	case MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_MULTI:
	case MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_SINGLE:
	case MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_RGB_SINGLE:
	case MALI_C720_DISC_V1_MODULE_ID_FGM:
	case MALI_C720_DISC_V1_MODULE_ID_FBIST_BRIDGE:
	case MALI_C720_DISC_V1_MODULE_ID_PIPELINE_CRC:
	case MALI_C720_DISC_V1_MODULE_ID_INPUT_CRC:
	case MALI_C720_DISC_V1_MODULE_ID_INTENSITY_STATS:
	case MALI_C720_DISC_V1_MODULE_ID_HISTOGRAM_STATS:
	case MALI_C720_DISC_V1_MODULE_ID_COLOR_STATS:
	case MALI_C720_DISC_V1_MODULE_ID_AUTO_FOCUS_STATS:
		return true;
	default:
		return false;
	}
}

static enum mali_c720_discovery_module_id
mali_c720_discovery_module_id_from_v1_id(u8 v1_module_id)
{
	switch (v1_module_id) {
	case MALI_C720_DISC_V1_MODULE_ID_CM_FULL:
		return MALI_C720_DISC_MODULE_ID_CM_FULL;
	case MALI_C720_DISC_V1_MODULE_ID_GLOBAL_ISP_CONTROL:
		return MALI_C720_DISC_MODULE_ID_GLOBAL_ISP_CONTROL;
	case MALI_C720_DISC_V1_MODULE_ID_INTERRUPT_CONTROLLER:
		return MALI_C720_DISC_MODULE_ID_INTERRUPT_CONTROLLER;
	case MALI_C720_DISC_V1_MODULE_ID_FAULT_COLLECTOR:
		return MALI_C720_DISC_MODULE_ID_FAULT_COLLECTOR;
	case MALI_C720_DISC_V1_MODULE_ID_MODULE_BIST_CONTROL:
		return MALI_C720_DISC_MODULE_ID_MODULE_BIST_CONTROL;
	case MALI_C720_DISC_V1_MODULE_ID_MODULE_CRC_CONTROL:
		return MALI_C720_DISC_MODULE_ID_MODULE_CRC_CONTROL;
	case MALI_C720_DISC_V1_MODULE_ID_MODULE_CG_CONTROL:
		return MALI_C720_DISC_MODULE_ID_MODULE_CG_CONTROL;
	case MALI_C720_DISC_V1_MODULE_ID_CM_CONTROL:
		return MALI_C720_DISC_MODULE_ID_CM_CONTROL;
	case MALI_C720_DISC_V1_MODULE_ID_CM_SLOT_LOADABLE:
		return MALI_C720_DISC_MODULE_ID_CM_SLOT_LOADABLE;
	case MALI_C720_DISC_V1_MODULE_ID_PIPELINE_GLOBAL_CONTROL:
		return MALI_C720_DISC_MODULE_ID_PIPELINE_GLOBAL_CONTROL;
	case MALI_C720_DISC_V1_MODULE_ID_DIGITAL_GAIN:
		return MALI_C720_DISC_MODULE_ID_DIGITAL_GAIN;
	case MALI_C720_DISC_V1_MODULE_ID_GAIN_AND_OFFSET:
		return MALI_C720_DISC_MODULE_ID_GAIN_AND_OFFSET;
	case MALI_C720_DISC_V1_MODULE_ID_BLACK_LEVEL:
		return MALI_C720_DISC_MODULE_ID_BLACK_LEVEL;
	case MALI_C720_DISC_V1_MODULE_ID_WB_GAIN:
		return MALI_C720_DISC_MODULE_ID_WB_GAIN;
	case MALI_C720_DISC_V1_MODULE_ID_OFFSET:
		return MALI_C720_DISC_MODULE_ID_OFFSET;
	case MALI_C720_DISC_V1_MODULE_ID_SQUARE_ROOT_RAW:
		return MALI_C720_DISC_MODULE_ID_SQUARE_ROOT_RAW;
	case MALI_C720_DISC_V1_MODULE_ID_SQUARE_RAW:
		return MALI_C720_DISC_MODULE_ID_SQUARE_RAW;
	case MALI_C720_DISC_V1_MODULE_ID_SQUARE_ROOT_RGB:
		return MALI_C720_DISC_MODULE_ID_SQUARE_ROOT_RGB;
	case MALI_C720_DISC_V1_MODULE_ID_SQUARE_RGB:
		return MALI_C720_DISC_MODULE_ID_SQUARE_RGB;
	case MALI_C720_DISC_V1_MODULE_ID_DECOMPANDER:
		return MALI_C720_DISC_MODULE_ID_DECOMPANDER;
	case MALI_C720_DISC_V1_MODULE_ID_FRAME_STITCHING:
		return MALI_C720_DISC_MODULE_ID_FRAME_STITCHING;
	case MALI_C720_DISC_V1_MODULE_ID_DEFECT_PIXEL:
		return MALI_C720_DISC_MODULE_ID_DEFECT_PIXEL;
	case MALI_C720_DISC_V1_MODULE_ID_RGBIR_REMOSAIC:
		return MALI_C720_DISC_MODULE_ID_RGBIR_REMOSAIC;
	case MALI_C720_DISC_V1_MODULE_ID_CA_CORRECTION:
		return MALI_C720_DISC_MODULE_ID_CA_CORRECTION;
	case MALI_C720_DISC_V1_MODULE_ID_TEMPER:
		return MALI_C720_DISC_MODULE_ID_TEMPER;
	case MALI_C720_DISC_V1_MODULE_ID_SINTER:
		return MALI_C720_DISC_MODULE_ID_SINTER;
	case MALI_C720_DISC_V1_MODULE_ID_SMART_SHADING:
		return MALI_C720_DISC_MODULE_ID_SMART_SHADING;
	case MALI_C720_DISC_V1_MODULE_ID_RAW_TO_INTENSITY:
		return MALI_C720_DISC_MODULE_ID_RAW_TO_INTENSITY;
	case MALI_C720_DISC_V1_MODULE_ID_DEHAZE:
		return MALI_C720_DISC_MODULE_ID_DEHAZE;
	case MALI_C720_DISC_V1_MODULE_ID_IRIDIX:
		return MALI_C720_DISC_MODULE_ID_IRIDIX;
	case MALI_C720_DISC_V1_MODULE_ID_DEMOSAIC_STD:
		return MALI_C720_DISC_MODULE_ID_DEMOSAIC_STD;
	case MALI_C720_DISC_V1_MODULE_ID_DEMOSAIC_DFD:
		return MALI_C720_DISC_MODULE_ID_DEMOSAIC_DFD;
	case MALI_C720_DISC_V1_MODULE_ID_PURPLE_FRINGE:
		return MALI_C720_DISC_MODULE_ID_PURPLE_FRINGE;
	case MALI_C720_DISC_V1_MODULE_ID_CCM:
		return MALI_C720_DISC_MODULE_ID_CCM;
	case MALI_C720_DISC_V1_MODULE_ID_CNR:
		return MALI_C720_DISC_MODULE_ID_CNR;
	case MALI_C720_DISC_V1_MODULE_ID_SCALER_RGB:
		return MALI_C720_DISC_MODULE_ID_SCALER_RGB;
	case MALI_C720_DISC_V1_MODULE_ID_SCALER_RAW:
		return MALI_C720_DISC_MODULE_ID_SCALER_RAW;
	case MALI_C720_DISC_V1_MODULE_ID_CROP:
		return MALI_C720_DISC_MODULE_ID_CROP;
	case MALI_C720_DISC_V1_MODULE_ID_SHARPENING_RGB:
		return MALI_C720_DISC_MODULE_ID_SHARPENING_RGB;
	case MALI_C720_DISC_V1_MODULE_ID_SHARPENING_RAW:
		return MALI_C720_DISC_MODULE_ID_SHARPENING_RAW;
	case MALI_C720_DISC_V1_MODULE_ID_GAMMA:
		return MALI_C720_DISC_MODULE_ID_GAMMA;
	case MALI_C720_DISC_V1_MODULE_ID_TC_LUT_RGB:
		return MALI_C720_DISC_MODULE_ID_TC_LUT_RGB;
	case MALI_C720_DISC_V1_MODULE_ID_TC_LUT_RAW:
		return MALI_C720_DISC_MODULE_ID_TC_LUT_RAW;
	case MALI_C720_DISC_V1_MODULE_ID_OUTPUT_FORMATTER:
		return MALI_C720_DISC_MODULE_ID_OUTPUT_FORMATTER;
	case MALI_C720_DISC_V1_MODULE_ID_OUTPUT_MULTIPLEXER:
		return MALI_C720_DISC_MODULE_ID_OUTPUT_MULTIPLEXER;
	case MALI_C720_DISC_V1_MODULE_ID_GAUSSIAN_PYRAMID:
		return MALI_C720_DISC_MODULE_ID_GAUSSIAN_PYRAMID;
	case MALI_C720_DISC_V1_MODULE_ID_RAW_STREAM_PACKER:
		return MALI_C720_DISC_MODULE_ID_RAW_STREAM_PACKER;
	case MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_MULTI:
		return MALI_C720_DISC_MODULE_ID_TPG_RAW_MULTI;
	case MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_SINGLE:
		return MALI_C720_DISC_MODULE_ID_TPG_RAW_SINGLE;
	case MALI_C720_DISC_V1_MODULE_ID_TPG_RAW_RGB_SINGLE:
		return MALI_C720_DISC_MODULE_ID_TPG_RAW_RGB_SINGLE;
	case MALI_C720_DISC_V1_MODULE_ID_FGM:
		return MALI_C720_DISC_MODULE_ID_FGM;
	case MALI_C720_DISC_V1_MODULE_ID_FBIST_BRIDGE:
		return MALI_C720_DISC_MODULE_ID_FBIST_BRIDGE;
	case MALI_C720_DISC_V1_MODULE_ID_PIPELINE_CRC:
		return MALI_C720_DISC_MODULE_ID_PIPELINE_CRC;
	case MALI_C720_DISC_V1_MODULE_ID_INPUT_CRC:
		return MALI_C720_DISC_MODULE_ID_INPUT_CRC;
	case MALI_C720_DISC_V1_MODULE_ID_INTENSITY_STATS:
		return MALI_C720_DISC_MODULE_ID_INTENSITY_STATS;
	case MALI_C720_DISC_V1_MODULE_ID_HISTOGRAM_STATS:
		return MALI_C720_DISC_MODULE_ID_HISTOGRAM_STATS;
	case MALI_C720_DISC_V1_MODULE_ID_COLOR_STATS:
		return MALI_C720_DISC_MODULE_ID_COLOR_STATS;
	case MALI_C720_DISC_V1_MODULE_ID_AUTO_FOCUS_STATS:
		return MALI_C720_DISC_MODULE_ID_AUTO_FOCUS_STATS;
	default:
		return MALI_C720_DISC_MODULE_ID_NONE;
	}
}

static enum mali_c720_discovery_pipeline_id
mali_c720_discovery_v1_subpipe_id_to_pipeline_id(u8 subpipe_id)
{
	switch (subpipe_id) {
	case MALI_C720_DISC_V1_SUBPIPE_ID_INPUT:
		return MALI_C720_DISC_PIPE_INPUT;
	case MALI_C720_DISC_V1_SUBPIPE_ID_HV:
		return MALI_C720_DISC_PIPE_HV;
	case MALI_C720_DISC_V1_SUBPIPE_ID_CV:
		return MALI_C720_DISC_PIPE_CV;
	case MALI_C720_DISC_V1_SUBPIPE_ID_RAW_OUTPUT:
		return MALI_C720_DISC_PIPE_RAW_OUTPUT;
	case MALI_C720_DISC_V1_SUBPIPE_ID_FR_OUTPUT:
		return MALI_C720_DISC_PIPE_FR_OUTPUT;
	case MALI_C720_DISC_V1_SUBPIPE_ID_PYR_OUTPUT:
		return MALI_C720_DISC_PIPE_PYR_OUTPUT;
	case MALI_C720_DISC_V1_SUBPIPE_ID_FOV_OUTPUT:
		return MALI_C720_DISC_PIPE_FOV_OUTPUT;
	case MALI_C720_DISC_V1_SUBPIPE_ID_STATS:
		return MALI_C720_DISC_PIPE_STATS;
	case MALI_C720_DISC_V1_SUBPIPE_ID_TPG:
		return MALI_C720_DISC_PIPE_TPG;
	default:
		return MALI_C720_DISC_PIPE_MAX;
	}
}

static u16 mali_c720_discovery_crc16_ibm_3740(const __le32 *records, u16 count)
{
	u16 crc = 0xffff;
	u16 i;

	for (i = 0; i < count; i++) {
		u32 v = le32_to_cpu(records[i]);

		crc = crc_itu_t_byte(crc, (u8)(v >> 24));
		crc = crc_itu_t_byte(crc, (u8)(v >> 16));
		crc = crc_itu_t_byte(crc, (u8)(v >> 8));
		crc = crc_itu_t_byte(crc, (u8)v);
	}

	return crc;
}

struct mali_c720_disc_v1_module_desc {
	enum mali_c720_discovery_module_id id;
	u32 offset;
	u8 instance_id;
	bool removed;
};

struct mali_c720_disc_v1_memory_desc {
	u32 offset;
	u32 size;
	u8 table_id;
};

struct mali_c720_disc_v1_version_desc {
	u8 major;
	u8 minor;
	u8 patch;
};

struct mali_c720_disc_v1_param_desc {
	u8 id;
	u16 value;
};

static struct mali_c720_discovery_pipeline *
mali_c720_discovery_get_pipeline(struct mali_c720_discovery *discovery,
				 enum mali_c720_discovery_pipeline_id pipeline_id,
				 unsigned int instance_id)
{
	if (!discovery || pipeline_id >= MALI_C720_DISC_PIPE_MAX ||
	    instance_id >= MALI_C720_DISCOVERY_MAX_PIPELINE_INSTANCES)
		return NULL;

	return &discovery->pipelines[pipeline_id][instance_id];
}

static struct mali_c720_discovery_module *
mali_c720_discovery_alloc_module(struct mali_c720 *mali_c720,
				 struct mali_c720_discovery *discovery)
{
	struct mali_c720_discovery_module *module;

	if (discovery->next_module_id >= MALI_C720_DISCOVERY_MAX_MODULES) {
		dev_err(mali_c720->dev, "Failed to allocate discovery module\n");
		return NULL;
	}

	module = &discovery->modules[discovery->next_module_id++];
	INIT_LIST_HEAD(&module->memories);
	INIT_LIST_HEAD(&module->fbists);
	INIT_LIST_HEAD(&module->mbists);
	INIT_LIST_HEAD(&module->config_crcs);
	INIT_LIST_HEAD(&module->clock_gating);
	INIT_LIST_HEAD(&module->params);
	INIT_LIST_HEAD(&module->node);

	return module;
}

static struct mali_c720_discovery_table *
mali_c720_discovery_alloc_memory(struct mali_c720 *mali_c720,
				 struct mali_c720_discovery *discovery)
{
	struct mali_c720_discovery_table *memory;

	if (discovery->next_memory_id >= MALI_C720_DISCOVERY_MAX_MEMORIES) {
		dev_err(mali_c720->dev, "Failed to allocate discovery memory\n");
		return NULL;
	}

	memory = &discovery->memories[discovery->next_memory_id++];
	INIT_LIST_HEAD(&memory->node);

	return memory;
}

static struct mali_c720_discovery_param *
mali_c720_discovery_alloc_param(struct mali_c720 *mali_c720,
				struct mali_c720_discovery *discovery)
{
	struct mali_c720_discovery_param *param;

	if (discovery->next_param_id >= MALI_C720_DISCOVERY_MAX_PARAMS) {
		dev_err(mali_c720->dev, "Failed to allocate discovery param\n");
		return NULL;
	}

	param = &discovery->params[discovery->next_param_id++];
	INIT_LIST_HEAD(&param->node);

	return param;
}

static void
mali_c720_discovery_link_pipeline_module(struct mali_c720_discovery_pipeline *pipeline,
					 struct mali_c720_discovery_module *module)
{
	list_add_tail(&module->node, &pipeline->modules);
}

static void
mali_c720_discovery_link_memory(struct mali_c720_discovery_module *module,
				struct mali_c720_discovery_table *memory)
{
	list_add_tail(&memory->node, &module->memories);
	module->num_memories++;
}

static void
mali_c720_discovery_link_param(struct mali_c720_discovery_module *module,
			       struct mali_c720_discovery_param *param)
{
	list_add_tail(&param->node, &module->params);
	module->num_params++;
}

static void
mali_c720_discovery_link_fbist(struct mali_c720_discovery_module *module,
			       struct mali_c720_discovery_module *fbist)
{
	list_add_tail(&fbist->node, &module->fbists);
	fbist->fbist_instance_id = module->num_fbists;
	module->num_fbists++;
}

static void
mali_c720_discovery_link_mbist(struct mali_c720_discovery_module *module,
			       struct mali_c720_discovery_module *mbist)
{
	list_add_tail(&mbist->node, &module->mbists);
	mbist->mbist_instance_id = module->num_mbists;
	module->num_mbists++;
}

static void
mali_c720_discovery_link_config_crc(struct mali_c720_discovery_module *module,
				    struct mali_c720_discovery_module *config_crc)
{
	list_add_tail(&config_crc->node, &module->config_crcs);
	config_crc->config_crc_instance_id = module->num_config_crcs;
	module->num_config_crcs++;
}

static void
mali_c720_discovery_link_clock_gating(struct mali_c720_discovery_module *module,
				      struct mali_c720_discovery_module *clock_gating)
{
	list_add_tail(&clock_gating->node, &module->clock_gating);
	clock_gating->clock_gating_instance_id = module->num_clock_gating;
	module->num_clock_gating++;
}

static struct mali_c720_discovery_module *
mali_c720_discovery_find_module(struct mali_c720_discovery_pipeline *pipeline,
				enum mali_c720_discovery_module_id module_id,
				unsigned int module_instance)
{
	struct mali_c720_discovery_module *module;

	if (!pipeline)
		return NULL;

	list_for_each_entry(module, &pipeline->modules, node) {
		if (module->id == module_id &&
		    module->instance_id == module_instance)
			return module;
	}

	return NULL;
}

static struct mali_c720_discovery_module *
mali_c720_discovery_get_module(struct mali_c720_discovery *discovery,
			       const struct mali_c720_discovery_target *target)
{
	struct mali_c720_discovery_pipeline *pipeline;

	if (target->module_id <= MALI_C720_DISC_MODULE_ID_NONE ||
	    target->module_id >= MALI_C720_DISC_MODULE_ID_MAX ||
	    target->module_instance >= MALI_C720_DISCOVERY_MAX_MODULE_INSTANCES)
		return NULL;

	pipeline = mali_c720_discovery_get_pipeline(discovery, target->pipeline_id,
						    target->pipeline_instance);
	if (!pipeline || !pipeline->present)
		return NULL;

	return mali_c720_discovery_find_module(pipeline, target->module_id,
					       target->module_instance);
}

static const struct mali_c720_discovery_table *
mali_c720_discovery_find_memory(const struct mali_c720_discovery_module *module,
				unsigned int table_id)
{
	const struct mali_c720_discovery_table *memory;

	list_for_each_entry(memory, &module->memories, node) {
		if (memory->id == table_id)
			return memory;
	}

	return NULL;
}

static int mali_c720_discovery_parse_module_record(struct mali_c720 *mali_c720,
						   u32 record,
						   struct mali_c720_disc_v1_module_desc *module)
{
	module->id =
		mali_c720_discovery_module_id_from_v1_id(mali_c720_discovery_field_id(record));
	module->instance_id =
		FIELD_GET(MALI_C720_DISC_V1_MODULE_INSTANCE_MASK, record);
	module->removed = !!(record & MALI_C720_DISC_V1_MODULE_REMOVED_MASK);
	module->offset =
		FIELD_GET(MALI_C720_DISC_V1_MODULE_OFFSET_MASK, record) << 4;

	if ((u64)module->offset + sizeof(u32) > mali_c720->registers.context_size)
		return -EINVAL;

	return 0;
}

static int mali_c720_discovery_parse_memory_record(struct mali_c720 *mali_c720,
						   u32 record,
						   struct mali_c720_disc_v1_memory_desc *memory)
{
	u32 log2_size;
	u64 end;

	memory->table_id =
		FIELD_GET(MALI_C720_DISC_V1_MEMORY_TABLE_ID_MASK, record);
	memory->offset =
		FIELD_GET(MALI_C720_DISC_V1_MEMORY_OFFSET_MASK, record) << 4;

	log2_size = FIELD_GET(MALI_C720_DISC_V1_MEMORY_SIZE_MASK, record);
	if (log2_size > 10)
		return -EINVAL;

	memory->size = 16U << log2_size;
	if (record & MALI_C720_DISC_V1_MEMORY_SCALAR_SUFFIX_MASK)
		memory->size += 32;

	end = (u64)memory->offset + memory->size;
	if (end > mali_c720->registers.context_size)
		return -EINVAL;

	return 0;
}

static void mali_c720_discovery_parse_version_record(u32 record,
						     struct mali_c720_disc_v1_version_desc *version)
{
	version->patch = FIELD_GET(MALI_C720_DISC_V1_VERSION_PATCH_MASK, record);
	version->minor = FIELD_GET(MALI_C720_DISC_V1_VERSION_MINOR_MASK, record);
	version->major = FIELD_GET(MALI_C720_DISC_V1_VERSION_MAJOR_MASK, record);
}

static void mali_c720_discovery_parse_param_record(u32 record,
						   struct mali_c720_disc_v1_param_desc *param)
{
	param->id = FIELD_GET(MALI_C720_DISC_V1_PARAM_ID_MASK, record);
	param->value = FIELD_GET(MALI_C720_DISC_V1_PARAM_VALUE_MASK, record);
}

static int mali_c720_discovery_parse_subpipe_header(u32 record,
						    struct mali_c720_disc_v1_subpipe_header *header)
{
	header->type = FIELD_GET(MALI_C720_DISC_V1_SUBPIPE_TYPE_MASK, record);
	header->instance_id =
		FIELD_GET(MALI_C720_DISC_V1_SUBPIPE_INSTANCE_MASK, record);
	header->desc_count =
		FIELD_GET(MALI_C720_DISC_V1_SUBPIPE_DESC_COUNT_MASK, record);

	if (header->type >= MALI_C720_DISC_V1_SUBPIPE_ID_MAX)
		return -EINVAL;

	return 0;
}

static int mali_c720_discovery_parse_v1(struct mali_c720 *mali_c720,
					struct mali_c720_discovery *discovery,
					const __le32 *records,
					u16 desc_count)
{
	struct mali_c720_discovery_pipeline *pipeline;
	struct mali_c720_discovery_pipeline *current_subpipe = NULL;
	struct mali_c720_discovery_module *current_module = NULL;
	struct mali_c720_discovery_module *last_module = NULL;
	enum mali_c720_discovery_pipeline_id current_pipeline_id =
		MALI_C720_DISC_PIPE_MAX;
	unsigned int current_pipeline_instance = 0;
	u16 subpipe_desc_count = 0;
	unsigned int i;

	pipeline = NULL;

	for (i = 0; i <= desc_count; i++) {
		struct mali_c720_discovery_module *module;
		struct mali_c720_disc_v1_subpipe_header subpipe;
		u32 record = le32_to_cpu(records[i]);
		u8 field_id = mali_c720_discovery_field_id(record);
		bool in_subpipe = current_subpipe;

		switch (field_id) {
		case MALI_C720_DISC_V1_FIELD_LIST_HEADER:
			if (i) {
				dev_err(mali_c720->dev,
					"unexpected discovery list header at record %u\n", i);
				return -EINVAL;
			}

			pipeline = mali_c720_discovery_get_pipeline(discovery,
								    MALI_C720_DISC_PIPE_GLOBAL, 0);
			if (!pipeline) {
				dev_err(mali_c720->dev,
					"Failed to fetch global pipeline\n");
				return -EINVAL;
			}

			pipeline->present = true;
			pipeline->id = MALI_C720_DISC_PIPE_GLOBAL;
			pipeline->instance_id = 0;
			current_module = NULL;
			last_module = NULL;
			current_pipeline_id = MALI_C720_DISC_PIPE_GLOBAL;
			current_pipeline_instance = 0;
			break;
		case MALI_C720_DISC_V1_FIELD_LIST_FOOTER:
			if (i != desc_count || current_subpipe) {
				dev_err(mali_c720->dev,
					"unexpected discovery list footer at record %u\n", i);
				return -EINVAL;
			}
			break;
		case MALI_C720_DISC_V1_FIELD_SUBPIPE_HEADER:
		{
			enum mali_c720_discovery_pipeline_id pipeline_id;

			if (current_subpipe) {
				dev_err(mali_c720->dev,
					"nested sub-pipeline header at record %u\n", i);
				return -EINVAL;
			}

			if (mali_c720_discovery_parse_subpipe_header(record, &subpipe)) {
				dev_err(mali_c720->dev,
					"invalid sub-pipeline header at record %u\n", i);
					return -EINVAL;
			}

			if (!subpipe.desc_count) {
				dev_err(mali_c720->dev,
					"sub-pipeline descriptor count is zero at record %u\n", i);
				return -EINVAL;
			}

			pipeline_id =
				mali_c720_discovery_v1_subpipe_id_to_pipeline_id(subpipe.type);
			if (pipeline_id >= MALI_C720_DISC_PIPE_MAX) {
				dev_err(mali_c720->dev,
					"unsupported v1 sub-pipeline type %u at record %u\n",
					subpipe.type, i);
				return -EINVAL;
			}

			pipeline = mali_c720_discovery_get_pipeline(discovery, pipeline_id,
								    subpipe.instance_id);
			if (!pipeline) {
				dev_err(mali_c720->dev,
					"Failed to fetch sub-pipeline type %u instance %u\n",
					pipeline_id, subpipe.instance_id);
				return -EINVAL;
			}

			pipeline->present = true;
			pipeline->id = pipeline_id;
			pipeline->instance_id = subpipe.instance_id;
			subpipe_desc_count = subpipe.desc_count;
			current_subpipe = pipeline;
			current_module = NULL;
			last_module = NULL;
			current_pipeline_id = pipeline_id;
			current_pipeline_instance = subpipe.instance_id;
			break;
		}
		case MALI_C720_DISC_V1_FIELD_SUBPIPE_FOOTER:
			if (!current_subpipe) {
				dev_err(mali_c720->dev,
					"sub-pipeline footer without matching header at record %u\n",
					i);
				return -EINVAL;
			}

			if (subpipe_desc_count - 1 != 0) {
				dev_err(mali_c720->dev,
					"sub-pipeline descriptor count mismatch at record %u\n",
					i);
				return -EINVAL;
			}

			current_subpipe = NULL;
			pipeline = NULL;
			current_module = NULL;
			last_module = NULL;
			current_pipeline_id = MALI_C720_DISC_PIPE_MAX;
			current_pipeline_instance = 0;
			break;
		case MALI_C720_DISC_V1_FIELD_EXT_MEMORY:
		{
			struct mali_c720_discovery_table *memory_entry;
			struct mali_c720_disc_v1_memory_desc memory;

			if (!current_module) {
				dev_err(mali_c720->dev,
					"memory descriptor without active module at record %u\n",
					i);
				return -EINVAL;
			}

			if (mali_c720_discovery_parse_memory_record(mali_c720, record,
								    &memory)) {
				dev_err(mali_c720->dev,
					"invalid memory descriptor at record %u\n", i);
				return -EINVAL;
			}

			memory_entry = mali_c720_discovery_alloc_memory(mali_c720,
									discovery);
			if (!memory_entry)
				return -ENOMEM;

			memory_entry->id = memory.table_id;
			memory_entry->base = memory.offset;
			memory_entry->size = memory.size;
			mali_c720_discovery_link_memory(current_module, memory_entry);

			break;
		}
		case MALI_C720_DISC_V1_FIELD_EXT_VERSION:
		{
			struct mali_c720_disc_v1_version_desc version_desc;

			if (!last_module) {
				dev_err(mali_c720->dev,
					"version descriptor without preceding module at record %u\n",
					i);
				return -EINVAL;
			}

			mali_c720_discovery_parse_version_record(record,
								 &version_desc);

			last_module->version.present = true;
			last_module->version.major = version_desc.major;
			last_module->version.minor = version_desc.minor;
			last_module->version.patch = version_desc.patch;

			break;
		}
		case MALI_C720_DISC_V1_FIELD_EXT_PARAMETER:
		{
			struct mali_c720_discovery_param *param_entry;
			struct mali_c720_disc_v1_param_desc param_desc;

			if (!last_module) {
				dev_err(mali_c720->dev,
					"parameter descriptor without preceding module at record %u\n",
					i);
				return -EINVAL;
			}

			param_entry = mali_c720_discovery_alloc_param(mali_c720,
								      discovery);
			if (!param_entry)
				return -ENOMEM;

			mali_c720_discovery_parse_param_record(record, &param_desc);
			param_entry->id = param_desc.id;
			param_entry->value = param_desc.value;
			mali_c720_discovery_link_param(last_module, param_entry);

			break;
		}
		case MALI_C720_DISC_V1_MODULE_ID_MODULE_BIST_CONTROL:
		case MALI_C720_DISC_V1_MODULE_ID_MODULE_CRC_CONTROL:
		case MALI_C720_DISC_V1_MODULE_ID_MODULE_CG_CONTROL:
		{
			struct mali_c720_disc_v1_module_desc module_desc;
			struct mali_c720_discovery_module *control_module;

			if (!current_module) {
				dev_err(mali_c720->dev,
					"control descriptor without active module at record %u\n",
					i);
				return -EINVAL;
			}

			if (mali_c720_discovery_parse_module_record(mali_c720, record,
								    &module_desc)) {
				dev_err(mali_c720->dev,
					"invalid control module descriptor at record %u\n", i);
				return -EINVAL;
			}

			control_module = mali_c720_discovery_alloc_module(mali_c720,
									  discovery);
			if (!control_module)
				return -ENOMEM;

			control_module->id = module_desc.id;
			control_module->instance_id = module_desc.instance_id;
			control_module->found = true;
			control_module->present = !module_desc.removed;
			control_module->base = module_desc.offset;

			switch (field_id) {
			case MALI_C720_DISC_V1_MODULE_ID_MODULE_BIST_CONTROL:
				mali_c720_discovery_link_mbist(current_module,
							       control_module);
				break;
			case MALI_C720_DISC_V1_MODULE_ID_MODULE_CRC_CONTROL:
				mali_c720_discovery_link_config_crc(current_module,
								    control_module);
				break;
			default:
				mali_c720_discovery_link_clock_gating(current_module,
								      control_module);
				break;
			}

			last_module = control_module;

			break;
		}
		default:
		{
			struct mali_c720_disc_v1_module_desc module_desc;

			if (!mali_c720_discovery_is_module_id(field_id)) {
				dev_err(mali_c720->dev,
					"unknown discovery field id %u at record %u\n",
					field_id, i);
				return -EINVAL;
			}
			if (!pipeline) {
				dev_err(mali_c720->dev,
					"module descriptor without active pipeline at record %u\n",
					i);
				return -EINVAL;
			}

			if (mali_c720_discovery_parse_module_record(mali_c720, record,
								    &module_desc)) {
				dev_err(mali_c720->dev,
					"invalid module descriptor at record %u\n", i);
				return -EINVAL;
			}
			if (module_desc.id == MALI_C720_DISC_MODULE_ID_NONE) {
				dev_err(mali_c720->dev,
					"unsupported module id %u at record %u\n",
					field_id, i);
				return -EINVAL;
			}

			if (module_desc.instance_id >=
			    MALI_C720_DISCOVERY_MAX_MODULE_INSTANCES) {
				dev_err(mali_c720->dev,
					"module %u instance %u exceeds supported limit %u at record %u\n",
					module_desc.id,
					module_desc.instance_id,
					MALI_C720_DISCOVERY_MAX_MODULE_INSTANCES, i);
				return -EINVAL;
			}

			module = mali_c720_discovery_alloc_module(mali_c720, discovery);
			if (!module)
				return -ENOMEM;

			module->id = module_desc.id;
			module->instance_id = module_desc.instance_id;
			module->found = true;
			module->present = !module_desc.removed;
			module->base = module_desc.offset;

			if (current_module) {
				if (module_desc.id == MALI_C720_DISC_MODULE_ID_FBIST_BRIDGE &&
				    current_module->id != MALI_C720_DISC_MODULE_ID_FBIST_BRIDGE) {
					mali_c720_discovery_link_fbist(current_module,
								       module);
				} else {
					mali_c720_discovery_link_pipeline_module(pipeline, module);
					current_module = module;
				}
			} else if (list_empty(&pipeline->modules)) {
				mali_c720_discovery_link_pipeline_module(pipeline, module);
				current_module = module;
			} else {
				dev_err(mali_c720->dev,
					"failed to link module id %u instance %u in pipeline %u instance %u\n",
					module_desc.id, module_desc.instance_id,
					current_pipeline_id, current_pipeline_instance);
				return -EINVAL;
			}

			last_module = module;

			break;
		}
		}

		if (in_subpipe) {
			if (!subpipe_desc_count) {
				dev_err(mali_c720->dev,
					"sub-pipeline descriptor count exhausted at record %u\n",
					i);
				return -EINVAL;
			}

			subpipe_desc_count--;
		}
	}

	return 0;
}

int mali_c720_discovery_init(struct mali_c720 *mali_c720, resource_size_t reg_size)
{
	struct mali_c720_discovery *discovery = &mali_c720->discovery;
	__le32 *records;
	u32 header;
	u32 footer;
	u16 desc_count;
	u16 version;
	u16 footer_crc;
	u16 computed_crc;
	unsigned int pipeline_id;
	unsigned int instance_id;
	size_t word_count;
	int ret;

	header = mali_c720_read_raw(mali_c720, CAPABILITY_DISCOVERY_HEADER_OFFSET);
	if (mali_c720_discovery_field_id(header) !=
	    MALI_C720_DISC_V1_FIELD_LIST_HEADER) {
		dev_err(mali_c720->dev, "discovery header missing\n");
		return -EINVAL;
	}

	version = FIELD_GET(MALI_C720_DISC_V1_LIST_VERSION_MASK, header);
	if (version != 1) {
		dev_err(mali_c720->dev, "unsupported discovery version %u\n",
			version);
		return -EINVAL;
	}

	desc_count = FIELD_GET(MALI_C720_DISC_V1_LIST_DESC_COUNT_MASK, header);
	if (desc_count < 2) {
		dev_err(mali_c720->dev, "discovery descriptor count %u is invalid\n",
			desc_count);
		return -EINVAL;
	}

	word_count = (size_t)desc_count + 1;
	if (word_count > reg_size / sizeof(u32)) {
		dev_err(mali_c720->dev,
			"discovery table exceeds mapped register space (%zu words > %pa bytes)\n",
			word_count, &reg_size);
		return -EINVAL;
	}

	records = kvcalloc(word_count, sizeof(*records), GFP_KERNEL);
	if (!records)
		return -ENOMEM;

	memcpy_fromio(records, mali_c720->base, word_count * sizeof(*records));

	footer = le32_to_cpu(records[desc_count]);
	if (mali_c720_discovery_field_id(footer) !=
	    MALI_C720_DISC_V1_FIELD_LIST_FOOTER) {
		dev_err(mali_c720->dev, "discovery footer missing\n");
		ret = -EINVAL;
		goto out_free_records;
	}

	footer_crc = FIELD_GET(MALI_C720_DISC_V1_FOOTER_CRC_MASK, footer);
	computed_crc = mali_c720_discovery_crc16_ibm_3740(records, desc_count);
	if (computed_crc != footer_crc) {
		dev_err(mali_c720->dev,
			"discovery CRC mismatch: computed 0x%04x expected 0x%04x\n",
			computed_crc, footer_crc);
		ret = -EINVAL;
		goto out_free_records;
	}

	memset(discovery, 0, sizeof(*discovery));
	discovery->version = MALI_C720_DISCOVERY_VERSION_1;
	for (pipeline_id = 0; pipeline_id < MALI_C720_DISC_PIPE_MAX; pipeline_id++)
		for (instance_id = 0;
		     instance_id < MALI_C720_DISCOVERY_MAX_PIPELINE_INSTANCES;
		     instance_id++)
			INIT_LIST_HEAD(&discovery->pipelines[pipeline_id][instance_id].modules);

	ret = mali_c720_discovery_parse_v1(mali_c720, discovery, records,
					   desc_count);

out_free_records:
	kvfree(records);
	return ret;
}

int mali_c720_discovery_get_module_base(struct mali_c720 *mali_c720,
					const struct mali_c720_discovery_target *target,
					u32 *base)
{
	const struct mali_c720_discovery_module *module;

	module = mali_c720_discovery_get_module(&mali_c720->discovery, target);
	if (!module || !module->found) {
		dev_warn_once(mali_c720->dev,
			      "No module found for module id %u instance %u in pipeline %u instance %u\n",
			      target->module_id, target->module_instance,
			      target->pipeline_id, target->pipeline_instance);
		return -ENOENT;
	}

	if (!module->present) {
		dev_warn_once(mali_c720->dev,
			      "module id %u instance %u in pipeline %u instance %u is not present\n",
			      target->module_id, target->module_instance,
			      target->pipeline_id, target->pipeline_instance);
		return -ENODEV;
	}

	*base = module->base;
	return 0;
}

int mali_c720_discovery_get_module_table_base(struct mali_c720 *mali_c720,
					      const struct mali_c720_discovery_target *target,
					      unsigned int table_id,
					      u32 *base)
{
	const struct mali_c720_discovery_module *module;
	const struct mali_c720_discovery_table *memory;

	module = mali_c720_discovery_get_module(&mali_c720->discovery, target);
	if (!module || !module->found) {
		dev_warn_once(mali_c720->dev,
			      "No module found for module id %u instance %u in pipeline %u instance %u\n",
			      target->module_id, target->module_instance,
			      target->pipeline_id, target->pipeline_instance);
		return -ENOENT;
	}

	if (!module->present) {
		dev_warn_once(mali_c720->dev,
			      "module id %u instance %u in pipeline %u instance %u is not present\n",
			      target->module_id, target->module_instance,
			      target->pipeline_id, target->pipeline_instance);
		return -ENODEV;
	}

	memory = mali_c720_discovery_find_memory(module, table_id);
	if (!memory)
		return -ENOENT;

	*base = memory->base;

	return 0;
}

bool mali_c720_discovery_module_present(struct mali_c720 *mali_c720,
					const struct mali_c720_discovery_target *target)
{
	const struct mali_c720_discovery_module *module;

	module = mali_c720_discovery_get_module(&mali_c720->discovery, target);
	if (!module || !module->found)
		return false;

	return module->present;
}
