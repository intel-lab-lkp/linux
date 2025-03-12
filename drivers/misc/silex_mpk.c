// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2021 Silex Insight sa
 * Copyright (c) 2018-2021 Beerten Engineering scs
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/cdev.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/idr.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/eventfd.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/bitops.h>
#include <uapi/linux/eventpoll.h>
#include <uapi/misc/silex_mpk.h>

#include "silex_mpk_defs.h"

/*
 * Silex MultiPK device is a PKI offload device, which has multiple PK engines to
 * perform asymmetric crypto operations like ECDSA, RSA, Point Multiplication etc.
 *
 * Following provides the brief overview of the device interface with the software:
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
 * |   v                Silex MultiPK device                |
 * |  +-------------------+     +------------------------+  |
 * |  | New request FIFO  | --> |       PK engines       |  |
 * |  +-------------------+     +------------------------+  |
 * +--------------------------------------------------------+
 *
 * To perform a crypto operation, the software writes a sequence of descriptors,
 * into the RQ memory. This includes input data and designated location for the
 * output data. After preparing the request, request offset (from the RQ memory
 * region) is written into the NEW_REQUEST register. Request is then stored in a
 * common hardware FIFO shared among all RQs. When a PK engine becomes available,
 * device pops the request from the FIFO and fetches the descriptors. It DMAs the
 * input data from RQ memory and executes the necessary computations. After
 * computation is complete, the device writes output data back to RAM via DMA.
 * Device then writes a new entry in CQ ring buffer in RAM, indicating completion
 * of the request. Device also generates an interrupt for notifying completion to
 * the software.
 *
 * RQ memory is provided to the user-space via mmap, so that application can
 * directly write to the descriptors.
 */

#define DRIVER_NAME "multipk"

static struct class *multipk_class;
static int multipk_major, multipk_minor;

static void sx_wrreg(char __iomem *regs, int addr, u64 val)
{
	iowrite64(val, regs + addr);
}

static u64 sx_rdreg(char __iomem *regs, int addr)
{
	return ioread64(regs + addr);
}

static void sx_wrreg32(char __iomem *regs, int addr, u32 val)
{
	iowrite32(val, regs + addr);
}

static u32 sx_rdreg32(char __iomem *regs, int addr)
{
	return ioread32(regs + addr);
}

static void sx_pk_init_cq(struct multipk_dev *mpkdev, struct sx_pk_cq *cq,
			  int szcode, char *base)
{
	cq->mpkdev = mpkdev;
	cq->generation = 1;
	cq->szcode = szcode;
	cq->base = (u32 *)base;
	cq->tail = 0;
}

static int sx_pk_pop_cq(struct sx_pk_cq *cq, int *rid)
{
	u32 status = CQ_STATUS_VALID;
	u32 completion;
	unsigned int sz;

	completion = cq->base[cq->tail + 1];
	if ((completion & 1) != cq->generation) {
		dev_err(cq->mpkdev->dev, "CQ completion error\n");
		return CQ_STATUS_INVALID;
	}
	*rid = (completion >> 16) & 0xffff;
	/* read memory barrier: to avoid a race condition, the status field may
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
	if (completion & 0x2)
		// error reported by the PK scheduler
		status |= CQ_COMPLETION_ERROR;

	return status;
}

static int sx_pk_trigpos(struct sx_pk_cq *cq)
{
	int trigpos;

	trigpos = cq->tail / 2 + (cq->generation << (cq->szcode - 3));
	/* Set trigger position on next completed operation */
	trigpos++;
	trigpos &= (1 << (cq->szcode - 2)) - 1;

	return trigpos;
}

static void cq_work_function(struct kthread_work *work)
{
	struct multipk_dev *mpkdev;
	struct multipk_work *mpkwork;
	struct multipk_user *user;
	int qid, rid, trigpos;
	u32 status;

	mpkwork = container_of(work, struct multipk_work, cq_work);
	mpkdev = mpkwork->mpkdev;
	qid = mpkwork->qid;

	mutex_lock(&mpkdev->lock[qid]);
	/* Return in case mpkdev user is closed */
	if (!mpkdev->users[qid]) {
		mutex_unlock(&mpkdev->lock[qid]);
		return;
	}

	user = mpkwork->user;
	status = sx_pk_pop_cq(&mpkdev->work[qid].pk_cq, &rid);
	if (status != CQ_STATUS_INVALID) {
		u32 *status_mem;

		status_mem = (u32 *)user->shmem;
		status_mem[rid] = status;
		eventfd_signal(user->evfd_ctx[rid]);
	}

	trigpos = sx_pk_trigpos(&mpkdev->work[qid].pk_cq);
	sx_wrreg(mpkdev->regs, REG_CTL_CQ_NTFY(user->qid), trigpos);
	mutex_unlock(&mpkdev->lock[qid]);
}

