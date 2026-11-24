/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Capability discovery
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_DISCOVERY_H
#define MALI_C720_DISCOVERY_H

#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/types.h>

/*
 * Capability discovery describes the ISP configuration selected at hardware
 * synthesis time, including module presence, instances, register addresses
 * and memory tables. The driver parses it during probe and caches the
 * configuration for runtime lookups. The hardware topology remains fixed
 * at runtime.
 *
 * The discovery version refers to the format of the table. Future hardware
 * revisions may introduce a new version.
 */

struct mali_c720;

#define MALI_C720_DISCOVERY_MAX_MODULES		384
#define MALI_C720_DISCOVERY_MAX_MEMORIES	32
#define MALI_C720_DISCOVERY_MAX_PARAMS		32
#define MALI_C720_DISCOVERY_MAX_PIPELINE_INSTANCES	4
#define MALI_C720_DISCOVERY_MAX_MODULE_INSTANCES	8
#define MALI_C720_DISCOVERY_MAX_TABLE_INSTANCES		4

enum mali_c720_discovery_version_id {
	MALI_C720_DISCOVERY_VERSION_INVALID,
	MALI_C720_DISCOVERY_VERSION_1,
};

enum mali_c720_discovery_pipeline_id {
	MALI_C720_DISC_PIPE_GLOBAL,
	MALI_C720_DISC_PIPE_INPUT,
	MALI_C720_DISC_PIPE_HV,
	MALI_C720_DISC_PIPE_CV,
	MALI_C720_DISC_PIPE_RAW_OUTPUT,
	MALI_C720_DISC_PIPE_FR_OUTPUT,
	MALI_C720_DISC_PIPE_PYR_OUTPUT,
	MALI_C720_DISC_PIPE_FOV_OUTPUT,
	MALI_C720_DISC_PIPE_STATS,
	MALI_C720_DISC_PIPE_TPG,
	MALI_C720_DISC_PIPE_MAX,
};

enum mali_c720_discovery_module_id {
	MALI_C720_DISC_MODULE_ID_NONE = 0,
	MALI_C720_DISC_MODULE_ID_CM_FULL,
	MALI_C720_DISC_MODULE_ID_GLOBAL_ISP_CONTROL,
	MALI_C720_DISC_MODULE_ID_INTERRUPT_CONTROLLER,
	MALI_C720_DISC_MODULE_ID_FAULT_COLLECTOR,
	MALI_C720_DISC_MODULE_ID_MODULE_BIST_CONTROL,
	MALI_C720_DISC_MODULE_ID_MODULE_CRC_CONTROL,
	MALI_C720_DISC_MODULE_ID_MODULE_CG_CONTROL,
	MALI_C720_DISC_MODULE_ID_CM_CONTROL,
	MALI_C720_DISC_MODULE_ID_CM_SLOT_LOADABLE,
	MALI_C720_DISC_MODULE_ID_PIPELINE_GLOBAL_CONTROL,
	MALI_C720_DISC_MODULE_ID_DIGITAL_GAIN,
	MALI_C720_DISC_MODULE_ID_GAIN_AND_OFFSET,
	MALI_C720_DISC_MODULE_ID_BLACK_LEVEL,
	MALI_C720_DISC_MODULE_ID_WB_GAIN,
	MALI_C720_DISC_MODULE_ID_OFFSET,
	MALI_C720_DISC_MODULE_ID_SQUARE_ROOT_RAW,
	MALI_C720_DISC_MODULE_ID_SQUARE_RAW,
	MALI_C720_DISC_MODULE_ID_SQUARE_ROOT_RGB,
	MALI_C720_DISC_MODULE_ID_SQUARE_RGB,
	MALI_C720_DISC_MODULE_ID_DECOMPANDER,
	MALI_C720_DISC_MODULE_ID_FRAME_STITCHING,
	MALI_C720_DISC_MODULE_ID_DEFECT_PIXEL,
	MALI_C720_DISC_MODULE_ID_RGBIR_REMOSAIC,
	MALI_C720_DISC_MODULE_ID_CA_CORRECTION,
	MALI_C720_DISC_MODULE_ID_TEMPER,
	MALI_C720_DISC_MODULE_ID_SINTER,
	MALI_C720_DISC_MODULE_ID_SMART_SHADING,
	MALI_C720_DISC_MODULE_ID_RAW_TO_INTENSITY,
	MALI_C720_DISC_MODULE_ID_DEHAZE,
	MALI_C720_DISC_MODULE_ID_IRIDIX,
	MALI_C720_DISC_MODULE_ID_DEMOSAIC_STD,
	MALI_C720_DISC_MODULE_ID_DEMOSAIC_DFD,
	MALI_C720_DISC_MODULE_ID_PURPLE_FRINGE,
	MALI_C720_DISC_MODULE_ID_CCM,
	MALI_C720_DISC_MODULE_ID_CNR,
	MALI_C720_DISC_MODULE_ID_SCALER_RGB,
	MALI_C720_DISC_MODULE_ID_SCALER_RAW,
	MALI_C720_DISC_MODULE_ID_CROP,
	MALI_C720_DISC_MODULE_ID_SHARPENING_RGB,
	MALI_C720_DISC_MODULE_ID_SHARPENING_RAW,
	MALI_C720_DISC_MODULE_ID_GAMMA,
	MALI_C720_DISC_MODULE_ID_TC_LUT_RGB,
	MALI_C720_DISC_MODULE_ID_TC_LUT_RAW,
	MALI_C720_DISC_MODULE_ID_OUTPUT_FORMATTER,
	MALI_C720_DISC_MODULE_ID_OUTPUT_MULTIPLEXER,
	MALI_C720_DISC_MODULE_ID_GAUSSIAN_PYRAMID,
	MALI_C720_DISC_MODULE_ID_RAW_STREAM_PACKER,
	MALI_C720_DISC_MODULE_ID_TPG_RAW_MULTI,
	MALI_C720_DISC_MODULE_ID_TPG_RAW_SINGLE,
	MALI_C720_DISC_MODULE_ID_TPG_RAW_RGB_SINGLE,
	MALI_C720_DISC_MODULE_ID_FGM,
	MALI_C720_DISC_MODULE_ID_FBIST_BRIDGE,
	MALI_C720_DISC_MODULE_ID_PIPELINE_CRC,
	MALI_C720_DISC_MODULE_ID_INPUT_CRC,
	MALI_C720_DISC_MODULE_ID_INTENSITY_STATS,
	MALI_C720_DISC_MODULE_ID_HISTOGRAM_STATS,
	MALI_C720_DISC_MODULE_ID_COLOR_STATS,
	MALI_C720_DISC_MODULE_ID_AUTO_FOCUS_STATS,
	MALI_C720_DISC_MODULE_ID_MAX,
};

