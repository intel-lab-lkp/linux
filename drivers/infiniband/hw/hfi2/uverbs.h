/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef HFI2_UVERBS_H
#define HFI2_UVERBS_H

#include <rdma/uverbs_ioctl.h>

int hfi2_alloc_ucontext(struct ib_ucontext *ucontext, struct ib_udata *udata);
void hfi2_dealloc_ucontext(struct ib_ucontext *ucontext);
int hfi2_rdma_mmap(struct ib_ucontext *ucontext, struct vm_area_struct *vma);

extern const struct uapi_definition hfi2_ib_defs[];

#endif /* HFI2_UVERBS_H */
