// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2021 Silex Insight sa
 * Copyright (c) 2018-2021 Beerten Engineering scs
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

/*
 * Device Overview
 * ===============
 * AMD PKI accelerator is a device on AMD versal-net to execute public
 * key asymmetric crypto operations like ECDSA, ECDH, RSA etc. with high
 * performance. The driver provides accel interface to applications for
 * configuring the device and performing the required operations. AMD PKI
 * device comprises of multiple Barco Silex ba414 PKI engines bundled together,
 * and providing a queue based interface to interact with these devices on AMD
 * versal-net.
 *
 * Following figure provides the brief overview of the device interface with
 * the software:
 *
 * +------------------+
 * |    Software      |
 * +------------------+
 *     |          |
 *     |          v
 *     |     +-----------------------------------------------------------+
 *     |     |                     RAM                                   |
 *     |     |  +----------------------------+   +---------------------+ |
 *     |     |  |           RQ pages         |   |       CQ pages      | |
 *     |     |  | +------------------------+ |   | +-----------------+ | |
 *     |     |  | |   START (cmd)          | |   | | req_id | status | | |
 *     |     |  | |   TFRI (addr, sz)---+  | |   | | req_id | status | | |
 *     |     |  | | +-TFRO (addr, sz)   |  | |   | | ...             | | |
 *     |     |  | | | NTFY (req_id)     |  | |   | +-----------------+ | |
 *     |     |  | +-|-------------------|--+ |   |                     | |
 *     |     |  |   |                   v    |   +---------------------+ |
 *     |     |  |   |         +-----------+  |                           |
 *     |     |  |   |         | input     |  |                           |
 *     |     |  |   |         | data      |  |                           |
 *     |     |  |   v         +-----------+  |                           |
 *     |     |  |  +----------------+        |                           |
 *     |     |  |  |  output data   |        |                           |
 *     |     |  |  +----------------+        |                           |
 *     |     |  +----------------------------+                           |
 *     |     |                                                           |
 *     |     +-----------------------------------------------------------+
 *     |
 *     |
 * +---|----------------------------------------------------+
 * |   v                AMD PKI device                      |
 * |  +-------------------+     +------------------------+  |
 * |  | New request FIFO  | --> |       PK engines       |  |
 * |  +-------------------+     +------------------------+  |
 * +--------------------------------------------------------+
 *
 * To perform a crypto operation, the software writes a sequence of descriptors,
 * into the RQ memory. This includes input data and designated location for the
 * output data. After preparing the request, request offset (from the RQ memory
 * region) is written into the NEW_REQUEST register. Request is then stored in a
 * common hardware FIFO shared among all RQs.
 *
 * When a PK engine becomes available, device pops the request from the FIFO and
 * fetches the descriptors. It DMAs the input data from RQ memory and executes
 * the necessary computations. After computation is complete, the device writes
 * output data back to RAM via DMA. Device then writes a new entry in CQ ring
 * buffer in RAM, indicating completion of the request. Device also generates
 * an interrupt for notifying completion to the software.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/eventfd.h>
#include <drm/drm_accel.h>
#include <drm/drm_ioctl.h>

#include "amdpk_drv.h"

#define DRIVER_NAME "amdpk"

static void amdpk_init_cq(struct amdpk_dev *pkdev, struct amdpk_cq *cq,
			  int szcode, char *base)
{
	cq->pkdev = pkdev;
	cq->generation = 1;
	cq->szcode = szcode;
	cq->base = (u32 *)base;
	cq->tail = 0;
}

static int amdpk_pop_cq(struct amdpk_cq *cq, int *rid)
{
	u32 status = CQ_STATUS_VALID;
	unsigned int sz;
	u32 completion;

	completion = cq->base[cq->tail + 1];
	if ((completion & CQ_GENERATION_BIT) != cq->generation)
		return CQ_STATUS_INVALID;

	*rid = (completion >> 16) & 0xffff;
	/* read memory barrier: to avoid a race condition, the status field should
	 * not be read before the completion generation bit. Otherwise we could
	 * get stale outdated status data.
	 */
	rmb();
	status |= cq->base[cq->tail];
	/* advance completion queue tail */
	cq->tail += 2;
	sz = 1 << (cq->szcode - 2);
	if (cq->tail >= sz) {
		cq->tail = 0;
		cq->generation ^= 1; /* invert generation bit */
	}

	/* evaluate status from the completion queue */
	if (completion & CQ_COMPLETION_BIT)
		status |= CQ_COMPLETION_ERROR;

	return status;
}

