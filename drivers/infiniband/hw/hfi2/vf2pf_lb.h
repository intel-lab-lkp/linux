/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 * Internal defs for SRIOV support for VF to PF0 over hfi2_loopback.
 */

#ifndef _VF2PF_LB_H
#define _VF2PF_LB_H

struct vf2pf_lbdata {
	struct hfi2_ctxtrsrcs c;
	struct hfi2_devdata *dd;
	u8 pidx;
	u8 flags;
	u16 pf0_ctxt;
	struct xarray tid_xa;
	int tid_next;
	struct hfi2_ctxtbufs b;
	spinlock_t pio_lock;	/* protection for pio_next */
	void __iomem *pio_mem;	/* start of PIO mmap */
	void __iomem *pio_wrap;	/* end of PIO mmap */
	void __iomem *pio_next;	/* next SOP in PIO mmap */
	int rcv_irq;
	spinlock_t rcv_lock;	/* serialize receives */
};

#define VF2PF_LB_FL_SHUTDOWN	0x01

struct vf2pf_devops *hfi2_get_lb_devops(void);

/* 9B packet header - preceded in send by (u64)PBC */
struct vf2pf_lb_hdr {
	__be16 lrh[4];
	__be32 bth[3];
	/* KDETH: __le64 kdeth; instead: */
	__le32 ver_tid_offset;
	__le16 jkey;
	__le16 hcrc;	/* filled by send engine */
} __packed;

#define VF2PF_LB_MAX_MSG	(PAGE_SIZE - sizeof(struct vf2pf_hdr) - \
				 sizeof(struct vf2pf_lb_hdr))

struct vf2pf_lb_msg {
	struct vf2pf_prefix pfx;
	struct vf2pf_lb_hdr lbh __aligned(sizeof(u64));
	struct vf2pf_hdr hdr;
	u8 data[VF2PF_LB_MAX_MSG];
} __packed;

#endif /* _VF2PF_LB_H */
