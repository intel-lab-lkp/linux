// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe Controller Data Queue (CDQ) support.
 */

#include <linux/anon_inodes.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/uaccess.h>

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


static inline void *nvme_get_cdq_entryvaddr(struct cdq_nvme_queue *cdq,
					    unsigned int entry_idx)
{
	return cdq->chunks[entry_idx / NVME_CDQ_MQ_ENTRY_PER_CHUNK].vaddr +
		(entry_idx % NVME_CDQ_MQ_ENTRY_PER_CHUNK) * NVME_CDQ_MQ_ENTRY_NRBYTES;
}

/* Advance cdq->host_head at most nrbytes and return actual advanced bytes */
static size_t nvme_advance_cdq(struct cdq_nvme_queue *cdq, size_t max_nrbyte)
{
	size_t target_nbyte = 0;
	void *entry;
	u8 phase_bit;
	u32 cdq_nrentry = cdq->size_nbyte / NVME_CDQ_MQ_ENTRY_NRBYTES;

	while (target_nbyte < max_nrbyte) {
		entry = nvme_get_cdq_entryvaddr(cdq, cdq->host_head);
		phase_bit = (*(u8 *)(entry + NVME_CDQ_MQ_PHASE_OFFSET) & NVME_CDQ_MQ_PHASE_MASK);

		if (phase_bit == cdq->phase_bit)
			break;

		cdq->host_head = (cdq->host_head + 1) % cdq_nrentry;
		target_nbyte += NVME_CDQ_MQ_ENTRY_NRBYTES;
		if (unlikely(cdq->host_head == 0))
			cdq->phase_bit = ~cdq->phase_bit & NVME_CDQ_MQ_PHASE_MASK;
	}

	return target_nbyte;
}

static void nvme_submit_sfcmd_cdq(struct cdq_nvme_queue *cdq);

/* updates cntl_head, exits sf_inflight and re-arms if needed */
static enum rq_end_io_ret nvme_endio_sfcmd_cdq(struct request *rq,
					       blk_status_t status,
					       const struct io_comp_batch *iob)
{
	struct cdq_nvme_queue *cdq = rq->end_io_data;
	unsigned long flags;
	bool rearm = false;

	WRITE_ONCE(cdq->cntl_head, cdq->sent_head);
	blk_mq_free_request(rq);

	spin_lock_irqsave(&cdq->sf_lock, flags);
	cdq->sf_inflight = false;
	/* cntl_head was just written above, plain read under the lock. */
	if (READ_ONCE(cdq->host_head) != cdq->cntl_head) {
		cdq->sf_inflight = true;
		rearm = true;
	}
	spin_unlock_irqrestore(&cdq->sf_lock, flags);

	/* Re-arm (takes its own ref) before dropping ours, so cdq stays alive. */
	if (rearm)
		nvme_submit_sfcmd_cdq(cdq);

	nvme_cdq_put(cdq);
	return RQ_END_IO_NONE;
}

