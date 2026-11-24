// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/bits.h>

#include "mali-c720-registers-ctx-mgr.h"

static const struct mali_c720_cm_capabilities caps_r0 = {
	.num_inputs = 4,
	.num_input_streams = 4,
	.cdma_sg_size = 32,
	.num_raw_buffers = 64,
	.num_slots = 20,
	.num_out_buffers = 208,
	.input_fifo_size = 4096,
	.output_fifo_size = 2048,
};

static const struct mali_c720_cm_fifo_regs input_fifo_r0 = {
	.group = { .offset = 0x6020, .size = 0x10 },
	.base_address = { .offset = 0x0, .mask = GENMASK(15, 0), .shift = 0 },
	.size = { .offset = 0x0, .mask = GENMASK(31, 16), .shift = 16 },
};

static const struct mali_c720_cm_temper_fifo_regs temper_fifo_r0 = {
	.w_fifo_base_address = { .offset = 0x6140, .mask = GENMASK(15, 0), .shift = 0 },
	.w_fifo_size = { .offset = 0x6140, .mask = GENMASK(31, 16), .shift = 16 },
	.r_fifo_base_address = { .offset = 0x6150, .mask = GENMASK(15, 0), .shift = 0 },
	.r_fifo_size = { .offset = 0x6150, .mask = GENMASK(31, 16), .shift = 16 },
};

static const struct mali_c720_cm_job_control_regs job_control_r0 = {
	.sch_command = { .offset = 0x6160, .mask = GENMASK(7, 0), .shift = 0 },
	.sch_command_slot = { .offset = 0x6160, .mask = GENMASK(15, 8), .shift = 8 },
	.sch_status_arch = { .offset = 0x6164, .mask = GENMASK(2, 0), .shift = 0 },
	.m2m_queue_command = { .offset = 0x616c, .mask = GENMASK(1, 0), .shift = 0 },
	.m2m_queue_command_slot_no = { .offset = 0x616c, .mask = GENMASK(15, 8), .shift = 8 },
};

static const struct mali_c720_cm_input_regs input_r0 = {
	.group = { .offset = 0x62fc, .size = 0x20 },
	.command = { .offset = 0x0, .mask = GENMASK(7, 0), .shift = 0 },
	.status_arch = { .offset = 0x4, .mask = GENMASK(2, 0), .shift = 0 },
	.pixel_size = { .offset = 0xc, .mask = GENMASK(5, 0), .shift = 0 },
	.data_format = { .offset = 0xc, .mask = GENMASK(9, 7), .shift = 7 },
	.nonsecure = { .offset = 0xc, .mask = BIT(10), .shift = 10 },
	.stream_id = { .offset = 0xc, .mask = GENMASK(15, 11), .shift = 11 },
	.data_type1_enable = { .offset = 0xc, .mask = BIT(16), .shift = 16 },
	.data_type2_enable = { .offset = 0xc, .mask = BIT(17), .shift = 17 },
	.crop_enable = { .offset = 0xc, .mask = BIT(19), .shift = 19 },
	.drop_oldest = { .offset = 0xc, .mask = BIT(20), .shift = 20 },
	.flow_control = { .offset = 0xc, .mask = BIT(21), .shift = 21 },
	.drop_if_error = { .offset = 0xc, .mask = BIT(22), .shift = 22 },
	.check_parity = { .offset = 0xc, .mask = BIT(23), .shift = 23 },
	.buffer_pool = { .offset = 0xc, .mask = GENMASK(31, 24), .shift = 24 },
	.data_type1 = { .offset = 0x10, .mask = GENMASK(7, 0), .shift = 0 },
	.data_type2 = { .offset = 0x10, .mask = GENMASK(15, 8), .shift = 8 },
	.start_x = { .offset = 0x14, .mask = GENMASK(15, 0), .shift = 0 },
	.start_y = { .offset = 0x14, .mask = GENMASK(31, 16), .shift = 16 },
	.size_x = { .offset = 0x18, .mask = GENMASK(15, 0), .shift = 0 },
	.size_y = { .offset = 0x18, .mask = GENMASK(31, 16), .shift = 16 },
	.write_threshold = { .offset = 0x1c, .mask = GENMASK(15, 0), .shift = 0 },
	.write_threshold_sec = { .offset = 0x1c, .mask = GENMASK(31, 16), .shift = 16 },
};

