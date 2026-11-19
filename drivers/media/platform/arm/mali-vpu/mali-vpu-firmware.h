/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_FIRMWARE_H__
#define __MALI_VPU_FIRMWARE_H__

#include <linux/hashtable.h>
#include <linux/media/arm/mali-vpu-config.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include "mali-vpu-buffer.h"
#include "mali-vpu-mmu.h"

#define MALI_VPU_FW_HTABLE_BITS 3
#define MALI_VPU_FW_TEXT_BASE_ADDR 0x1000u

struct device;
struct mali_vpu_mmu;
struct mali_vpu_fw_bin;
struct mali_vpu_mmu_pages;
struct mve_buffer_frame;
struct mve_buffer_bitstream;

/**
 * enum mali_vpu_fw_state - Firmware execution state.
 * @MALI_VPU_FW_STATE_STOPPED: Firmware is STOPPED.
 * @MALI_VPU_FW_STATE_RUNNING: Firmware is RUNNING.
 */
enum mali_vpu_fw_state {
	MALI_VPU_FW_STATE_STOPPED,
	MALI_VPU_FW_STATE_RUNNING
};

/**
 * struct mali_vpu_fw_job - Job request.
 * @cores: Number for cores to use.
 * @frames: Number of frames to process before job is switched out.
 */
struct mali_vpu_fw_job {
	unsigned int cores;
	unsigned int frames;
};

/**
 * enum mali_vpu_fw_set_option_code - Firmware option selectors
 * @MALI_VPU_FW_SET_NALU_FORMAT: Set the NAL-unit format.
 * @MALI_VPU_FW_SET_STREAM_ESCAPING: Configure stream escaping.
 * @MALI_VPU_FW_SET_HOST_PROTOCOL_PRINTS: Configure firmware logging.
 * @MALI_VPU_FW_SET_IGNORE_STREAM_HEADERS: Configure stream-header handling.
 * @MALI_VPU_FW_SET_FRAME_REORDERING: Configure frame reordering.
 * @MALI_VPU_FW_SET_INTBUF_SIZE: Set the suggested intermediate-buffer size.
 */
enum mali_vpu_fw_set_option_code {
	MALI_VPU_FW_SET_NALU_FORMAT,
	MALI_VPU_FW_SET_STREAM_ESCAPING,
	MALI_VPU_FW_SET_HOST_PROTOCOL_PRINTS,
	MALI_VPU_FW_SET_IGNORE_STREAM_HEADERS,
	MALI_VPU_FW_SET_FRAME_REORDERING,
	MALI_VPU_FW_SET_INTBUF_SIZE,
};

/**
 * struct mali_vpu_fw_set_option - Set firmware options.
 * @code: Code for the option to set.
 * @nalu_format: NAL-unit encapsulation format.
 * @stream_escaping: Whether stream escaping is enabled.
 * @host_protocol_prints: Whether firmware protocol logging is enabled.
 * @ignore_stream_headers: Whether firmware ignores stream headers.
 * @frame_reordering: Whether frame reordering is enabled.
 * @intbuf_size: Suggested intermediate-buffer size.
 */
struct mali_vpu_fw_set_option {
	enum mali_vpu_fw_set_option_code code;
	union {
		enum mali_vpu_nalu_format nalu_format;
		bool stream_escaping;
		bool host_protocol_prints;
		bool ignore_stream_headers;
		bool frame_reordering;
		u32 intbuf_size;
	};
};

/**
 * struct mali_vpu_fw_seq_param - Sequence parameters.
 * @interlaced: Whether the decoded sequence is interlaced.
 * @planar: Planar output-buffer requirements.
 * @planar.buffers_min:	Minimum number of planar buffers required.
 * @bitdepth: Decoded luma and chroma bitdepths.
 * @bitdepth.luma: Decoded luma bitdepth.
 * @bitdepth.chroma: Decoded chroma bitdepth.
 */
struct mali_vpu_fw_seq_param {
	bool interlaced;
	struct {
		u32 buffers_min;
	} planar;
	struct mali_vpu_fw_bitdepth {
		u32 chroma;
		u32 luma;
	} bitdepth;
};