/* Inform ctrl that head as advance by sending a CDQ set-feature command*/
static void nvme_submit_sfcmd_cdq(struct cdq_nvme_queue *cdq)
{
	struct nvme_command c = { };
	struct request *rq;
	unsigned long flags;
	u32 head = READ_ONCE(cdq->host_head);
	u32 tpt = READ_ONCE(cdq->pending_tpt);
	u32 dword11 = cdq->id & NVME_FEAT_CDQ_ID_MASK;
	u32 cdq_nrentry = cdq->size_nbyte / NVME_CDQ_MQ_ENTRY_NRBYTES;

	c.features.opcode = nvme_admin_set_features;
	c.features.fid = cpu_to_le32(NVME_FEAT_CDQ);

	if (unlikely(tpt != 0)) {
		/*
		 * FIXME: There is a small chance that the sent tpt will have
		 * already been handled by the time this command completes. If
		 * we find this to be true in the CDQ, we need to send a
		 * subsequent feature_id to disable the tail pointer trigger.
		 * section 5.1.25.1.23 nvme base spec.
		 */
		dword11 |= NVME_FEAT_CDQ_ETPT_MASK;
		c.features.dword13 = cpu_to_le32((head + tpt) % cdq_nrentry);
	}

	c.features.dword11 = cpu_to_le32(dword11);
	c.features.dword12 = cpu_to_le32(head);

	rq = blk_mq_alloc_request(cdq->ctrl->admin_q, nvme_req_op(&c),
				  BLK_MQ_REQ_NOWAIT);
	if (IS_ERR(rq)) {
		/*
		 * No admin tag right now and we cannot sleep. Drop the slot; the
		 * next read() will re-arm.
		 */
		spin_lock_irqsave(&cdq->sf_lock, flags);
		cdq->sf_inflight = false;
		spin_unlock_irqrestore(&cdq->sf_lock, flags);
		return;
	}

	cdq->sent_head = head;
	WRITE_ONCE(cdq->pending_tpt, 0);
	nvme_init_request(rq, &c);
	rq->end_io = nvme_endio_sfcmd_cdq;
	rq->end_io_data = cdq;

	/* Pin cdq for the lifetime of the command */
	nvme_cdq_get(cdq);
	blk_execute_rq_nowait(rq, false);
}

/* submit set feature if none are in-flight */
static void nvme_kick_cdq(struct cdq_nvme_queue *cdq)
{
	unsigned long flags;
	bool submit = false;

	spin_lock_irqsave(&cdq->sf_lock, flags);
	if (!cdq->sf_inflight) {
		cdq->sf_inflight = true;
		submit = true;
	}
	spin_unlock_irqrestore(&cdq->sf_lock, flags);

	if (submit)
		nvme_submit_sfcmd_cdq(cdq);
}