static const struct mali_c720_cm_slot_regs slot_r0 = {
	.group = { .offset = 0x6500, .size = 0x40 },
	.mode = { .offset = 0x0, .mask = GENMASK(2, 0), .shift = 0 },
	.timeout_enable = { .offset = 0x0, .mask = BIT(3), .shift = 3 },
	.schedule_priority = { .offset = 0x0, .mask = BIT(8), .shift = 8 },
	.nonsecure = { .offset = 0x0, .mask = BIT(10), .shift = 10 },
	.stream_id = { .offset = 0x0, .mask = GENMASK(15, 11), .shift = 11 },
	.status = { .offset = 0x0, .mask = GENMASK(26, 24), .shift = 24 },
	.delay_mant = { .offset = 0x4, .mask = GENMASK(11, 0), .shift = 0 },
	.delay_exp = { .offset = 0x4, .mask = GENMASK(15, 12), .shift = 12 },
	.slot_event = { .offset = 0x4, .mask = GENMASK(19, 16), .shift = 16 },
	.slot_input_0 = { .offset = 0x8, .mask = GENMASK(7, 0), .shift = 0 },
	.slot_input_1 = { .offset = 0x8, .mask = GENMASK(15, 8), .shift = 8 },
	.timeout_mant = { .offset = 0xc, .mask = GENMASK(11, 0), .shift = 0 },
	.timeout_exp = { .offset = 0xc, .mask = GENMASK(15, 12), .shift = 12 },
	.pool_roi0 = { .offset = 0x10, .mask = GENMASK(7, 0), .shift = 0 },
	.pool_roi1 = { .offset = 0x10, .mask = GENMASK(15, 8), .shift = 8 },
	.pool_roi2 = { .offset = 0x10, .mask = GENMASK(23, 16), .shift = 16 },
	.pool_raw = { .offset = 0x10, .mask = GENMASK(31, 24), .shift = 24 },
	.pool_scaler0_rgb = { .offset = 0x14, .mask = GENMASK(7, 0), .shift = 0 },
	.pool_scaler0_yuv = { .offset = 0x14, .mask = GENMASK(15, 8), .shift = 8 },
	.pool_scaler1_rgb = { .offset = 0x14, .mask = GENMASK(23, 16), .shift = 16 },
	.pool_scaler1_yuv = { .offset = 0x14, .mask = GENMASK(31, 24), .shift = 24 },
	.pool_scaler2_rgb = { .offset = 0x18, .mask = GENMASK(7, 0), .shift = 0 },
	.pool_scaler2_yuv = { .offset = 0x18, .mask = GENMASK(15, 8), .shift = 8 },
	.pool_scaler3_rgb = { .offset = 0x18, .mask = GENMASK(23, 16), .shift = 16 },
	.pool_scaler3_yuv = { .offset = 0x18, .mask = GENMASK(31, 24), .shift = 24 },
	.pool_gdc_main = { .offset = 0x1c, .mask = GENMASK(7, 0), .shift = 0 },
	.pool_gdc_roi0 = { .offset = 0x1c, .mask = GENMASK(15, 8), .shift = 8 },
	.pool_gdc_roi1 = { .offset = 0x1c, .mask = GENMASK(23, 16), .shift = 16 },
	.pool_gdc_roi2 = { .offset = 0x1c, .mask = GENMASK(31, 24), .shift = 24 },
	.pool_pyramid = { .offset = 0x20, .mask = GENMASK(7, 0), .shift = 0 },
	.pool_consistency = { .offset = 0x20, .mask = GENMASK(15, 8), .shift = 8 },
	.crc = { .offset = 0x24, .mask = GENMASK(31, 0), .shift = 0 },
};

