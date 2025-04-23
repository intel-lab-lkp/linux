/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#ifndef _IONIC_IBDEV_H_
#define _IONIC_IBDEV_H_

#include <rdma/ib_umem.h>
#include <rdma/ib_verbs.h>
#include <linux/ionic/ionic_api.h>
#include <linux/ionic/ionic_regs.h>

#include "ionic_fw.h"
#include "ionic_queue.h"
#include "ionic_res.h"

#define DRIVER_NAME		"ionic_rdma"
#define DRIVER_SHORTNAME	"ionr"

#define IONIC_MIN_RDMA_VERSION	0
#define IONIC_MAX_RDMA_VERSION	2

/* Config knobs */
#define IONIC_EQ_DEPTH 511
#define IONIC_EQ_COUNT 32
#define IONIC_AQ_DEPTH 63
#define IONIC_AQ_COUNT 4
#define IONIC_EQ_ISR_BUDGET 10
#define IONIC_EQ_WORK_BUDGET 1000

#define IONIC_CQ_GRACE 100

struct ionic_aq;
struct ionic_cq;
struct ionic_eq;
struct ionic_vcq;

enum ionic_admin_state {
	IONIC_ADMIN_ACTIVE, /* submitting admin commands to queue */
	IONIC_ADMIN_PAUSED, /* not submitting, but may complete normally */
	IONIC_ADMIN_KILLED, /* not submitting, locally completed */
};

enum ionic_admin_flags {
	IONIC_ADMIN_F_BUSYWAIT  = BIT(0),	/* Don't sleep */
	IONIC_ADMIN_F_TEARDOWN  = BIT(1),	/* In destroy path */
	IONIC_ADMIN_F_INTERRUPT = BIT(2),	/* Interruptible w/timeout */
};

struct ionic_qdesc {
	__aligned_u64 addr;
	__u32 size;
	__u16 mask;
	__u8 depth_log2;
	__u8 stride_log2;
};

struct ionic_mmap_info {
	struct list_head ctx_ent;
	unsigned long offset;
	unsigned long size;
	unsigned long pfn;
	bool writecombine;
};

struct ionic_ibdev {
	struct ib_device	ibdev;

	struct device		*hwdev;
	struct net_device	*ndev;

	const union ionic_lif_identity	*ident;

	void		*handle;
	int			lif_index;

	u32			dbid;
	u64			__iomem *dbpage;
	struct ionic_intr	__iomem *intr_ctrl;

	u8			rdma_version;
	u8			qp_opcodes;
	u8			admin_opcodes;

	u32			aq_base;
	u32			cq_base;
	u32			eq_base;

	u8			aq_qtype;
	u8			sq_qtype;
	u8			rq_qtype;
	u8			cq_qtype;
	u8			eq_qtype;
	u8			udma_count;
	u8			udma_qgrp_shift;
	struct xarray		cq_tbl;
	rwlock_t		cq_tbl_rw;
	struct mutex		inuse_lock; /* for id reservation */
	struct ionic_resid_bits	inuse_cqid;
	int			next_cqid[2];
	u8			half_cqid_udma_shift;
	struct work_struct	reset_work;
	bool			reset_posted;
	u32			reset_cnt;

	struct delayed_work	admin_dwork;
	struct ionic_aq		**aq_vec;
	int			aq_count;
	enum ionic_admin_state	admin_state;

	struct ionic_eq		**eq_vec;
	int			eq_count;
};

struct ionic_eq {
	struct ionic_ibdev	*dev;

	u32			eqid;
	u32			intr;

	struct ionic_queue	q;

	bool			enable;
	bool			armed;

	struct work_struct	work;

	int			irq;
	char			name[32];

	u64			poll_isr;
	u64			poll_isr_single;
	u64			poll_isr_full;
	u64			poll_wq;
	u64			poll_wq_single;
	u64			poll_wq_full;
};

struct ionic_admin_wr {
	struct completion		work;
	struct list_head		aq_ent;
	struct ionic_v1_admin_wqe	wqe;
	struct ionic_v1_cqe		cqe;
	struct ionic_aq			*aq;
	int				status;
};

