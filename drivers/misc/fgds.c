// SPDX-License-Identifier: GPL-2.0
/*
 * Fast GPU Direct Storage via dma-buf.
 *
 * Copyright (C) 2026 KylinSoft. Co., Ltd. All rights reserved.
 *
 * Maps GPU memory into user space to enable direct NVME-to-GPU DMA
 * pread/pwrite syscalls. BAR pages are remapped into ZONE_DEVICE via
 * devm_memremap_pages() and populated using dma-buf backing pages.
 */
#define pr_fmt(fmt) "fgds: " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/memremap.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include <linux/fgds.h>

/*
 * Upper bound for chrdev minor allocation and IDA range
 */
#define FGDS_MAX_MINORS		512
#define FGDS_MIN_GPU_BAR_SIZE	(64UL * 1024 * 1024)

struct fgds_dev {
	struct list_head node;		/* Entry in fgds_dev_list */
	struct pci_dev *pdev;		/* Underlying GPU PCI device */
	int idx;			/* Minor number from fgds_ida */
	phys_addr_t bar_paddr;		/* GPU PCIe BAR physical base address */
	resource_size_t bar_size;	/* GPU PCIe BAR size */
	void __iomem *pci_mem_va;	/* Remapped kernel virtual address */
	struct dev_pagemap *pgmap;	/* ZONE_DEVICE page map for the BAR */
	struct device device;
	struct cdev cdev;
};

/*
 * Contiguous GPU memory extent mapped from dma-buf scatterlist
 */
struct fgds_extent {
	u64 vma_offset;		/* Start offset within VMA (bytes) */
	dma_addr_t dma_addr;	/* Bus address within BAR window */
	u64 len;		/* Extent length(bytes) */
};

/*
 * Registered dma-buf range. Reference counted by per-file registry and
 * each active VMA mapped from it.
 */
struct fgds_buffer {
	struct kref ref;
	struct fgds_dev *fdev;
	u64 idx;			/* Token passed to mmap(2) */
	u64 size;			/* Range size(page aligned) */
	u64 dmabuf_offset;		/* Offset inside the dma-buf */
	struct dma_buf *dbuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct fgds_extent *extents;	/* Merged physical extents */
	u32 nr_extents;
	bool invalidated;		/* Buffer moved by exporter */
};

/* Per-open session state. */
struct fgds_file_ctx {
	struct fgds_dev *fdev;
	struct mutex lock;		/* Protects @buffers */
	struct xarray buffers;		/* idx -> struct fgds_buffer */
};

static dev_t fgds_chr_devt;
static struct class *fgds_chr_class;

static DEFINE_IDA(fgds_ida);
static LIST_HEAD(fgds_dev_list);
static u32 fgds_dev_count;

struct fgds_bdf_entry {
	u16 domain;
	u8  bus;
	u8  slot;
	u8  func;
	struct list_head list;
};

static LIST_HEAD(fgds_whitelist);
static char *devices = "all";

module_param(devices, charp, 0444);
MODULE_PARM_DESC(devices, "all | comma-separated BDF whitelist (e.g. 0000:1e:00.0,0000:1f:00.0)");

static int __init fgds_parse_bdf_list(const char *str, struct list_head *head)
{
	char *dup, *token, *cur;

	if (!str || !strcmp(str, "all") || !strlen(str))
		return 0;

	dup = kstrdup(str, GFP_KERNEL);
	if (!dup)
		return -ENOMEM;

	cur = dup;
	while ((token = strsep(&cur, ",")) != NULL) {
		unsigned int dom, b, s, f;
		struct fgds_bdf_entry *entry;

		if (sscanf(token, "%x:%x:%x.%x", &dom, &b, &s, &f) != 4)
			continue;

		entry = kzalloc_obj(*entry, GFP_KERNEL);
		if (!entry) {
			kfree(dup);
			return -ENOMEM;
		}
		entry->domain = dom;
		entry->bus    = b;
		entry->slot   = s;
		entry->func   = f;
		list_add_tail(&entry->list, head);
	}
	kfree(dup);
	return 0;
}

