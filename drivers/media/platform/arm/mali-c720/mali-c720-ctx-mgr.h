/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_CTX_MGR_H
#define MALI_C720_CTX_MGR_H

#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct mali_c720;
struct mali_c720_buffer_cfg;
struct mali_c720_context;

#define MALI_C720_MAX_BUFFER_POOL_SIZE	2
#define MALI_C720_MAX_RAW_BUFFERS	64
#define MALI_C720_MAX_OUT_BUFFERS	208
#define MALI_C720_MAX_BUFFER_PLANES	4
#define MALI_C720_MAX_INPUTS		4
#define MALI_C720_MAX_STREAMS_PER_INPUT	4
#define MALI_C720_MAX_VIRTUAL_INPUTS	(MALI_C720_MAX_INPUTS * MALI_C720_MAX_STREAMS_PER_INPUT)
#define MALI_C720_MAX_CONTEXTS		16
#define MALI_C720_MAX_CDMA_SG_DESCRIPTORS 32
#define MALI_C720_NUM_SAFETY_SLOTS	4

enum mali_c720_input_command {
	MALI_C720_INPUT_COMMAND_DISABLE = 1,
	MALI_C720_INPUT_COMMAND_ENABLE,
	MALI_C720_INPUT_COMMAND_CANCEL,
	MALI_C720_INPUT_COMMAND_FLUSH,
	MALI_C720_INPUT_COMMAND_RESET_COUNTER,
};

enum mali_c720_input_state {
	MALI_C720_INPUT_DISABLED = 1,
	MALI_C720_INPUT_ENABLED,
	MALI_C720_INPUT_CANCELLING,
	MALI_C720_INPUT_DISABLING = 5,
	MALI_C720_INPUT_ENABLING = 6,
};

enum mali_c720_scheduler_command_ids {
	MALI_C720_SCH_COMMAND_ENABLE = 0x1,
	MALI_C720_SCH_COMMAND_DISABLE,
	MALI_C720_SCH_COMMAND_CANCEL,
	MALI_C720_SCH_COMMAND_BYPASS_SLOT,
	MALI_C720_SCH_COMMAND_ENABLE_SLOT,
	MALI_C720_SCH_COMMAND_DISABLE_SLOT,
	MALI_C720_SCH_COMMAND_CANCEL_SLOT,
};

enum mali_c720_slot_state {
	MALI_C720_SLOT_STATE_PREPARED,
	MALI_C720_SLOT_STATE_STARTING,
	MALI_C720_SLOT_STATE_STARTED,
	MALI_C720_SLOT_STATE_STOPPING,
	MALI_C720_SLOT_STATE_STOPPED,
};

enum mali_c720_slot_status {
	MALI_C720_SLOT_DISABLED,
	MALI_C720_SLOT_ENABLED,
	MALI_C720_SLOT_CANCELLING,
	MALI_C720_SLOT_BYPASSED,
	MALI_C720_SLOT_ERROR,
	MALI_C720_SLOT_DISABLING,
};

enum mali_c720_slot_mode {
	MALI_C720_SLOT_MODE_DISABLED,
	MALI_C720_SLOT_MODE_STREAMING,
	MALI_C720_SLOT_MODE_TDMF_FLEX,
	MALI_C720_SLOT_MODE_TDMF_FIX,
	MALI_C720_SLOT_MODE_M2M,
};

enum mali_c720_slot_cdma_state {
	MALI_C720_SLOT_CDMA_STATE_DISABLED,
	MALI_C720_SLOT_CDMA_STATE_CANCELLED,
	MALI_C720_SLOT_CDMA_STATE_SW_CONTROL,
	MALI_C720_SLOT_CDMA_STATE_HW_CONTROL,
};

enum mali_c720_buffer_type_ids {
	MALI_C720_BUF_TYPE_RAW,
	MALI_C720_BUF_TYPE_OUT,
	MALI_C720_BUF_NUM_TYPES,
};