static int multipk_open(struct inode *inode, struct file *filep)
{
	struct multipk_dev *mpkdev;
	struct multipk_user *user;
	int ret, idx;

	mpkdev = container_of(inode->i_cdev, struct multipk_dev, cdev);
	idx = ida_alloc_range(&mpkdev->available_rqcq, 0, mpkdev->max_queues - 1, GFP_KERNEL);
	if (idx < 0)
		return -ENOSPC;

	get_device(mpkdev->dev);

	user = kzalloc(sizeof(*user), GFP_KERNEL);
	if (!user) {
		ret = -ENOMEM;
		goto err_alloc_user;
	}
	user->mpkdev = mpkdev;
	user->qid = idx;
	user->rq_entries = 0;
	filep->private_data = user;
	mpkdev->users[idx] = user;

	return 0;

err_alloc_user:
	ida_free(&mpkdev->available_rqcq, idx);
	put_device(mpkdev->dev);
	return ret;
}

static long multipk_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct multipk_user *user = filep->private_data;
	struct multipk_conf conf;
	int i, ret = 0;

	/* Extract the type and number */
	if (_IOC_TYPE(cmd) != MULTIPK_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > MULTIPK_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case MULTIPK_CONF:
		if (copy_from_user(&conf, (struct multipk_conf __user *)arg,
				   sizeof(conf)))
			return -EFAULT;
		/* Maximal value of rq_entries is 512. There is 1 CQ of 4K bytes.
		 * Each completion status is 8 Bytes. Only 4096 / 8 = 512 entries
		 * are possible at any time
		 */
		if (conf.max_queue_depth > 512)
			return -ENOTTY;
		/* If queue already opened */
		if (user->cqmem)
			return -ENOTTY;
		user->rq_entries = conf.max_queue_depth;

		for (i = 0; i < MAX_PK_REQS; i++) {
			user->evfd_ctx[i] = eventfd_ctx_fdget(conf.eventfd[i]);
			if (IS_ERR(user->evfd_ctx[i])) {
				dev_err(user->mpkdev->dev, "Invalid eventfd: %d\n",
					conf.eventfd[i]);
				return -ENOTTY;
			}
		}

		break;
	default:
		return -ENOTTY;
	}
	return ret;
}

static void multipk_free_rqcqsh(struct multipk_dev *mpkdev, struct multipk_user *user)
{
	int pages = user->rq_pages;
	int pagemult = pages / 4;
	int i;

	dma_free_coherent(mpkdev->dev, PAGE_SIZE,
			  user->shmem, user->physsh);
	user->shmem = NULL;
	dma_free_coherent(mpkdev->dev, PAGE_SIZE,
			  user->cqmem, user->physcq);
	user->cqmem = NULL;
	for (i = 0; i < pages / pagemult; i++) {
		dma_free_coherent(mpkdev->dev, PAGE_SIZE * pagemult,
				  user->rqmem[i], user->physrq[i]);
		user->rqmem[i] = NULL;
	}
}