static bool fgds_match_bdf(struct pci_dev *pdev, struct list_head *head)
{
	struct fgds_bdf_entry *entry;
	u16 dom = pci_domain_nr(pdev->bus);
	u8  b   = pdev->bus->number;
	u8  s   = PCI_SLOT(pdev->devfn);
	u8  f   = PCI_FUNC(pdev->devfn);

	list_for_each_entry(entry, head, list) {
		if (entry->domain == dom && entry->bus == b &&
		    entry->slot == s && entry->func == f)
			return true;
	}
	return false;
}

static bool fgds_should_bind(struct pci_dev *pdev)
{
	if (!devices || !strcmp(devices, "all"))
		return true;
	return fgds_match_bdf(pdev, &fgds_whitelist);
}

/*
 * BAR-based mapping requires device physical addresses. When using
 * IOMMU, DMA addresses are IOVAs, which cannot be mapped directly.
 */
static int fgds_check_gpu_iommu(struct pci_dev *pdev)
{
	struct iommu_domain *domain;

	domain = iommu_get_domain_for_dev(&pdev->dev);
	if (domain && domain->type != IOMMU_DOMAIN_IDENTITY) {
		pr_warn("%s: reject attaching a translating IOMMU domain (requires iommu=pt or off\n",
			dev_name(&pdev->dev));
		return -EPERM;
	}
	return 0;
}

/* Prefetchable memory BARs are the only ones that can back GPU memory. */
static bool fgds_bar_is_prefetch_mem(struct pci_dev *pdev, int bar)
{
	unsigned long flags = pci_resource_flags(pdev, bar);

	return (flags & (IORESOURCE_MEM | IORESOURCE_PREFETCH)) ==
	       (IORESOURCE_MEM | IORESOURCE_PREFETCH);
}

/*
 * Check if the device exposes a large prefetchable memory BAR
 */
static bool fgds_has_large_memory_bar(struct pci_dev *pdev)
{
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (!fgds_bar_is_prefetch_mem(pdev, i))
			continue;
		if (pci_resource_len(pdev, i) >= FGDS_MIN_GPU_BAR_SIZE)
			return true;
	}
	return false;
}

/* Map the GPU PCIe BAR into ZONE_DEVICE kernel virtual memory. */
static int fgds_devm_memremap(struct fgds_dev *gdev)
{
	struct dev_pagemap *pgmap;
	int ret;
	void *addr;

	gdev->pgmap = devm_kzalloc(&gdev->pdev->dev, sizeof(struct dev_pagemap),
				   GFP_KERNEL);
	if (!gdev->pgmap)
		return -ENOMEM;

	pgmap = gdev->pgmap;
	pgmap->range.start = gdev->bar_paddr;
	pgmap->range.end = gdev->bar_paddr + gdev->bar_size - 1;
	pgmap->nr_range = 1;
	pgmap->type = MEMORY_DEVICE_GENERIC;

	addr = devm_memremap_pages(&gdev->pdev->dev, pgmap);
	if (IS_ERR(addr)) {
		ret = PTR_ERR(addr);
		pr_err("%s: cannot map BAR [%#llx, +0x%llx] as device memory (%d)\n",
		       dev_name(&gdev->pdev->dev),
		       (u64)gdev->bar_paddr, (u64)gdev->bar_size, ret);
		devm_kfree(&gdev->pdev->dev, gdev->pgmap);
		gdev->pgmap = NULL;
		return ret;
	}

	gdev->pci_mem_va = addr;

	pr_info("%s: BAR [%#llx, %#llx] remapped to kernel VA %#lx\n",
		dev_name(&gdev->pdev->dev), (u64)gdev->bar_paddr,
			     (u64)(gdev->bar_paddr + gdev->bar_size - 1),
		(uintptr_t)gdev->pci_mem_va);
	return 0;
}

