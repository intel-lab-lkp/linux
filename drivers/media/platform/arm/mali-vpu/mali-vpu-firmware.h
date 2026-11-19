/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_FIRMWARE_H__
#define __MALI_VPU_FIRMWARE_H__

#include <linux/types.h>

#define MALI_VPU_FW_TEXT_BASE_ADDR 0x1000u

struct device;
struct mali_vpu_mmu;
struct mali_vpu_fw_bin;
struct mali_vpu_mmu_pages;

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
 * @MALI_VPU_FW_CODE_PING: Ping request sent to firmware.
 * @MALI_VPU_FW_CODE_PONG: Firmware responded to a ping request.
 * @MALI_VPU_FW_CODE_IDLE: Firmware informs the driver that it's idle.
 * @MALI_VPU_FW_CODE_STATE_CHANGE: Firmware state-change request or response.
 * @MALI_VPU_FW_CODE_SWITCH_IN: Firmware was booted on a Core in a session.
 * @MALI_VPU_FW_CODE_SWITCH_OUT: The last core in a session was switched out.
 * @MALI_VPU_FW_CODE_UNKNOWN: Unrecognized firmware message.
 * @MALI_VPU_FW_CODE_MAX: Sentinel marking the end of the message-code enum.
 */
enum mali_vpu_fw_code {
	MALI_VPU_FW_CODE_ERROR,        /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_JOB,          /* Driver -> Firmware. */
	MALI_VPU_FW_CODE_PING,         /* Driver -> Firmware. */
	MALI_VPU_FW_CODE_PONG,         /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_IDLE,         /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_STATE_CHANGE, /* Driver <-> Firmware. */
	MALI_VPU_FW_CODE_SWITCH_IN,    /* Driver <- Firmware. */
	MALI_VPU_FW_CODE_SWITCH_OUT,   /* Driver <-> Firmware. */
	MALI_VPU_FW_CODE_UNKNOWN,      /* Driver <- Firmware. */
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
};

int mali_vpu_fw_construct(struct mali_vpu_fw *fw, const struct mali_vpu_fw_bin *fw_bin,
			  struct device *dev, struct mali_vpu_mmu *mmu, u32 ncores);
void mali_vpu_fw_cleanup(struct mali_vpu_fw *fw);
int mali_vpu_fw_send_job(struct mali_vpu_fw *fw, u16 cores, u16 frames);
int mali_vpu_fw_send_ping(struct mali_vpu_fw *fw);
int mali_vpu_fw_send_state_change(struct mali_vpu_fw *fw, enum mali_vpu_fw_state state);
int mali_vpu_fw_parse_msg(struct mali_vpu_fw *fw, struct mali_vpu_fw_msg *msg);

#endif /* __MALI_VPU_FIRMWARE_H__ */
