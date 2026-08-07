// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 * Copyright(c) 2015-2020 Intel Corporation.
 */

#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <linux/sched/mm.h>
#include <linux/bitmap.h>

#include <rdma/ib.h>

#include "hfi2.h"
#include "affinity.h"
#include "pio.h"
#include "common.h"
#include "trace.h"
#include "mmu_rb.h"
#include "user_sdma.h"
#include "user_exp_rcv.h"
#include "pinning.h"
#include "file_ops.h"

#undef pr_fmt
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#define SEND_CTXT_HALT_TIMEOUT 1000 /* msecs */

/*
 * File operation functions
 */
static u64 kvirt_to_phys(void *addr);
static void init_subctxts(struct hfi2_ctxtdata *uctxt,
			  const struct hfi2_assign_ctxt_cmd *uinfo);
static int init_user_ctxt(struct hfi2_filedata *fd,
			  struct hfi2_ctxtdata *uctxt);
static void user_init(struct hfi2_ctxtdata *uctxt);
static int setup_base_ctxt(struct hfi2_filedata *fd,
			   struct hfi2_ctxtdata *uctxt);
static int setup_subctxt(struct hfi2_ctxtdata *uctxt);

static int find_sub_ctxt(struct hfi2_filedata *fd,
			 const struct hfi2_assign_ctxt_cmd *uinfo);
static int allocate_ctxt(struct hfi2_filedata *fd,
			 const struct hfi2_assign_ctxt_cmd *uinfo,
			 struct hfi2_ctxtdata **cd);
static void deallocate_ctxt(struct hfi2_ctxtdata *uctxt);
static vm_fault_t vma_fault(struct vm_fault *vmf);

static const struct vm_operations_struct vm_ops = {
	.fault = vma_fault,
};

/*
 * Masks and offsets defining the mmap tokens
 */
#define HFI2_MMAP_OFFSET_MASK 0xfffULL
#define HFI2_MMAP_OFFSET_SHIFT 0
#define HFI2_MMAP_SUBCTXT_MASK 0xfULL
#define HFI2_MMAP_SUBCTXT_SHIFT 12
#define HFI2_MMAP_CTXT_MASK 0xffULL
#define HFI2_MMAP_CTXT_SHIFT 16
#define HFI2_MMAP_TYPE_MASK 0xfULL
#define HFI2_MMAP_TYPE_SHIFT 24
#define HFI2_MMAP_MAGIC_MASK 0xffffffffULL
#define HFI2_MMAP_MAGIC_SHIFT 32

#define HFI2_MMAP_MAGIC 0xdabbad00

