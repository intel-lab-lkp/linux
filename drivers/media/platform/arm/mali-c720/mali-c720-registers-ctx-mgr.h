/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_REGISTERS_CTX_MGR_H
#define MALI_C720_REGISTERS_CTX_MGR_H

#include "mali-c720-registers.h"

struct mali_c720_cm_capabilities {
	u32 num_inputs;
	u32 num_input_streams;
	u32 cdma_sg_size;
	u32 num_raw_buffers;
	u32 num_slots;
	u32 num_out_buffers;
	u32 input_fifo_size;
	u32 output_fifo_size;
};

struct mali_c720_cm_fifo_regs {
	struct mali_c720_group_regs group;
	struct mali_c720_reg_field base_address;
	struct mali_c720_reg_field size;
};

struct mali_c720_cm_temper_fifo_regs {
	struct mali_c720_reg_field w_fifo_base_address;
	struct mali_c720_reg_field w_fifo_size;
	struct mali_c720_reg_field r_fifo_base_address;
	struct mali_c720_reg_field r_fifo_size;
};

struct mali_c720_cm_job_control_regs {
	struct mali_c720_reg_field sch_command;
	struct mali_c720_reg_field sch_command_slot;
	struct mali_c720_reg_field sch_status_arch;
	struct mali_c720_reg_field m2m_queue_command;
	struct mali_c720_reg_field m2m_queue_command_slot_no;
};

struct mali_c720_cm_input_regs {
	struct mali_c720_group_regs group;
	struct mali_c720_reg_field command;
	struct mali_c720_reg_field status_arch;
	struct mali_c720_reg_field pixel_size;
	struct mali_c720_reg_field data_format;
	struct mali_c720_reg_field nonsecure;
	struct mali_c720_reg_field stream_id;
	struct mali_c720_reg_field data_type1_enable;
	struct mali_c720_reg_field data_type2_enable;
	struct mali_c720_reg_field crop_enable;
	struct mali_c720_reg_field drop_oldest;
	struct mali_c720_reg_field flow_control;
	struct mali_c720_reg_field drop_if_error;
	struct mali_c720_reg_field check_parity;
	struct mali_c720_reg_field buffer_pool;
	struct mali_c720_reg_field data_type1;
	struct mali_c720_reg_field data_type2;
	struct mali_c720_reg_field start_x;
	struct mali_c720_reg_field start_y;
	struct mali_c720_reg_field size_x;
	struct mali_c720_reg_field size_y;
	struct mali_c720_reg_field write_threshold;
	struct mali_c720_reg_field write_threshold_sec;
};

struct mali_c720_cm_slot_regs {
	struct mali_c720_group_regs group;
	struct mali_c720_reg_field mode;
	struct mali_c720_reg_field timeout_enable;
	struct mali_c720_reg_field schedule_priority;
	struct mali_c720_reg_field nonsecure;
	struct mali_c720_reg_field stream_id;
	struct mali_c720_reg_field status;
	struct mali_c720_reg_field delay_mant;
	struct mali_c720_reg_field delay_exp;
	struct mali_c720_reg_field slot_event;
	struct mali_c720_reg_field slot_input_0;
	struct mali_c720_reg_field slot_input_1;
	struct mali_c720_reg_field timeout_mant;
	struct mali_c720_reg_field timeout_exp;
	struct mali_c720_reg_field pool_roi0;
	struct mali_c720_reg_field pool_roi1;
	struct mali_c720_reg_field pool_roi2;
	struct mali_c720_reg_field pool_raw;
	struct mali_c720_reg_field pool_scaler0_rgb;
	struct mali_c720_reg_field pool_scaler0_yuv;
	struct mali_c720_reg_field pool_scaler1_rgb;
	struct mali_c720_reg_field pool_scaler1_yuv;
	struct mali_c720_reg_field pool_scaler2_rgb;
	struct mali_c720_reg_field pool_scaler2_yuv;
	struct mali_c720_reg_field pool_scaler3_rgb;
	struct mali_c720_reg_field pool_scaler3_yuv;
	struct mali_c720_reg_field pool_gdc_main;
	struct mali_c720_reg_field pool_gdc_roi0;
	struct mali_c720_reg_field pool_gdc_roi1;
	struct mali_c720_reg_field pool_gdc_roi2;
	struct mali_c720_reg_field pool_pyramid;
	struct mali_c720_reg_field pool_consistency;
	struct mali_c720_reg_field crc;
};