/**
 * struct mali_vpu_fw_alloc_param - Allocation parameters
 * @height: in pixels
 * @width: in pixels
 *
 * Dimensions of the decoded frame buffer
 */
struct mali_vpu_fw_alloc_param {
	u32 height;
	u32 width;
};

/**
 * struct mali_vpu_fw_error - Firmware error message.
 * @error_code: What kind of error that was reported.
 * @message: Error message string.
 */
struct mali_vpu_fw_error {
	enum {
		MALI_VPU_FW_ERROR_UNKNOWN,
		MALI_VPU_FW_ERROR_ABORT,
		MALI_VPU_FW_ERROR_OUT_OF_MEMORY,
		MALI_VPU_FW_ERROR_ASSERT,
		MALI_VPU_FW_ERROR_UNSUPPORTED,
		MALI_VPU_FW_ERROR_INVALID_BUFFER,
		MALI_VPU_FW_ERROR_INVALID_STATE,
		MALI_VPU_FW_ERROR_WATCHDOG
	} error_code;
	char message[128];
};

/**
 * enum mali_vpu_fw_code - Driver representation of firmware messages.
 * @MALI_VPU_FW_CODE_ERROR: Firmware reported fatal error.
 * @MALI_VPU_FW_CODE_JOB: Firmware dequeued a job.
 * @MALI_VPU_FW_CODE_BUFFER: Send and receive buffer.
 * @MALI_VPU_FW_CODE_PING: Ping request sent to firmware.
 * @MALI_VPU_FW_CODE_PONG: Firmware responded to a ping request.
 * @MALI_VPU_FW_CODE_IDLE: Firmware informs the driver that it's idle.
 * @MALI_VPU_FW_CODE_SET_OPTION: Firmware set option request or success response.
 * @MALI_VPU_FW_CODE_SET_OPTION_FAIL: Firmware set option failure response.
 * @MALI_VPU_FW_CODE_STATE_CHANGE: Firmware state-change request or response.
 * @MALI_VPU_FW_CODE_SWITCH_IN: Firmware was booted on a Core in a session.
 * @MALI_VPU_FW_CODE_SWITCH_OUT: The last core in a session was switched out.
 * @MALI_VPU_FW_CODE_EOS: Firmware returned an end-of-stream marker.
 * @MALI_VPU_FW_CODE_INPUT_FLUSH: Firmware successfully completed a OUTPUT queue flush.
 * @MALI_VPU_FW_CODE_OUTPUT_FLUSH: Firmware successfully completed a CAPTURE queue flush.
 * @MALI_VPU_FW_CODE_SEQUENCE_PARAMS: Firmware reported new sequence parameters.
 * @MALI_VPU_FW_CODE_ALLOC_PARAM: Firmware reported new frame allocation parameters.
 * @MALI_VPU_FW_CODE_COLOR_DESC: Firmware reported colour-description metadata.
 * @MALI_VPU_FW_CODE_DPB_HELD_FRAMES: Firmware reported the number of frames held in the DPB.
 * @MALI_VPU_FW_CODE_STREAM_NOT_SUPPORTED: Firmware rejected the coded stream.
 * @MALI_VPU_FW_CODE_EVENT: Firmware reported an informational event.
 * @MALI_VPU_FW_CODE_UNKNOWN: Unrecognized firmware message.
 * @MALI_VPU_FW_CODE_MAX: Sentinel marking the end of the message-code enum.
 */
enum mali_vpu_fw_code {
	MALI_VPU_FW_CODE_ERROR,              /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_JOB,                /* Driver -> Firmware. */
	MALI_VPU_FW_CODE_BUFFER,             /* Driver <-> Firmware. */
	MALI_VPU_FW_CODE_PING,               /* Driver -> Firmware. */
	MALI_VPU_FW_CODE_PONG,               /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_IDLE,               /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_SET_OPTION,         /* Driver <-> Firmware. */
	MALI_VPU_FW_CODE_SET_OPTION_FAIL,    /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_STATE_CHANGE,       /* Driver <-> Firmware. */
	MALI_VPU_FW_CODE_SWITCH_IN,          /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_SWITCH_OUT,         /* Driver <-> Firmware. */
	MALI_VPU_FW_CODE_EOS,                /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_INPUT_FLUSH,        /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_OUTPUT_FLUSH,       /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_SEQUENCE_PARAMS,    /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_ALLOC_PARAM,        /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_COLOR_DESC,         /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_DPB_HELD_FRAMES,    /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_STREAM_NOT_SUPPORTED, /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_EVENT,              /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_UNKNOWN,            /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_MAX
};