static ssize_t nvme_traversecopy_cdq(struct cdq_nvme_queue *cdq, size_t max_nrbyte,
				   void *priv_data)
{
	char __user *to_buf = priv_data;
	void *from_buf;
	u32 init_host_head = cdq->host_head;
	u8 init_phase_bit = cdq->phase_bit;

	size_t target_nbyte = nvme_advance_cdq(cdq, max_nrbyte);
	size_t copied_nbyte = 0, chunks_idx, entry_idx, tx_nbytes;

	if (target_nbyte == 0)
		goto out;

	for (chunks_idx = init_host_head / NVME_CDQ_MQ_ENTRY_PER_CHUNK,
	     entry_idx = init_host_head % NVME_CDQ_MQ_ENTRY_PER_CHUNK;
	     copied_nbyte < target_nbyte;
	     chunks_idx = (chunks_idx + 1) % cdq->nr_chunks, entry_idx = 0) {
		from_buf = cdq->chunks[chunks_idx].vaddr +
			(entry_idx * NVME_CDQ_MQ_ENTRY_NRBYTES);
		tx_nbytes = min(target_nbyte - copied_nbyte,
				NVME_CDQ_CHUNK_SIZE - (entry_idx * NVME_CDQ_MQ_ENTRY_NRBYTES));
		if (copy_to_user(to_buf, from_buf, tx_nbytes))
			goto err_out;
		copied_nbyte += tx_nbytes;
		to_buf += tx_nbytes;
	}

out:
	/*
	 * host_head advanced past consumed entries; tell the controller its head
	 * can move up. Decoupled from this read: the set-feature admin
	 * round-trip must not delay the data path.
	 */
	if (copied_nbyte) {
		nvme_kick_cdq(cdq);
	} else if (cdq->tpt_efd_ctx) {
		/* Controller will one-shot AEN when more entries are added */
		WRITE_ONCE(cdq->pending_tpt, 1);
		nvme_kick_cdq(cdq);
	}
	return copied_nbyte;

err_out:
	cdq->host_head = init_host_head;
	cdq->phase_bit = init_phase_bit;
	return -EFAULT;
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

	if (!READ_ONCE(cdq->valid_mem))
		return -EINVAL;

	return nvme_traversecopy_cdq(cdq, nbytes, buf);
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

/* Should only handle cdq struct and ctrl kref */
void nvme_free_cdq(struct kref *ref)
{
	struct cdq_nvme_queue *cdq = container_of(ref, struct cdq_nvme_queue, ref);

	/* Drop the ctrl kref held since creation */
	nvme_put_ctrl(cdq->ctrl);
	kfree(cdq);
}

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

static void nvme_put_cdq_tpt(struct cdq_nvme_queue *cdq)
{
	if (cdq->tpt_efd_ctx)
		eventfd_ctx_put(cdq->tpt_efd_ctx);
	cdq->tpt_efd_ctx = NULL;
}

static int nvme_get_cdq_tpt(struct cdq_nvme_queue *cdq, const int tpt_fd)
{
	struct eventfd_ctx *tmp;

	if (tpt_fd <= 0)
		return 0;

	/* put the old one */
	nvme_put_cdq_tpt(cdq);

	tmp = eventfd_ctx_fdget(tpt_fd);
	if (IS_ERR(tmp))
		return -EINVAL;

	cdq->tpt_efd_ctx = tmp;
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

static int nvme_submit_startstop_cdq_cmd(struct cdq_nvme_queue *cdq, uint startstop)
{
	struct nvme_command c = {
		.track_send.opcode = nvme_admin_track_send,
		.track_send.sel = NVME_TRSND_CMD_MGMT_LOG_USR_DATA,
		.track_send.mos = cpu_to_le16(startstop),
		.track_send.dw11 = cpu_to_le32((u32)cdq->id)
	};

	return  __nvme_submit_sync_cmd(cdq->ctrl->admin_q, &c, NULL, NULL, 0, NVME_QID_ANY, 0);
}
#define nvme_start_cdq(cdq) \
	nvme_submit_startstop_cdq_cmd(cdq, NVME_TRSND_CMD_MGMT_LOG_USR_DATA_START)
#define nvme_stop_cdq(cdq) \
	nvme_submit_startstop_cdq_cmd(cdq, NVME_TRSND_CMD_MGMT_LOG_USR_DATA_STOP)

/* Sends a CDQ delete NVMe cmd */
static void nvme_delete_cdq_ctrl(struct cdq_nvme_queue *cdq)
{
	if (nvme_stop_cdq(cdq))
		WARN_ONCE(1, "Failed to stop CDQ (id: %d)", cdq->id);
	if (nvme_submit_delete_cdq_cmd(cdq))
		WARN_ONCE(1, "Failed delete CDQ (id: %d)", cdq->id);
}

/* Does NOT send a CDQ delete NVMe cmd */
static void nvme_delete_cdq_host(struct cdq_nvme_queue *cdq)
{
	struct nvme_ctrl *ctrl = cdq->ctrl;

	if (xa_erase(&ctrl->cdqs, cdq->id) != cdq)
		return;

	WRITE_ONCE(cdq->valid_mem, false);

	nvme_put_cdq_tpt(cdq);

	nvme_release_cdq_backing(cdq);
	nvme_cdq_put(cdq);
}

void nvme_delete_cdq(struct cdq_nvme_queue *cdq)
{
	nvme_delete_cdq_ctrl(cdq);
	nvme_delete_cdq_host(cdq);
}
EXPORT_SYMBOL_GPL(nvme_delete_cdq);

int nvme_handle_cdq_aen_tpevent(struct nvme_ctrl *ctrl, u32 event_param)
{
	u16 cdq_id = event_param & NVME_FEAT_CDQ_ID_MASK;
	struct cdq_nvme_queue *cdq;

	cdq = xa_load(&ctrl->cdqs, cdq_id);
	if (xa_is_err(cdq))
		return xa_err(cdq);
	if (!cdq->tpt_efd_ctx)
		return -EINVAL;

	eventfd_signal(cdq->tpt_efd_ctx);

	return 0;
}

static int nvme_submit_create_cdq_cmd(struct cdq_nvme_queue *cdq)
{
	int ret;
	union nvme_result result = {};
	struct nvme_command c = {
		.cdq.opcode = nvme_admin_cdq,
		.cdq.sel = NVME_CDQ_CMD_MGMT_CREATE,
		.cdq.mos = cpu_to_le16(NVME_CDQ_CMD_MGMT_CREATE_MOS_QT_UDMQ),
		.cdq.dw11.cqs = cpu_to_le16(cdq->mc_id),
		.cdq.cdqsize = cpu_to_le32(cdq->size_nbyte >> 2) // size is in dwords
	};

	if (cdq->nr_chunks < 2) {
		c.cdq.dw11.flags = cpu_to_le16(NVME_CDQ_CMD_MGMT_CREATE_PC_CONT);
		c.cdq.prp1 = cpu_to_le64(cdq->chunks[0].dma_addr);
	} else {
		c.cdq.dw11.flags = cpu_to_le16(NVME_CDQ_CMD_MGMT_CREATE_PC_DISCONT);
		c.cdq.prp1 = cpu_to_le64(cdq->prp_lists_dma[0]);
	}

	ret = __nvme_submit_sync_cmd(cdq->ctrl->admin_q, &c, &result, NULL, 0, NVME_QID_ANY, 0);
	if (ret)
		return ret;

	cdq->id = le16_to_cpu(result.u16);

	return ret;
}

int nvme_create_cdq(struct nvme_ctrl *ctrl, const u32 entry_nr, const u16 mc_id, const int tpt_fd)
{
	u64 size_nbyte = (u64)entry_nr * NVME_CDQ_MQ_ENTRY_NRBYTES;
	struct cdq_nvme_queue *cdq = NULL;
	int ret, cdq_fd;

	/* The backing size and the CDQSIZE field are both u32 (bytes). */
	if (size_nbyte > U32_MAX)
		return -EINVAL;

	cdq = kzalloc_obj(*cdq);
	if (!cdq)
		return -ENOMEM;

	cdq->mc_id = mc_id;
	cdq->ctrl = ctrl;
	cdq->size_nbyte = (u32)size_nbyte;
	spin_lock_init(&cdq->sf_lock);

	ret = nvme_create_cdq_backing(cdq);
	if (ret) {
		kfree(cdq);
		return ret;
	}

	kref_init(&cdq->ref);
	nvme_get_ctrl(cdq->ctrl);

	ret = nvme_get_cdq_tpt(cdq, tpt_fd);
	if (ret)
		goto del_cdqmem;

	ret = nvme_submit_create_cdq_cmd(cdq);
	if (ret)
		goto put_tpt;

	WRITE_ONCE(cdq->valid_mem, true);

	ret = xa_insert(&cdq->ctrl->cdqs, cdq->id, cdq, GFP_KERNEL);
	if (ret)
		goto del_cmd;

	ret = nvme_create_cdqfd(cdq, &cdq_fd);
	if (ret)
		goto del_xarray;

	ret = nvme_start_cdq(cdq);
	if (ret)
		goto del_xarray;
	return 0;

del_xarray:
	nvme_delete_cdq(cdq);
	/*nvme_delete_cdq, has everything */
	return ret;

del_cmd:
	if (nvme_submit_delete_cdq_cmd(cdq))
		WARN_ONCE(1, "Failed delete CDQ (id: %d)", cdq->id);

put_tpt:
	nvme_put_cdq_tpt(cdq);

del_cdqmem:
	/* puts the ref acquired by kref_init */
	nvme_release_cdq_backing(cdq);
	nvme_cdq_put(cdq);

	return ret;
}
EXPORT_SYMBOL_GPL(nvme_create_cdq);

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
