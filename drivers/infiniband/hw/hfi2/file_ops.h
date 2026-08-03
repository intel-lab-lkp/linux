/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _HFI2_FILE_OPS_H
#define _HFI2_FILE_OPS_H

#include "hfi2.h"

int hfi2_set_uevent_bits(struct hfi2_pportdata *ppd, const int evtbit);
struct hfi2_filedata *hfi2_alloc_filedata(struct hfi2_devdata *dd);
void hfi2_dealloc_filedata(struct hfi2_filedata *fdata);
int hfi2_do_assign_ctxt(struct hfi2_filedata *fd,
			const struct hfi2_assign_ctxt_cmd *uinfo);
int hfi2_manage_rcvq(struct hfi2_ctxtdata *uctxt, u16 subctxt, int start_stop);
int hfi2_user_event_ack(struct hfi2_ctxtdata *uctxt, u16 subctxt,
		   unsigned long events);
int hfi2_user_set_ctxt_pkey(struct hfi2_ctxtdata *uctxt, u16 pkey);
int hfi2_ctxt_reset(struct hfi2_ctxtdata *uctxt);
int hfi2_get_pinning_stats(struct hfi2_filedata *fd,
			   struct hfi2_pin_stats *stats);
int hfi2_do_mmap(struct hfi2_filedata *fd, u8 type, struct vm_area_struct *vma,
		 struct rdma_user_mmap_entry *rdma_entry,
		 struct ib_ucontext *ucontext);
ssize_t hfi2_do_write_iter(struct hfi2_filedata *fd, struct iov_iter *from);

/*
 * Types of memories mapped into user processes' space
 */
enum mmap_types {
	PIO_BUFS = 1,
	PIO_BUFS_SOP,
	PIO_CRED,
	RCV_HDRQ,
	RCV_EGRBUF,
	UREGS,
	EVENTS,
	STATUS,
	RTAIL,
	SUBCTXT_UREGS,
	SUBCTXT_RCV_HDRQ,
	SUBCTXT_EGRBUF,
	SDMA_COMP,
	RCV_RHEQ,
	MMAP_TYPE_MAX,
};

#endif /* _HFI2_FILE_OPS_H */