static int multipk_release(struct inode *inode, struct file *filep)
{
	struct multipk_user *user = filep->private_data;
	struct multipk_dev *mpkdev = user->mpkdev;
	char __iomem *regs = mpkdev->regs;
	int ret = 0;

	/* As kthread worker handling CQ will be using user memory, free needs
	 * to be done in a lock to avoid race condition.
	 */
	mutex_lock(&mpkdev->lock[user->qid]);
	if (user->rq_entries) {
		unsigned int attempts = 0;
		/* Disable RQCQ pages to help the hardware finish potential
		 * pending requests sooner.
		 */
		sx_wrreg(regs, REG_RQ_CFG_PAGE_SIZE(user->qid), 0);
		sx_wrreg(regs, REG_RQ_CFG_PAGES_WREN(user->qid), 0);
		sx_wrreg(regs, REG_CQ_CFG_SIZE(user->qid), 0);
		/* The hardware does not have a flush mechanism for the requests
		 * pending in the RQ. Instead check periodically with
		 * REG_CTL_PENDING_REQS if the user still has requests going on.
		 * If the hardware never completes the requests, abort after
		 * a MAX_FLUSH_WAIT_ATTEMPTS and don't free the resources.
		 */
		while (sx_rdreg(regs, REG_CTL_BASE(user->qid) + REG_CTL_PENDING_REQS)) {
			attempts++;
			if (attempts > MAX_FLUSH_WAIT_ATTEMPTS) {
				dev_err(mpkdev->dev,
					"Time out waiting for hw completions. Resources leaked.\n");
				goto abort_cleanup;
			}
			msleep(20);
		}

		/* Once memory has been allocated in mmap we need to free more resources */
		if (user->shmem) {
			atomic_add(user->rq_entries, &mpkdev->allowed_reqs);
			multipk_free_rqcqsh(mpkdev, user);
			kthread_destroy_worker(mpkdev->work[user->qid].cq_wq);
			mpkdev->work[user->qid].cq_wq = NULL;
		}
	}
	ida_free(&mpkdev->available_rqcq, user->qid);

abort_cleanup:
	clear_bit(user->qid, &mpkdev->ntfy_mask);
	mpkdev->users[user->qid] = NULL;
	put_device(mpkdev->dev);
	mutex_unlock(&mpkdev->lock[user->qid]);
	kfree(user);

	return ret;
}

static const struct vm_operations_struct multipk_physical_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

