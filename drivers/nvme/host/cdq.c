// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe Controller Data Queue (CDQ) support.
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>

#include "nvme.h"
#include "cdq.h"

static inline void nvme_free_cdqmem_chunks(struct cdq_nvme_queue *cdq)
{
	struct device *dev = cdq->ctrl->dev;
	unsigned int i;

	if (!cdq->chunks)
		return;
	for (i = 0; i < cdq->nr_chunks; i++) {
		if (cdq->chunks[i].vaddr)
			dma_free_coherent(dev, cdq->chunks[i].size,
					  cdq->chunks[i].vaddr,
					  cdq->chunks[i].dma_addr);
	}
	kfree(cdq->chunks);
	cdq->chunks = NULL;
	cdq->nr_chunks = 0;
}

static inline int nvme_alloc_cdqmem_chunks(struct cdq_nvme_queue *cdq)
{
	struct device *dev = cdq->ctrl->dev;
	unsigned int i, nr = 1;

	/*
	 * Try to get a single pointer to the whole cdq, it will take
	 * the iommu path within the dma_alloc_coherent call
	 */
	cdq->chunks = kcalloc(nr, sizeof(*cdq->chunks), GFP_KERNEL);
	if (!cdq->chunks)
		return -ENOMEM;

	cdq->chunks[0].vaddr = dma_alloc_coherent(dev, cdq->size_nbyte,
			&cdq->chunks[0].dma_addr, GFP_KERNEL);
	if (cdq->chunks[0].vaddr)
		goto out;

	/* Fall back to allocating several chunks */
	nr = DIV_ROUND_UP(cdq->size_nbyte, NVME_CDQ_CHUNK_SIZE);

	cdq->chunks = krealloc_array(cdq->chunks, nr, sizeof(*cdq->chunks), GFP_KERNEL);
	if (!cdq->chunks)
		return -ENOMEM;

	for (i = 0; i < nr; i++) {
		cdq->chunks[i].vaddr = dma_alloc_coherent(dev,
				NVME_CDQ_CHUNK_SIZE, &cdq->chunks[i].dma_addr,
				GFP_KERNEL);
		if (!cdq->chunks[i].vaddr) {
			nvme_free_cdqmem_chunks(cdq);
			return -ENOMEM;
		}
		cdq->chunks[i].size = NVME_CDQ_CHUNK_SIZE;
	}

out:
	cdq->nr_chunks = nr;
	return 0;
}

static inline void nvme_free_cdqmem_prp_lists(struct cdq_nvme_queue *cdq)
{
	struct device *dev = cdq->ctrl->dev;
	unsigned int prp_idx;

	for (prp_idx = 0; prp_idx < cdq->nr_prp_lists; prp_idx++)
		dma_free_coherent(dev, PAGE_SIZE, cdq->prp_lists[prp_idx],
				  cdq->prp_lists_dma[prp_idx]);
	cdq->nr_prp_lists = 0;
}

static inline dma_addr_t nvme_get_cdq_pagedma(struct cdq_nvme_queue *cdq,
					      unsigned int page_idx)
{
	return cdq->chunks[page_idx / NVME_CDQ_PAGES_PER_CHUNK].dma_addr +
		(page_idx % NVME_CDQ_PAGES_PER_CHUNK) * NVME_CTRL_PAGE_SIZE;
}

static inline int nvme_build_cdqmem_prp_list(struct cdq_nvme_queue *cdq)
{
	struct device *dev = cdq->ctrl->dev;
	const unsigned int prps_per_page = PAGE_SIZE >> 3;
	unsigned int total_pages =
		DIV_ROUND_UP(cdq->size_nbyte, NVME_CTRL_PAGE_SIZE);
	dma_addr_t prp_list_dma;
	__le64 *prp_list;
	unsigned int prp_idx, page_idx;

	prp_list = dma_alloc_coherent(dev, PAGE_SIZE, &prp_list_dma, GFP_KERNEL);
	if (!prp_list)
		return -ENOMEM;
	cdq->prp_lists[0] = prp_list;
	cdq->prp_lists_dma[0] = prp_list_dma;
	cdq->nr_prp_lists = 1;

	for (page_idx = 0, prp_idx = 0; page_idx < total_pages; page_idx++) {
		dma_addr_t page_dma = nvme_get_cdq_pagedma(cdq, page_idx);

		/* Current prp_list page full with entries still to place: chain. */
		if (prp_idx == prps_per_page) {
			__le64 *old = prp_list;

			if (cdq->nr_prp_lists == MAX_NR_CDQ_PRPS)
				goto err;
			prp_list = dma_alloc_coherent(dev, PAGE_SIZE, &prp_list_dma,
						      GFP_KERNEL);
			if (!prp_list)
				goto err;
			cdq->prp_lists[cdq->nr_prp_lists] = prp_list;
			cdq->prp_lists_dma[cdq->nr_prp_lists++] = prp_list_dma;

			/* Chain from old to new prp_list */
			prp_list[0] = old[prps_per_page - 1];
			old[prps_per_page - 1] = cpu_to_le64(prp_list_dma);
			prp_idx = 1;
		}
		prp_list[prp_idx++] = cpu_to_le64(page_dma);
	}
	return 0;
err:
	nvme_free_cdqmem_prp_lists(cdq);
	return -ENOMEM;
}