static int amdpk_trigpos(struct amdpk_cq *cq)
{
	int trigpos;

	/* Set trigger position on next completed operation */
	trigpos = cq->tail / 2 + (cq->generation << (cq->szcode - 3));
	trigpos++;
	trigpos &= (1 << (cq->szcode - 2)) - 1;

	return trigpos;
}

static void amdpk_cq_workfn(struct kthread_work *work)
{
	struct amdpk_work *pkwork;
	struct amdpk_dev *pkdev;
	struct amdpk_user *user;
	int qid, rid, trigpos;
	u32 status;

	pkwork = to_amdpk_work(work);
	pkdev = pkwork->pkdev;
	qid = pkwork->qid;

	user = pkwork->user;
	status = amdpk_pop_cq(&pkdev->work[qid]->pk_cq, &rid);
	if (rid < user->rq_entries && status != CQ_STATUS_INVALID) {
		u32 *status_mem;

		status_mem = (u32 *)user->stmem;
		status_mem[rid] = status;
		eventfd_signal(user->evfd_ctx[rid]);
	}

	trigpos = amdpk_trigpos(&pkdev->work[qid]->pk_cq);
	pk_wrreg(pkdev->regs, REG_CTL_CQ_NTFY(user->qid), trigpos);
}

static irqreturn_t amdpk_cq_irq(int irq, void *dev)
{
	struct amdpk_dev *pkdev = (struct amdpk_dev *)dev;
	u64 active = 0;
	int i;

	active = pk_rdreg(pkdev->regs, REG_PK_IRQ_STATUS);
	pk_wrreg(pkdev->regs, REG_PK_IRQ_RESET, active);

	for (i = 0; i < pkdev->max_queues && active; i++, active >>= 1) {
		if (!(active & 1))
			continue;
		if (!pkdev->users[i])
			continue;
		kthread_queue_work(pkdev->work[i]->cq_wq, &pkdev->work[i]->cq_work);
	}

	return IRQ_HANDLED;
}

static void amdpk_free_rqmem(struct amdpk_dev *pkdev, struct amdpk_user *user)
{
	int pages = user->rq_pages;
	int pagemult = pages / 4;
	int i;

	for (i = 0; i < pages / pagemult; i++) {
		if (!user->rqmem[i])
			continue;
		dma_free_coherent(pkdev->dev, PAGE_SIZE * pagemult,
				  user->rqmem[i], user->physrq[i]);
		user->rqmem[i] = NULL;
	}
}

static int amdpk_accel_get_info(struct drm_device *dev, void *data, struct drm_file *fp)
{
	struct amdpk_user *user = fp->driver_priv;
	struct amdpk_dev *pkdev = user->pkdev;
	struct amdpk_info *info = data;

	info->avail_qdepth = atomic_read(&pkdev->avail_qdepth);
	return 0;
}

