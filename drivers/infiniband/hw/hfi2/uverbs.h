/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef HFI2_UVERBS_H
#define HFI2_UVERBS_H

#include <rdma/uverbs_ioctl.h>
#include <rdma/ib_verbs.h>

/*
 * Driver-specific mmap entry, embedding the core rdma_user_mmap_entry.
 * One entry is created per mappable region and tracked for the lifetime
 * of the user context.
 */
struct hfi2_user_mmap_entry {
	struct rdma_user_mmap_entry rdma_entry;
	u64 address;
	void *memvirt;
	dma_addr_t memdma;
	u8 mmap_flag;
};

static inline struct hfi2_user_mmap_entry *
to_hfi2_mmap(struct rdma_user_mmap_entry *rdma_entry)
{
	return container_of(rdma_entry, struct hfi2_user_mmap_entry,
			    rdma_entry);
}

int hfi2_alloc_ucontext(struct ib_ucontext *ucontext, struct ib_udata *udata);
void hfi2_dealloc_ucontext(struct ib_ucontext *ucontext);
int hfi2_mmap(struct ib_ucontext *ucontext, struct vm_area_struct *vma);
void hfi2_mmap_free(struct rdma_user_mmap_entry *rdma_entry);

extern const struct uapi_definition hfi2_ib_defs[];

#endif /* HFI2_UVERBS_H */