/*
 * Dynamic dma-buf attachment callbacks. Peer2peer ability hints exporters
 * to retain buffers in device VRAM. Buffer movement invalidates existing
 * VMA mappings and requires user-space re-registration.
 */
static void fgds_invalidate_mappings(struct dma_buf_attachment *attach)
{
	struct fgds_buffer *buf = attach->importer_priv;

	pr_warn_ratelimited("dma-buf moved while mapped; re-register required\n");
	if (buf)
		WRITE_ONCE(buf->invalidated, true);
}

static const struct dma_buf_attach_ops fgds_attach_ops = {
	.allow_peer2peer = true,
	.invalidate_mappings = fgds_invalidate_mappings,
};

static void fgds_buffer_free(struct kref *kref)
{
	struct fgds_buffer *buf = container_of(kref, struct fgds_buffer, ref);

	if (buf->sgt)
		dma_buf_unmap_attachment(buf->attach, buf->sgt,
					 DMA_BIDIRECTIONAL);
	if (buf->attach)
		dma_buf_detach(buf->dbuf, buf->attach);
	if (buf->dbuf)
		dma_buf_put(buf->dbuf);

	kvfree(buf->extents);
	kfree(buf);
	module_put(THIS_MODULE);
}

static inline void fgds_buffer_get(struct fgds_buffer *buf)
{
	kref_get(&buf->ref);
}

static inline void fgds_buffer_put(struct fgds_buffer *buf)
{
	kref_put(&buf->ref, fgds_buffer_free);
}

/*
 * Collect scatterlist segments into a coalesced extent array within the
 * BAR
 */
static int fgds_fill_extents(struct fgds_buffer *buf)
{
	struct fgds_dev *gdev = buf->fdev;
	struct scatterlist *sg;
	dma_addr_t addr, seg_end;
	unsigned int len;
	u64 cur_vma_offset = 0;
	u64 skip = buf->dmabuf_offset;
	int i, idx = 0;

	buf->extents = kvmalloc_array(buf->sgt->nents, sizeof(*buf->extents),
				      GFP_KERNEL | __GFP_NOWARN);
	if (!buf->extents)
		return -ENOMEM;

	pr_info_ratelimited("exporter %s attached to %s, dbuf size 0x%zx, offset 0x%llx, nents %u\n",
			    buf->dbuf->exp_name, dev_name(buf->attach->dev),
			    buf->dbuf->size, buf->dmabuf_offset, buf->sgt->nents);

	for_each_sgtable_dma_sg(buf->sgt, sg, i) {
		addr = sg_dma_address(sg);
		len = sg_dma_len(sg);
		if (!len)
			len = sg->length;

		if (!addr) {
			pr_err("invalid dma address at sg[%d]\n", i);
			goto err_free;
		}

		/* Verify the segment falls within the BAR window
		 */
		if (check_add_overflow(addr, (dma_addr_t)len, &seg_end) ||
		    addr < gdev->bar_paddr ||
		    seg_end > gdev->bar_paddr + gdev->bar_size) {
			pr_err("segment sg[%d] [0x%llx, +0x%x] outside %s BAR window [0x%llx, 0x%llx]\n",
			       i, (u64)addr, len,
			       dev_name(&gdev->pdev->dev),
			       (u64)gdev->bar_paddr,
			       (u64)(gdev->bar_paddr + gdev->bar_size - 1));
			goto err_free;
		}

		if ((addr & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1))) {
			pr_err("unaligned sg[%d] addr 0x%llx len %u\n",
			       i, (u64)addr, len);
			goto err_free;
		}
		if (skip) {
			if (skip >= len) {
				skip -= len;
				continue;
			}
			addr += skip;
			len -= (unsigned int)skip;
			skip = 0;
		}
		if (cur_vma_offset + len > buf->size)
			len = (unsigned int)(buf->size - cur_vma_offset);

		/*
		 * Merge segments which is contiguous in both bus address and
		 * VMA offset
		 */
		if (idx > 0 &&
		    (buf->extents[idx - 1].dma_addr + buf->extents[idx - 1].len == addr) &&
		    (buf->extents[idx - 1].vma_offset + buf->extents[idx - 1].len ==
		     cur_vma_offset)) {
			buf->extents[idx - 1].len += len;
			cur_vma_offset += len;
			if (cur_vma_offset >= buf->size)
				break;
			continue;
		}

		buf->extents[idx].vma_offset = cur_vma_offset;
		buf->extents[idx].dma_addr   = addr;
		buf->extents[idx].len        = len;
		cur_vma_offset += len;
		idx++;
		if (cur_vma_offset >= buf->size)
			break;
	}

	if (skip || cur_vma_offset < buf->size) {
		pr_err("coverage mismatch (actual=0x%llx, req=0x%llx)\n",
		       cur_vma_offset, buf->size);
		goto err_free;
	}

	pr_info_ratelimited("%u extents, BAR window [0x%llx, 0x%llx], first 0x%llx last 0x%llx\n",
			    idx, (u64)gdev->bar_paddr,
			    (u64)(gdev->bar_paddr + gdev->bar_size - 1),
			    (u64)buf->extents[0].dma_addr,
			    (u64)(buf->extents[idx - 1].dma_addr +
				  buf->extents[idx - 1].len - 1));

	/* Shrink extent array to the actual merged count */
	if (idx > 0 && idx < buf->sgt->nents) {
		struct fgds_extent *e;

		e = kvmalloc_array(idx, sizeof(*e), GFP_KERNEL | __GFP_NOWARN);
		if (e) {
			memcpy(e, buf->extents, idx * sizeof(*e));
			kvfree(buf->extents);
			buf->extents = e;
		}
	}

	buf->nr_extents = idx;
	return 0;