struct mali_c720_cm_buffer_regs {
	struct mali_c720_group_regs out_group;
	struct mali_c720_group_regs raw_group;
	struct mali_c720_group_regs temper_group;
	struct mali_c720_reg_field state;
	struct mali_c720_reg_field state_request;
	struct mali_c720_reg_field data_format;
	struct mali_c720_reg_field data_align;
	struct mali_c720_reg_field nonsecure;
	struct mali_c720_reg_field stream_id;
	struct mali_c720_reg_field data_layout;
	struct mali_c720_reg_field crc_check;
	struct mali_c720_reg_field pool;
	struct mali_c720_reg_field mode;
	struct mali_c720_reg_field block_size;
	struct mali_c720_reg_field base_address_low;
	struct mali_c720_reg_field base_address_high;
	struct mali_c720_reg_field size;
	struct mali_c720_reg_field plane2_offset;
	struct mali_c720_reg_field plane3_offset;
	struct mali_c720_reg_field plane4_offset;
	struct mali_c720_reg_field line1_offset;
	struct mali_c720_reg_field line2_offset;
	struct mali_c720_reg_field line3_offset;
	struct mali_c720_reg_field line4_offset;
	struct mali_c720_reg_field width;
	struct mali_c720_reg_field height;
	struct mali_c720_reg_field colorspace;
	struct mali_c720_reg_field subsampling;
	struct mali_c720_reg_field component_width;
	struct mali_c720_reg_field msb_align;
	struct mali_c720_reg_field phase;
	struct mali_c720_reg_field height_modulo;
	struct mali_c720_reg_field compression;
	struct mali_c720_reg_field compression23;
	struct mali_c720_reg_field descriptor_crc;
	struct mali_c720_reg_field frame_no;
};

struct mali_c720_cm_cdma_slot_regs {
	struct mali_c720_group_regs group;
	struct mali_c720_reg_field cdma_state;
	struct mali_c720_reg_field cdma_state_request;
	struct mali_c720_reg_field infinite;
	struct mali_c720_reg_field nonsecure;
	struct mali_c720_reg_field stream_id;
	struct mali_c720_reg_field transfer_size;
	struct mali_c720_reg_field units_low;
	struct mali_c720_reg_field units_high;
	struct mali_c720_reg_field base_address_low;
	struct mali_c720_reg_field base_address_high;
	struct mali_c720_reg_field ld_sg_count;
	struct mali_c720_reg_field st_sg_count;
	struct mali_c720_reg_field alt_transfer_size;
	struct mali_c720_reg_field alt_base_address_low;
	struct mali_c720_reg_field alt_base_address_high;
	struct mali_c720_reg_field crc;
};

struct mali_c720_cm_load_out_fifo_regs {
	struct mali_c720_group_regs scaler_group;
	struct mali_c720_group_regs roi_group;
	struct mali_c720_group_regs pyramid_group;
	struct mali_c720_group_regs raw_group;
	struct mali_c720_reg_field base_address;
	struct mali_c720_reg_field size;
};

struct mali_c720_cm_sg_regs {
	struct mali_c720_group_regs group;
	struct mali_c720_reg_field isp_address;
	struct mali_c720_reg_field length_low;
	struct mali_c720_reg_field length_high;
	struct mali_c720_reg_field axi_address;
	struct mali_c720_reg_field alt_address;
};

struct mali_c720_cm_sg_crc_regs {
	struct mali_c720_reg_field cdma_sg_crc;
};

struct mali_c720_ctx_mgr_regs {
	const struct mali_c720_cm_capabilities *caps;
	const struct mali_c720_cm_fifo_regs *input_fifo;
	const struct mali_c720_cm_temper_fifo_regs *temper_fifo;
	const struct mali_c720_cm_job_control_regs *job_control;
	const struct mali_c720_cm_input_regs *input;
	const struct mali_c720_cm_slot_regs *slot;
	const struct mali_c720_cm_buffer_regs *buffer;
	const struct mali_c720_cm_cdma_slot_regs *cdma_slot;
	const struct mali_c720_cm_load_out_fifo_regs *load_out_fifo;
	const struct mali_c720_cm_sg_regs *load_cdma_sg_load;
	const struct mali_c720_cm_sg_crc_regs *load_cdma_sg_load_crc;
	const struct mali_c720_cm_sg_regs *load_cdma_sg_store;
	const struct mali_c720_cm_sg_crc_regs *load_cdma_sg_store_crc;
	const struct mali_c720_cm_fifo_regs *reader_fifo;
};

const struct mali_c720_ctx_mgr_regs *mali_c720_get_ctx_mgr_regs(void);

#endif /* MALI_C720_REGISTERS_CTX_MGR_H */
