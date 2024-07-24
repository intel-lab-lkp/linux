/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2021, Intel Corporation. */

#ifndef _IIDC_RDMA_H_
#define _IIDC_RDMA_H_

#include <linux/dcbnl.h>

#define IIDC_MAX_USER_PRIORITY		8
#define IIDC_MAX_DSCP_MAPPING		64
#define IIDC_DSCP_PFC_MODE		0x1

/* Struct to hold per RDMA Qset info */
struct iidc_rdma_qset_params {
	/* Qset TEID returned to the RDMA driver in
	 * ice_add_rdma_qset and used by RDMA driver
	 * for calls to ice_del_rdma_qset
	 */
	u32 teid;	/* Qset TEID */
	u16 qs_handle; /* RDMA driver provides this */
	u16 vport_id; /* VSI index */
	u8 tc; /* TC branch the Qset should belong to */
};

struct iidc_rdma_qos_info {
	u64 tc_ctx;
	u8 rel_bw;
	u8 prio_type;
	u8 egress_virt_up;
	u8 ingress_virt_up;
};

/* Struct to pass QoS info */
struct iidc_rdma_qos_params {
	struct iidc_rdma_qos_info tc_info[IEEE_8021QAZ_MAX_TCS];
	u8 up2tc[IIDC_MAX_USER_PRIORITY];
	u8 vport_relative_bw;
	u8 vport_priority_type;
	u8 num_tc;
	u8 pfc_mode;
	u8 dscp_map[IIDC_MAX_DSCP_MAPPING];
};

struct iidc_rdma_priv_ops {
	int (*alloc_res)(struct idc_rdma_core_dev_info *cdev_info,
			 struct iidc_rdma_qset_params *qset);
	int (*free_res)(struct idc_rdma_core_dev_info *cdev_info,
			struct iidc_rdma_qset_params *qset);
	int (*update_vport_filter)(struct idc_rdma_core_dev_info *cdev_info,
				   u16 vport_id, bool enable);
};

struct iidc_rdma_priv_dev_info {
	u8 pf_id;
	u16 vport_id;
	struct net_device *netdev;
	struct iidc_rdma_qos_params qos_info;
	const struct iidc_rdma_priv_ops *priv_ops;
	u8 __iomem *hw_addr;
};
#endif /* _IDC_RDMA_H_*/
