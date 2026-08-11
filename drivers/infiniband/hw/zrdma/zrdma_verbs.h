/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_VERBS_H
#define ZRDMA_VERBS_H

#include <rdma/ib_umem.h>
#include <rdma/uverbs_ioctl.h>

#define CHIP_VERSION 1
#define ZXDH_PAGE_OFFSET_NUM (PAGE_SIZE - 1)
#define MAX_HW_WQ_FRAGS 15
#define MAX_HW_READ_SGES 15
#define MAX_HW_INLINE 217
#define MAX_HW_SRQ_WR 15
#define MAX_HW_RQ_QUANTA 15
#define MAX_HW_SRQ_QUANTA 15
#define MAX_HW_WQ_QUANTA 15
#define MAX_HW_SQ_CHUNK 0x8000
#define MAX_HW_CQ_SIZE 0x200000
#define MIN_HW_CQ_SIZE 4

#define ZXDH_MTU_HEADER_RSV 102

#define ZXDH_PKEY_TBL_SZ 1
#define ZXDH_DEFAULT_PKEY 0xFFFF

#define SQ_DB_BAR_OFF 0x4c000
#define CQ_DB_BAR_OFF 0x43000
#define SRQ_DB_MMAP_SIZE 0x10000
#define SRQ_DB_BAR_OFF 0x62000
#define SRQ_DB_OFFSET 0

#define MAX_MR_SIZE 0x200000000000
#define MAX_QP 0x3fff
#define MAX_QP_WR 0x1000
#define MAX_CQ 0x3fff
#define MAX_CQE 0x200000
#define MAX_MR 0xfffe
#define MAX_MW 0xfffe
#define MAX_PD 0xfffff
#define MAX_SGE_RD 0xf
#define MAX_QP_RD_ATOM 0x10
#define MAX_RES_RD_ATOM 0x3fff0
#define MAX_QP_INIT_RD_ATOM 0x10
#define MAX_SRQ 0x200
#define MAX_SRQ_WR 0x7fff
#define MAX_SRQ_SGE 0xf
#define LOCAL_CA_ACK_DELAY 16
#define HCA_CORE_CLOCK (1000 * 1000UL)
#define MAX_WQ_TYPE_RQ 0x3fff
#define MAX_AH 0x10000
#define MAX_FAST_REG_PAGE_LIST_LEN 0x200
#define MAX_CQ_MODERATION_COUNT 0xffff
#define MAX_CQ_MODERATION_PERIOD 0x1cc
#define TIMESTAMP_MASK 0x1ffff
#define MAX_MSG_SZ 0x100000

enum zxdh_mmap_flag {
	ZXDH_MMAP_IO_NC,
	ZXDH_MMAP_IO_WC,
	ZXDH_MMAP_PFN,
	ZXDH_MMAP_HMC,
	ZXDH_MMAP_IO_NC_BY_SIZE,
};

struct zxdh_ucontext {
	struct ib_ucontext ibucontext;
	struct zxdh_device *zdev;
	struct rdma_user_mmap_entry *sq_db_mmap_entry;
	struct rdma_user_mmap_entry *cq_db_mmap_entry;
	struct rdma_user_mmap_entry *srq_db_mmap_entry;
	struct list_head cq_reg_mem_list;
	spinlock_t cq_reg_mem_list_lock; /* protect CQ memory list */
	struct list_head qp_reg_mem_list;
	spinlock_t qp_reg_mem_list_lock; /* protect QP memory list */
	struct list_head srq_reg_mem_list;
	spinlock_t srq_reg_mem_list_lock; /* protect SRQ memory list */
	int abi_ver;
};

struct zxdh_pd {
	struct ib_pd ibpd;
	struct zxdh_sc_pd sc_pd;
};

struct zxdh_user_mmap_entry {
	struct rdma_user_mmap_entry rdma_entry;
	u64 bar_offset;
	u8 mmap_flag;
};

static inline struct zxdh_pd *to_zpd(struct ib_pd *ibpd)
{
	return container_of(ibpd, struct zxdh_pd, ibpd);
}

static inline struct zxdh_ucontext *to_ucontext(struct ib_ucontext *ibucontext)
{
	return container_of(ibucontext, struct zxdh_ucontext, ibucontext);
}

static inline struct zxdh_user_mmap_entry *
to_zxdh_mmap_entry(struct rdma_user_mmap_entry *rdma_entry)
{
	return container_of(rdma_entry, struct zxdh_user_mmap_entry,
			    rdma_entry);
}

static inline enum ib_mtu zxdh_mtu_int_to_enum(int mtu)
{
	mtu = mtu - ZXDH_MTU_HEADER_RSV;
	if (mtu >= 4096)
		return IB_MTU_4096;
	else if (mtu >= 2048)
		return IB_MTU_2048;
	else if (mtu >= 1024)
		return IB_MTU_1024;
	else if (mtu >= 512)
		return IB_MTU_512;
	else
		return IB_MTU_256;
}

#endif /* ZRDMA_VERBS_H */