static int multipk_mmap_regs(struct vm_area_struct *vma)
{
	struct multipk_user *user = vma->vm_private_data;
	struct multipk_dev *mpkdev = user->mpkdev;

	vma->vm_ops = &multipk_physical_vm_ops;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return io_remap_pfn_range(vma, vma->vm_start,
				  (mpkdev->regsphys + REG_CTL_BASE(user->qid)) >> PAGE_SHIFT,
				  vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static int mmap_dmamem(struct vm_area_struct *vma, struct multipk_dev *mpkdev,
		       void *addr, dma_addr_t phys, off_t offset, size_t sz)
{
	unsigned long vmstart = vma->vm_start;
	unsigned long pgoff = vma->vm_pgoff;
	int ret;

	/* Here vm_pgoff has a fake offset to tell which mapping to do.
	 * Unfortunately, dma_mmap_coherent() does not take an explicit offset
	 * argument. Force it in vma temporarily to 0 to make sure
	 * dma_mmap_coherent() maps the buffer from the beginning.
	 */
	vma->vm_pgoff = 0;
	vma->vm_start = vmstart + offset;
	vma->vm_end = vma->vm_start + sz;
	ret = dma_mmap_coherent(mpkdev->dev, vma, addr, phys, sz);
	vma->vm_pgoff = pgoff;
	vma->vm_start = vmstart;

	return ret;
}

static int multipk_mmap_rqcq(struct vm_area_struct *vma)
{
	struct multipk_user *user = vma->vm_private_data;
	struct multipk_dev *mpkdev = user->mpkdev;
	struct multipk_work *mpkwork = NULL;
	int rq_entries = user->rq_entries;
	int pagemult, pagemultshift;
	int requested_pages;
	char wq_name[32];
	int i, qid, ret;
	int trigpos;

	requested_pages = vma_pages(vma);
	/* As the first page is reserved for the cq and the next ones are for
	 * the rq, the mmap must be at least 2 pages big.
	 */
	if (requested_pages < 2)
		return -EINVAL;
	if (requested_pages > (8 + 1))
		return -EINVAL;
	/* Store number of rq pages. 1 page is reserved for cq */
	user->rq_pages = requested_pages - 1;
	/* Requests memory can have up to 4 hardware pages. All hardware
	 * pages have the same size. If requesting more than 4 OS pages, the
	 * hardware pages will use the same multiple (pagemult) of OS pages.
	 * Thus the requested size for the request queue must be a multiple
	 * of pagemult.
	 */
	pagemult = (requested_pages - 1 + 3) / 4;
	if ((requested_pages - 1) % pagemult != 0)
		return -EINVAL;
	/* hardware page size must be a power of 2, and as a consequence
	 * pagemult too.
	 */
	if ((pagemult & (pagemult - 1)) != 0)
		return -EINVAL;
	/* Set rq_entries if it has not already been set through ioctl */
	if (!rq_entries) {
		/* For the purpose of computing the allowed number of entries in the
		 * request queue, the size of the rq is divided by a typical small
		 * request of 128 bytes (4 descriptors (4 * 8 bytes) with 3 operands of
		 * 32 bytes).
		 */
		rq_entries = ((requested_pages - 1) << PAGE_SHIFT) / 128;
	}
	/* The first page is reserved for the completion queue. With page sizes
	 * of 4KB, this means that a cq can receive at least 4K / (4*2) = 512
	 * completions. To be safe, only up to 512 requests can be pending.
	 */
	if (rq_entries > 512)
		return -EINVAL;
	if (user->cqmem)
		/* Mapping already done */
		return -EINVAL;
	i = atomic_sub_return(rq_entries, &mpkdev->allowed_reqs);
	if (i < 0) {
		/* On failure give back the reserved entries.
		 * This construction could lead to smaller reservations being
		 * refused while a bigger one is going on. This is very
		 * unlikely and should never happen in a correctly dimensioned
		 * system. Thus, there's no need to worry about that.
		 */
		atomic_add(rq_entries, &mpkdev->allowed_reqs);
		return -ENOSPC;
	}

	/* Check if status for RQ entries can be accommodated in PAGE_SIZE */
	if (rq_entries * sizeof(u32) > PAGE_SIZE)
		return -EINVAL;
	user->shmem = dma_alloc_coherent(mpkdev->dev, PAGE_SIZE,
					 &user->physsh, GFP_KERNEL);
	if (!user->shmem) {
		ret = -ENOMEM;
		goto fail;
	}

	user->cqmem = dma_alloc_coherent(mpkdev->dev, PAGE_SIZE,
					 &user->physcq, GFP_KERNEL);
	if (!user->cqmem) {
		ret = -ENOMEM;
		goto fail;
	}

	user->rq_entries = rq_entries;
	qid = user->qid;
	for (i = 0; i < (requested_pages - 1) / pagemult; i++) {
		user->rqmem[i] = dma_alloc_coherent(mpkdev->dev, PAGE_SIZE * pagemult,
						    &user->physrq[i], GFP_KERNEL);
		if (!user->rqmem[i]) {
			ret = -ENOMEM;
			goto fail;
		}
		sx_wrreg(mpkdev->regs, REG_RQ_CFG_PAGE(qid, i),
			 user->physrq[i]);
	}

	vma->vm_ops = &multipk_physical_vm_ops;

	/* Configure unused rq pages with start of allocated shared mem.
	 * Those should not be accessed, but if descriptors of a (malicious)
	 * user writes descriptors for those pages, it will not break the rest
	 * of the system.
	 */
	for (i = (requested_pages - 1) / pagemult; i < 4; i++) {
		sx_wrreg(mpkdev->regs, REG_RQ_CFG_PAGE(qid, i),
			 user->physrq[0]);
	}
	pagemultshift = pagemult - 1;
	pagemultshift = (pagemultshift & 5) + ((pagemultshift & 0xa) >> 1);
	pagemultshift = (pagemultshift & 3) + ((pagemultshift >> 2) & 3);
	sx_wrreg(mpkdev->regs, REG_RQ_CFG_PAGE_SIZE(qid), PAGE_SHIFT + pagemultshift);
	sx_wrreg(mpkdev->regs, REG_RQ_CFG_CQID(qid), qid);
	sx_wrreg(mpkdev->regs, REG_CQ_CFG_IRQ_NR(qid), qid);
	set_bit(qid, &mpkdev->ntfy_mask);
	sx_wrreg(mpkdev->regs, REG_RQ_CFG_PAGES_WREN(qid),
		 (1 << ((requested_pages - 1) / pagemult)));
	sx_wrreg(mpkdev->regs, REG_RQ_CFG_DEPTH(qid), rq_entries);
	sx_wrreg(mpkdev->regs, REG_CQ_CFG_ADDR(qid), user->physcq);
	sx_wrreg(mpkdev->regs, REG_CQ_CFG_SIZE(qid), PAGE_SHIFT);

	mutex_init(&mpkdev->lock[qid]);
	mpkwork = &mpkdev->work[qid];
	sx_pk_init_cq(mpkdev, &mpkwork->pk_cq, 12, user->cqmem);
	mpkwork->qid = qid;
	mpkwork->mpkdev = mpkdev;
	mpkwork->user = user;

	/* set trigger position for notifications */
	trigpos = sx_pk_trigpos(&mpkwork->pk_cq);
	sx_wrreg(mpkdev->regs, REG_CTL_CQ_NTFY(qid), trigpos);

	ret = mmap_dmamem(vma, mpkdev, user->shmem, user->physsh, 0, PAGE_SIZE);
	if (ret)
		goto fail;
	for (i = 0; i < (requested_pages - 1) / pagemult; i++) {
		ret = mmap_dmamem(vma, mpkdev, user->rqmem[i], user->physrq[i],
				  (i * pagemult + 1) * PAGE_SIZE,
				  PAGE_SIZE * pagemult);
		if (ret)
			goto fail;
	}

	snprintf(wq_name, sizeof(wq_name), "cq_worker_%d", qid);
	mpkwork->cq_wq = kthread_create_worker(0, wq_name);
	if (IS_ERR(mpkwork->cq_wq)) {
		ret = -ENOMEM;
		mpkwork->cq_wq = NULL;
		goto fail;
	}

	kthread_init_work(&mpkwork->cq_work, cq_work_function);
	return ret;

fail:
	if (mpkwork && mpkwork->cq_wq) {
		kthread_destroy_worker(mpkwork->cq_wq);
		mpkwork->cq_wq = NULL;
	}
	multipk_free_rqcqsh(mpkdev, user);
	user->rq_entries = 0;
	atomic_add(rq_entries, &mpkdev->allowed_reqs);
	return ret;
}

static int multipk_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct multipk_user *user = filep->private_data;
	int ret = 0;

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;

	vma->vm_private_data = user;

	switch (vma->vm_pgoff) {
	case 0:
		ret = multipk_mmap_regs(vma);
		break;
	case 1:
		ret = multipk_mmap_rqcq(vma);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct file_operations multipk_fops = {
	.owner          = THIS_MODULE,
	.open           = multipk_open,
	.release        = multipk_release,
	.mmap           = multipk_mmap,
	.unlocked_ioctl = multipk_ioctl,
};

static ssize_t hardware_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	int v, hwv, cnt, maxtotalreqs, rqmaxpending, mults;
	struct multipk_dev *mpkdev = dev_get_drvdata(dev);

	v = (int)sx_rdreg(mpkdev->regs, REG_SEMVER);
	hwv = (int)sx_rdreg(mpkdev->regs, REG_HW_VERSION);
	cnt = (int)sx_rdreg(mpkdev->regs, REQ_CFG_REQ_QUEUES_CNT);
	maxtotalreqs = (int)sx_rdreg(mpkdev->regs, REQ_CFG_MAX_PENDING_REQ);
	rqmaxpending = (int)sx_rdreg(mpkdev->regs, REQ_CFG_MAX_REQ_QUEUE_ENTRIES);
	mults = (int)sx_rdreg(mpkdev->regs, REQ_CFG_PK_INST);

	return sprintf(buf,
		"Hardware interface version: %d.%d.%d\n"
		"Hardware implementation version: %d.%d.%d\n"
		"Count request queues: %d\n"
		"Total max pending requests: %d\n"
		"Max pending requests per request queue: %d\n"
		"Pkcores 64 multipliers: %d\n"
		"Pkcores 256 multipliers: %d\n",
		MPK_SEMVER_MAJOR(v), MPK_SEMVER_MINOR(v), MPK_SEMVER_PATCH(v),
		MPK_HWVER_MAJOR(hwv), MPK_HWVER_MINOR(hwv), MPK_HWVER_SVN(hwv),
		cnt, maxtotalreqs, rqmaxpending,
		mults >> 16, mults & 0xFFFF);
}
static DEVICE_ATTR_RO(hardware);

static irqreturn_t multipk_cq_irq(int irq, void *dev)
{
	struct multipk_dev *mpkdev = (struct multipk_dev *)dev;
	u64 active = 0;
	int i;

	active = sx_rdreg(mpkdev->regs, REG_PK_IRQ_STATUS);
	active &= READ_ONCE(mpkdev->ntfy_mask);
	sx_wrreg(mpkdev->regs, REG_PK_IRQ_RESET, active);

	for (i = 0; i < mpkdev->max_queues && active; i++, active >>= 1) {
		if (!(active & 1))
			continue;
		if (!mpkdev->users[i]) {
			dev_err(mpkdev->dev, "multipk: interrupt for unknown CQ %d\n", i);
			continue;
		}
		kthread_queue_work(mpkdev->work[i].cq_wq, &mpkdev->work[i].cq_work);
	}

	return IRQ_HANDLED;
}

static void multipk_dbrg_init(struct multipk_dev *mpkdev)
{
	u32 perso_string[2] = {0xB0A04033, 0xB0A04052};
	int pers_str_len;
	int entropy_len;
	u32 reg_out;
	u32 buffer;
	size_t i;

	reg_out = sx_rdreg32(mpkdev->dbrg_regs, REG_HWCONFIG);
	/* Sizes in 32b words */
	entropy_len = (reg_out >> 12) & 0xF;
	pers_str_len = (reg_out >> 20) & 0xF;
	sx_wrreg32(mpkdev->dbrg_regs, REG_CONFIG,
		   CONFIG_REQUEST_NB_BITS(512) | CONFIG_USE_AES_128);

	/* Reset 32bit data control counter */
	sx_wrreg32(mpkdev->dbrg_regs, REG_DATACTRL, 0x01);
	for (i = 0; i < entropy_len; i++) {
		get_random_bytes(&buffer, sizeof(buffer));
		sx_wrreg32(mpkdev->dbrg_regs, REG_ENTROPYIN, buffer);
	}
	for (i = 0; i < 2 && i < pers_str_len; i++)
		sx_wrreg32(mpkdev->dbrg_regs, REG_PERSSTR, perso_string[i]);

	sx_wrreg32(mpkdev->dbrg_regs, REG_CONTROL,
		   CONTROL_START | CONTROL_USE_ENTROPY_INPUT_REG | CONTROL_DO_INSTANTIATE);
}

static int multipk_create_device(struct multipk_dev *mpkdev, struct device *dev, int irq)
{
	u64 max_total_reqs;
	long magic;
	u64 ver;
	int ret;

	magic = sx_rdreg(mpkdev->regs, REG_MAGIC);
	if (magic != 0x5113C50C) {
		dev_err(dev, "Invalid magic constant %08lx !\n", magic);
		return -ENODEV;
	}
	dev_err(dev, "Correct magic\n");
	ver = sx_rdreg(mpkdev->regs, REG_SEMVER);
	if (MPK_SEMVER_MAJOR(ver) != 1 || MPK_SEMVER_MINOR(ver) < 1) {
		dev_err(dev, "Hardware version (%d.%d) not supported.\n",
			(int)MPK_SEMVER_MAJOR(ver), (int)MPK_SEMVER_MINOR(ver));
		return -ENODEV;
	}

	/* Reset all accelerators and the hw scheduler */
	sx_wrreg(mpkdev->regs, REG_PK_GLOBAL_STATE, 0x1);
	sx_wrreg(mpkdev->regs, REG_PK_GLOBAL_STATE, 0x0);

	mpkdev->max_queues = (int)sx_rdreg(mpkdev->regs, REQ_CFG_REQ_QUEUES_CNT);
	mpkdev->users = devm_kzalloc(dev, sizeof(*mpkdev->users) * mpkdev->max_queues,
				     GFP_KERNEL);

	ida_init(&mpkdev->available_rqcq);
	max_total_reqs = sx_rdreg(mpkdev->regs, REQ_CFG_MAX_PENDING_REQ);
	atomic_set(&mpkdev->allowed_reqs, max_total_reqs);
	mpkdev->max_queues = (int)sx_rdreg(mpkdev->regs, REQ_CFG_REQ_QUEUES_CNT);

	sx_wrreg(mpkdev->regs, REG_IRQ_ENABLE, 0);
	sx_wrreg(mpkdev->regs, REG_PK_IRQ_RESET, ~0);
	sx_wrreg(mpkdev->regs, REG_IRQ_ENABLE, (1 << mpkdev->max_queues) - 1);
	mpkdev->ntfy_mask = 0;

	ret = devm_request_irq(dev, irq, multipk_cq_irq, 0, "multipk", mpkdev);
	if (ret)
		return -ENODEV;

	multipk_dbrg_init(mpkdev);

	if (IS_ERR(device_create(multipk_class, dev,
				 MKDEV(multipk_major, multipk_minor), NULL,
				 "multipk%u", multipk_minor))) {
		dev_err(dev, "can't create device\n");
		ret = -ENODEV;
		goto faildevcreate;
	}
	device_create_file(dev, &dev_attr_hardware);

	cdev_init(&mpkdev->cdev, &multipk_fops);
	mpkdev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&mpkdev->cdev, MKDEV(multipk_major, multipk_minor), 1);
	if (ret)
		goto failcdevadd;
	return 0;

failcdevadd:
	dev_err(dev, "chardev registration failed\n");
	device_remove_file(mpkdev->dev, &dev_attr_hardware);
	device_destroy(multipk_class, MKDEV(multipk_major, multipk_minor));
faildevcreate:
	ida_destroy(&mpkdev->available_rqcq);

	return ret;
}

static void multipk_remove_device(struct multipk_dev *mpkdev)
{
	cdev_del(&mpkdev->cdev);

	device_remove_file(mpkdev->dev, &dev_attr_hardware);
	sx_wrreg(mpkdev->regs, REG_IRQ_ENABLE, 0);
	device_destroy(multipk_class, MKDEV(multipk_major, multipk_minor));
	ida_destroy(&mpkdev->available_rqcq);
}

static int multipk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct multipk_dev *mpkdev;
	struct resource *memres;
	int irq, ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret < 0)
		return ret;

	mpkdev = devm_kzalloc(dev, sizeof(*mpkdev), GFP_KERNEL);
	if (!mpkdev)
		return -ENOMEM;
	mpkdev->dev = dev;

	memres = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mpkdev->regs = devm_ioremap_resource(dev, memres);
	if (IS_ERR(mpkdev->regs))
		return PTR_ERR(mpkdev->regs);
	mpkdev->regsphys = memres->start;
	memres = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	mpkdev->dbrg_regs = devm_ioremap_resource(dev, memres);
	if (IS_ERR(mpkdev->dbrg_regs))
		return PTR_ERR(mpkdev->dbrg_regs);
	mpkdev->dbrg_regsphys = memres->start;
	platform_set_drvdata(pdev, mpkdev);

	/* Only a single IRQ is supported */
	if (platform_irq_count(pdev) != 1)
		return -ENODEV;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -ENODEV;

	return multipk_create_device(mpkdev, dev, irq);
}