struct ionic_admin_wr_q {
	struct ionic_admin_wr	*wr;
	int			wqe_strides;
};

struct ionic_aq {
	struct ionic_ibdev	*dev;
	struct ionic_vcq	*vcq;

	struct work_struct	work;

	unsigned long		stamp;
	bool			armed;

	u32			aqid;
	u32			cqid;

	spinlock_t		lock; /* for posting */
	struct ionic_queue	q;
	struct ionic_admin_wr_q	*q_wr;
	struct list_head	wr_prod;
	struct list_head	wr_post;
};

struct ionic_ctx {
	struct ib_ucontext	ibctx;

	u32			dbid;

	struct mutex		mmap_mut; /* for mmap_list */
	unsigned long long	mmap_off;
	struct list_head	mmap_list;
	struct ionic_mmap_info	mmap_dbell;
};

struct ionic_tbl_buf {
	u32		tbl_limit;
	u32		tbl_pages;
	size_t		tbl_size;
	__le64		*tbl_buf;
	dma_addr_t	tbl_dma;
	u8		page_size_log2;
};

struct ionic_cq {
	struct ionic_vcq	*vcq;

	u32			cqid;
	u32			eqid;

	spinlock_t		lock; /* for polling */
	struct list_head	poll_sq;
	bool			flush;
	struct list_head	flush_sq;
	struct list_head	flush_rq;
	struct list_head	cq_list_ent;

	struct ionic_queue	q;
	bool			color;
	int			reserve;
	u16			arm_any_prod;
	u16			arm_sol_prod;

	struct kref		cq_kref;
	struct completion	cq_rel_comp;

	/* infrequently accessed, keep at end */
	struct ib_umem		*umem;
};

struct ionic_vcq {
	struct ib_cq		ibcq;
	struct ionic_cq		cq[2];
	u8			udma_mask;
	u8			poll_idx;
};

static inline struct ionic_ibdev *to_ionic_ibdev(struct ib_device *ibdev)
{
	return container_of(ibdev, struct ionic_ibdev, ibdev);
}

static inline void ionic_cq_complete(struct kref *kref)
{
	struct ionic_cq *cq = container_of(kref, struct ionic_cq, cq_kref);

	complete(&cq->cq_rel_comp);
}

/* ionic_admin.c */
extern struct workqueue_struct *ionic_evt_workq;
void ionic_admin_post(struct ionic_ibdev *dev, struct ionic_admin_wr *wr);
int ionic_admin_wait(struct ionic_ibdev *dev, struct ionic_admin_wr *wr,
		     enum ionic_admin_flags);

int ionic_rdma_reset_devcmd(struct ionic_ibdev *dev);

int ionic_create_rdma_admin(struct ionic_ibdev *dev);
void ionic_destroy_rdma_admin(struct ionic_ibdev *dev);
void ionic_kill_rdma_admin(struct ionic_ibdev *dev, bool fatal_path);

/* ionic_controlpath.c */
int ionic_create_cq_common(struct ionic_vcq *vcq,
			   struct ionic_tbl_buf *buf,
			   const struct ib_cq_init_attr *attr,
			   struct ionic_ctx *ctx,
			   struct ib_udata *udata,
			   struct ionic_qdesc *req_cq,
			   __u32 *resp_cqid,
			   int udma_idx);
void ionic_destroy_cq_common(struct ionic_ibdev *dev, struct ionic_cq *cq);

/* ionic_pgtbl.c */
int ionic_pgtbl_page(struct ionic_tbl_buf *buf, u64 dma);
int ionic_pgtbl_init(struct ionic_ibdev *dev,
		     struct ionic_tbl_buf *buf,
		     struct ib_umem *umem,
		     dma_addr_t dma,
		     int limit,
		     u64 page_size);
void ionic_pgtbl_unbuf(struct ionic_ibdev *dev, struct ionic_tbl_buf *buf);

/* ionic_ibdev.c */
void ionic_port_event(struct ionic_ibdev *dev, enum ib_event_type event);
#endif /* _IONIC_IBDEV_H_ */
