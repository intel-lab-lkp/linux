/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NVMe Controller Data Queue (CDQ) support.
 */

#ifndef _NVME_CDQ_H
#define _NVME_CDQ_H

#include "nvme.h"

#define NVME_CDQ_MQ_ENTRY_NRBYTES	32

/*
 * The CDQ backing is a set of coherent DMA chunks. Chunk size expressed in
 * host pages to match dma_alloc_coherency granularity.
 */
#define NVME_CDQ_CHUNK_ORDER		2
#define NVME_CDQ_CHUNK_SIZE		(PAGE_SIZE << NVME_CDQ_CHUNK_ORDER)
#define NVME_CDQ_PAGES_PER_CHUNK	(NVME_CDQ_CHUNK_SIZE / NVME_CTRL_PAGE_SIZE)

/* Max PRP List pages we are willing to chain to describe a discontiguous CDQ. */
#define MAX_NR_CDQ_PRPS		20

struct nvme_cdq_chunk {
	void		*vaddr;
	dma_addr_t	dma_addr;
	size_t		size;
};

struct cdq_nvme_queue {
	u16 id;
	struct nvme_ctrl *ctrl;
	u32 size_nbyte;

	/* Coherent backing store. */
	struct nvme_cdq_chunk *chunks;
	unsigned int nr_chunks;

	/* PRP List pages describing the chunks to the controller (PC_DISCONT). */
	__le64 *prp_lists[MAX_NR_CDQ_PRPS];
	dma_addr_t prp_lists_dma[MAX_NR_CDQ_PRPS];
	unsigned int nr_prp_lists;

	/* Manage refs for read FD and controller xarray */
	struct kref ref;
};

/* Must not touch cdq->ctrl: Ctrl may have been freed */
static inline void nvme_free_cdq(struct kref *ref)
{
	kfree(container_of(ref, struct cdq_nvme_queue, ref));
}

static inline void nvme_cdq_get(struct cdq_nvme_queue *cdq)
{
	kref_get(&cdq->ref);
}

static inline void nvme_cdq_put(struct cdq_nvme_queue *cdq)
{
	kref_put(&cdq->ref, nvme_free_cdq);
}

void nvme_delete_cdq(struct cdq_nvme_queue *cdq);
void nvme_delete_cdqs_host(struct nvme_ctrl *ctrl);
void nvme_free_cdqs(struct nvme_ctrl *ctrl);

#endif /* _NVME_CDQ_H */