static const struct mali_c720_cm_buffer_regs buffer_r0 = {
	.out_group = { .offset = 0x12000, .size = 0x80 },
	.raw_group = { .offset = 0x10000, .size = 0x80 },
	.temper_group = { .offset = 0x18800, .size = 0x80 },
	.state = { .offset = 0x0, .mask = GENMASK(3, 0), .shift = 0 },
	.state_request = { .offset = 0x0, .mask = GENMASK(7, 4), .shift = 4 },
	.data_format = { .offset = 0x4, .mask = GENMASK(7, 0), .shift = 0 },
	.data_align = { .offset = 0x4, .mask = GENMASK(9, 8), .shift = 8 },
	.nonsecure = { .offset = 0x4, .mask = BIT(10), .shift = 10 },
	.stream_id = { .offset = 0x4, .mask = GENMASK(15, 11), .shift = 11 },
	.data_layout = { .offset = 0x4, .mask = BIT(16), .shift = 16 },
	.crc_check = { .offset = 0x4, .mask = BIT(17), .shift = 17 },
	.pool = { .offset = 0x4, .mask = BIT(18), .shift = 18 },
	.mode = { .offset = 0x4, .mask = GENMASK(21, 19), .shift = 19 },
	.block_size = { .offset = 0x4, .mask = GENMASK(23, 22), .shift = 22 },
	.base_address_low = { .offset = 0x8, .mask = GENMASK(31, 0), .shift = 0 },
	.base_address_high = { .offset = 0xc, .mask = GENMASK(31, 0), .shift = 0 },
	.size = { .offset = 0x10, .mask = GENMASK(31, 0), .shift = 0 },
	.plane2_offset = { .offset = 0x14, .mask = GENMASK(31, 0), .shift = 0 },
	.plane3_offset = { .offset = 0x18, .mask = GENMASK(31, 0), .shift = 0 },
	.plane4_offset = { .offset = 0x1c, .mask = GENMASK(31, 0), .shift = 0 },
	.line1_offset = { .offset = 0x20, .mask = GENMASK(31, 0), .shift = 0 },
	.line2_offset = { .offset = 0x24, .mask = GENMASK(31, 0), .shift = 0 },
	.line3_offset = { .offset = 0x28, .mask = GENMASK(31, 0), .shift = 0 },
	.line4_offset = { .offset = 0x2c, .mask = GENMASK(31, 0), .shift = 0 },
	.width = { .offset = 0x30, .mask = GENMASK(15, 0), .shift = 0 },
	.height = { .offset = 0x30, .mask = GENMASK(31, 16), .shift = 16 },
	.colorspace = { .offset = 0x34, .mask = GENMASK(3, 0), .shift = 0 },
	.subsampling = { .offset = 0x34, .mask = GENMASK(5, 4), .shift = 4 },
	.component_width = { .offset = 0x34, .mask = GENMASK(10, 8), .shift = 8 },
	.msb_align = { .offset = 0x34, .mask = BIT(11), .shift = 11 },
	.phase = { .offset = 0x34, .mask = GENMASK(14, 12), .shift = 12 },
	.height_modulo = { .offset = 0x34, .mask = GENMASK(31, 16), .shift = 16 },
	.compression = { .offset = 0x38, .mask = GENMASK(31, 0), .shift = 0 },
	.compression23 = { .offset = 0x3c, .mask = GENMASK(31, 0), .shift = 0 },
	.descriptor_crc = { .offset = 0x40, .mask = GENMASK(15, 0), .shift = 0 },
	.frame_no = { .offset = 0x4c, .mask = GENMASK(31, 0), .shift = 0 },
};

static const struct mali_c720_cm_cdma_slot_regs cdma_slot_r0 = {
	.group = { .offset = 0x19200, .size = 0x40 },
	.cdma_state = { .offset = 0x0, .mask = GENMASK(1, 0), .shift = 0 },
	.cdma_state_request = { .offset = 0x0, .mask = GENMASK(5, 4), .shift = 4 },
	.infinite = { .offset = 0x0, .mask = BIT(8), .shift = 8 },
	.nonsecure = { .offset = 0x0, .mask = BIT(10), .shift = 10 },
	.stream_id = { .offset = 0x0, .mask = GENMASK(15, 11), .shift = 11 },
	.transfer_size = { .offset = 0x4, .mask = GENMASK(31, 0), .shift = 0 },
	.units_low = { .offset = 0x8, .mask = GENMASK(31, 0), .shift = 0 },
	.units_high = { .offset = 0xc, .mask = GENMASK(31, 0), .shift = 0 },
	.base_address_low = { .offset = 0x10, .mask = GENMASK(31, 0), .shift = 0 },
	.base_address_high = { .offset = 0x14, .mask = GENMASK(31, 0), .shift = 0 },
	.ld_sg_count = { .offset = 0x18, .mask = GENMASK(5, 0), .shift = 0 },
	.st_sg_count = { .offset = 0x18, .mask = GENMASK(13, 8), .shift = 8 },
	.alt_transfer_size = { .offset = 0x1c, .mask = GENMASK(31, 0), .shift = 0 },
	.alt_base_address_low = { .offset = 0x20, .mask = GENMASK(31, 0), .shift = 0 },
	.alt_base_address_high = { .offset = 0x24, .mask = GENMASK(31, 0), .shift = 0 },
	.crc = { .offset = 0x28, .mask = GENMASK(15, 0), .shift = 0 },
};

