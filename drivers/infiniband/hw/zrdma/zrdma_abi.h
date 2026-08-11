/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_ABI_H
#define ZRDMA_ABI_H

/* user-space whose last ABI ver is 5 */
#define ZXDH_ABI_VER 5
#define ZXDH_CONTEXT_VER_V1 5
#define ZXDH_CONTEXT_VER_V2 6
#define ZXDH_CONTEXT_VER_V3 7

enum zxdh_rdma_tool_flags {
	ZXDH_QP_EXTEND_OP = 1 << 0,
	ZXDH_CAPTURE = 1 << 1,
	ZXDH_GET_HW_DATA = 1 << 2,
	ZXDH_GET_HW_OBJECT_DATA = 1 << 3,
	ZXDH_CHECK_HW_HEALTH = 1 << 4,
	ZXDH_RDMA_TOOL_CFG_DEV_PARAM = 1 << 5,
	ZXDH_RDMA_TOOL_SHOW_RES_MAP = 1 << 5,
	ZXDH_RDMA_TOOL_READ_RAM = 1 << 6,
	ZXDH_RDMA_TOOL_DEVX_MODIFY_CQ = 1 << 7,
	ZXDH_RDMA_SET_CREDIT_CAP = 1 << 8,
	ZXDH_RDMA_SWITCH = 1 << 9,
	ZXDH_RDMA_TOOL_DEVX_EXT_MEM = 1 << 10,
	ZXDH_RDMA_TOOL_PRIV_EXT = 1 << 11,
};

enum zxdh_db_addr_type {
	ZXDH_DB_ADDR_PHY = 0,
	ZXDH_DB_ADDR_BAR = 1,
};

struct zxdh_alloc_pd_resp {
	__u32 pd_id;
	__u8 rsvd[4];
};

struct zxdh_alloc_ucontext_req {
	__u32 rsvd32;
	__u8 userspace_ver;
	__u8 rsvd8[3];
};

struct zxdh_alloc_ucontext_resp {
	__u32 max_pds;
	__u32 max_qps;
	__u32 wq_size; /* size of the WQs (SQ+RQ) in the mmaped area */
	__u8 kernel_ver;
	__u8 db_addr_type;
	__u16 rdma_tool_flags;
	__aligned_u64 feature_flags;
	__aligned_u64 sq_db_mmap_key;
	__aligned_u64 cq_db_mmap_key;
	__u32 max_hw_wq_frags;
	__u32 max_hw_read_sges;
	__u32 max_hw_inline;
	__u32 max_hw_rq_quanta;
	__u32 max_hw_srq_quanta;
	__u32 max_hw_wq_quanta;
	__u32 max_hw_srq_wr;
	__u32 min_hw_cq_size;
	__u32 max_hw_cq_size;
	__u16 max_hw_sq_chunk;
	__u8 hw_rev;
	__u8 chip_rev;
	__aligned_u64 srq_db_mmap_key;
	__aligned_u64 sq_db_bar_off;
	__aligned_u64 cq_db_bar_off;
	__aligned_u64 srq_db_bar_off;
	__u32 srq_db_mmap_size;
	__u16 rsv[6];
};

#endif /* ZRDMA_ABI_H */
