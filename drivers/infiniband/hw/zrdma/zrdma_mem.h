/* SPDX-License-Identifier: GPL-2.0-only
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_MEM_H
#define ZRDMA_MEM_H

#define ZXDH_TABLE5_VF_EN 0x04

#define ZXDH_HMC_MAX_SD_COUNT 8192

enum zxdh_indicate_id {
	ZXDH_INDICATE_L2D = 0,
	ZXDH_INDICATE_DPU_DDR = ZXDH_INDICATE_L2D,
	ZXDH_INDICATE_REGISTER = ZXDH_INDICATE_L2D,
	ZXDH_INDICATE_RESERVED = 1,
	ZXDH_INDICATE_HOST_NOSMMU = 2,
	ZXDH_INDICATE_HOST_SMMU = 3,
};

enum zxdh_axid_type {
	ZXDH_AXID_L2D,
	ZXDH_AXID_DPUDDR,
	ZXDH_AXID_HOST_EP0,
	ZXDH_AXID_HOST_EP1,
	ZXDH_AXID_HOST_EP2,
	ZXDH_AXID_HOST_EP3,
	ZXDH_AXID_HOST_EP4,
};

enum zxdh_object_id {
	ZXDH_PBLE_MR_OBJ_ID = 0,
	ZXDH_PBLE_QUEUE_OBJ_ID = 1,
	ZXDH_MR_OBJ_ID = 2,
	ZXDH_AH_OBJ_ID = 3,
	ZXDH_IRD_OBJ_ID = 4,
	ZXDH_TX_WINDOW_OBJ_ID = 5,
	ZXDH_SRQC_OBJ_ID = 6,
	ZXDH_CQC_OBJ_ID = 7,
	ZXDH_MG_PAYLOAD_OBJ_ID = 8,
	ZXDH_MG_OBJ_ID = 9,
	ZXDH_RW_PAYLOAD = 10,
	ZXDH_SQ = 11,
	ZXDH_SQ_SHADOW_AREA = 12,
	ZXDH_RQ = 13,
	ZXDH_RQ_SHADOW_AREA = 14,
	ZXDH_SRQP = 15,
	ZXDH_SRQ = 16,
	ZXDH_SRQ_SHADOW_AREA = 17,
	ZXDH_CQ = 18,
	ZXDH_CQ_SHADOW_AREA = 19,
	ZXDH_CEQ = 20,
	ZXDH_AEQ = 21,
	ZXDH_MG_QPN = 22,
	ZXDH_CPU_DDR = 24,
	ZXDH_QPC_OBJ_ID = 29,
	ZXDH_DMA_OBJ_ID = 30,
	ZXDH_L2D_OBJ_ID = 31,
	ZXDH_REG_OBJ_ID = ZXDH_L2D_OBJ_ID,
};

struct zxdh_dma_mem {
	void *va;
	dma_addr_t pa;
	u32 size;
} __packed;

struct zxdh_virt_mem {
	void *va;
	u32 size;
} __packed;

struct zxdh_hmc_sd_entry {
	bool valid;
	struct zxdh_dma_mem addr;
	struct zxdh_dma_mem addr_hardware;
};

struct zxdh_hmc_sd_table {
	struct zxdh_virt_mem addr;
	u32 sd_cnt;
	struct zxdh_hmc_sd_entry *sd_entry;
};

struct zxdh_hmc_info {
	u32 signature;
	u8 hmc_fn_id;
	u32 pble_hmc_index;
	u32 pble_mr_hmc_index;
	u32 hmc_entry_total;
	u32 hmc_first_entry_pble;
	u32 hmc_first_entry_pble_mr;
	struct zxdh_hmc_obj_info *hmc_obj;
	struct zxdh_virt_mem hmc_obj_virt_mem;
	struct zxdh_hmc_sd_table sd_table;
	u16 sd_indexes[ZXDH_HMC_MAX_SD_COUNT];
	u8 pble_mr_cachid;
	u8 pble_ird_cachid;
};

#endif
