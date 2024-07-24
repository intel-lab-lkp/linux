/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2021, Intel Corporation. */

#ifndef _IDC_RDMA_H_
#define _IDC_RDMA_H_

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>

#define IDC_RDMA_ROCE_NAME	"roce"
#define IDC_RDMA_IWARP_NAME	"iwarp"

enum idc_rdma_reset_type {
	IDC_FUNC_RESET,
	IDC_DEV_RESET,
};

enum idc_rdma_event_type {
	IDC_RDMA_EVENT_BEFORE_MTU_CHANGE,
	IDC_RDMA_EVENT_AFTER_MTU_CHANGE,
	IDC_RDMA_EVENT_BEFORE_TC_CHANGE,
	IDC_RDMA_EVENT_AFTER_TC_CHANGE,
	IDC_RDMA_EVENT_WARN_RESET,
	IDC_RDMA_EVENT_CRIT_ERR,
	IDC_RDMA_EVENT_NBITS,		/* must be last */
};

struct idc_rdma_event {
	DECLARE_BITMAP(type, IDC_RDMA_EVENT_NBITS);
	u32 reg;
};

enum idc_rdma_protocol {
	IDC_RDMA_PROTOCOL_IWARP = BIT(0),
	IDC_RDMA_PROTOCOL_ROCEV2 = BIT(1),
};

struct idc_rdma_qv_info {
	u32 v_idx;
	u16 ceq_idx;
	u16 aeq_idx;
	u8 itr_idx;
};

struct idc_rdma_qvlist_info {
	u32 num_vectors;
	struct idc_rdma_qv_info qv_info[];
};

struct idc_rdma_core_dev_info;

/* Following APIs are implemented by core PCI driver */
struct idc_rdma_core_ops {
	int (*vc_send_sync)(struct idc_rdma_core_dev_info *cdev_info, u8 *msg,
			    u16 len, u8 *recv_msg, u16 *recv_len);
	int (*vc_queue_vec_map_unmap)(struct idc_rdma_core_dev_info *cdev_info,
				      struct idc_rdma_qvlist_info *qvl_info,
				      bool map);
	/* vport_dev_ctrl is for RDMA CORE driver to indicate it is either ready
	 * for individual vport aux devices, or it is leaving the state where it
	 * can support vports and they need to be downed
	 */
	int (*vport_dev_ctrl)(struct idc_rdma_core_dev_info *cdev_info,
			      bool up);
	int (*request_reset)(struct idc_rdma_core_dev_info *cdev_info,
			     enum idc_rdma_reset_type reset_type);
};

enum idc_function_type {
	IDC_FUNCTION_TYPE_PF,
	IDC_FUNCTION_TYPE_VF,
};

struct idc_rdma_lan_mapped_mem_region {
	u8 __iomem *region_addr;
	__le64 size;
	__le64 start_offset;
};

/* struct to be populated by core LAN PCI driver */
struct idc_rdma_core_dev_info {
	struct pci_dev *pdev; /* PCI device of corresponding to main function */
	struct auxiliary_device *adev;
	struct idc_rdma_lan_mapped_mem_region *mapped_mem_regions;
	__le16 num_memory_regions;
	/* Current active RDMA protocol */
	enum idc_rdma_protocol rdma_protocol;
	enum idc_function_type ftype;
	struct msix_entry *msix_entries;
	u16 msix_count; /* How many vectors are reserved for this device */
	/* Following struct contains function pointers to be initialized
	 * by core PCI driver and called by auxiliary driver
	 */
	const struct idc_rdma_core_ops *ops;
	void *idc_priv;
};

struct idc_rdma_core_auxiliary_dev {
	struct auxiliary_device adev;
	struct idc_rdma_core_dev_info *cdev_info;
};

/* struct to be populated by core LAN PCI driver */
struct idc_rdma_vport_dev_info {
	struct auxiliary_device *adev;
	struct auxiliary_device *core_adev;
	struct net_device *netdev;
	u16 vport_id;
};

struct idc_rdma_vport_auxiliary_dev {
	struct auxiliary_device adev;
	struct idc_rdma_vport_dev_info *vdev_info;
};

/* structures representing the auxiliary drivers. These structs are to be
 * allocated and populated by the auxiliary drivers' owner. The core PCI
 * driver will access these ops by performing a container_of on the
 * auxiliary_device->dev.driver.
 */
struct idc_rdma_core_auxiliary_drv {
	struct auxiliary_driver adrv;
	void (*event_handler)(struct idc_rdma_core_dev_info *cdev,
			      struct idc_rdma_event *event);
	int (*vc_receive)(struct idc_rdma_core_dev_info *cdev_info, u8 *msg,
			  u16 len);
};

struct idc_rdma_vport_auxiliary_drv {
	struct auxiliary_driver adrv;
	void (*event_handler)(struct idc_rdma_vport_dev_info *vdev,
			      struct idc_rdma_event *event);
};

#endif /* _IDC_RDMA_H_*/
