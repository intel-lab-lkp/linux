/* SPDX-License-Identifier: GPL-2.0-only
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_MAIN_H
#define ZRDMA_MAIN_H

#include <linux/auxiliary_bus.h>
#include "zrdma_type.h"
#include "zrdma_ctrl.h"

#define ZXDH_PF_NAME "dinghai10e"
#define ZXDH_RDMA_DEV_NAME "rdma_aux"

#define ZXDH_FUNC_TYPE(vport_id) (((vport_id) >> 11) & 0x1)
#define ZXDH_PF_ID(vport_id) (((vport_id) >> 8) & 0x7)
#define ZXDH_EP_ID(vport_id) (((vport_id) >> 12) & 0x7)

enum init_completion_state {
	INVALID_STATE = 0,
	INITIAL_STATE,
	CQP_CREATED,
	SMMU_PAGETABLE_INITIALIZED,
	DATA_CAP_CREATED,
	HMC_OBJS_CREATED,
	HW_RSRC_INITIALIZED,
	CQP_QP_CREATED,
	AEQ_CREATED,
	CCQ_CREATED,
	CEQ0_CREATED,
	CEQS_CREATED,
	PBLE_CHUNK_MEM,
};

struct zxdh_fw_compat {
	u8 module_id;
	u8 major;
	u8 fw_minor;
	u8 drv_minor;
	u16 patch;
	u16 rsv;
} __packed;

struct zxdh_handler {
	struct list_head list;
	struct zxdh_device *zdev;
};

struct zxdh_pci_f {
	u8 reset : 1;
	u8 rsrc_created : 1;
	u8 ftype : 1;
	u8 max_rdma_vfs;
	u8 *hmc_info_mem;
	u8 *mem_rsrc;
	u8 pf_id;
	u8 vf_id;
	u8 ep_id;
	u32 msix_count;
	u32 max_mr;
	u32 max_qp;
	u32 max_cq;
	u32 max_ah;
	u32 next_ah;
	u32 max_pd;
	u32 next_qp;
	u32 next_cq;
	u32 next_pd;
	u32 next_mr;
	u32 max_mr_size;
	u32 max_cqe;
	u32 used_pds;
	u32 used_cqs;
	u32 used_mrs;
	u32 used_qps;
	u32 max_srq;
	u32 next_srq;
	u32 used_srqs;
	u32 max_pri;
	u16 max_8k_idx;
	u32 *qp_cnt_8k_idxs;
	u32 ceqs_count;
	u64 base_bar_offset;

	unsigned long *allocated_qps;
	unsigned long *allocated_cqs;
	unsigned long *allocated_mrs;
	unsigned long *allocated_pds;
	unsigned long *allocated_mcgs;
	unsigned long *allocated_ahs;
	unsigned long *allocated_srqs;
	unsigned long *allocated_pris;
	unsigned long *allocated_8k_idx;

	enum init_completion_state init_state;
	struct zxdh_sc_dev sc_dev;
	struct zxdh_handler *hdl;
	struct pci_dev *pcidev;
	struct zxdh_core_dev_info *zdev_info;
	struct zxdh_hw hw;
	struct zxdh_cqp cqp;
	struct zxdh_ccq ccq;
	struct zxdh_dma_mem cqp_host_ctx;
	spinlock_t rsrc_lock; /* protect HardWare resource array access */
	struct workqueue_struct *cqp_cmpl_wq;
	struct work_struct cqp_cmpl_work;
	struct zxdh_device *zdev;
	u8 mcode_type;
	u16 pcie_id;
	void *dh_dev;
};

struct zxdh_device {
	struct ib_device ibdev;
	const struct uverbs_object_tree_def *driver_trees[6];
	struct zxdh_pci_f *rf;
	struct net_device *netdev;
	struct net_device *source_netdev;
	struct zxdh_handler *hdl;
	struct workqueue_struct *cleanup_wq;
	struct list_head ah_list;
	struct mutex ah_list_lock; /* protects ah_list operations */
	u32 ah_list_cnt;
	u32 ah_list_hwm;
	u32 vendor_id;
	u32 vendor_part_id;
	u32 device_cap_flags;
	enum init_completion_state init_state;
	wait_queue_head_t suspend_wq;
	u32 netdev_speed;
	struct zxdh_fw_compat fw_ver;
	struct zxdh_auxiliary_dev *zxdh_adev;
};

int zxdh_ctrl_init_hw(struct zxdh_pci_f *rf);

#endif /* ZRDMA_MAIN_H */
