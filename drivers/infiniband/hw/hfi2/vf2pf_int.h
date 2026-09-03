/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 * Internal defs for SRIOV support for VFs making requests to PF0.
 */

#ifndef _VF2PF_INT_H
#define _VF2PF_INT_H

#include "vf2pf.h"

/*
 * This selects the hfi2_loopback port implementation for
 * VF-PF communication in SRIOV.
 */
#define HFI_VF2PF_LOOPBACK

enum {
	VF2PF_OP_PING = 0,
	VF2PF_GET_CFG,
	VF2PF_ASGN_RES,
	VF2PF_FREE_RES,
	VF2PF_PREG_OP,
	VF2PF_RCSR_OP,
	VF2PF_RCCTRL_OP,
	VF2PF_TIDCFG_OP,
	VF2PF_RXERSM_OP,
	VF2PF_QPMAP_OP,
	VF2PF_STOP,
	VF2PF_MAD_SNDRCV,
	VF2PF_MAD_SND,
	VF2PF_READY,

	PF0_PUSH_PI,
	PF0_PUSH_VLT,
};

#define VF2PF_OP_RESP	0x80

/*
 * prefix area of 'msg' object to 'send', return from 'msg_alloc'.
 * must be qword multiple (following element aligned u64).
 */
struct vf2pf_prefix {
	u8 type;
	u8 _pad[7];
	union {
		wait_queue_head_t wait;	/* for sending requests */
		struct semaphore sema;	/* for sending MAD requests */
		struct {		/* for recvd requests */
			struct work_struct work;
			struct hfi2_devdata *dd;
			u32 len;
		};
	};
};

#define VF2PF_PFX_TYPE_RESP    0
#define VF2PF_PFX_TYPE_WAIT    1
#define VF2PF_PFX_TYPE_SEMA    2

struct vf2pf_hdr {
	u8 op;		/* response is ored with VF2PF_OP_RESP */
	u8 si;		/* sender SI - from */
	u16 len;	/* payload length, bytes (based on op) */
	s16 status;	/* -errno or 0 - valid on response */
	u16 tid;	/* tid for matching resp */
};

struct vf2pf_ping_msg {
	struct vf2pf_hdr hdr;
	u8 data[256];
};

struct vf2pf_getcfg_msg {
	struct vf2pf_hdr hdr;
	u8 si;
	struct hfi2_devrsrcs rsrcs;
	u64 base_guid;
	u64 revision;
	u8 hfi2_id;
	u8 icode;
	u16 irev;
	u64 cport_ver;
};

struct vf2pf_asgnrs_msg {
	struct vf2pf_hdr hdr;
	struct hfi2_devrsrcs rsrcs;
};

struct vf2pf_pregop_msg {
	struct vf2pf_hdr hdr;
	u64 arg;
	u32 ctxt;
	s32 type;
	u8 op;
	u8 pidx;
};

struct vf2pf_readcsr_msg {
	struct vf2pf_hdr hdr;
	u32 off;
	u64 reg;
};

struct vf2pf_rcctrl_msg {
	struct vf2pf_hdr hdr;
	u16 ctxt;
	int op;
	u64 reg;
};

struct vf2pf_tidcfg_msg {
	struct vf2pf_hdr hdr;
	u8 pidx;
	u16 ctxt;
	u16 alloced;
	u32 egr_base;
	u32 exp_base;
	u32 exp_cnt;
};

struct vf2pf_qpmap_msg {
	struct vf2pf_hdr hdr;
	u8 pidx;
	u16 idx;
	u16 res;
};

struct pf0_pushpi_msg {
	struct vf2pf_hdr hdr;
	u8 pidx;
	struct opa_smp smp;
};

struct pf0_pushvlt_msg {
	struct vf2pf_hdr hdr;
	u8 pidx;
	u64 sc2vl[4];
};

/* allocate full size MAD always */
struct vf2pf_mad {
	struct vf2pf_hdr hdr;
	u8 sb;
	u8 _pad[7];
	struct opa_smp mad; /* contains result MAD if requested */
};

#define VF2PF_MAD_OVERHEAD	sizeof(u64)

struct vf2pf_devops {
	int num_ctxts;
	int num_irq;
	int (*init)(struct hfi2_devdata *dd, u8 si);
	void (*deinit)(struct hfi2_devdata *dd, u8 si);
	int (*send)(struct hfi2_devdata *dd, u8 si, void *msg);
	void *(*msg_alloc)(struct hfi2_devdata *dd, struct vf2pf_hdr **msg);
	u16 (*set_tid)(struct hfi2_devdata *dd, void *tok);
	void *(*get_tid)(struct hfi2_devdata *dd, u16 tid);
	struct vf2pf_hdr *(*get_msg)(struct hfi2_devdata *dd, void *buf);
	int (*probe_si)(struct hfi2_devdata *dd);
	void (*init_sysfs)(struct hfi2_devdata *dd, struct device *class_dev);
	int (*sysfs_emit_at)(struct hfi2_devdata *dd, char *buf, int at);
	int (*init_irq)(struct hfi2_devdata *dd);
	void (*deinit_irq)(struct hfi2_devdata *dd);
	int (*rcv_wait)(struct hfi2_devdata *dd, void *buf, long timeout);
	void (*set_si_enables)(struct hfi2_devdata *dd, int si, u64 *csrs,
			       void (*si_enables)(struct hfi2_devdata *dd,
						  u64 *csrs, u32 start, u32 end));
};

#define VF2PF_INIT_ALL	((u8)-1)

/* call-outs from implementations to vf2pf main */
void hfi2_vf2pf_rcv_msg(struct hfi2_devdata *dd, struct vf2pf_hdr *hdr, void *msg);
void hfi2_vf2pf_rsp_msg(struct hfi2_devdata *dd, void *buf);

#endif /* _VF2PF_INT_H */