err_free:
	kvfree(buf->extents);
	buf->extents = NULL;
	return -EIO;
}

static int fgds_buffer_import_dmabuf(struct fgds_buffer *buf, s32 dmabuf_fd)
{
	struct dma_buf *dbuf;
	int ret;
	u64 end;

	dbuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(dbuf))
		return PTR_ERR(dbuf);

	if (check_add_overflow(buf->dmabuf_offset, buf->size, &end) ||
	    end > dbuf->size) {
		pr_err("request [0x%llx, +0x%llx) exceeds buffer size 0x%zx\n",
		       buf->dmabuf_offset, buf->size, dbuf->size);
		dma_buf_put(dbuf);
		return -EINVAL;
	}
	buf->dbuf = dbuf;

	/* Attach using GPU PCI device to satisfy dma_mask requirements */
	buf->attach = dma_buf_dynamic_attach(dbuf, &buf->fdev->pdev->dev,
					     &fgds_attach_ops, buf);
	if (IS_ERR(buf->attach)) {
		ret = PTR_ERR(buf->attach);
		buf->attach = NULL;
		goto err_put_dbuf;
	}

	buf->sgt = dma_buf_map_attachment(buf->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(buf->sgt)) {
		ret = PTR_ERR(buf->sgt);
		buf->sgt = NULL;
		goto err_detach;
	}

	ret = fgds_fill_extents(buf);
	if (ret)
		goto err_unmap;

	return 0;

err_unmap:
	dma_buf_unmap_attachment(buf->attach, buf->sgt, DMA_BIDIRECTIONAL);
	buf->sgt = NULL;
err_detach:
	dma_buf_detach(buf->dbuf, buf->attach);
	buf->attach = NULL;
err_put_dbuf:
	dma_buf_put(buf->dbuf);
	buf->dbuf = NULL;
	return ret;
}

static void fgds_vma_open(struct vm_area_struct *vma)
{
	struct fgds_buffer *buf = vma->vm_private_data;

	if (buf)
		fgds_buffer_get(buf);
}

static void fgds_vma_close(struct vm_area_struct *vma)
{
	struct fgds_buffer *buf = vma->vm_private_data;

	if (buf)
		fgds_buffer_put(buf);
}