static void multipk_remove(struct platform_device *pdev)
{
	struct multipk_dev *mpkdev = platform_get_drvdata(pdev);

	multipk_remove_device(mpkdev);
}

static const struct of_device_id multipk_match[] = {
	{ .compatible = "multipk" },
	{ },
};

static struct platform_driver multipk_pdrv = {
	.probe = multipk_probe,
	.remove = multipk_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = of_match_ptr(multipk_match),
	},
};

static int __init multipk_init(void)
{
	dev_t devt;
	int ret;

	multipk_class = class_create("multipk");
	if (IS_ERR(multipk_class)) {
		ret = PTR_ERR(multipk_class);
		pr_err("can't register class\n");
		goto err;
	}
	ret = alloc_chrdev_region(&devt, 0, MULTIPK_MAX_DEVICES, "multipk");
	if (ret) {
		pr_err("can't register character device\n");
		goto err_class;
	}
	multipk_major = MAJOR(devt);
	multipk_minor = MINOR(devt);

	ret = platform_driver_register(&multipk_pdrv);
	if (ret) {
		pr_err("can't register platform driver\n");
		goto err_unchr;
	}

	return 0;
err_unchr:
	unregister_chrdev_region(devt, MULTIPK_MAX_DEVICES);
err_class:
	class_destroy(multipk_class);
err:
	return ret;
}

static void __exit multipk_exit(void)
{
	platform_driver_unregister(&multipk_pdrv);

	unregister_chrdev_region(MKDEV(multipk_major, 0), MULTIPK_MAX_DEVICES);

	class_destroy(multipk_class);
}

module_init(multipk_init);
module_exit(multipk_exit);

MODULE_DESCRIPTION("Driver for Silex Multipk Asymmetric crypto accelerator");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