static const struct mali_c720_cm_load_out_fifo_regs load_out_fifo_r0 = {
	.scaler_group = { .offset = 0x808a0, .size = 0x10 },
	.roi_group = { .offset = 0x80810, .size = 0x10 },
	.pyramid_group = { .offset = 0x80a20, .size = 0x10 },
	.raw_group = { .offset = 0x80a60, .size = 0x10 },
	.base_address = { .offset = 0x0, .mask = GENMASK(15, 0), .shift = 0 },
	.size = { .offset = 0x0, .mask = GENMASK(31, 16), .shift = 16 },
};

static const struct mali_c720_cm_sg_regs load_cdma_sg_load_r0 = {
	.group = { .offset = 0x80a80, .size = 0x8 },
	.isp_address = { .offset = 0x0, .mask = GENMASK(19, 0), .shift = 0 },
	.length_low = { .offset = 0x0, .mask = GENMASK(31, 20), .shift = 20 },
	.length_high = { .offset = 0x4, .mask = GENMASK(3, 0), .shift = 0 },
	.axi_address = { .offset = 0x4, .mask = GENMASK(23, 4), .shift = 4 },
	.alt_address = { .offset = 0x4, .mask = BIT(31), .shift = 31 },
};

static const struct mali_c720_cm_sg_crc_regs load_cdma_sg_load_crc_r0 = {
	.cdma_sg_crc = { .offset = 0x80b80, .mask = GENMASK(15, 0), .shift = 0 },
};

static const struct mali_c720_cm_sg_regs load_cdma_sg_store_r0 = {
	.group = { .offset = 0x80ba0, .size = 0x8 },
	.isp_address = { .offset = 0x0, .mask = GENMASK(19, 0), .shift = 0 },
	.length_low = { .offset = 0x0, .mask = GENMASK(31, 20), .shift = 20 },
	.length_high = { .offset = 0x4, .mask = GENMASK(3, 0), .shift = 0 },
	.axi_address = { .offset = 0x4, .mask = GENMASK(23, 4), .shift = 4 },
	.alt_address = { .offset = 0x4, .mask = BIT(31), .shift = 31 },
};

static const struct mali_c720_cm_sg_crc_regs load_cdma_sg_store_crc_r0 = {
	.cdma_sg_crc = { .offset = 0x80ca0, .mask = GENMASK(15, 0), .shift = 0 },
};

static const struct mali_c720_cm_fifo_regs reader_fifo_r0 = {
	.base_address = { .offset = 0x0, .mask = GENMASK(15, 0), .shift = 0 },
	.size = { .offset = 0x0, .mask = GENMASK(31, 16), .shift = 16 },
	.group = { .offset = 0x6120, .size = 0x10 },
};

static const struct mali_c720_ctx_mgr_regs mali_c720_ctx_mgr_regs_r0 = {
	.caps = &caps_r0,
	.input_fifo = &input_fifo_r0,
	.temper_fifo = &temper_fifo_r0,
	.job_control = &job_control_r0,
	.input = &input_r0,
	.slot = &slot_r0,
	.buffer = &buffer_r0,
	.cdma_slot = &cdma_slot_r0,
	.load_out_fifo = &load_out_fifo_r0,
	.load_cdma_sg_load = &load_cdma_sg_load_r0,
	.load_cdma_sg_load_crc = &load_cdma_sg_load_crc_r0,
	.load_cdma_sg_store = &load_cdma_sg_store_r0,
	.load_cdma_sg_store_crc = &load_cdma_sg_store_crc_r0,
	.reader_fifo = &reader_fifo_r0,
};

const struct mali_c720_ctx_mgr_regs *mali_c720_get_ctx_mgr_regs(void)
{
	return &mali_c720_ctx_mgr_regs_r0;
}