/* Binary search extents by VMA byte offset */
static struct fgds_extent *fgds_lookup_extent(struct fgds_buffer *buf,
					      u64 offset)
{
	struct fgds_extent *ext;
	int low, high;

	if (unlikely(!buf->nr_extents))
		return NULL;

	if (likely(buf->nr_extents == 1)) {
		ext = &buf->extents[0];
		if (offset < ext->vma_offset + ext->len)
			return ext;
		return NULL;
	}

	low = 0;
	high = buf->nr_extents - 1;
	while (low <= high) {
		int mid = low + (high - low) / 2;

		ext = &buf->extents[mid];

		if (offset < ext->vma_offset)
			high = mid - 1;
		else if (offset >= ext->vma_offset + ext->len)
			low = mid + 1;
		else
			return ext;
	}
	return NULL;
}

static vm_fault_t fgds_vma_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct fgds_buffer *buf = vma->vm_private_data;
	u64 offset;
	struct fgds_extent *ext;
	phys_addr_t phys;
	unsigned long pfn;
	struct page *page;

	if (!buf || unlikely(READ_ONCE(buf->invalidated)))
		return VM_FAULT_SIGBUS;

	offset = vmf->address - vma->vm_start;
	if (offset >= buf->size)
		return VM_FAULT_SIGBUS;

	ext = fgds_lookup_extent(buf, offset);
	if (!ext)
		return VM_FAULT_SIGBUS;

	phys = ext->dma_addr + (offset - ext->vma_offset);
	pfn = phys >> PAGE_SHIFT;

	if (unlikely(!pfn_valid(pfn)))
		return VM_FAULT_SIGBUS;
	page = pfn_to_page(pfn);
	if (unlikely(!is_zone_device_page(page)))
		return VM_FAULT_SIGBUS;

	return vmf_insert_page(vma, vmf->address, page);
}

/*
 * .page_mkwrite is omitted: vmf_insert_page() sets writable PTEs directly
 * from vma->vm_page_prot for MAP_SHARED mappings, avoiding do_wp_page().
 */
static const struct vm_operations_struct fgds_vm_ops = {
	.open	= fgds_vma_open,
	.close	= fgds_vma_close,
	.fault	= fgds_vma_fault,
};

static int fgds_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct fgds_file_ctx *ctx = filp->private_data;
	struct fgds_buffer *buf;
	int ret;

	mutex_lock(&ctx->lock);
	buf = xa_load(&ctx->buffers, vma->vm_pgoff);
	if (buf)
		fgds_buffer_get(buf);
	mutex_unlock(&ctx->lock);

	if (!buf)
		return -EINVAL;

	if (READ_ONCE(buf->invalidated)) {
		pr_err("mmap failed: buffer invalidated by GPU driver\n");
		ret = -EIO;
		goto err_put;
	}
	if (vma->vm_end - vma->vm_start != buf->size) {
		ret = -EINVAL;
		goto err_put;
	}

	vm_flags_clear(vma, VM_PFNMAP | VM_IO);
	vm_flags_set(vma, VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP |
			   current->mm->def_flags);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_ops = &fgds_vm_ops;
	vma->vm_private_data = buf;

	return 0;
err_put:
	fgds_buffer_put(buf);
	return ret;
}

static int fgds_ioctl_reg_buffer(struct fgds_file_ctx *ctx,
				 struct fgds_ioctl_reg_buffer __user *argp)
{
	struct fgds_ioctl_reg_buffer arg;
	struct fgds_buffer *buf = NULL;
	u32 id;
	int ret;
	u64 end;

	if (copy_from_user(&arg, argp, sizeof(arg))) {
		ret = -EFAULT;
		goto err;
	}

	if (arg.flags || arg.dmabuf_fd < 0 ||
	    (arg.dmabuf_offset & (PAGE_SIZE - 1)) ||
	    !arg.size || (arg.size & (PAGE_SIZE - 1))) {
		ret = -EINVAL;
		goto err;
	}