static int amdpk_accel_configure(struct amdpk_user *user, struct amdpk_conf *conf)
{
	struct amdpk_dev *pkdev = user->pkdev;
	struct amdpk_work *pkwork = NULL;
	int qid = user->qid;
	int trigpos, ret, i;
	char wq_name[32];

	i = atomic_sub_return(conf->qdepth, &pkdev->avail_qdepth);
	if (i < 0) {
		/* If enough entries are not present, give back the reserved entries. */
		dev_err(user->pkdev->dev, "Out of descriptors\n");
		atomic_add(conf->qdepth, &pkdev->avail_qdepth);
		return -ENOSPC;
	}
	user->rq_entries = conf->qdepth;

	for (i = 0; i < user->rq_entries; i++) {
		if (conf->eventfd[i] <= 0) {
			dev_err(user->pkdev->dev, "Invalid eventfd: %d\n", conf->eventfd[i]);
			ret = -EINVAL;
			goto fail;
		}

		user->evfd_ctx[i] = eventfd_ctx_fdget(conf->eventfd[i]);
		if (IS_ERR(user->evfd_ctx[i])) {
			dev_err(user->pkdev->dev, "Invalid eventfd: %d\n", conf->eventfd[i]);
			ret = PTR_ERR(user->evfd_ctx[i]);
			goto fail;
		}
	}

	user->cqmem = dma_alloc_coherent(pkdev->dev, PAGE_SIZE, &user->physcq, GFP_KERNEL);
	if (!user->cqmem) {
		ret = -ENOMEM;
		goto fail;
	}

	/* Initialize completion queue handler */
	pkwork = pkdev->work[qid];
	amdpk_init_cq(pkdev, &pkwork->pk_cq, __builtin_ctz(PAGE_SIZE), user->cqmem);

	snprintf(wq_name, sizeof(wq_name), "cq_worker_%d", qid);
	pkwork->cq_wq = kthread_create_worker(0, wq_name);
	if (IS_ERR(pkwork->cq_wq)) {
		ret = PTR_ERR(pkwork->cq_wq);
		pkwork->cq_wq = NULL;
		goto fail;
	}
	kthread_init_work(&pkwork->cq_work, amdpk_cq_workfn);

	pk_wrreg(pkdev->regs, REG_CQ_CFG_IRQ_NR(qid), qid);
	pk_wrreg(pkdev->regs, REG_CQ_CFG_ADDR(qid), user->physcq);
	pk_wrreg(pkdev->regs, REG_CQ_CFG_SIZE(qid), PAGE_SHIFT);
	pk_wrreg(pkdev->regs, REG_RQ_CFG_CQID(qid), qid);
	pk_wrreg(pkdev->regs, REG_RQ_CFG_DEPTH(qid), user->rq_entries);

	/* set trigger position for notifications */
	trigpos = amdpk_trigpos(&pkwork->pk_cq);
	pk_wrreg(pkdev->regs, REG_CTL_CQ_NTFY(qid), trigpos);

	return 0;
fail:
	if (pkwork->cq_wq) {
		kthread_destroy_worker(pkwork->cq_wq);
		pkwork->cq_wq = NULL;
	}
	if (user->cqmem) {
		dma_free_coherent(pkdev->dev, PAGE_SIZE, user->cqmem, user->physcq);
		user->cqmem = NULL;
	}
	atomic_add(user->rq_entries, &pkdev->avail_qdepth);
	user->rq_entries = 0;

	return ret;
}

static int amdpk_accel_set_conf(struct drm_device *dev, void *data, struct drm_file *fp)
{
	struct amdpk_user *user = fp->driver_priv;
	struct amdpk_conf *conf = data;
	int ret;

	if (conf->qdepth == 0 || conf->qdepth > MAX_CQ_ENTRIES_ON_PAGE) {
		dev_err(user->pkdev->dev, "Invalid qdepth: %d\n", conf->qdepth);
		return -EINVAL;
	}

	if (user->configured) {
		dev_err(user->pkdev->dev, "User already configured\n");
		return -EEXIST;
	}

	ret = amdpk_accel_configure(user, conf);
	if (ret)
		return ret;

	user->configured = true;
	return 0;
}