struct mali_c720_discovery_target {
	enum mali_c720_discovery_module_id module_id;
	u8 module_instance;
	enum mali_c720_discovery_pipeline_id pipeline_id;
	u8 pipeline_instance;
};

enum mali_c720_discovery_table_scaler_rgb_id {
	MALI_C720_DISC_TABLE_SCALER_RGB_H_MEM_ID = 0,
	MALI_C720_DISC_TABLE_SCALER_RGB_V_MEM_ID,
	MALI_C720_DISC_TABLE_SCALER_RGB_MAX_ID,
};

struct mali_c720_discovery_table {
	u32 base;
	u32 size;
	unsigned int id;
	struct list_head node;
};

struct mali_c720_discovery_version {
	bool present;
	u8 major;
	u8 minor;
	u8 patch;
};

struct mali_c720_discovery_param {
	u8 id;
	u16 value;
	struct list_head node;
};

struct mali_c720_discovery_module {
	enum mali_c720_discovery_module_id id;
	unsigned int instance_id;
	bool found;
	bool present;
	u32 base;
	struct mali_c720_discovery_version version;
	struct list_head memories;
	struct list_head fbists;
	struct list_head mbists;
	struct list_head config_crcs;
	struct list_head clock_gating;
	struct list_head params;
	struct list_head node;
	unsigned int num_memories;
	unsigned int num_params;
	unsigned int num_fbists;
	unsigned int num_mbists;
	unsigned int num_config_crcs;
	unsigned int num_clock_gating;
	unsigned int fbist_instance_id;
	unsigned int mbist_instance_id;
	unsigned int config_crc_instance_id;
	unsigned int clock_gating_instance_id;
};

struct mali_c720_discovery_pipeline {
	bool present;
	unsigned int instance_id;
	enum mali_c720_discovery_pipeline_id id;
	struct list_head modules;
};

struct mali_c720_discovery {
	enum mali_c720_discovery_version_id version;
	unsigned int next_module_id;
	unsigned int next_memory_id;
	unsigned int next_param_id;
	struct mali_c720_discovery_module
		modules[MALI_C720_DISCOVERY_MAX_MODULES];
	struct mali_c720_discovery_table
		memories[MALI_C720_DISCOVERY_MAX_MEMORIES];
	struct mali_c720_discovery_param
		params[MALI_C720_DISCOVERY_MAX_PARAMS];
	struct mali_c720_discovery_pipeline
		pipelines[MALI_C720_DISC_PIPE_MAX][MALI_C720_DISCOVERY_MAX_PIPELINE_INSTANCES];
};

int mali_c720_discovery_init(struct mali_c720 *mali_c720, resource_size_t reg_size);

int mali_c720_discovery_get_module_base(struct mali_c720 *mali_c720,
					const struct mali_c720_discovery_target *target,
					u32 *base);

int mali_c720_discovery_get_module_table_base(struct mali_c720 *mali_c720,
					      const struct mali_c720_discovery_target *target,
					      unsigned int table_id,
					      u32 *base);

bool mali_c720_discovery_module_present(struct mali_c720 *mali_c720,
					const struct mali_c720_discovery_target *target);

#endif /* MALI_C720_DISCOVERY_H */
