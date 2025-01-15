/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  loopback-ism device structure definitions.
 *
 *  Copyright (c) 2024, Alibaba Inc.
 *
 *  Author: Wen Gu <guwen@linux.alibaba.com>
 *          Tony Lu <tonylu@linux.alibaba.com>
 *
 */

#ifndef _ISM_LOOPBACK_H
#define _ISM_LOOPBACK_H

#include <linux/device.h>
#include <linux/err.h>
#include <linux/hashtable.h>
#include <linux/ism.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#if IS_ENABLED(CONFIG_ISM_LO)
#define ISM_LO_DMBS_HASH_BITS	12

struct ism_lo_dmb_node {
	struct hlist_node list;
	u64 token;
	u32 len;
	u32 sba_idx;
	void *cpu_addr;
	dma_addr_t dma_addr;
	refcount_t refcnt;
};

struct ism_lo_dev {
	struct ism_dev ism;
	struct device dev;
	atomic_t dmb_cnt;
	rwlock_t dmb_ht_lock;
	DECLARE_BITMAP(sba_idx_mask, ISM_LO_MAX_DMBS);
	DECLARE_HASHTABLE(dmb_ht, ISM_LO_DMBS_HASH_BITS);
	wait_queue_head_t ldev_release;
};

int ism_loopback_init(void);
void ism_loopback_exit(void);
#else
static inline int ism_loopback_init(void)
{
	return 0;
}

static inline void ism_loopback_exit(void)
{
}
#endif

#endif /* _ISM_LOOPBACK_H */