enum mali_c720_buffer_state {
	MALI_C720_BUFFER_STATE_UNINITIALIZED,
	MALI_C720_BUFFER_STATE_CANCELLED,
	MALI_C720_BUFFER_STATE_SW_EMPTY,
	MALI_C720_BUFFER_STATE_SW_FULL,
	MALI_C720_BUFFER_STATE_HW_EMPTY,
	MALI_C720_BUFFER_STATE_HW_FULL,
	MALI_C720_BUFFER_STATE_HW_ERROR,
	MALI_C720_BUFFER_STATE_CANCELLING,
	MALI_C720_BUFFER_STATE_HW_WRITING,
	MALI_C720_BUFFER_STATE_HW_READING,
	MALI_C720_BUFFER_STATE_HW_READING_WRITING,
};

enum mali_c720_raw_buffer_subtype {
	MALI_C720_RAW_BUF_SUBTYPE_NORMAL,
};

enum mali_c720_raw_buffer_mode {
	MALI_C720_RAW_BUFFER_MODE_NORMAL,
	MALI_C720_RAW_BUFFER_MODE_INFINITE,
	MALI_C720_RAW_BUFFER_MODE_SOFTWARE,
	MALI_C720_RAW_BUFFER_MODE_HW_CLONED,
	MALI_C720_RAW_BUFFER_MODE_SW_CLONED,
};

enum mali_c720_out_buffer_mode {
	MALI_C720_OUT_BUFFER_MODE_NORMAL,
	MALI_C720_OUT_BUFFER_MODE_INFINITE,
};

struct mali_c720_input_cfg {
	unsigned int id;
	unsigned int pool_id;
	unsigned int bit_depth;
	bool started;
};

struct mali_c720_top_cfg {
	u32 width;
	u32 height;
	u32 rggb_start;
};

struct mali_c720_slot_desc {
	u16 crc;
	u8 mode;
	u8 timeout_enable;
	u8 schedule_priority;
	u8 nonsecure;
	u8 stream_id;
	u16 delay_mant;
	u8 delay_exp;
	u8 slot_event;
	u8 slot_input_0;
	u8 slot_input_1;
	u16 timeout_mant;
	u8 timeout_exp;
	u8 pool_roi0;
	u8 pool_roi1;
	u8 pool_roi2;
	u8 pool_raw;
	u8 pool_scaler0_rgb;
	u8 pool_scaler0_yuv;
	u8 pool_scaler1_rgb;
	u8 pool_scaler1_yuv;
	u8 pool_scaler2_rgb;
	u8 pool_scaler2_yuv;
	u8 pool_scaler3_rgb;
	u8 pool_scaler3_yuv;
	u8 pool_gdc_main;
	u8 pool_gdc_roi0;
	u8 pool_gdc_roi1;
	u8 pool_gdc_roi2;
	u8 pool_pyramid;
	u8 pool_consistency;
};

struct mali_c720_cdma_desc {
	u16 crc;
	u8 infinite;
	u8 nonsecure;
	u8 stream_id;
	u32 transfer_size;
	u32 units_low;
	u32 units_high;
	u32 base_address_low;
	u32 base_address_high;
	u8 ld_sg_count;
	u8 st_sg_count;
	u32 alt_transfer_size;
	u32 alt_base_address_low;
	u32 alt_base_address_high;
};

struct mali_c720_cdma_sg_cfg {
	u32 isp_address;
	u16 length_low;
	u8 length_high;
	u32 axi_address;
	u8 alt_address;
};

struct mali_c720_cdma_sg_table {
	struct mali_c720_cdma_sg_cfg desc[MALI_C720_MAX_CDMA_SG_DESCRIPTORS];
	u16 crc;
	u8 num_desc;
};

struct mali_c720_buffer_desc {
	u16 crc;
	u8 data_format;
	u8 data_align;
	u8 nonsecure;
	u8 stream_id;
	u8 data_layout;
	u8 crc_check;
	u8 pool;
	u8 mode;
	u8 block_size;
	u32 base_address_low;
	u32 base_address_high;
	u32 buffer_size;
	u32 plane2_offset;
	u32 plane3_offset;
	u32 plane4_offset;
	u32 line1_offset;
	u32 line2_offset;
	u32 line3_offset;
	u32 line4_offset;
	u16 width;
	u16 height;
	u8 colorspace;
	u8 subsampling;
	u8 component_width;
	u8 msb_align;
	u8 phase;
	u16 height_modulo;
	u32 compression;
	u32 compression23;
};