/*
 * Allocate the coherent store and prps for a CDQ.
 * Expects cdq with size_nbytes and ctrl set.
 */
static inline int nvme_create_cdq_backing(struct cdq_nvme_queue *cdq)
{
	int ret;

	if (!cdq->size_nbyte)
		return -EINVAL;

	ret = nvme_alloc_cdqmem_chunks(cdq);
	if (ret)
		return ret;

	/* We pass cdq->chunks[0].dma_addr when cdq->nr_chunks == 1 */
	if (cdq->nr_chunks > 1) {
		ret = nvme_build_cdqmem_prp_list(cdq);
		if (ret)
			goto err_chunks;
	}

	/* FIXME: put this on the create_cdq function*/
	kref_init(&cdq->ref);

	return 0;

err_chunks:
	nvme_free_cdqmem_chunks(cdq);

	return ret;
}

/* Release the coherent DMA backing allocated by nvme_create_cdq_backing(). */
static inline void nvme_release_cdq_backing(struct cdq_nvme_queue *cdq)
{
	nvme_free_cdqmem_prp_lists(cdq);
	nvme_free_cdqmem_chunks(cdq);
}

static ssize_t nvme_cdq_fops_read(struct file *filep, char __user *buf,
				  size_t size_nbyte, loff_t *ppos)
{
	struct cdq_nvme_queue *cdq = filep->private_data;
	size_t nbytes = round_down(size_nbyte, NVME_CDQ_MQ_ENTRY_NRBYTES);

	if (*ppos)
		return -ESPIPE;

	if (size_nbyte < NVME_CDQ_MQ_ENTRY_NRBYTES)
		return -EINVAL;

	if (nbytes > (cdq->size_nbyte))
		return -EINVAL;

	/* CDQ traversal not implemented yet. */
	return -EOPNOTSUPP;
}

/* File reference already dropped by the close path, so don't fput() */
static int nvme_release_cdqfd(struct cdq_nvme_queue *cdq)
{
	nvme_cdq_put(cdq);
	return 0;
}

static int nvme_cdq_fops_release(struct inode *inode, struct file *filep)
{
	return nvme_release_cdqfd(filep->private_data);
}

static const struct file_operations cdq_fops = {
	.owner		= THIS_MODULE,
	.open		= nonseekable_open,
	.read		= nvme_cdq_fops_read,
	.release	= nvme_cdq_fops_release,
};

__maybe_unused
static int nvme_create_cdqfd(struct cdq_nvme_queue *cdq, int *cdq_fdno)
{
	int fdno;
	struct file *filep;

	filep = anon_inode_getfile("[cdq-readfd]", &cdq_fops, cdq, O_RDWR);
	if (IS_ERR(filep))
		return PTR_ERR(filep);

	/* cdq is being pionted at by ->private_data. increase ref */
	nvme_cdq_get(cdq);

	fdno = get_unused_fd_flags(O_CLOEXEC | O_RDONLY | O_DIRECT);
	if (fdno < 0) {
		fput(filep); /* nvme_cdq_put through release */
		return fdno;
	}

	fd_install(fdno, filep);
	*cdq_fdno = fdno;

	return 0;
}

static int nvme_submit_delete_cdq_cmd(const struct cdq_nvme_queue *cdq)
{
	struct nvme_command c = {
		.cdq.opcode = nvme_admin_cdq,
		.cdq.sel = NVME_CDQ_CMD_MGMT_DELETE,
		.cdq.dw11.cdqid = cpu_to_le16(cdq->id)
	};

	return __nvme_submit_sync_cmd(cdq->ctrl->admin_q, &c, NULL, NULL, 0, NVME_QID_ANY, 0);
}

/* Sends a CDQ delete NVMe cmd */
static void nvme_delete_cdq_ctrl(struct cdq_nvme_queue *cdq)
{
	if (nvme_submit_delete_cdq_cmd(cdq))
		WARN_ONCE(1, "Failed delete CDQ (id: %d)", cdq->id);
}

/* Does NOT send a CDQ delete NVMe cmd */
static void nvme_delete_cdq_host(struct cdq_nvme_queue *cdq)
{
	struct nvme_ctrl *ctrl = cdq->ctrl;

	if (xa_erase(&ctrl->cdqs, cdq->id) != cdq)
		return;

	nvme_release_cdq_backing(cdq);

	nvme_cdq_put(cdq);
}

void nvme_delete_cdq(struct cdq_nvme_queue *cdq)
{
	nvme_delete_cdq_ctrl(cdq);
	nvme_delete_cdq_host(cdq);
}
EXPORT_SYMBOL_GPL(nvme_delete_cdq);

/* Will NOT send a CDQ delete NVMe cmd. */
void nvme_delete_cdqs_host(struct nvme_ctrl *ctrl)
{
	struct cdq_nvme_queue *cdq;
	unsigned long i;

	xa_for_each(&ctrl->cdqs, i, cdq)
		nvme_delete_cdq_host(cdq);
}

/* Final teardown at device->release: free all CDQs and destroy the xarray. */
void nvme_free_cdqs(struct nvme_ctrl *ctrl)
{
	/*
	 * Delete host side CDQ only. NOT sending delete cmd as
	 * Ctrl should delete on disable.
	 */
	nvme_delete_cdqs_host(ctrl);
	xa_destroy(&ctrl->cdqs);
}
