/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_TYPE_H
#define ZRDMA_TYPE_H

#include <linux/msi.h>

enum zxdh_rdma_reset_type {
	ZXDH_RDMA_PFR,
	ZXDH_RDMA_CORER,
	ZXDH_RDMA_GLOBR,
};

enum zxdh_function_type {
	ZXDH_FUNCTION_TYPE_PF,
	ZXDH_FUNCTION_TYPE_VF,
};

struct zxdh_rdma_core_dev;

struct zxdh_rdma_core_ops {
	int (*request_reset)(struct zxdh_rdma_core_dev *cdev,
			     enum zxdh_rdma_reset_type reset_type);
	int (*zxdh_common_func)(void *in_param, void *out_param, u32 opcode);
};

struct zxdh_rdma_ver_info {
	u16 major;
	u16 minor;
	u64 support; /* 0~7:net_major,8~15:net_minor,16~23:rdma_minor,24~63:rscv */
};

enum zxdh_rdma_protocol {
	ZXDH_RDMA_PROTOCOL_IWARP = BIT(0),
	ZXDH_RDMA_PROTOCOL_ROCEV2 = BIT(1),
};

struct zxdh_qos_params {
	u8 num_tc;
};

struct zxdh_rdma_core_dev {
	struct pci_dev *pdev;
	struct auxiliary_device *adev;
	u8 __iomem *hw_addr;
	int zdev_info_id;
	struct zxdh_rdma_ver_info ver;
	void *auxiliary_priv;
	enum zxdh_function_type ftype;
	u16 vport_id;
	u16 slot_id;
	enum zxdh_rdma_protocol rdma_protocol;

	struct zxdh_qos_params qos_info;
	struct msix_entry *msix_entries;
	u16 msix_count;
	const struct zxdh_rdma_core_ops *ops;
	void *dh_dev;
};

struct zxdh_rdma_if {
	void *(*get_rdma_netdev)(void *dh_dev);
};

struct zxdh_auxiliary_dev {
	struct auxiliary_device adev;
	struct zxdh_rdma_core_dev *zxdh_info;
	struct zxdh_rdma_if *rdma_ops;
	void *ops;
	void *parent;
	s32 aux_id;
	void *auxiliary_ops[18];
};

struct zxdh_sc_pd {
	struct zxdh_sc_dev *dev;
	u32 pd_id;
	int abi_ver;
};

#endif /* ZRDMA_TYPE_H */
