/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_MAIN_H
#define ZRDMA_MAIN_H

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>
#include <net/addrconf.h>

#include "zrdma_type.h"
#include "zrdma_ctrl.h"
#include "zrdma_abi.h"
#include "zrdma_defs.h"

#define ZXDH_PF_NAME "dinghai10e"
#define ZXDH_RDMA_DEV_NAME "rdma_aux"

#define ZXDH_FUNC_TYPE(vport_id) (((vport_id) >> 11) & 0x1)
#define ZXDH_PF_ID(vport_id) (((vport_id) >> 8) & 0x7)
#define ZXDH_EP_ID(vport_id) (((vport_id) >> 12) & 0x7)

#define PCI_SBDF_SAFE_LEN 64
#define ZXDH_RST_TIMEOUT_HZ 4

#define ZXDH_RDMA_COMMON_FUNC_CAP 1
#define ZXDH_RDMA_COMM_FUNC BIT_ULL(24)

extern struct list_head zxdh_handlers;
extern spinlock_t zxdh_handler_lock;

struct zxdh_pci_f;

struct zxdh_rdma_node {
	char name[IB_DEVICE_NAME_MAX];
	char sbdf[PCI_SBDF_SAFE_LEN];
	struct list_head list;
};

enum zxdh_func_type {
	ZXDH_FUNC_NUM_REQUIRE = 0,
	ZXDH_FUNC_NP_MAC = 1,
	ZXDH_FUNC_IRQ_REQUEST = 2,
	ZXDH_FUNC_IRQ_FREE = 3,
	ZXDH_FUNC_PMTU_INFO_ADD = 4,
	ZXDH_FUNC_PMTU_INFO_DEL = 5,
	ZXDH_FUNC_QP_INFO_DEL_WX = 6,
	ZXDH_FUNC_NETDEV_SPEED_GET = 7,
	ZXDH_FUNC_SEARCH_MAC_FROM_FW = 8,
	ZXDH_COMM_FUNC_GET_TRUST_TYPE = 9,
	ZXDH_FUNC_VF_VLAN_GET = 10,
	ZXDH_FUNC_NUM_MAX,
};

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

enum zxdh_rdma_vers {
	ZXDH_GEN_RSVD,
	ZXDH_GEN_1,
	ZXDH_GEN_2,
};

struct zxdh_handler {
	struct list_head list;
	struct zxdh_device *zdev;
};

struct zxdh_gen_ops {
	void (*request_reset)(struct zxdh_pci_f *rf);
	int (*zxdh_common_func)(void *in_param, void *out_param, u32 opcode);
};

struct zxdh_pci_f {
	u8 ftype : 1;
	u8 *mem_rsrc;
	u8 rdma_ver;
	u8 rst_to;
	u8 pf_id;
	u8 ep_id;
	u32 msix_count;
	u32 max_mr;
	u32 max_qp;
	u32 max_cq;
	u32 max_ah;
	u32 max_mcg;
	u32 max_pd;
	u32 next_pd;
	u32 max_cqe;
	u32 max_srq;
	u32 max_pri;
	u16 max_8k_idx;
	u32 *qp_cnt_8k_idxs;
	u32 ceqs_count;

	unsigned long *allocated_qps;
	unsigned long *allocated_cqs;
	unsigned long *allocated_mrs;
	unsigned long *allocated_pds;
	unsigned long *allocated_mcgs;
	unsigned long *allocated_ahs;
	unsigned long *allocated_srqs;
	unsigned long *allocated_pris;
	unsigned long *allocated_8k_idx;

	struct zxdh_sc_dev sc_dev;
	struct pci_dev *pcidev;
	struct zxdh_rdma_core_dev *zdev_info;
	struct zxdh_hw hw;

	spinlock_t rsrc_lock; /* protects resource allocation arrays */
	spinlock_t qptable_lock; /* protects qp_table access */
	spinlock_t cqtable_lock; /* protects cq_table access */
	struct zxdh_qp **qp_table;
	struct zxdh_cq **cq_table;
	spinlock_t srqtable_lock; /* protects srq_table access */
	struct zxdh_srq **srq_table;

	struct msix_entry *msix_entries;
	struct zxdh_gen_ops gen_ops;
	struct zxdh_device *zdev;
	u16 pcie_id;
	u16 qp_index;
	u32 srq_mem_size;
	bool drv_np_cap;
	bool net_irq_cap;
	int common_func_num_max;
	void *dh_dev;
};

struct zxdh_device {
	struct ib_device ibdev;
	struct zxdh_pci_f *rf;
	struct net_device *netdev;
	struct net_device *source_netdev;
	struct zxdh_handler *hdl;
	struct list_head ah_list;
	struct mutex ah_list_lock; /* protects ah_list operations */
	u8 ibdev_status;
	u8 roce_mode : 1;
	enum init_completion_state init_state;
	u32 netdev_speed;
	struct zxdh_auxiliary_dev *zxdh_adev;
};

static inline struct zxdh_device *to_zdev(struct ib_device *ibdev)
{
	return container_of(ibdev, struct zxdh_device, ibdev);
}

static inline int zxdh_alloc_rsrc(struct zxdh_pci_f *rf,
				  unsigned long *rsrc_array, u32 max_rsrc,
				  u32 *req_rsrc_num, u32 *next)
{
	unsigned long flags;
	u32 rsrc_num;

	spin_lock_irqsave(&rf->rsrc_lock, flags);
	rsrc_num = find_next_zero_bit(rsrc_array, max_rsrc, *next);
	if (rsrc_num >= max_rsrc) {
		rsrc_num = find_first_zero_bit(rsrc_array, max_rsrc);
		if (rsrc_num >= max_rsrc) {
			spin_unlock_irqrestore(&rf->rsrc_lock, flags);
			pr_err("zrdma: resource [%d] allocation failed\n",
			       rsrc_num);
			return -EOVERFLOW;
		}
	}
	__set_bit(rsrc_num, rsrc_array);
	*next = rsrc_num + 1;
	if (*next == max_rsrc)
		*next = 0;
	*req_rsrc_num = rsrc_num;
	spin_unlock_irqrestore(&rf->rsrc_lock, flags);

	return 0;
}

static inline void zxdh_free_rsrc(struct zxdh_pci_f *rf,
				  unsigned long *rsrc_array, u32 rsrc_num)
{
	unsigned long flags;

	spin_lock_irqsave(&rf->rsrc_lock, flags);
	__clear_bit(rsrc_num, rsrc_array);
	spin_unlock_irqrestore(&rf->rsrc_lock, flags);
}

int zxdh_get_del_rdma_name(const char *sbdf, char *name);
int zxdh_ctrl_init_hw(struct zxdh_pci_f *rf);
int zxdh_ib_register_device(struct zxdh_device *zdev);
void zxdh_del_handler(struct zxdh_handler *hdl);

#endif /* ZRDMA_MAIN_H */
