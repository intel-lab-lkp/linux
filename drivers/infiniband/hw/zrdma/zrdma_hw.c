// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#include <linux/vmalloc.h>
#include "zrdma_hw.h"
#include "zrdma_ctrl.h"
#include "zrdma_main.h"

static void zxdh_set_hw_rsrc(struct zxdh_pci_f *rf)
{
	rf->allocated_srqs = (void *)(rf->mem_rsrc);
	rf->allocated_qps = &rf->allocated_srqs[BITS_TO_LONGS(rf->max_srq)];
	rf->allocated_cqs = &rf->allocated_qps[BITS_TO_LONGS(rf->max_qp)];
	rf->allocated_mrs = &rf->allocated_cqs[BITS_TO_LONGS(rf->max_cq)];
	rf->allocated_pds = &rf->allocated_mrs[BITS_TO_LONGS(rf->max_mr)];
	rf->allocated_ahs = &rf->allocated_pds[BITS_TO_LONGS(rf->max_pd)];
	rf->allocated_mcgs = &rf->allocated_ahs[BITS_TO_LONGS(rf->max_ah)];
	rf->allocated_pris = &rf->allocated_mcgs[BITS_TO_LONGS(rf->max_mcg)];
	rf->allocated_8k_idx = &rf->allocated_pris[BITS_TO_LONGS(rf->max_pri)];
	rf->qp_table = (struct zxdh_qp **)
				   (&rf->allocated_8k_idx[BITS_TO_LONGS(rf->max_8k_idx)]);
	rf->cq_table = (struct zxdh_cq **)(&rf->qp_table[rf->max_qp]);
	rf->srq_table = (struct zxdh_srq **)(&rf->cq_table[rf->max_cq]);

	spin_lock_init(&rf->rsrc_lock);
	spin_lock_init(&rf->qptable_lock);
	spin_lock_init(&rf->cqtable_lock);
	spin_lock_init(&rf->srqtable_lock);
}

static u32 zxdh_calc_mem_rsrc_size(struct zxdh_pci_f *rf)
{
	u32 rsrc_size;

	rsrc_size = sizeof(unsigned long) * BITS_TO_LONGS(rf->max_srq);
	rsrc_size += sizeof(unsigned long) * BITS_TO_LONGS(rf->max_qp);
	rsrc_size += sizeof(unsigned long) * BITS_TO_LONGS(rf->max_mr);
	rsrc_size += sizeof(unsigned long) * BITS_TO_LONGS(rf->max_cq);
	rsrc_size += sizeof(unsigned long) * BITS_TO_LONGS(rf->max_pd);
	rsrc_size += sizeof(unsigned long) * BITS_TO_LONGS(rf->max_ah);
	rsrc_size += sizeof(unsigned long) * BITS_TO_LONGS(rf->max_mcg);
	rsrc_size += sizeof(unsigned long) * BITS_TO_LONGS(rf->max_pri);
	rsrc_size += sizeof(unsigned long) * BITS_TO_LONGS(rf->max_8k_idx);
	rsrc_size += sizeof(struct zxdh_qp **) * rf->max_qp;
	rsrc_size += sizeof(struct zxdh_cq **) * rf->max_cq;
	rsrc_size += sizeof(struct zxdh_srq **) * rf->max_srq;

	return rsrc_size;
}

static int zxdh_initialize_hw_rsrc(struct zxdh_pci_f *rf)
{
	u32 rsrc_size;
	int ret;

	rf->max_cqe = ZXDH_MAX_CQE;
	rf->max_qp = ZXDH_MAX_QP;
	rf->max_mr = ZXDH_MAX_MR;
	rf->max_cq = ZXDH_MAX_CQ;
	rf->max_srq = ZXDH_MAX_SRQ;
	rf->max_pd = ZXDH_MAX_PD;
	rf->max_ah = ZXDH_MAX_AH;
	rf->max_mcg = ZXDH_MAX_MCG;
	rf->max_pri = ZXDH_MAX_USER_PRIORITY;
	rf->max_8k_idx = ZXDH_MAX_8K_IDX;

	rf->qp_cnt_8k_idxs = vzalloc(rf->max_8k_idx * sizeof(u32));
	if (!rf->qp_cnt_8k_idxs) {
		ret = -ENOMEM;
		goto mem_8k_qp_cnt_vmalloc_fail;
	}

	rsrc_size = zxdh_calc_mem_rsrc_size(rf);
	rf->mem_rsrc = vzalloc(rsrc_size);
	if (!rf->mem_rsrc) {
		ret = -ENOMEM;
		goto mem_rsrc_vmalloc_fail;
	}

	zxdh_set_hw_rsrc(rf);

	set_bit(0, rf->allocated_mrs);
	set_bit(1, rf->allocated_mrs);
	set_bit(0, rf->allocated_pds);
	set_bit(0, rf->allocated_qps);
	set_bit(0, rf->allocated_ahs);
	set_bit(0, rf->allocated_mcgs);

	return 0;

mem_rsrc_vmalloc_fail:
	vfree(rf->qp_cnt_8k_idxs);
mem_8k_qp_cnt_vmalloc_fail:
	return ret;
}

int zxdh_ctrl_init_hw(struct zxdh_pci_f *rf)
{
	return zxdh_initialize_hw_rsrc(rf);
}