	if (check_add_overflow(arg.dmabuf_offset, arg.size, &end)) {
		ret = -EOVERFLOW;
		goto err;
	}

	if (arg.size > ctx->fdev->bar_size) {
		pr_debug("%s: size 0x%llx exceeds BAR window 0x%llx\n",
			 dev_name(&ctx->fdev->pdev->dev), (u64)arg.size,
			 (u64)ctx->fdev->bar_size);
		ret = -EINVAL;
		goto err;
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err;
	}

	kref_init(&buf->ref);
	/*
	 * Pin the module for as long as the buffer exists: a VMA outlives
	 * the file descriptor it was mapped from, so after close(2) nothing
	 * keeps the module alive while .fault and .close still run from it,
	 * and module exit would free the fgds_dev backing those pages.
	 *
	 * __module_get() rather than try_module_get() because the get
	 * cannot fail here: the caller's open file already holds a module
	 * reference, so this ioctl cannot overlap module removal.
	 */
	__module_get(THIS_MODULE);
	buf->fdev = ctx->fdev;
	buf->size = arg.size;
	buf->dmabuf_offset = arg.dmabuf_offset;

	ret = fgds_buffer_import_dmabuf(buf, arg.dmabuf_fd);
	if (ret)
		goto err_put;

	mutex_lock(&ctx->lock);
	ret = xa_alloc(&ctx->buffers, &id, buf, xa_limit_32b, GFP_KERNEL);
	if (!ret)
		buf->idx = (u64)id << PAGE_SHIFT;
	mutex_unlock(&ctx->lock);
	if (ret)
		goto err_put;

	arg.idx = buf->idx;
	if (copy_to_user(argp, &arg, sizeof(arg))) {
		struct fgds_buffer *erased;
		u32 id = buf->idx >> PAGE_SHIFT;

		/*
		 * Drops reference only if this thread successfully erases
		 * the entry.
		 */
		mutex_lock(&ctx->lock);
		erased = xa_erase(&ctx->buffers, id);
		mutex_unlock(&ctx->lock);
		if (erased)
			fgds_buffer_put(buf);
		ret = -EFAULT;
		goto err;
	}

	pr_debug("reg: %s dmabuf %d offset 0x%llx size 0x%llx -> token 0x%llx\n",
		 dev_name(&buf->fdev->pdev->dev), arg.dmabuf_fd,
		 buf->dmabuf_offset, buf->size, buf->idx);
	ret = 0;

err:
	return ret;

err_put:
	fgds_buffer_put(buf);
	return ret;
}

static int fgds_ioctl_unreg_buffer(struct fgds_file_ctx *ctx,
				   struct fgds_ioctl_unreg_buffer __user *argp)
{
	struct fgds_ioctl_unreg_buffer arg;
	struct fgds_buffer *buf;
	u32 id;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;

	if (arg.idx & (PAGE_SIZE - 1))
		return -EINVAL;

	id = arg.idx >> PAGE_SHIFT;

	mutex_lock(&ctx->lock);
	buf = xa_erase(&ctx->buffers, id);
	mutex_unlock(&ctx->lock);

	if (!buf)
		return -ENOENT;

	fgds_buffer_put(buf);
	return 0;
}

static long fgds_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct fgds_file_ctx *ctx = filp->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case FGDS_IOCTL_REG_BUFFER:
		return fgds_ioctl_reg_buffer(ctx, argp);
	case FGDS_IOCTL_UNREG_BUFFER:
		return fgds_ioctl_unreg_buffer(ctx, argp);
	default:
		return -ENOTTY;
	}
}

static int fgds_open(struct inode *inode, struct file *filp)
{
	struct fgds_dev *gdev = container_of(inode->i_cdev,
					     struct fgds_dev, cdev);
	struct fgds_file_ctx *ctx;

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->fdev = gdev;
	mutex_init(&ctx->lock);
	xa_init_flags(&ctx->buffers, XA_FLAGS_ALLOC);
	filp->private_data = ctx;

	pr_debug("open: %s\n", dev_name(&gdev->pdev->dev));
	return 0;
}