static int amdpk_mmap_regs(struct vm_area_struct *vma)
{
	struct amdpk_user *user = vma->vm_private_data;
	struct amdpk_dev *pkdev = user->pkdev;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return io_remap_pfn_range(vma, vma->vm_start,
				  (pkdev->regsphys + REG_CTL_BASE(user->qid)) >> PAGE_SHIFT,
				  vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static int mmap_dmamem(struct vm_area_struct *vma, struct amdpk_dev *pkdev,
		       void *addr, dma_addr_t phys, off_t offset, size_t sz)
{
	unsigned long vmstart = vma->vm_start;
	unsigned long pgoff = vma->vm_pgoff;
	int ret;

	vma->vm_pgoff = 0;
	vma->vm_start = vmstart + offset;
	vma->vm_end = vma->vm_start + sz;
	ret = dma_mmap_coherent(pkdev->dev, vma, addr, phys, sz);
	vma->vm_pgoff = pgoff;
	vma->vm_start = vmstart;

	return ret;
}

static int amdpk_mmap_mem(struct vm_area_struct *vma)
{
	struct amdpk_user *user = vma->vm_private_data;
	struct amdpk_dev *pkdev = user->pkdev;
	int pagemult, pagemultshift;
	int requested_pages;
	int qid = user->qid;
	int ret, i;

	if (!user->configured) {
		dev_err(pkdev->dev, "configuration not found!");
		return -ENODEV;
	}
	/* Mapping already done */
	if (user->stmem) {
		dev_err(pkdev->dev, "memory already mapped\n");
		return -EINVAL;
	}

	requested_pages = vma_pages(vma);
	/* As the last page is reserved for the status and the starting ones are for
	 * the rq, the mmap must be at least 2 pages big.
	 */
	if (requested_pages < 2) {
		dev_err(pkdev->dev, "Invalid request pages: %d\n", requested_pages);
		return -EINVAL;
	}
	/* Store number of rq pages. 1 page is reserved for status */
	user->rq_pages = requested_pages - 1;
	/* Requests memory can have up to 4 hardware pages. All hardware pages have the
	 * same size. If requesting more than 4 OS pages, the hardware pages will use
	 * the same multiple (pagemult) of OS pages. Thus the requested size for the
	 * request queue must be a multiple of pagemult.
	 */
	pagemult = (requested_pages - 1 + 3) / 4;
	if ((requested_pages - 1) % pagemult != 0) {
		dev_err(pkdev->dev, "requested pages: %d not multiple of page multiplier: %d\n",
			requested_pages, pagemult);
		return -EINVAL;
	}
	/* hardware page size must be a power of 2, and as a consequence pagemult too. */
	if ((pagemult & (pagemult - 1)) != 0) {
		dev_err(pkdev->dev, "page multiplier: %d is not power of 2\n", pagemult);
		return -EINVAL;
	}

	for (i = 0; i < (requested_pages - 1) / pagemult; i++) {
		user->rqmem[i] = dma_alloc_coherent(pkdev->dev, PAGE_SIZE * pagemult,
						    &user->physrq[i], GFP_KERNEL);
		if (!user->rqmem[i]) {
			ret = -ENOMEM;
			goto fail;
		}
		pk_wrreg(pkdev->regs, REG_RQ_CFG_PAGE(qid, i), user->physrq[i]);
	}

	user->stmem = dma_alloc_coherent(pkdev->dev, PAGE_SIZE, &user->physst, GFP_KERNEL);
	if (!user->stmem) {
		ret = -ENOMEM;
		goto fail;
	}

	/* Configure unused rq pages with start of allocated shared mem. Those should not
	 * be accessed, but if descriptors of a (malicious) user writes descriptors for
	 * those pages, it will not break the rest of the system.
	 */
	for (i = (requested_pages - 1) / pagemult; i < MAX_RQMEM_PER_QUEUE; i++)
		pk_wrreg(pkdev->regs, REG_RQ_CFG_PAGE(qid, i), user->physrq[0]);

	pagemultshift = pagemult - 1;
	pagemultshift = (pagemultshift & 5) + ((pagemultshift & 0xa) >> 1);
	pagemultshift = (pagemultshift & 3) + ((pagemultshift >> 2) & 3);
	pk_wrreg(pkdev->regs, REG_RQ_CFG_PAGE_SIZE(qid), PAGE_SHIFT + pagemultshift);
	pk_wrreg(pkdev->regs, REG_RQ_CFG_PAGES_WREN(qid),
		 (1 << ((requested_pages - 1) / pagemult)));

	ret = mmap_dmamem(vma, pkdev, user->stmem, user->physst, 0, PAGE_SIZE);
	if (ret)
		goto fail;
	for (i = 0; i < (requested_pages - 1) / pagemult; i++) {
		ret = mmap_dmamem(vma, pkdev, user->rqmem[i], user->physrq[i],
				  (i * pagemult + 1) * PAGE_SIZE, PAGE_SIZE * pagemult);
		if (ret)
			goto fail;
	}

	return 0;

fail:
	amdpk_free_rqmem(pkdev, user);
	if (user->stmem) {
		dma_free_coherent(pkdev->dev, PAGE_SIZE, user->stmem, user->physst);
		user->stmem = NULL;
	}
	return ret;
}

static int amdpk_accel_mmap(struct file *fp, struct vm_area_struct *vma)
{
	struct drm_file *dfp = fp->private_data;
	struct amdpk_user *user;
	int ret = 0;

	user = dfp->driver_priv;
	if (vma->vm_end < vma->vm_start)
		return -EINVAL;

	vma->vm_private_data = user;

	switch (vma->vm_pgoff) {
	case AMDPK_MMAP_REGS:
		ret = amdpk_mmap_regs(vma);
		break;
	case AMDPK_MMAP_MEM:
		ret = amdpk_mmap_mem(vma);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int amdpk_open(struct drm_device *dev, struct drm_file *file)
{
	struct amdpk_work *pkwork = NULL;
	struct amdpk_user *user = NULL;
	struct amdpk_dev *pkdev;
	int ret, qid;

	pkdev = to_amdpk_dev(dev);
	qid = ida_alloc_range(&pkdev->avail_queues, 0, pkdev->max_queues - 1, GFP_KERNEL);
	if (qid < 0)
		return -ENOSPC;

	get_device(pkdev->dev);

	user = kzalloc(sizeof(*user), GFP_KERNEL);
	if (!user) {
		ret = -ENOMEM;
		goto fail;
	}
	user->pkdev = pkdev;
	user->qid = qid;
	user->rq_entries = 0;
	file->driver_priv = user;
	pkdev->users[qid] = user;

	pkwork = kzalloc(sizeof(*pkwork), GFP_KERNEL);
	if (!pkwork) {
		ret = -ENOMEM;
		goto fail;
	}
	pkwork->qid = qid;
	pkwork->pkdev = pkdev;
	pkwork->user = user;
	pkdev->work[qid] = pkwork;

	return 0;

fail:
	kfree(user);
	kfree(pkwork);
	ida_free(&pkdev->avail_queues, qid);
	put_device(pkdev->dev);
	return ret;
}

static void amdpk_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct amdpk_user *user = file->driver_priv;
	struct amdpk_dev *pkdev = user->pkdev;
	char __iomem *regs = pkdev->regs;

	/* Set pkdev->users[qid] to NULL first, so that Completion interrupt handler gets
	 * to know that this user will not exists, and so it does not schedule any completion
	 * work on cq worker kthread.
	 */
	pkdev->users[user->qid] = NULL;

	if (user->configured) {
		unsigned int attempts = 0;

		/* Disable RQCQ pages to help the hardware finish potential
		 * pending requests sooner.
		 */
		pk_wrreg(regs, REG_RQ_CFG_PAGE_SIZE(user->qid), 0);
		pk_wrreg(regs, REG_RQ_CFG_PAGES_WREN(user->qid), 0);
		pk_wrreg(regs, REG_CQ_CFG_SIZE(user->qid), 0);

		/* The hardware does not have a flush mechanism for the requests pending
		 * in the RQ. Instead check periodically with REG_CTL_PENDING_REQS if the
		 * user still has requests going on. If the hardware never completes the
		 * requests, abort after a MAX_FLUSH_WAIT_ATTEMPTS and don't free resources.
		 */
		while (pk_rdreg(regs, REG_CTL_BASE(user->qid) + REG_CTL_PENDING_REQS)) {
			attempts++;
			if (attempts > MAX_FLUSH_WAIT_ATTEMPTS) {
				dev_err(pkdev->dev,
					"Time out waiting for hw completions. Resources leaked.\n");
				goto abort_cleanup;
			}
			msleep(20);
		}

		if (pkdev->work[user->qid]->cq_wq) {
			kthread_cancel_work_sync(&pkdev->work[user->qid]->cq_work);
			kthread_destroy_worker(pkdev->work[user->qid]->cq_wq);
		}

		amdpk_free_rqmem(pkdev, user);
		if (user->cqmem) {
			dma_free_coherent(pkdev->dev, PAGE_SIZE, user->cqmem, user->physcq);
			user->cqmem = NULL;
		}
		if (user->stmem) {
			dma_free_coherent(pkdev->dev, PAGE_SIZE, user->stmem, user->physst);
			user->stmem = NULL;
		}

		atomic_add(user->rq_entries, &pkdev->avail_qdepth);
	}
	ida_free(&pkdev->avail_queues, user->qid);

abort_cleanup:
	put_device(pkdev->dev);
	kfree(pkdev->work[user->qid]);
	pkdev->work[user->qid] = NULL;
	kfree(user);
}

static const struct drm_ioctl_desc amdpk_accel_ioctls[] = {
	DRM_IOCTL_DEF_DRV(AMDPK_GET_INFO, amdpk_accel_get_info, 0),
	DRM_IOCTL_DEF_DRV(AMDPK_SET_CONF, amdpk_accel_set_conf, 0),
};

static const struct file_operations amdpk_accel_fops = {
	.owner		= THIS_MODULE,
	.open		= accel_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
	.compat_ioctl	= drm_compat_ioctl,
	.llseek		= noop_llseek,
	.mmap		= amdpk_accel_mmap,
};

static const struct drm_driver amdpk_accel_driver = {
	.driver_features	= DRIVER_COMPUTE_ACCEL,

	.name			= "amdpk_accel_driver",
	.desc			= "AMD PKI Accelerator for versal-net",

	.fops			= &amdpk_accel_fops,
	.open			= amdpk_open,
	.postclose		= amdpk_postclose,

	.ioctls			= amdpk_accel_ioctls,
	.num_ioctls		= ARRAY_SIZE(amdpk_accel_ioctls),
};

static int amdpk_create_device(struct amdpk_dev *pkdev, struct device *dev, int irq)
{
	u64 qdepth, ver;
	long magic;
	int ret;

	magic = pk_rdreg(pkdev->regs, REG_MAGIC);
	if (magic != AMDPK_MAGIC) {
		dev_err(dev, "Invalid magic constant %08lx !\n", magic);
		return -ENODEV;
	}
	ver = pk_rdreg(pkdev->regs, REG_SEMVER);
	if (AMDPK_SEMVER_MAJOR(ver) != 1 || AMDPK_SEMVER_MINOR(ver) < 1) {
		dev_err(dev, "Hardware version (%d.%d) not supported.\n",
			(int)AMDPK_SEMVER_MAJOR(ver), (int)AMDPK_SEMVER_MINOR(ver));
		return -ENODEV;
	}

	/* Reset all accelerators and the hw scheduler */
	pk_wrreg(pkdev->regs, REG_PK_GLOBAL_STATE, 0x1);
	pk_wrreg(pkdev->regs, REG_PK_GLOBAL_STATE, 0x0);

	pkdev->max_queues = (int)pk_rdreg(pkdev->regs, REG_CFG_REQ_QUEUES_CNT);
	qdepth = pk_rdreg(pkdev->regs, REG_CFG_MAX_PENDING_REQ);
	atomic_set(&pkdev->avail_qdepth, qdepth);
	pkdev->max_queues = (int)pk_rdreg(pkdev->regs, REG_CFG_REQ_QUEUES_CNT);

	pk_wrreg(pkdev->regs, REG_IRQ_ENABLE, 0);
	pk_wrreg(pkdev->regs, REG_PK_IRQ_RESET, ~0);
	pk_wrreg(pkdev->regs, REG_IRQ_ENABLE, (1 << pkdev->max_queues) - 1);

	ret = devm_request_irq(dev, irq, amdpk_cq_irq, 0, "amdpk", pkdev);
	if (ret)
		return ret;

	ida_init(&pkdev->avail_queues);

	return 0;
}

static void amdpk_remove_device(struct amdpk_dev *pkdev)
{
	drm_dev_unplug(&pkdev->ddev);
	pk_wrreg(pkdev->regs, REG_IRQ_ENABLE, 0);
	ida_destroy(&pkdev->avail_queues);
}

static int amdpk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct amdpk_dev *pkdev;
	struct resource *memres;
	int irq, ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret < 0)
		return ret;

	pkdev = devm_drm_dev_alloc(dev, &amdpk_accel_driver, typeof(*pkdev), ddev);
	if (IS_ERR(pkdev))
		return PTR_ERR(pkdev);
	pkdev->dev = dev;

	memres = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pkdev->regs = devm_ioremap_resource(dev, memres);
	if (IS_ERR(pkdev->regs))
		return PTR_ERR(pkdev->regs);
	pkdev->regsphys = memres->start;
	platform_set_drvdata(pdev, pkdev);

	if (platform_irq_count(pdev) != 1)
		return -ENODEV;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -ENODEV;

	ret = drm_dev_register(&pkdev->ddev, 0);
	if (ret) {
		dev_err(&pdev->dev, "DRM register failed, ret %d", ret);
		return ret;
	}
	amdpk_debugfs_init(pkdev);

	return amdpk_create_device(pkdev, dev, irq);
}

static void amdpk_remove(struct platform_device *pdev)
{
	struct amdpk_dev *pkdev = platform_get_drvdata(pdev);

	amdpk_remove_device(pkdev);
}

static void amdpk_shutdown(struct platform_device *pdev)
{
	amdpk_remove(pdev);
}

static const struct of_device_id amdpk_match_table[] = {
	{ .compatible = "amd,versal-net-pki" },
	{ },
};
MODULE_DEVICE_TABLE(of, amdpk_match_table);

static struct platform_driver amdpk_pdrv = {
	.probe = amdpk_probe,
	.remove = amdpk_remove,
	.shutdown = amdpk_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = amdpk_match_table,
	},
};

static int __init amdpk_init(void)
{
	int ret;

	ret = platform_driver_register(&amdpk_pdrv);
	if (ret) {
		pr_err("can't register platform driver\n");
		return ret;
	}

	return 0;
}

static void __exit amdpk_exit(void)
{
	platform_driver_unregister(&amdpk_pdrv);
}

module_init(amdpk_init);
module_exit(amdpk_exit);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("AMD PKI accelerator for versal-net");
MODULE_LICENSE("GPL");