#define HFI2_MMAP_TOKEN_SET(field, val) \
	(((val) & HFI2_MMAP_##field##_MASK) << HFI2_MMAP_##field##_SHIFT)
#define HFI2_MMAP_TOKEN_GET(field, token) \
	(((token) >> HFI2_MMAP_##field##_SHIFT) & HFI2_MMAP_##field##_MASK)
#define HFI2_MMAP_TOKEN(type, ctxt, subctxt, addr)                           \
	(HFI2_MMAP_TOKEN_SET(MAGIC, HFI2_MMAP_MAGIC) |                       \
	 HFI2_MMAP_TOKEN_SET(TYPE, type) | HFI2_MMAP_TOKEN_SET(CTXT, ctxt) | \
	 HFI2_MMAP_TOKEN_SET(SUBCTXT, subctxt) |                             \
	 HFI2_MMAP_TOKEN_SET(OFFSET, (offset_in_page(addr))))

#define dbg(fmt, ...) pr_info(fmt, ##__VA_ARGS__)

static inline int is_valid_mmap(u64 token)
{
	return (HFI2_MMAP_TOKEN_GET(MAGIC, token) == HFI2_MMAP_MAGIC);
}

struct hfi2_filedata *hfi2_alloc_filedata(struct hfi2_devdata *dd)
{
	struct hfi2_filedata *fd;

	/* The real work is performed later in assign_ctxt() */

	fd = kzalloc_obj(fd, GFP_KERNEL);

	if (!fd || init_srcu_struct(&fd->pq_srcu))
		goto nomem;
	spin_lock_init(&fd->pq_rcu_lock);
	spin_lock_init(&fd->tid_lock);
	spin_lock_init(&fd->invalid_lock);
	fd->dd = dd;
	/* no port yet */
	fd->ppd = NULL;
	return fd;
nomem:
	kfree(fd);
	return NULL;
}

ssize_t hfi2_do_write_iter(struct hfi2_filedata *fd, struct iov_iter *from)
{
	struct hfi2_user_sdma_pkt_q *pq;
	struct hfi2_user_sdma_comp_q *cq = fd->cq;
	int done = 0, reqs = 0;
	unsigned long dim = from->nr_segs;
	int idx;

	if (!HFI2_CAP_IS_KSET(SDMA))
		return -EINVAL;
	if (!user_backed_iter(from))
		return -EINVAL;
	idx = srcu_read_lock(&fd->pq_srcu);
	pq = srcu_dereference(fd->pq, &fd->pq_srcu);
	if (!cq || !pq) {
		srcu_read_unlock(&fd->pq_srcu, idx);
		return -EIO;
	}

	trace_hfi2_sdma_request(fd->dd, fd->uctxt->ctxt, fd->subctxt, dim);

	if (atomic_read(&pq->n_reqs) == pq->n_max_reqs) {
		srcu_read_unlock(&fd->pq_srcu, idx);
		return -ENOSPC;
	}

	while (dim) {
		const struct iovec *iov = iter_iov(from);
		int ret;
		unsigned long count = 0;

		ret = hfi2_user_sdma_process_request(
			fd, (struct iovec *)(iov + done), dim, &count);
		if (ret) {
			reqs = ret;
			break;
		}
		dim -= count;
		done += count;
		reqs++;
	}

	srcu_read_unlock(&fd->pq_srcu, idx);
	return reqs;
}

static inline void mmap_cdbg(u16 ctxt, u16 subctxt, u8 type, u8 mapio, u8 vmf,
			     u64 memaddr, void *memvirt, dma_addr_t memdma,
			     ssize_t memlen, struct vm_area_struct *vma)
{
	hfi2_cdbg(
		PROC,
		"%u:%u type:%u io/vf/dma:%d/%d/%d, addr:0x%llx, len:%lu(%lu), flags:0x%lx",
		ctxt, subctxt, type, mapio, vmf, !!memdma,
		memaddr ?: (u64)memvirt, memlen, vma->vm_end - vma->vm_start,
		vma->vm_flags);
}

int hfi2_do_mmap(struct hfi2_filedata *fd, u8 type, struct vm_area_struct *vma)
{
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	struct hfi2_devdata *dd;
	unsigned long flags;
	u64 memaddr = 0;
	void *memvirt = NULL;
	dma_addr_t memdma = 0;
	u8 mapio = 0, vmf = 0;
	ssize_t memlen = 0;
	int ret = 0;
	u32 cbi;
	u32 cbc;
	u16 ctxt;
	u16 subctxt;

	if (!uctxt || !(vma->vm_flags & VM_SHARED)) {
		ret = -EINVAL;
		goto done;
	}
	dd = uctxt->dd;
	ctxt = uctxt->ctxt;
	subctxt = fd->subctxt;

	/*
	 * vm_pgoff is used as a buffer selector cookie.  Always mmap from
	 * the beginning.
	 */
	vma->vm_pgoff = 0;
	flags = vma->vm_flags;

	switch (type) {
	case PIO_BUFS:
	case PIO_BUFS_SOP:
		cbi = ctxt_bar_idx(uctxt->sc->hw_context);
		cbc = ctxt_bar_ctxt(uctxt->sc->hw_context);
		memaddr = ((dd->bar_maps[cbi].physaddr + TXE_PIO_SEND) +
			   /* chip pio base */
			   (cbc * BIT(16))) +
			  /* 64K PIO space / ctxt */
			  (type == PIO_BUFS_SOP ? (TXE_PIO_SIZE / 2) :
						  0); /* sop? */
		/*
		 * Map only the amount allocated to the context, not the
		 * entire available context's PIO space.
		 */
		memlen = PAGE_ALIGN(uctxt->sc->credits * PIO_BLOCK_SIZE);
		flags &= ~VM_MAYREAD;
		flags |= VM_DONTCOPY | VM_DONTEXPAND;
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		mapio = 1;
		break;
	case PIO_CRED: {
		u64 cr_page_offset;
		if (flags & VM_WRITE) {
			ret = -EPERM;
			goto done;
		}
		/*
		 * The credit return location for this context could be on the
		 * second or third page allocated for credit returns (if number
		 * of enabled contexts > 64 and 128 respectively).
		 */
		cr_page_offset = ((u64)uctxt->sc->hw_free -
				  (u64)dd->cr_base[uctxt->numa_id].va) &
				 PAGE_MASK;
		memvirt =
			(void *)dd->cr_base[uctxt->numa_id].va + cr_page_offset;
		memdma = dd->cr_base[uctxt->numa_id].dma + cr_page_offset;
		memlen = PAGE_SIZE;
		flags &= ~VM_MAYWRITE;
		flags |= VM_DONTCOPY | VM_DONTEXPAND;
		/*
		 * The driver has already allocated memory for credit
		 * returns and programmed it into the chip. Has that
		 * memory been flagged as non-cached?
		 */
		/* vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot); */
		break;
	}
	case RCV_RHEQ:
		memlen = rheq_size(uctxt);
		memvirt = uctxt->rheq;
		memdma = uctxt->rheq_dma;
		if (!memvirt) {
			ret = -EINVAL;
			goto done;
		}
		if (vma->vm_flags & VM_WRITE) {
			ret = -EPERM;
			goto done;
		}
		break;
	case RCV_HDRQ:
		memlen = rcvhdrq_size(uctxt);
		memvirt = uctxt->rcvhdrq;
		memdma = uctxt->rcvhdrq_dma;
		break;
	case RCV_EGRBUF: {
		unsigned long vm_start_save;
		unsigned long vm_end_save;
		int i;
		/*
		 * The RcvEgr buffer need to be handled differently
		 * as multiple non-contiguous pages need to be mapped
		 * into the user process.
		 */
		memlen = uctxt->egrbufs.size;
		if ((vma->vm_end - vma->vm_start) != memlen) {
			dd_dev_err(
				dd,
				"Eager buffer map size invalid (%lu != %lu)\n",
				(vma->vm_end - vma->vm_start), memlen);
			ret = -EINVAL;
			goto done;
		}
		if (vma->vm_flags & VM_WRITE) {
			ret = -EPERM;
			goto done;
		}
		vm_flags_clear(vma, VM_MAYWRITE);
		/*
		 * Mmap multiple separate allocations into a single vma.  From
		 * here, dma_mmap_coherent() calls dma_direct_mmap(), which
		 * requires the mmap to exactly fill the vma starting at
		 * vma_start.  Adjust the vma start and end for each eager
		 * buffer segment mapped.  Restore the originals when done.
		 */
		vm_start_save = vma->vm_start;
		vm_end_save = vma->vm_end;
		vma->vm_end = vma->vm_start;
		for (i = 0; i < uctxt->egrbufs.numbufs; i++) {
			memlen = uctxt->egrbufs.buffers[i].len;
			memvirt = uctxt->egrbufs.buffers[i].addr;
			memdma = uctxt->egrbufs.buffers[i].dma;
			vma->vm_end += memlen;
			mmap_cdbg(ctxt, subctxt, type, mapio, vmf, memaddr,
				  memvirt, memdma, memlen, vma);
			ret = dma_mmap_coherent(&dd->pcidev->dev, vma, memvirt,
						memdma, memlen);
			if (ret < 0) {
				vma->vm_start = vm_start_save;
				vma->vm_end = vm_end_save;
				goto done;
			}
			vma->vm_start += memlen;
		}
		vma->vm_start = vm_start_save;
		vma->vm_end = vm_end_save;
		ret = 0;
		goto done;
	}
	case UREGS:
		/*
		 * Map the part of BAR0 that contains this context's user
		 * registers.  RcvHdrTail is the first register in the hardware
		 * UCTXT block.  The TidFlow table is contained within this
		 * memory range.
		 */
		cbi = ctxt_bar_idx(uctxt->ctxt);
		cbc = ctxt_bar_ctxt(uctxt->ctxt);
		memaddr = (unsigned long)dd->bar_maps[cbi].physaddr +
			  dd->params->rcv_hdr_tail_reg +
			  (cbc * dd->params->rxe_uctxt_stride);
		memlen = dd->params->rxe_uctxt_stride;
		// hack: accept a 4K mmap for uregs
		{
			ssize_t sz = vma->vm_end - vma->vm_start;
			if (sz != memlen && sz == PAGE_SIZE) {
				printk("%s: UREGS override memlen to 4K\n",
				       __func__);
				memlen = PAGE_SIZE;
			}
		}
		flags |= VM_DONTCOPY | VM_DONTEXPAND;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		mapio = 1;
		break;
	case EVENTS:
		/*
		 * Use the page where this context's flags are. User level
		 * knows where it's own bitmap is within the page.
		 */
		memaddr = (unsigned long)(dd->events + uctxt_offset(uctxt)) &
			  PAGE_MASK;
		memlen = PAGE_SIZE;
		/*
		 * v3.7 removes VM_RESERVED but the effect is kept by
		 * using VM_IO.
		 */
		flags |= VM_IO | VM_DONTEXPAND;
		vmf = 1;
		break;
	case STATUS:
		if (flags & VM_WRITE) {
			ret = -EPERM;
			goto done;
		}
		memaddr = kvirt_to_phys((void *)dd->status);
		memlen = PAGE_SIZE;
		flags |= VM_IO | VM_DONTEXPAND;
		break;
	case RTAIL:
		if (!HFI2_CAP_IS_USET(DMA_RTAIL)) {
			/*
			 * If the memory allocation failed, the context alloc
			 * also would have failed, so we would never get here
			 */
			ret = -EINVAL;
			goto done;
		}
		if ((flags & VM_WRITE) || !hfi2_rcvhdrtail_kvaddr(uctxt)) {
			ret = -EPERM;
			goto done;
		}
		memlen = PAGE_SIZE;
		memvirt = (void *)hfi2_rcvhdrtail_kvaddr(uctxt);
		memdma = uctxt->rcvhdrqtailaddr_dma;
		flags &= ~VM_MAYWRITE;
		break;
	case SUBCTXT_UREGS:
		memaddr = (u64)uctxt->subctxt_uregbase;
		memlen = PAGE_SIZE;
		flags |= VM_IO | VM_DONTEXPAND;
		vmf = 1;
		break;
	case SUBCTXT_RCV_HDRQ:
		memaddr = (u64)uctxt->subctxt_rcvhdr_base;
		memlen = rcvhdrq_size(uctxt) * uctxt->subctxt_cnt;
		flags |= VM_IO | VM_DONTEXPAND;
		vmf = 1;
		break;
	case SUBCTXT_EGRBUF:
		memaddr = (u64)uctxt->subctxt_rcvegrbuf;
		memlen = uctxt->egrbufs.size * uctxt->subctxt_cnt;
		flags |= VM_IO | VM_DONTEXPAND;
		flags &= ~VM_MAYWRITE;
		vmf = 1;
		break;
	case SDMA_COMP: {
		struct hfi2_user_sdma_comp_q *cq = fd->cq;

		if (!cq) {
			ret = -EFAULT;
			goto done;
		}
		memaddr = (u64)cq->comps;
		memlen = PAGE_ALIGN(sizeof(*cq->comps) * cq->nentries);
		flags |= VM_IO | VM_DONTEXPAND;
		vmf = 1;
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	if ((vma->vm_end - vma->vm_start) != memlen) {
		hfi2_cdbg(PROC, "%u:%u Memory size mismatch %lu:%lu",
			  uctxt->ctxt, fd->subctxt,
			  (vma->vm_end - vma->vm_start), memlen);
		ret = -EINVAL;
		goto done;
	}

	vm_flags_reset(vma, flags);
	mmap_cdbg(ctxt, subctxt, type, mapio, vmf, memaddr, memvirt, memdma,
		  memlen, vma);
	if (vmf) {
		vma->vm_pgoff = PFN_DOWN(memaddr);
		vma->vm_ops = &vm_ops;
		ret = 0;
	} else if (memdma) {
		ret = dma_mmap_coherent(&dd->pcidev->dev, vma, memvirt, memdma,
					memlen);
	} else if (mapio) {
		ret = io_remap_pfn_range(vma, vma->vm_start, PFN_DOWN(memaddr),
					 memlen, vma->vm_page_prot);
	} else if (memvirt) {
		ret = remap_pfn_range(vma, vma->vm_start,
				      PFN_DOWN(__pa(memvirt)), memlen,
				      vma->vm_page_prot);
	} else {
		ret = remap_pfn_range(vma, vma->vm_start, PFN_DOWN(memaddr),
				      memlen, vma->vm_page_prot);
	}
done:
	return ret;
}

/*
 * Local (non-chip) user memory is not mapped right away but as it is
 * accessed by the user-level code.
 */
static vm_fault_t vma_fault(struct vm_fault *vmf)
{
	struct page *page;

	page = vmalloc_to_page((void *)(vmf->pgoff << PAGE_SHIFT));
	if (!page)
		return VM_FAULT_SIGBUS;

	get_page(page);
	vmf->page = page;

	return 0;
}

void hfi2_dealloc_filedata(struct hfi2_filedata *fdata)
{
	struct hfi2_ctxtdata *uctxt = fdata->uctxt;
	struct hfi2_devdata *dd = fdata->dd;
	unsigned long flags, *ev;

	if (!uctxt)
		goto done;

	hfi2_cdbg(PROC, "closing ctxt %u:%u", uctxt->ctxt, fdata->subctxt);

	flush_wc();
	/* drain user sdma queue */
	hfi2_user_sdma_free_queues(fdata, uctxt);

	/* clean up rcv side */
	hfi2_user_exp_rcv_free(fdata);

	/*
	 * fdata->uctxt is used in the above cleanup.  It is not ready to be
	 * removed until here.
	 */
	fdata->uctxt = NULL;
	hfi2_rcd_put(uctxt);

	/*
	 * Clear any left over, unhandled events so the next process that
	 * gets this context doesn't get confused.
	 */
	ev = dd->events + uctxt_offset(uctxt) + fdata->subctxt;
	*ev = 0;

	spin_lock_irqsave(&dd->uctxt_lock, flags);
	__clear_bit(fdata->subctxt, uctxt->in_use_ctxts);
	if (!bitmap_empty(uctxt->in_use_ctxts, HFI2_MAX_SHARED_CTXTS)) {
		spin_unlock_irqrestore(&dd->uctxt_lock, flags);
		goto done;
	}
	spin_unlock_irqrestore(&dd->uctxt_lock, flags);

	/*
	 * Disable receive context and interrupt available, reset all
	 * RcvCtxtCtrl bits to default values.
	 */
	hfi2_rcvctrl(
		dd,
		HFI2_RCVCTRL_CTXT_DIS | HFI2_RCVCTRL_TIDFLOW_DIS |
			HFI2_RCVCTRL_INTRAVAIL_DIS | HFI2_RCVCTRL_TAILUPD_DIS |
			HFI2_RCVCTRL_ONE_PKT_EGR_DIS |
			HFI2_RCVCTRL_NO_RHQ_DROP_DIS |
			HFI2_RCVCTRL_NO_EGR_DROP_DIS | HFI2_RCVCTRL_URGENT_DIS,
		uctxt);
	/* Clear the context's J_KEY */
	hfi2_clear_ctxt_jkey(dd, uctxt);
	/*
	 * If a send context is allocated, reset context integrity
	 * checks to default and disable the send context.
	 */
	if (uctxt->sc) {
		hfi2_sc_disable(uctxt->sc);
		hfi2_priv_reg_op(dd, uctxt->sc->ppd->hw_pidx, uctxt->sc->hw_context,
			    uctxt->sc->type, SC_CHK_ADJ_OP, 0);
	}

	hfi2_free_ctxt_rcv_groups(uctxt);
	hfi2_clear_ctxt_pkey(dd, uctxt);

	uctxt->event_flags = 0;

	deallocate_ctxt(uctxt);
done:
	cleanup_srcu_struct(&fdata->pq_srcu);
	kfree(fdata);
}

/*
 * Convert kernel *virtual* addresses to physical addresses.
 * This is used to vmalloc'ed addresses.
 */
static u64 kvirt_to_phys(void *addr)
{
	struct page *page;
	u64 paddr = 0;

	page = vmalloc_to_page(addr);
	if (page)
		paddr = page_to_pfn(page) << PAGE_SHIFT;

	return paddr;
}

/**
 * complete_subctxt - complete sub-context info
 * @fd: valid filedata pointer
 *
 * Sub-context info can only be set up after the base context
 * has been completed.  This is indicated by the clearing of the
 * HFI2_CTXT_BASE_UINIT bit.
 *
 * Wait for the bit to be cleared, and then complete the subcontext
 * initialization.
 *
 */
static int complete_subctxt(struct hfi2_filedata *fd)
{
	int ret;
	unsigned long flags;

	/*
	 * sub-context info can only be set up after the base context
	 * has been completed.
	 */
	ret = wait_event_interruptible(fd->uctxt->wait,
				       !test_bit(HFI2_CTXT_BASE_UNINIT,
						 &fd->uctxt->event_flags));

	if (test_bit(HFI2_CTXT_BASE_FAILED, &fd->uctxt->event_flags))
		ret = -ENOMEM;

	/* Finish the sub-context init */
	if (!ret)
		ret = init_user_ctxt(fd, fd->uctxt);

	if (ret) {
		int last;

		spin_lock_irqsave(&fd->dd->uctxt_lock, flags);
		__clear_bit(fd->subctxt, fd->uctxt->in_use_ctxts);
		last = bitmap_empty(fd->uctxt->in_use_ctxts,
				    HFI2_MAX_SHARED_CTXTS);
		spin_unlock_irqrestore(&fd->dd->uctxt_lock, flags);
		hfi2_rcd_put(fd->uctxt);

		/*
		 * When last is true this was the last reference to fd->uctxt.
		 * No new references to uctxt will be taken. So this task
		 * must free uctxt.
		 */
		if (last)
			deallocate_ctxt(fd->uctxt);
		fd->uctxt = NULL;
	}

	return ret;
}

int hfi2_do_assign_ctxt(struct hfi2_filedata *fd,
			const struct hfi2_assign_ctxt_cmd *uinfo)
{
	struct hfi2_ctxtdata *uctxt = NULL;
	int ret;
	u8 pidx = uinfo->port - 1;
	u8 kdeth_rcv_hdr = uinfo->kdeth_rcvhdrsz;

	if (fd->uctxt)
		return -EINVAL;

	if (uinfo->subctxt_cnt > HFI2_MAX_SHARED_CTXTS)
		return -EINVAL;

	/* check, then assign port ASAP */
	if (pidx >= fd->dd->num_pports)
		return -EINVAL;
	fd->ppd = fd->dd->pport + pidx;

	/* verify kdeth receive header size */
	if (kdeth_rcv_hdr == 0) /* change to default size */
		kdeth_rcv_hdr = DEFAULT_RCVHDRSIZE;
	if (kdeth_rcv_hdr < 2 || kdeth_rcv_hdr > 31) /* valid HW range */
		return -EINVAL;

	/*
	 * Acquire the mutex to protect against multiple creations of what
	 * could be a shared base context.
	 */
	mutex_lock(&hfi2_mutex);
	/*
	 * Get a sub context if available  (fd->uctxt will be set).
	 * ret < 0 error, 0 no context, 1 sub-context found
	 */
	ret = find_sub_ctxt(fd, uinfo);

	/*
	 * Allocate a base context if context sharing is not required or a
	 * sub context wasn't found.
	 */
	if (!ret) {
		ret = allocate_ctxt(fd, uinfo, &uctxt);
		if (ret == 0) {
			/* override - must be done before setup_base_ctxt() */
			uctxt->kdeth_rcv_hdr = kdeth_rcv_hdr;
		}
	}

	mutex_unlock(&hfi2_mutex);

	/* Depending on the context type, finish the appropriate init */
	switch (ret) {
	case 0:
		ret = setup_base_ctxt(fd, uctxt);
		if (ret)
			deallocate_ctxt(uctxt);
		break;
	case 1:
		ret = complete_subctxt(fd);
		break;
	default:
		break;
	}

	return ret;
}

/**
 * match_ctxt - match context
 * @fd: valid filedata pointer
 * @uinfo: user info to compare base context with
 * @uctxt: context to compare uinfo to.
 *
 * Compare the given context with the given information to see if it
 * can be used for a sub context.
 */
static int match_ctxt(struct hfi2_filedata *fd,
		      const struct hfi2_assign_ctxt_cmd *uinfo,
		      struct hfi2_ctxtdata *uctxt)
{
	struct hfi2_devdata *dd = fd->dd;
	unsigned long flags;
	u16 subctxt;

	/* Skip dynamically allocated kernel contexts */
	if (uctxt->sc && (uctxt->sc->type == SC_KERNEL))
		return 0;

	/* Skip ctxt if it doesn't match the requested one */
	if (memcmp(uctxt->uuid, uinfo->uuid, sizeof(uctxt->uuid)) ||
	    uctxt->jkey != generate_jkey(current_uid()) ||
	    uctxt->subctxt_id != uinfo->subctxt_id ||
	    uctxt->subctxt_cnt != uinfo->subctxt_cnt)
		return 0;

	/* Verify the sharing process matches the base */
	if (uctxt->userversion != uinfo->userversion)
		return -EINVAL;

	/* Find an unused sub context */
	spin_lock_irqsave(&dd->uctxt_lock, flags);
	if (bitmap_empty(uctxt->in_use_ctxts, HFI2_MAX_SHARED_CTXTS)) {
		/* context is being closed, do not use */
		spin_unlock_irqrestore(&dd->uctxt_lock, flags);
		return 0;
	}

	subctxt =
		find_first_zero_bit(uctxt->in_use_ctxts, HFI2_MAX_SHARED_CTXTS);
	if (subctxt >= uctxt->subctxt_cnt) {
		spin_unlock_irqrestore(&dd->uctxt_lock, flags);
		return -EBUSY;
	}

	fd->subctxt = subctxt;
	__set_bit(fd->subctxt, uctxt->in_use_ctxts);
	spin_unlock_irqrestore(&dd->uctxt_lock, flags);

	fd->uctxt = uctxt;
	hfi2_rcd_get(uctxt);

	return 1;
}

/**
 * find_sub_ctxt - fund sub-context
 * @fd: valid filedata pointer
 * @uinfo: matching info to use to find a possible context to share.
 *
 * The hfi2_mutex must be held when this function is called.  It is
 * necessary to ensure serialized creation of shared contexts.
 *
 * Return:
 *    0      No sub-context found
 *    1      Subcontext found and allocated
 *    errno  EINVAL (incorrect parameters)
 *           EBUSY (all sub contexts in use)
 */
static int find_sub_ctxt(struct hfi2_filedata *fd,
			 const struct hfi2_assign_ctxt_cmd *uinfo)
{
	struct hfi2_ctxtdata *uctxt;
	struct hfi2_devdata *dd = fd->dd;
	struct hfi2_pportdata *ppd = fd->ppd;
	struct hfi2_portrsrcs *pr = &dd->rsrcs.ppr[ppd->hw_pidx];
	u16 i;
	int ret;

	if (!uinfo->subctxt_cnt)
		return 0;

	for (i = pr->first_dyn_alloc_ctxt;
	     i < pr->rcv_context_base + pr->num_rcv_contexts; i++) {
		uctxt = hfi2_rcd_get_by_index(dd, i);
		if (uctxt) {
			ret = match_ctxt(fd, uinfo, uctxt);
			hfi2_rcd_put(uctxt);
			/* value of != 0 will return */
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int allocate_ctxt(struct hfi2_filedata *fd,
			 const struct hfi2_assign_ctxt_cmd *uinfo,
			 struct hfi2_ctxtdata **rcd)
{
	struct hfi2_devdata *dd = fd->dd;
	struct hfi2_pportdata *ppd = fd->ppd;
	struct hfi2_ctxtdata *uctxt;
	int ret, numa;

	if (dd->flags & HFI2_FROZEN) {
		/*
		 * Pick an error that is unique from all other errors
		 * that are returned so the user process knows that
		 * it tried to allocate while the SPC was frozen.  It
		 * it should be able to retry with success in a short
		 * while.
		 */
		return -EIO;
	}

	if (!ppd->freectxts)
		return -EBUSY;

	/*
	 * Allocate context memory on the device's NUMA node.  Per-process
	 * CPU affinity is not enforced by the driver -- user space
	 * (sched_setaffinity, taskset) is responsible.
	 */
	numa = dd->node;
	ret = hfi2_create_ctxtdata(ppd, numa, DYNAMIC_CONTEXT, &uctxt);
	if (ret < 0) {
		dd_dev_err(dd, "user ctxtdata allocation failed\n");
		return ret;
	}
	hfi2_cdbg(PROC, "[%u:%u] pid %u assigned to NUMA %u", uctxt->ctxt,
		  fd->subctxt, current->pid, uctxt->numa_id);

	/*
	 * Allocate and enable a PIO send context.
	 */
	uctxt->sc = hfi2_sc_alloc(ppd, SC_USER, uctxt->rcvhdrqentsize, numa);
	if (!uctxt->sc) {
		ret = -ENOMEM;
		goto ctxdata_free;
	}
	hfi2_cdbg(PROC, "allocated send context %u(%u)", uctxt->sc->sw_index,
		  uctxt->sc->hw_context);
	ret = hfi2_sc_enable(uctxt->sc);
	if (ret)
		goto ctxdata_free;

	/*
	 * Setup sub context information if the user-level has requested
	 * sub contexts.
	 * This has to be done here so the rest of the sub-contexts find the
	 * proper base context.
	 * NOTE: _set_bit() can be used here because the context creation is
	 * protected by the mutex (rather than the spin_lock), and will be the
	 * very first instance of this context.
	 */
	__set_bit(0, uctxt->in_use_ctxts);
	if (uinfo->subctxt_cnt)
		init_subctxts(uctxt, uinfo);
	uctxt->userversion = uinfo->userversion;
	uctxt->flags = hfi2_cap_mask; /* save current flag state */
	init_waitqueue_head(&uctxt->wait);
	strscpy(uctxt->comm, current->comm, sizeof(uctxt->comm));
	memcpy(uctxt->uuid, uinfo->uuid, sizeof(uctxt->uuid));
	uctxt->jkey = generate_jkey(current_uid());
	hfi2_stats.sps_ctxts++;
	ppd->freectxts--;

	*rcd = uctxt;

	return 0;

ctxdata_free:
	hfi2_free_ctxt(uctxt);
	return ret;
}

static void deallocate_ctxt(struct hfi2_ctxtdata *uctxt)
{
	mutex_lock(&hfi2_mutex);
	hfi2_stats.sps_ctxts--;
	uctxt->ppd->freectxts++;
	mutex_unlock(&hfi2_mutex);

	hfi2_free_ctxt(uctxt);
}

static void init_subctxts(struct hfi2_ctxtdata *uctxt,
			  const struct hfi2_assign_ctxt_cmd *uinfo)
{
	uctxt->subctxt_cnt = uinfo->subctxt_cnt;
	uctxt->subctxt_id = uinfo->subctxt_id;
	set_bit(HFI2_CTXT_BASE_UNINIT, &uctxt->event_flags);
}

static int setup_subctxt(struct hfi2_ctxtdata *uctxt)
{
	int ret = 0;
	u16 num_subctxts = uctxt->subctxt_cnt;

	uctxt->subctxt_uregbase = vmalloc_user(PAGE_SIZE);
	if (!uctxt->subctxt_uregbase)
		return -ENOMEM;

	/* We can take the size of the RcvHdr Queue from the master */
	uctxt->subctxt_rcvhdr_base =
		vmalloc_user(rcvhdrq_size(uctxt) * num_subctxts);
	if (!uctxt->subctxt_rcvhdr_base) {
		ret = -ENOMEM;
		goto bail_ureg;
	}

	uctxt->subctxt_rcvegrbuf =
		vmalloc_user(uctxt->egrbufs.size * num_subctxts);
	if (!uctxt->subctxt_rcvegrbuf) {
		ret = -ENOMEM;
		goto bail_rhdr;
	}

	return 0;

bail_rhdr:
	vfree(uctxt->subctxt_rcvhdr_base);
	uctxt->subctxt_rcvhdr_base = NULL;
bail_ureg:
	vfree(uctxt->subctxt_uregbase);
	uctxt->subctxt_uregbase = NULL;

	return ret;
}

static void user_init(struct hfi2_ctxtdata *uctxt)
{
	unsigned int rcvctrl_ops = 0;

	/* initialize poll variables... */
	uctxt->urgent = 0;
	uctxt->urgent_poll = 0;

	/*
	 * Now enable the ctxt for receive.
	 * For chips that are set to DMA the tail register to memory
	 * when they change (and when the update bit transitions from
	 * 0 to 1.  So for those chips, we turn it off and then back on.
	 * This will (very briefly) affect any other open ctxts, but the
	 * duration is very short, and therefore isn't an issue.  We
	 * explicitly set the in-memory tail copy to 0 beforehand, so we
	 * don't have to wait to be sure the DMA update has happened
	 * (chip resets head/tail to 0 on transition to enable).
	 */
	if (hfi2_rcvhdrtail_kvaddr(uctxt))
		clear_rcvhdrtail(uctxt);

	/* Setup J_KEY before enabling the context */
	hfi2_set_ctxt_jkey(uctxt->dd, uctxt, uctxt->jkey);

	rcvctrl_ops = HFI2_RCVCTRL_CTXT_ENB;
	rcvctrl_ops |= HFI2_RCVCTRL_URGENT_ENB;
	if (HFI2_CAP_UGET_MASK(uctxt->flags, HDRSUPP))
		rcvctrl_ops |= HFI2_RCVCTRL_TIDFLOW_ENB;
	/*
	 * Ignore the bit in the flags for now until proper
	 * support for multiple packet per rcv array entry is
	 * added.
	 */
	if (!HFI2_CAP_UGET_MASK(uctxt->flags, MULTI_PKT_EGR))
		rcvctrl_ops |= HFI2_RCVCTRL_ONE_PKT_EGR_ENB;
	if (HFI2_CAP_UGET_MASK(uctxt->flags, NODROP_EGR_FULL))
		rcvctrl_ops |= HFI2_RCVCTRL_NO_EGR_DROP_ENB;
	if (HFI2_CAP_UGET_MASK(uctxt->flags, NODROP_RHQ_FULL))
		rcvctrl_ops |= HFI2_RCVCTRL_NO_RHQ_DROP_ENB;
	/*
	 * The RcvCtxtCtrl.TailUpd bit has to be explicitly written.
	 * We can't rely on the correct value to be set from prior
	 * uses of the chip or ctxt. Therefore, add the rcvctrl op
	 * for both cases.
	 */
	if (HFI2_CAP_UGET_MASK(uctxt->flags, DMA_RTAIL))
		rcvctrl_ops |= HFI2_RCVCTRL_TAILUPD_ENB;
	else
		rcvctrl_ops |= HFI2_RCVCTRL_TAILUPD_DIS;
	hfi2_rcvctrl(uctxt->dd, rcvctrl_ops, uctxt);
}

static int init_user_ctxt(struct hfi2_filedata *fd, struct hfi2_ctxtdata *uctxt)
{
	int ret;

	ret = hfi2_user_sdma_alloc_queues(uctxt, fd);
	if (ret)
		return ret;

	ret = hfi2_user_exp_rcv_init(fd, uctxt);
	if (ret)
		hfi2_user_sdma_free_queues(fd, uctxt);

	return ret;
}

static int setup_base_ctxt(struct hfi2_filedata *fd,
			   struct hfi2_ctxtdata *uctxt)
{
	struct hfi2_devdata *dd = uctxt->dd;
	int ret = 0;

	hfi2_init_ctxt(uctxt->sc);

	/* Now allocate the RcvHdr queue and eager buffers. */
	ret = hfi2_create_rcvhdrq(dd, uctxt);
	if (ret)
		goto done;

	ret = hfi2_setup_eagerbufs(uctxt);
	if (ret)
		goto done;

	/* If sub-contexts are enabled, do the appropriate setup */
	if (uctxt->subctxt_cnt)
		ret = setup_subctxt(uctxt);
	if (ret)
		goto done;

	ret = hfi2_alloc_ctxt_rcv_groups(uctxt);
	if (ret)
		goto done;

	ret = init_user_ctxt(fd, uctxt);
	if (ret) {
		hfi2_free_ctxt_rcv_groups(uctxt);
		goto done;
	}

	user_init(uctxt);

	/* Now that the context is set up, the fd can get a reference. */
	fd->uctxt = uctxt;
	hfi2_rcd_get(uctxt);

done:
	if (uctxt->subctxt_cnt) {
		/*
		 * On error, set the failed bit so sub-contexts will clean up
		 * correctly.
		 */
		if (ret)
			set_bit(HFI2_CTXT_BASE_FAILED, &uctxt->event_flags);

		/*
		 * Base context is done (successfully or not), notify anybody
		 * using a sub-context that is waiting for this completion.
		 */
		clear_bit(HFI2_CTXT_BASE_UNINIT, &uctxt->event_flags);
		wake_up(&uctxt->wait);
	}

	return ret;
}

/*
 * Find all user contexts in use, and set the specified bit in their
 * event mask.
 * See also find_ctxt() for a similar use, that is specific to send buffers.
 */
int hfi2_set_uevent_bits(struct hfi2_pportdata *ppd, const int evtbit)
{
	struct hfi2_ctxtdata *uctxt;
	struct hfi2_devdata *dd = ppd->dd;
	struct hfi2_portrsrcs *pr = &dd->rsrcs.ppr[ppd->hw_pidx];
	u16 ctxt;

	if (!dd->events)
		return -EINVAL;

	for (ctxt = pr->first_dyn_alloc_ctxt;
	     ctxt < pr->rcv_context_base + pr->num_rcv_contexts; ctxt++) {
		uctxt = hfi2_rcd_get_by_index(dd, ctxt);
		if (uctxt) {
			unsigned long *evs;
			int i;
			/*
			 * subctxt_cnt is 0 if not shared, so do base
			 * separately, first, then remaining subctxt, if any
			 */
			evs = dd->events + uctxt_offset(uctxt);
			set_bit(evtbit, evs);
			for (i = 1; i < uctxt->subctxt_cnt; i++)
				set_bit(evtbit, evs + i);
			hfi2_rcd_put(uctxt);
		}
	}

	return 0;
}

/**
 * hfi2_manage_rcvq - manage a context's receive queue
 * @uctxt: the context
 * @subctxt: the sub-context
 * @start_stop: action to carry out
 *
 * start_stop == 0 disables receive on the context, for use in queue
 * overflow conditions.  start_stop==1 re-enables, to be used to
 * re-init the software copy of the head register
 */
int hfi2_manage_rcvq(struct hfi2_ctxtdata *uctxt, u16 subctxt, int start_stop)
{
	struct hfi2_devdata *dd = uctxt->dd;
	unsigned int rcvctrl_op;

	if (subctxt)
		return 0;

	/* atomically clear receive enable ctxt. */
	if (start_stop) {
		/*
		 * On enable, force in-memory copy of the tail register to
		 * 0, so that protocol code doesn't have to worry about
		 * whether or not the chip has yet updated the in-memory
		 * copy or not on return from the system call. The chip
		 * always resets it's tail register back to 0 on a
		 * transition from disabled to enabled.
		 */
		if (hfi2_rcvhdrtail_kvaddr(uctxt))
			clear_rcvhdrtail(uctxt);
		rcvctrl_op = HFI2_RCVCTRL_CTXT_ENB;
	} else {
		rcvctrl_op = HFI2_RCVCTRL_CTXT_DIS;
	}
	hfi2_rcvctrl(dd, rcvctrl_op, uctxt);
	/* always; new head should be equal to new tail; see above */

	return 0;
}

/*
 * clear the event notifier events for this context.
 * User process then performs actions appropriate to bit having been
 * set, if desired, and checks again in future.
 */
int hfi2_user_event_ack(struct hfi2_ctxtdata *uctxt, u16 subctxt,
		   unsigned long events)
{
	int i;
	struct hfi2_devdata *dd = uctxt->dd;
	unsigned long *evs;

	if (!dd->events)
		return 0;

	evs = dd->events + uctxt_offset(uctxt) + subctxt;

	for (i = 0; i <= _HFI2_MAX_EVENT_BIT; i++) {
		if (!test_bit(i, &events))
			continue;
		clear_bit(i, evs);
	}
	return 0;
}

int hfi2_user_set_ctxt_pkey(struct hfi2_ctxtdata *uctxt, u16 pkey)
{
	int i;
	struct hfi2_pportdata *ppd = uctxt->ppd;
	struct hfi2_devdata *dd = uctxt->dd;

	if (!HFI2_CAP_IS_USET(PKEY_CHECK))
		return -EPERM;

	if (pkey == LIM_MGMT_P_KEY || pkey == FULL_MGMT_P_KEY)
		return -EINVAL;

	for (i = 0; i < dd->params->pkey_table_size; i++)
		if (pkey == ppd->pkeys[i])
			return hfi2_set_ctxt_pkey(dd, uctxt, pkey);

	return -ENOENT;
}

/**
 * hfi2_ctxt_reset - Reset the user context
 * @uctxt: valid user context
 */
int hfi2_ctxt_reset(struct hfi2_ctxtdata *uctxt)
{
	struct send_context *sc;
	struct hfi2_devdata *dd;
	int ret = 0;

	if (!uctxt || !uctxt->dd || !uctxt->sc)
		return -EINVAL;

	/*
	 * There is no protection here. User level has to guarantee that
	 * no one will be writing to the send context while it is being
	 * re-initialized.  If user level breaks that guarantee, it will
	 * break it's own context and no one else's.
	 */
	dd = uctxt->dd;
	sc = uctxt->sc;

	/*
	 * Wait until the interrupt handler has marked the context as
	 * halted or frozen. Report error if we time out.
	 */
	wait_event_interruptible_timeout(
		sc->halt_wait, (sc->flags & (SCF_HALTED | SCF_LINK_DOWN)),
		msecs_to_jiffies(SEND_CTXT_HALT_TIMEOUT));
	if (!(sc->flags & (SCF_HALTED | SCF_LINK_DOWN)))
		return -ENOLCK;

	/*
	 * If the send context was halted due to a Freeze, wait until the
	 * device has been "unfrozen" before resetting the context.
	 */
	if (sc->flags & SCF_FROZEN) {
		wait_event_interruptible_timeout(
			dd->event_queue, !(READ_ONCE(dd->flags) & HFI2_FROZEN),
			msecs_to_jiffies(SEND_CTXT_HALT_TIMEOUT));
		if (dd->flags & HFI2_FROZEN)
			return -ENOLCK;

		if (dd->flags & HFI2_FORCED_FREEZE)
			/*
			 * Don't allow context reset if we are into
			 * forced freeze
			 */
			return -ENODEV;

		hfi2_sc_disable(sc);
		ret = hfi2_sc_enable(sc);
		hfi2_rcvctrl(dd, HFI2_RCVCTRL_CTXT_ENB, uctxt);
	} else {
		ret = hfi2_sc_restart(sc);
	}
	if (!ret)
		hfi2_sc_return_credits(sc);

	return ret;
}

/* expects stats is already zeroed with memtype and index filled in */
int hfi2_get_pinning_stats(struct hfi2_filedata *fd,
			   struct hfi2_pin_stats *stats)
{
	struct hfi2_user_sdma_pkt_q *pq;
	int lockidx;
	int ret;

	if (!pinning_type_supported(stats->memtype))
		return -EINVAL;

	lockidx = srcu_read_lock(&fd->pq_srcu);
	pq = srcu_dereference(fd->pq, &fd->pq_srcu);
	if (pq)
		ret = hfi2_pinning_interfaces[stats->memtype].get_stats(
			pq, stats->index, stats);
	else
		ret = -EIO;
	srcu_read_unlock(&fd->pq_srcu, lockidx);

	return ret;
}