static int fgds_release(struct inode *inode, struct file *filp)
{
	struct fgds_file_ctx *ctx = filp->private_data;
	struct fgds_buffer *buf;
	unsigned long index;

	xa_for_each(&ctx->buffers, index, buf) {
		xa_erase(&ctx->buffers, index);
		fgds_buffer_put(buf);
	}
	xa_destroy(&ctx->buffers);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
	return 0;
}

static const struct file_operations fgds_fops = {
	.owner		= THIS_MODULE,
	.open		= fgds_open,
	.release	= fgds_release,
	.unlocked_ioctl	= fgds_ioctl,
	.mmap		= fgds_mmap,
};

static void fgds_dev_release(struct device *dev)
{
	struct fgds_dev *gdev = container_of(dev, struct fgds_dev, device);

	if (gdev->pgmap) {
		devm_memunmap_pages(&gdev->pdev->dev, gdev->pgmap);
		devm_kfree(&gdev->pdev->dev, gdev->pgmap);
		gdev->pgmap = NULL;
	}
	if (gdev->idx >= 0)
		ida_free(&fgds_ida, gdev->idx);
	if (gdev->pdev) {
		pci_dev_put(gdev->pdev);
		gdev->pdev = NULL;
	}
	kfree(gdev);
}

/* Device naming: /dev/fgds_<domain>_<bus>_<dev>_<func> */
#define FGDS_DEV_NAME_FMT	"fgds_%04x_%02x_%02x_%01x"

