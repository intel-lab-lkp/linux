/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NVMe Controller Data Queue (CDQ) support.
 */

#ifndef _NVME_CDQ_H
#define _NVME_CDQ_H

#include "nvme.h"

#define NVME_CDQ_MQ_ENTRY_NRBYTES	32
#define NVME_CDQ_MQ_PHASE_MASK		0x1
#define NVME_CDQ_MQ_PHASE_OFFSET	(NVME_CDQ_MQ_ENTRY_NRBYTES - 1)

/*
 * The CDQ backing is a set of coherent DMA chunks expressed in
 * host pages to match dma_alloc_coherency granularity.
 */
#define NVME_CDQ_CHUNK_ORDER		2
#define NVME_CDQ_CHUNK_SIZE		(PAGE_SIZE << NVME_CDQ_CHUNK_ORDER)
#define NVME_CDQ_PAGES_PER_CHUNK	(NVME_CDQ_CHUNK_SIZE / NVME_CTRL_PAGE_SIZE)
#define NVME_CDQ_MQ_ENTRY_PER_CHUNK	(NVME_CDQ_CHUNK_SIZE / NVME_CDQ_MQ_ENTRY_NRBYTES)

/* Max PRP List pages we are willing to chain to describe a discontiguous CDQ. */
#define MAX_NR_CDQ_PRPS		20

struct nvme_cdq_chunk {
	void		*vaddr;
	dma_addr_t	dma_addr;
	size_t		size;
};

struct cdq_nvme_queue {
	u16 id;
	struct nvme_ctrl *ctrl; // this is kref'ed for the life of the CDQ
	u32 size_nbyte;
	u16 mc_id; // migratable controller id

	/* Coherent backing store. */
	struct nvme_cdq_chunk *chunks;
	unsigned int nr_chunks;

	/* PRP List pages describing the chunks to the controller (PC_DISCONT). */
	__le64 *prp_lists[MAX_NR_CDQ_PRPS];
	dma_addr_t prp_lists_dma[MAX_NR_CDQ_PRPS];
	unsigned int nr_prp_lists;

	/* True if mem for chunks and prps is valid */
	bool valid_mem;

	/* How far the CDQ was consumed by the host */
	u32 host_head;

	/*
	 * Value sent by the in-flight set-feature cmd
	 * Differs from cntl_head until set-feature cmd completes
	 */
	u32 sent_head;

	/* Last acked CDQ head update. Trails host_head.*/
	u32 cntl_head;

	u8 phase_bit;

	/* sf_* controlls if the feature set cmd is done or is still inflight */
	spinlock_t sf_lock;
	bool sf_inflight;

	/* Manage refs for read FD and controller xarray */
	struct kref ref;
};

/*
 * nvme_free_cdq:
 *    - Is called with the cdq struct has no more krefs.
 *    - Will free (cdq) and decrease ctrl kref.
 */
void nvme_free_cdq(struct kref *ref);
/* nvme_cdq_delete:
 *    - Sends a nvme delete cmd to the controller
 *    - removes (cdq) from controller xarray
 *    - frees the backing CDQ mem
 *    - Calls nvme_free_cdq if there are no more refs
 */
void nvme_delete_cdq(struct cdq_nvme_queue *cdq);
int nvme_create_cdq(struct nvme_ctrl *ctrl, const u32 entry_nr, const u16 mc_id);

static inline void nvme_cdq_get(struct cdq_nvme_queue *cdq)
{
	kref_get(&cdq->ref);
}

static inline void nvme_cdq_put(struct cdq_nvme_queue *cdq)
{
	kref_put(&cdq->ref, nvme_free_cdq);
}

void nvme_delete_cdqs_host(struct nvme_ctrl *ctrl);
void nvme_free_cdqs(struct nvme_ctrl *ctrl);

#endif /* _NVME_CDQ_H */