/**
 * struct mali_vpu_fw_msg - Parsed firmware message.
 * @code: Driver representation of the firmware message code.
 * @data: Message-specific decoded payload.
 */
struct mali_vpu_fw_msg {
	enum mali_vpu_fw_code code;
	union {
		enum mali_vpu_fw_state state;
		struct mali_vpu_fw_job job;
		struct mali_vpu_fw_error error;
		struct mali_vpu_buffer *vpu_buf;
		struct mali_vpu_fw_alloc_param alloc_params;
		struct mali_vpu_fw_seq_param seq_params;
		u32 dpb_held_frames;
	} data;
};

/**
 * enum mali_vpu_fw_region - Regions in the firmware virtual address space.
 * @MALI_VPU_FW_REGION_MSG_HOST: Host-to-firmware message queue.
 * @MALI_VPU_FW_REGION_MSG_MVE: Firmware-to-host message queue.
 * @MALI_VPU_FW_REGION_BUF_IN_HOST: Host input-buffer queue.
 * @MALI_VPU_FW_REGION_BUF_IN_MVE: Returned input-buffer queue.
 * @MALI_VPU_FW_REGION_BUF_OUT_HOST: Host output-buffer queue.
 * @MALI_VPU_FW_REGION_BUF_OUT_MVE: Returned output-buffer queue.
 * @MALI_VPU_FW_REGION_RPC: Firmware RPC communication area.
 * @MALI_VPU_FW_REGION_CORE_0: Firmware instance region for core 0.
 * @MALI_VPU_FW_REGION_CORE_1: Firmware instance region for core 1.
 * @MALI_VPU_FW_REGION_CORE_2: Firmware instance region for core 2.
 * @MALI_VPU_FW_REGION_CORE_3: Firmware instance region for core 3.
 * @MALI_VPU_FW_REGION_CORE_4: Firmware instance region for core 4.
 * @MALI_VPU_FW_REGION_CORE_5: Firmware instance region for core 5.
 * @MALI_VPU_FW_REGION_CORE_6: Firmware instance region for core 6.
 * @MALI_VPU_FW_REGION_CORE_7: Firmware instance region for core 7.
 * @MALI_VPU_FW_REGION_PROTECTED: Protected firmware memory region.
 * @MALI_VPU_FW_REGION_FRAMEBUF: Frame-buffer memory region.
 */
enum mali_vpu_fw_region {
	MALI_VPU_FW_REGION_MSG_HOST,
	MALI_VPU_FW_REGION_MSG_MVE,
	MALI_VPU_FW_REGION_BUF_IN_HOST,
	MALI_VPU_FW_REGION_BUF_IN_MVE,
	MALI_VPU_FW_REGION_BUF_OUT_HOST,
	MALI_VPU_FW_REGION_BUF_OUT_MVE,
	MALI_VPU_FW_REGION_RPC,

	/* Firmware instance regions (one per core) */
	MALI_VPU_FW_REGION_CORE_0,
	MALI_VPU_FW_REGION_CORE_1,
	MALI_VPU_FW_REGION_CORE_2,
	MALI_VPU_FW_REGION_CORE_3,
	MALI_VPU_FW_REGION_CORE_4,
	MALI_VPU_FW_REGION_CORE_5,
	MALI_VPU_FW_REGION_CORE_6,
	MALI_VPU_FW_REGION_CORE_7,

	MALI_VPU_FW_REGION_PROTECTED,
	MALI_VPU_FW_REGION_FRAMEBUF,
};

/**
 * struct mali_vpu_fw_proto_page - Backing memory and mappings for a protocol page.
 * @linux_page: Original allocated page backing the queue/RPC area.
 * @cpu: Kernel CPU mapping used by host-side queue/RPC access.
 * @dma: DMA address of the queue/RPC page, mapped into the VPU address space.
 * @va: Firmware-visible virtual address used by firmware-side queue/RPC access.
 */
