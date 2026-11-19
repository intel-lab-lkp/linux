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

#endif /* __MALI_VPU_FIRMWARE_H__ */
