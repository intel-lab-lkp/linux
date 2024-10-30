/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_HWDEV_H
#define HINIC3_HWDEV_H

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/auxiliary_bus.h>

#include "hinic3_hwif.h"
#include "hinic3_hw_cfg.h"
#include "hinic3_hw_intf.h"

struct hinic3_aeqs;
struct hinic3_ceqs;
struct hinic3_mbox;
struct hinic3_msg_pf_to_mgmt;
struct hinic3_cmdqs;

enum hinic3_event_service_type {
	EVENT_SRV_COMM = 0,
#define SERVICE_EVENT_BASE    (EVENT_SRV_COMM + 1)
	EVENT_SRV_NIC  = SERVICE_EVENT_BASE + SERVICE_T_NIC,
};

#define HINIC3_SRV_EVENT_TYPE(svc, type)    ((((u32)(svc)) << 16) | (type))

enum hinic3_comm_event_type {
	EVENT_COMM_PCIE_LINK_DOWN,
	EVENT_COMM_HEART_LOST,
	EVENT_COMM_FAULT,
	EVENT_COMM_SRIOV_STATE_CHANGE,
	EVENT_COMM_CARD_REMOVE,
	EVENT_COMM_MGMT_WATCHDOG,
};

enum hinic3_fault_source_type {
	HINIC3_FAULT_SRC_HW_PHY_FAULT = 9,
	HINIC3_FAULT_SRC_TX_TIMEOUT   = 22,
};

/* driver-specific data of pci_dev */
struct hinic3_pcidev {
	struct pci_dev       *pdev;
	struct pci_device_id id;
	struct hinic3_hwdev  *hwdev;
	/* Auxiliary devices */
	struct hinic3_adev   *hadev[SERVICE_T_MAX];

	void __iomem         *cfg_reg_base;
	void __iomem         *intr_reg_base;
	void __iomem         *mgmt_reg_base;
	void __iomem         *db_base;
	u64                  db_dwqe_len;
	u64                  db_base_phy;

	/* lock for attach/detach uld */
	struct mutex         pdev_mutex;

	unsigned long        state;
	u16                  probe_fault_level;
};

struct hinic3_hwdev {
	struct hinic3_pcidev           *adapter;
	struct pci_dev                 *pdev;
	struct device                  *dev;
	int                            dev_id;

	struct hinic3_hwif             *hwif;
	struct cfg_mgmt_info           *cfg_mgmt;
	struct hinic3_aeqs             *aeqs;
	struct hinic3_ceqs             *ceqs;
	struct hinic3_mbox             *mbox;
	struct hinic3_msg_pf_to_mgmt   *pf_to_mgmt;
	struct hinic3_cmdqs            *cmdqs;

	struct delayed_work            sync_time_task;
	struct workqueue_struct        *workq;
	/* protect channel init and deinit */
	spinlock_t                     channel_lock;

	/* use polling mode or interrupt mode */
	bool                           poll;
	u64                            features[COMM_MAX_FEATURE_QWORD];
	u32                            wq_page_size;
	u8                             max_pf;
	u8                             max_cmdq;

	ulong                          func_state;
	int                            chip_present_flag;
	u16                            probe_fault_level;
};

struct hinic3_event_info {
	/* enum hinic3_event_service_type */
	u16 service;
	u16 type;
	u8  event_data[104];
};

struct hinic3_adev {
	struct auxiliary_device  adev;
	struct hinic3_hwdev      *hwdev;
	enum hinic3_service_type svc_type;

	void (*event)(struct auxiliary_device *adev,
		      struct hinic3_event_info *event);
};

int hinic3_init_hwdev(struct pci_dev *pdev);
void hinic3_free_hwdev(struct hinic3_hwdev *hwdev);

void hinic3_set_api_stop(struct hinic3_hwdev *hwdev);

#endif
