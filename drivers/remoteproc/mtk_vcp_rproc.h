/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (c) 2025 MediaTek Inc.
 */

#ifndef __MTK_VCP_RPROC_H__
#define __MTK_VCP_RPROC_H__

#include <linux/remoteproc/mtk_vcp_public.h>

/*
 * struct mtk_vcp_of_cluster - vcp cluster priv data.
 *
 * @sram_base: sram_base get from dtb
 * @cfg: cfg register get from dtb
 * @cfg_sec: cfg_sec register get from dtb
 * @cfg_core: cfg_core register get from dtb
 * @sram_size: total sram size get from dtb
 * @core_nums: total core numbers get from dtb
 * @hart_count: number of hardware threads (harts) per core
 * @sram_offset: core sram memory layout
 * @msg_vcp_ready0: core0 ready ipi msg data
 * @msg_vcp_ready1: core1 ready ipi msg data
 * @slp_ipi_ack_data: sleep ipi msg data
 * @feature_enable: feature status count data
 * @is_suspending: suspend status flag
 * @vcp_ready: vcp core status flag
 * @share_mem_iova: shared memory iova base
 * @share_mem_size: shared memory size
 * @vcp_feature_mutex: vcp feature register mutex structure
 * @vcp_ready_mutex: vcp core ready mutex structure
 * @vcp_ipidev: struct mtk_ipi_device
 * @vcp_workqueue: ready workqueue_struct
 * @vcp_memory_tb: vcp memory allocated table
 * @vcp_ready_notify_wk: vcp_work_struct structure
 */
struct mtk_vcp_of_cluster {
	void __iomem *sram_base;
	void __iomem *cfg;
	void __iomem *cfg_sec;
	void __iomem *cfg_core;
	u32 sram_size;
	u32 core_nums;
	u32 hart_count[VCP_CORE_TOTAL];
	u32 sram_offset[VCP_CORE_TOTAL];
	u32 msg_vcp_ready0;
	u32 msg_vcp_ready1;
	u32 slp_ipi_ack_data;
	bool feature_enable[NUM_FEATURE_ID];
	bool is_suspending;
	bool vcp_ready[VCP_CORE_TOTAL];
	dma_addr_t share_mem_iova;
	size_t share_mem_size;
	struct mutex vcp_feature_mutex;
	struct mutex vcp_ready_mutex;
	struct mtk_ipi_device vcp_ipidev;
	struct workqueue_struct *vcp_workqueue;
	struct vcp_reserve_mblock vcp_memory_tb[NUMS_MEM_ID];
	struct vcp_work_struct vcp_ready_notify_wk[VCP_CORE_TOTAL];
};

/**
 * struct mtk_vcp_platdata - vcp platform priv data.
 *
 * @auto_boot: rproc auto_boot flag
 * @sysfs_read_only: rproc sysfs_read_only flag
 * @rtos_static_iova: vcp dram binary static map iova
 * @mtk_mbox_table: mtk_mbox_table structure
 * @mtk_vcp_ipi_ops: vcp ipi api ops structure
 * @feature_tb: vcp feature table structure
 * @memory_tb: vcp memory table structure
 * @fw_name: vcp image name and path
 */
struct mtk_vcp_platdata {
	bool auto_boot;
	bool sysfs_read_only;
	dma_addr_t rtos_static_iova;
	struct mtk_mbox_table *ipc_data;
	struct mtk_vcp_ipi_ops *ipi_ops;
	struct mtk_vcp_feature_table *feature_tb;
	struct mtk_vcp_reserved_mem_table *memory_tb;
	char *fw_name;
};

/**
 * struct mtk_vcp_of_data - const vcp device data.
 *
 * @mtk_vcp_ops: mtk_vcp_ops structure
 * @mtk_vcp_platdata: mtk_vcp_platdata structure
 */
struct mtk_vcp_of_data {
	const struct mtk_vcp_ops ops;
	const struct mtk_vcp_platdata platdata;
};
#endif