struct mali_c720_buffer_request_info {
	const struct mali_c720_buffer_cfg *buffer_cfg;
	size_t plane_size[MALI_C720_MAX_BUFFER_PLANES];
	unsigned int line_offset[MALI_C720_MAX_BUFFER_PLANES];
	unsigned int width;
	unsigned int height;
	unsigned int type;
	unsigned int sub_type;
	unsigned int num_out_planes;
	unsigned int num_raw_buffers;
};

struct mali_c720_buffer_pool {
	struct {
		dma_addr_t addr;
		void *base;
	} mem[MALI_C720_MAX_BUFFER_POOL_SIZE];
	struct mali_c720_buffer_desc desc[MALI_C720_MAX_BUFFER_POOL_SIZE];
	const struct mali_c720_buffer_cfg *buffer_cfg;

	unsigned int width;
	unsigned int height;
	unsigned int type;
	unsigned int sub_type;
	unsigned int pool_id;
	unsigned int num_buffers;
	size_t buffer_size;
};

struct mali_c720_ctx_mgr {
	/* Protects input_context_map; acquire before a context's state_lock. */
	spinlock_t input_lock;
	u8 input_context_map[MALI_C720_MAX_VIRTUAL_INPUTS];
	unsigned long raw_buffer_descriptors[BITS_TO_LONGS(MALI_C720_MAX_RAW_BUFFERS)];
	unsigned long out_buffer_descriptors[BITS_TO_LONGS(MALI_C720_MAX_OUT_BUFFERS)];
	struct {
		struct mali_c720_cdma_sg_table load;
		struct mali_c720_cdma_sg_table store;
	} sg_descriptors;
	unsigned int num_raw_buffer_descriptors;
	unsigned int reuse_buffer;
	unsigned int num_out_buffer_descriptors;
	unsigned int num_inputs;
	unsigned int num_input_streams;
	unsigned int num_virtual_inputs;
	unsigned int num_contexts;
	unsigned int cdma_sg_size;
	unsigned int input_fifo_size;
	unsigned int output_fifo_size;
};

struct mali_c720_context *mali_c720_get_active_context(struct mali_c720 *mali_c720);

int mali_c720_init_contexts(struct mali_c720 *mali_c720);
int mali_c720_alloc_context(struct mali_c720_context *ctx);
void mali_c720_free_context(struct mali_c720_context *ctx);

int mali_c720_init_ctx_mgr(struct mali_c720 *mali_c720);

int mali_c720_ctx_mgr_prepare_cdma_sg_descriptors(struct mali_c720 *mali_c720);

int mali_c720_ctx_mgr_start_context_inputs(struct mali_c720_context *ctx);
void mali_c720_ctx_mgr_stop_context_inputs(struct mali_c720_context *ctx);

int mali_c720_ctx_mgr_request_buffer_pool(struct mali_c720_context *ctx,
					  const struct mali_c720_buffer_request_info *info,
					  struct mali_c720_buffer_pool *pool);
void mali_c720_ctx_mgr_release_buffer_pool(struct mali_c720_context *ctx,
					   struct mali_c720_buffer_pool *pool);
int mali_c720_ctx_mgr_check_output_buffer_status(struct mali_c720_context *ctx,
						 struct mali_c720_buffer_pool *pool);

void mali_c720_ctx_mgr_update_buffer(struct mali_c720_context *ctx,
				     struct mali_c720_buffer_pool *pool,
				     dma_addr_t *addr,
				     unsigned int index);
void mali_c720_ctx_mgr_set_stats_buffer_address(struct mali_c720_context *ctx,
						dma_addr_t *addr);

void mali_c720_ctx_mgr_configure_context(struct mali_c720_context *ctx,
					 const struct mali_c720_top_cfg *cfg);
void mali_c720_ctx_mgr_prepare_context(struct mali_c720_context *ctx,
				       const struct mali_c720_input_cfg *input_cfg);
void mali_c720_ctx_mgr_start_context(struct mali_c720_context *ctx);
void mali_c720_ctx_mgr_stop_context(struct mali_c720_context *ctx);
int mali_c720_ctx_mgr_check_context_status(struct mali_c720_context *ctx);

void mali_c720_ctx_mgr_prepare_frame(struct mali_c720_context *ctx);
void mali_c720_ctx_mgr_buffer_done(struct mali_c720_context *ctx, bool success);
void mali_c720_ctx_mgr_handle_sw_ready(struct mali_c720_context *ctx);

#endif /* MALI_C720_CTX_MGR_H */