struct mali_vpu_fw_proto_page {
	struct page *linux_page;
	void *cpu;
	dma_addr_t dma;
	u32 va;
};

/**
 * struct mali_vpu_fw - Firmware object
 * @dev: Pointer to device.
 * @mmu: Pointer to MMU object.
 * @msg_host: Host message queue.
 * @msg_mve: MVE message queue.
 * @buf_in_host: Input buffer queue. Host enqueue filled buffers.
 * @buf_in_mve: Input buffer queue. MVE return empty buffers.
 * @buf_out_host: Output buffer queue. Host enqueue empty buffers.
 * @buf_out_mve: Output buffer queue. MVE return filled buffers.
 * @rpc: RPC communication area.
 * @text_pages: Firmware text pages shared by all mapped core instances.
 * @bss_pages: Private BSS pages allocated separately for each core instance.
 * @shared_bss_pages: Shared BSS pages mapped into every core instance.
 * @ncores: Number of firmware core-instance regions constructed.
 * @pending_response_count: Number of requests awaiting a firmware response.
 * @inflight_buffers: XArray mapping non-zero firmware-visible buffer IDs to
 *			buffers currently owned by firmware.
 * @next_buffer_id: Next candidate ID for cyclic buffer-ID allocation.
 * @rpc_mem: Keeps track of RPC allocated memory. Maps MVE virtual address to
 *		'struct mali_vpu_mmu_pages' object.
 */
struct mali_vpu_fw {
	struct device *dev;
	struct mali_vpu_mmu *mmu;

	struct mali_vpu_fw_proto_page msg_host;
	struct mali_vpu_fw_proto_page msg_mve;
	struct mali_vpu_fw_proto_page buf_in_host;
	struct mali_vpu_fw_proto_page buf_in_mve;
	struct mali_vpu_fw_proto_page buf_out_host;
	struct mali_vpu_fw_proto_page buf_out_mve;
	struct mali_vpu_fw_proto_page rpc;

	struct mali_vpu_mmu_pages *text_pages;
	struct mali_vpu_mmu_pages *bss_pages;
	struct mali_vpu_mmu_pages *shared_bss_pages;
	u32 ncores;
	u32 pending_response_count;
	struct xarray inflight_buffers;
	u32 next_buffer_id;

	DECLARE_HASHTABLE(rpc_mem, MALI_VPU_FW_HTABLE_BITS);
};

int mali_vpu_fw_construct(struct mali_vpu_fw *fw, const struct mali_vpu_fw_bin *fw_bin,
			  struct device *dev, struct mali_vpu_mmu *mmu, u32 ncores);
void mali_vpu_fw_cleanup(struct mali_vpu_fw *fw);
int mali_vpu_fw_send_job(struct mali_vpu_fw *fw, u16 cores, u16 frames);
int mali_vpu_fw_send_buffer(struct mali_vpu_fw *fw, struct mali_vpu_fw_msg *fw_msg);
int mali_vpu_fw_send_ping(struct mali_vpu_fw *fw);
int mali_vpu_fw_send_state_change(struct mali_vpu_fw *fw, enum mali_vpu_fw_state state);
int mali_vpu_fw_send_set_option(struct mali_vpu_fw *fw, struct mali_vpu_fw_set_option *option);
int mali_vpu_fw_send_eos(struct mali_vpu_fw *fw);
int mali_vpu_fw_send_flush_src(struct mali_vpu_fw *fw);
int mali_vpu_fw_send_flush_dst(struct mali_vpu_fw *fw);
int mali_vpu_fw_parse_msg(struct mali_vpu_fw *fw, struct mali_vpu_fw_msg *msg);
int mali_vpu_fw_get_region(enum mali_vpu_fw_region region, mali_vpu_mmu_va *begin,
			   mali_vpu_mmu_va *end);
int mali_vpu_fw_handle_rpc(struct mali_vpu_fw *fw, bool *flush_mmu);

#endif /* __MALI_VPU_FIRMWARE_H__ */