/* Format char device name */
static int fgds_dev_name(struct fgds_dev *gdev)
{
	struct pci_dev *pdev = gdev->pdev;

	return dev_set_name(&gdev->device, FGDS_DEV_NAME_FMT,
			    pci_domain_nr(pdev->bus), pdev->bus->number,
			    PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
}

static int fgds_cdev_add(struct fgds_dev *gdev)
{
	struct device *dev = &gdev->device;
	int ret;

	dev->devt = MKDEV(MAJOR(fgds_chr_devt), gdev->idx);
	dev->class = fgds_chr_class;
	dev->parent = &gdev->pdev->dev;

	ret = fgds_dev_name(gdev);
	if (ret)
		return ret;

	cdev_init(&gdev->cdev, &fgds_fops);
	gdev->cdev.owner = THIS_MODULE;

	ret = cdev_device_add(&gdev->cdev, dev);
	if (ret)
		return ret;

	pr_info("registered PCI %s -> /dev/%s (minor=%d)\n",
		dev_name(&gdev->pdev->dev), dev_name(dev), gdev->idx);
	return 0;
}

/* Build and register an fgds device for a GPU */
static int fgds_create_device(struct pci_dev *pdev)
{
	struct fgds_dev *gdev;
	int id, ret, j;

	list_for_each_entry(gdev, &fgds_dev_list, node) {
		if (gdev->pdev == pdev)
			return -EEXIST;
	}

	if (fgds_check_gpu_iommu(pdev))
		return -EPERM;

	id = ida_alloc_range(&fgds_ida, 0, FGDS_MAX_MINORS - 1, GFP_KERNEL);
	if (id < 0)
		return id;

	gdev = kzalloc_obj(*gdev, GFP_KERNEL);
	if (!gdev) {
		ida_free(&fgds_ida, id);
		return -ENOMEM;
	}
	gdev->idx = id;
	gdev->pdev = pci_dev_get(pdev);

	device_initialize(&gdev->device);
	gdev->device.release = fgds_dev_release;

	/*
	 * Same predicate as fgds_has_large_memory_bar(): without the MEM
	 * and PREFETCH test the largest BAR could be a different one than
	 * the BAR that was vetted, and the dma-buf extents would then be
	 * checked against the wrong window.
	 */
	for (j = 0; j < PCI_STD_NUM_BARS; j++) {
		resource_size_t sz = pci_resource_len(pdev, j);

		if (!fgds_bar_is_prefetch_mem(pdev, j))
			continue;
		if (sz > gdev->bar_size) {
			gdev->bar_paddr = pci_resource_start(pdev, j);
			gdev->bar_size = sz;
		}
	}
	pr_info("GPU %s: bus %#x, BAR size 0x%llx, BAR phys %#llx, remapping BAR to kernel VA\n",
		dev_name(&pdev->dev), pdev->bus->number,
		(u64)gdev->bar_size, (u64)gdev->bar_paddr);

	ret = fgds_devm_memremap(gdev);
	if (ret)
		goto err_put_dev;

	ret = fgds_cdev_add(gdev);
	if (ret)
		goto err_put_dev;

	list_add_tail(&gdev->node, &fgds_dev_list);
	fgds_dev_count++;
	return 0;

err_put_dev:
	put_device(&gdev->device);
	return ret;
}

static void fgds_destroy_device(struct fgds_dev *gdev)
{
	list_del(&gdev->node);
	fgds_dev_count--;
	cdev_device_del(&gdev->cdev, &gdev->device);
	put_device(&gdev->device);
}

static void fgds_remove_devices(void)
{
	struct fgds_dev *gdev, *tmp;

	list_for_each_entry_safe(gdev, tmp, &fgds_dev_list, node)
		fgds_destroy_device(gdev);
}

static int __init fgds_init(void)
{
	struct pci_dev *pdev = NULL;
	int ret;

	ret = alloc_chrdev_region(&fgds_chr_devt, 0, FGDS_MAX_MINORS, "fgds");
	if (ret)
		return ret;

	fgds_chr_class = class_create("fgds");
	if (IS_ERR(fgds_chr_class)) {
		ret = PTR_ERR(fgds_chr_class);
		goto err_unreg_chrdev;
	}

	fgds_parse_bdf_list(devices, &fgds_whitelist);

	/* Scan PCI for GPUs: 3D controllers first, then VGA adapters. */
	while ((pdev = pci_get_class(PCI_CLASS_DISPLAY_3D << 8, pdev)) != NULL) {
		if (fgds_has_large_memory_bar(pdev) && fgds_should_bind(pdev))
			fgds_create_device(pdev);
	}

	pdev = NULL;
	while ((pdev = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, pdev)) != NULL) {
		if (fgds_has_large_memory_bar(pdev) && fgds_should_bind(pdev))
			fgds_create_device(pdev);
	}

	if (list_empty(&fgds_dev_list)) {
		pr_err("no GPU devices registered\n");
		ret = -ENODEV;
		goto err_destroy_class;
	}

	pr_info("loaded successfully: %u GPU(s) active\n", fgds_dev_count);
	return 0;

err_destroy_class:
	fgds_remove_devices();
	class_destroy(fgds_chr_class);
err_unreg_chrdev:
	unregister_chrdev_region(fgds_chr_devt, FGDS_MAX_MINORS);
	return ret;
}

static void __exit fgds_exit(void)
{
	struct fgds_bdf_entry *entry, *tmp;

	fgds_remove_devices();
	ida_destroy(&fgds_ida);
	class_destroy(fgds_chr_class);
	unregister_chrdev_region(fgds_chr_devt, FGDS_MAX_MINORS);

	list_for_each_entry_safe(entry, tmp, &fgds_whitelist, list) {
		list_del(&entry->list);
		kfree(entry);
	}

	pr_info("exit, Good bye!\n");
}

module_init(fgds_init);
module_exit(fgds_exit);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_AUTHOR("Mengmeng Zhao <zhaomengmeng@kylinos.cn>, Li Wang <liwang@kylinos.cn>");
MODULE_DESCRIPTION("Fast GPU Direct Storage");
MODULE_VERSION("1.0.0");
