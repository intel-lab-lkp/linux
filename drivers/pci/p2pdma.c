// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Peer 2 Peer DMA support.
 *
 * Copyright (c) 2016-2018, Logan Gunthorpe
 * Copyright (c) 2016-2017, Microsemi Corporation
 * Copyright (c) 2017, Christoph Hellwig
 * Copyright (c) 2018, Eideticom Inc.
 */

#define pr_fmt(fmt) "pci-p2pdma: " fmt
#include <linux/acpi.h>
#include <linux/ctype.h>
#include <linux/dma-map-ops.h>
#include <linux/pci-p2pdma.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/genalloc.h>
#include <linux/memremap.h>
#include <linux/percpu-refcount.h>
#include <linux/random.h>
#include <linux/seq_buf.h>
#include <linux/xarray.h>

#include "pci.h"

/*
 * Lifetime and RCU usage
 *
 * Within one driver bind, pdev->p2pdma is published exactly once, by
 * pcim_p2pdma_init(), and cleared exactly once, by the pci_p2pdma_release()
 * devres action that the same function installs. It is never re-pointed at a
 * second struct pci_p2pdma, so a reader that observes a non-NULL pointer
 * always observes the same, fully initialised object. That object is devres
 * memory allocated before the action is installed, so devres frees it only
 * after pci_p2pdma_release() has returned.
 *
 * Most exported entry points reach pdev->p2pdma through a struct pci_dev or a
 * struct p2pdma_provider owned by the provider driver, and
 * pcim_p2pdma_provider() requires callers to drop those references before the
 * driver's remove() completes. Those cannot run concurrently with
 * pci_p2pdma_release(), and their rcu_dereference() calls are simply how an
 * __rcu pointer is read.
 *
 * pci_p2pmem_find_many() and the p2pmem sysfs attributes are the exceptions.
 * The first walks every PCI device, so it can reach a provider whose driver is
 * unbinding: pci_get_device() pins the struct pci_dev, not the driver. The
 * second is reachable from userspace until sysfs_remove_group() runs at the end
 * of the release. pci_has_p2pmem() must dereference the object to determine
 * whether it owns a gen_pool, so even a poolless object must remain alive until
 * that RCU reader exits. The sysfs group is created with the pool.
 *
 * The grace period in pci_p2pdma_release() first protects the struct
 * pci_p2pdma itself from being freed while pci_has_p2pmem() is using it. For a
 * pool-backed provider it also fences the gen_pool: gen_pool_alloc_owner()
 * walks pool->chunks under RCU and gen_pool_destroy() frees those chunks
 * without waiting for a grace period of its own, so pci_alloc_p2pmem() and
 * p2pmem_alloc_mmap() hold rcu_read_lock() across the allocation.
 */
struct pci_p2pdma {
	struct gen_pool *pool;
	bool p2pmem_published;
	struct xarray map_types;
	struct p2pdma_provider mem[PCI_STD_NUM_BARS];
};

struct pci_p2pdma_pagemap {
	struct dev_pagemap pgmap;
	struct p2pdma_provider *mem;
};

/* Provider rank classes, ordered from most to least preferable. */
enum pci_p2pdma_rank_type {
	PCI_P2PDMA_RANK_DIRECT,
	PCI_P2PDMA_RANK_HMAT_BANDWIDTH,
	PCI_P2PDMA_RANK_HMAT_LATENCY,
	PCI_P2PDMA_RANK_DISTANCE,
};

struct pci_p2pdma_rank {
	enum pci_p2pdma_rank_type type;
	u32 bandwidth;
	u32 latency;
	int distance;
	bool latency_valid;
};

static struct pci_p2pdma_pagemap *to_p2p_pgmap(struct dev_pagemap *pgmap)
{
	return container_of(pgmap, struct pci_p2pdma_pagemap, pgmap);
}

static ssize_t size_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_p2pdma *p2pdma;
	size_t size = 0;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (p2pdma && p2pdma->pool)
		size = gen_pool_size(p2pdma->pool);
	rcu_read_unlock();

	return sysfs_emit(buf, "%zd\n", size);
}
static DEVICE_ATTR_RO(size);

static ssize_t available_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_p2pdma *p2pdma;
	size_t avail = 0;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (p2pdma && p2pdma->pool)
		avail = gen_pool_avail(p2pdma->pool);
	rcu_read_unlock();

	return sysfs_emit(buf, "%zd\n", avail);
}
static DEVICE_ATTR_RO(available);

static ssize_t published_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_p2pdma *p2pdma;
	bool published = false;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (p2pdma)
		published = p2pdma->p2pmem_published;
	rcu_read_unlock();

	return sysfs_emit(buf, "%d\n", published);
}
static DEVICE_ATTR_RO(published);

static int p2pmem_alloc_mmap(struct file *filp, struct kobject *kobj,
		const struct bin_attribute *attr, struct vm_area_struct *vma)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));
	size_t len = vma->vm_end - vma->vm_start;
	struct pci_p2pdma *p2pdma;
	struct percpu_ref *ref;
	unsigned long vaddr;
	void *kaddr;
	int ret;

	/* prevent private mappings from being established */
	if ((vma->vm_flags & VM_MAYSHARE) != VM_MAYSHARE) {
		pci_info_ratelimited(pdev,
				     "%s: fail, attempted private mapping\n",
				     current->comm);
		return -EINVAL;
	}

	if (vma->vm_pgoff) {
		pci_info_ratelimited(pdev,
				     "%s: fail, attempted mapping with non-zero offset\n",
				     current->comm);
		return -EINVAL;
	}

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (!p2pdma) {
		ret = -ENODEV;
		goto out;
	}

	kaddr = (void *)gen_pool_alloc_owner(p2pdma->pool, len, (void **)&ref);
	if (!kaddr) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * vm_insert_page() can sleep, so a reference is taken to mapping
	 * such that rcu_read_unlock() can be done before inserting the
	 * pages
	 */
	if (unlikely(!percpu_ref_tryget_live_rcu(ref))) {
		ret = -ENODEV;
		goto out_free_mem;
	}
	rcu_read_unlock();

	for (vaddr = vma->vm_start; vaddr < vma->vm_end; vaddr += PAGE_SIZE) {
		struct page *page = virt_to_page(kaddr);

		/*
		 * Initialise the refcount for the freshly allocated page. As
		 * we have just allocated the page no one else should be
		 * using it.
		 */
		VM_WARN_ON_ONCE_PAGE(page_ref_count(page), page);
		set_page_count(page, 1);
		ret = vm_insert_page(vma, vaddr, page);
		if (ret) {
			gen_pool_free(p2pdma->pool, (uintptr_t)kaddr, len);

			/*
			 * Reset the page count. We don't use put_page()
			 * because we don't want to trigger the
			 * p2pdma_folio_free() path.
			 */
			set_page_count(page, 0);
			percpu_ref_put(ref);
			return ret;
		}
		percpu_ref_get(ref);
		put_page(page);
		kaddr += PAGE_SIZE;
		len -= PAGE_SIZE;
	}

	percpu_ref_put(ref);

	return 0;
out_free_mem:
	gen_pool_free(p2pdma->pool, (uintptr_t)kaddr, len);
out:
	rcu_read_unlock();
	return ret;
}

static const struct bin_attribute p2pmem_alloc_attr = {
	.attr = { .name = "allocate", .mode = 0660 },
	.mmap = p2pmem_alloc_mmap,
	/*
	 * Some places where we want to call mmap (ie. python) will check
	 * that the file size is greater than the mmap size before allowing
	 * the mmap to continue. To work around this, just set the size
	 * to be very large.
	 */
	.size = SZ_1T,
};

static struct attribute *p2pmem_attrs[] = {
	&dev_attr_size.attr,
	&dev_attr_available.attr,
	&dev_attr_published.attr,
	NULL,
};

static const struct bin_attribute *const p2pmem_bin_attrs[] = {
	&p2pmem_alloc_attr,
	NULL,
};

static const struct attribute_group p2pmem_group = {
	.attrs = p2pmem_attrs,
	.bin_attrs = p2pmem_bin_attrs,
	.name = "p2pmem",
};

static void p2pdma_folio_free(struct folio *folio)
{
	struct page *page = &folio->page;
	struct pci_p2pdma_pagemap *pgmap = to_p2p_pgmap(page_pgmap(page));
	/* safe to dereference while a reference is held to the percpu ref */
	struct pci_p2pdma *p2pdma = rcu_dereference_protected(
		to_pci_dev(pgmap->mem->owner)->p2pdma, 1);
	struct percpu_ref *ref;

	gen_pool_free_owner(p2pdma->pool, (uintptr_t)page_to_virt(page),
			    PAGE_SIZE, (void **)&ref);
	percpu_ref_put(ref);
}

static const struct dev_pagemap_ops p2pdma_pgmap_ops = {
	.folio_free = p2pdma_folio_free,
};

static void pci_p2pdma_release(void *data)
{
	struct pci_dev *pdev = data;
	struct pci_p2pdma *p2pdma;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	if (!p2pdma)
		return;

	/*
	 * Stop new RCU readers and wait for readers that observed p2pdma before
	 * allowing devres to free it. This is required even without a pool,
	 * because pci_has_p2pmem() dereferences every non-NULL p2pdma it finds.
	 * For a pool-backed provider this also fences gen_pool_destroy().
	 */
	RCU_INIT_POINTER(pdev->p2pdma, NULL);
	synchronize_rcu();

	/*
	 * The grace period also ensures no RCU reader can still be accessing
	 * map_types here.
	 */
	xa_destroy(&p2pdma->map_types);

	if (!p2pdma->pool)
		return;

	gen_pool_destroy(p2pdma->pool);
	sysfs_remove_group(&pdev->dev.kobj, &p2pmem_group);
}

/**
 * pcim_p2pdma_init - Initialise peer-to-peer DMA providers
 * @pdev: The PCI device to enable P2PDMA for
 *
 * This function initializes the peer-to-peer DMA infrastructure
 * for a PCI device. It allocates and sets up the necessary data
 * structures to support P2PDMA operations, including mapping type
 * tracking.
 *
 * The state is published once per driver bind and torn down by a devres
 * action on unbind. Repeated calls for the same device are a no-op.
 */
int pcim_p2pdma_init(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2p;
	int i, ret;

	if (pdev->non_mappable_bars)
		return -EOPNOTSUPP;

	p2p = rcu_dereference_protected(pdev->p2pdma, 1);
	if (p2p)
		return 0;

	p2p = devm_kzalloc(&pdev->dev, sizeof(*p2p), GFP_KERNEL);
	if (!p2p)
		return -ENOMEM;

	xa_init(&p2p->map_types);
	/*
	 * Iterate over all standard PCI BARs and record only those that
	 * correspond to MMIO regions. Skip non-memory resources (e.g. I/O
	 * port BARs) since they cannot be used for peer-to-peer (P2P)
	 * transactions.
	 */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (!(pci_resource_flags(pdev, i) & IORESOURCE_MEM))
			continue;

		p2p->mem[i].owner = &pdev->dev;
		p2p->mem[i].bus_offset =
			pci_bus_address(pdev, i) - pci_resource_start(pdev, i);
	}

	ret = devm_add_action_or_reset(&pdev->dev, pci_p2pdma_release, pdev);
	if (ret)
		goto out_p2p;

	rcu_assign_pointer(pdev->p2pdma, p2p);
	return 0;

out_p2p:
	devm_kfree(&pdev->dev, p2p);
	return ret;
}
EXPORT_SYMBOL_GPL(pcim_p2pdma_init);

/**
 * pcim_p2pdma_provider - Get peer-to-peer DMA provider
 * @pdev: The PCI device to enable P2PDMA for
 * @bar: BAR index to get provider
 *
 * This function gets peer-to-peer DMA provider for a PCI device. The lifetime
 * of the provider (and of course the MMIO) is bound to the lifetime of the
 * driver. A driver calling this function must ensure that all references to the
 * provider, and any DMA mappings created for any MMIO, are all cleaned up
 * before the driver remove() completes.
 *
 * Since P2P is almost always shared with a second driver this means some system
 * to notify, invalidate and revoke the MMIO's DMA must be in place to use this
 * function. For example a revoke can be built using DMABUF.
 */
struct p2pdma_provider *pcim_p2pdma_provider(struct pci_dev *pdev, int bar)
{
	struct pci_p2pdma *p2p;

	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM) ||
	    pdev->non_mappable_bars)
		return NULL;

	p2p = rcu_dereference_protected(pdev->p2pdma, 1);
	if (WARN_ON(!p2p))
		/* Someone forgot to call to pcim_p2pdma_init() before */
		return NULL;

	return &p2p->mem[bar];
}
EXPORT_SYMBOL_GPL(pcim_p2pdma_provider);

static int pci_p2pdma_setup_pool(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2pdma;
	int ret;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	if (p2pdma->pool)
		/* We already setup pools, do nothing, */
		return 0;

	p2pdma->pool = gen_pool_create(PAGE_SHIFT, dev_to_node(&pdev->dev));
	if (!p2pdma->pool)
		return -ENOMEM;

	ret = sysfs_create_group(&pdev->dev.kobj, &p2pmem_group);
	if (ret)
		goto out_pool_destroy;

	return 0;

out_pool_destroy:
	gen_pool_destroy(p2pdma->pool);
	p2pdma->pool = NULL;
	return ret;
}

static void pci_p2pdma_unmap_mappings(void *data)
{
	struct pci_p2pdma_pagemap *p2p_pgmap = data;

	/*
	 * Removing the alloc attribute from sysfs will call
	 * unmap_mapping_range() on the inode, teardown any existing userspace
	 * mappings and prevent new ones from being created.
	 */
	sysfs_remove_file_from_group(&p2p_pgmap->mem->owner->kobj,
				     &p2pmem_alloc_attr.attr,
				     p2pmem_group.name);
}

/**
 * pci_p2pdma_add_resource - add memory for use as p2p memory
 * @pdev: the device to add the memory to
 * @bar: PCI BAR to add
 * @size: size of the memory to add, may be zero to use the whole BAR
 * @offset: offset into the PCI BAR
 *
 * The memory will be given ZONE_DEVICE struct pages so that it may
 * be used with any DMA request.
 */
int pci_p2pdma_add_resource(struct pci_dev *pdev, int bar, size_t size,
			    u64 offset)
{
	struct pci_p2pdma_pagemap *p2p_pgmap;
	struct p2pdma_provider *mem;
	struct dev_pagemap *pgmap;
	struct pci_p2pdma *p2pdma;
	void *addr;
	int error;

	if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM))
		return -EINVAL;

	if (offset >= pci_resource_len(pdev, bar))
		return -EINVAL;

	if (!size)
		size = pci_resource_len(pdev, bar) - offset;

	if (size + offset > pci_resource_len(pdev, bar))
		return -EINVAL;

	error = pcim_p2pdma_init(pdev);
	if (error)
		return error;

	error = pci_p2pdma_setup_pool(pdev);
	if (error)
		return error;

	mem = pcim_p2pdma_provider(pdev, bar);
	/*
	 * We checked validity of BAR prior to call
	 * to pcim_p2pdma_provider. It should never return NULL.
	 */
	if (WARN_ON(!mem))
		return -EINVAL;

	p2p_pgmap = devm_kzalloc(&pdev->dev, sizeof(*p2p_pgmap), GFP_KERNEL);
	if (!p2p_pgmap)
		return -ENOMEM;

	pgmap = &p2p_pgmap->pgmap;
	pgmap->range.start = pci_resource_start(pdev, bar) + offset;
	pgmap->range.end = pgmap->range.start + size - 1;
	pgmap->nr_range = 1;
	pgmap->type = MEMORY_DEVICE_PCI_P2PDMA;
	pgmap->ops = &p2pdma_pgmap_ops;
	p2p_pgmap->mem = mem;

	addr = devm_memremap_pages(&pdev->dev, pgmap);
	if (IS_ERR(addr)) {
		error = PTR_ERR(addr);
		goto pgmap_free;
	}

	error = devm_add_action(&pdev->dev, pci_p2pdma_unmap_mappings,
				p2p_pgmap);
	if (error)
		goto pages_free;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	error = gen_pool_add_owner(p2pdma->pool, (unsigned long)addr,
			pci_bus_address(pdev, bar) + offset,
			range_len(&pgmap->range), dev_to_node(&pdev->dev),
			&pgmap->ref);
	if (error)
		goto mappings_remove;

	pci_info(pdev, "added peer-to-peer DMA memory %#llx-%#llx\n",
		 pgmap->range.start, pgmap->range.end);

	return 0;

mappings_remove:
	devm_remove_action(&pdev->dev, pci_p2pdma_unmap_mappings, p2p_pgmap);
pages_free:
	devm_memunmap_pages(&pdev->dev, pgmap);
pgmap_free:
	devm_kfree(&pdev->dev, p2p_pgmap);
	return error;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_add_resource);

/*
 * Note this function returns the parent PCI device with a
 * reference taken. It is the caller's responsibility to drop
 * the reference.
 */
static struct pci_dev *find_parent_pci_dev(struct device *dev)
{
	struct device *parent;

	dev = get_device(dev);

	while (dev) {
		if (dev_is_pci(dev))
			return to_pci_dev(dev);

		parent = get_device(dev->parent);
		put_device(dev);
		dev = parent;
	}

	return NULL;
}

/*
 * PCIe r7.0, sec 6.12.3, table 6-11: decide how a peer-to-peer request at an
 * ACS-capable ingress port routes, given its Egress Control register @ctrl,
 * whether the target port is known (@has_target), and that target's Egress
 * Control Vector bit (@egress: 1 set, 0 clear, negative if it could not be
 * read).
 *
 * Egress Control applies only where the target is known (the path divergence).
 * There, a set vector bit redirects the request only when Request Redirect is
 * set; with Request Redirect clear it is an ACS Violation.  A clear vector bit
 * permits direct routing, subject to Completion Redirect.
 */
VISIBLE_IF_KUNIT enum pci_acs_p2pdma_state
pci_acs_p2pdma_decision(u16 ctrl, bool has_target, int egress)
{
	if (!has_target || !(ctrl & PCI_ACS_EC))
		return ctrl & (PCI_ACS_RR | PCI_ACS_CR) ?
			PCI_ACS_P2PDMA_REDIRECT : PCI_ACS_P2PDMA_DIRECT;

	if (egress < 0)
		return PCI_ACS_P2PDMA_NOT_SUPPORTED;
	if (egress)
		return ctrl & PCI_ACS_RR ? PCI_ACS_P2PDMA_REDIRECT :
					   PCI_ACS_P2PDMA_NOT_SUPPORTED;

	return ctrl & PCI_ACS_CR ? PCI_ACS_P2PDMA_REDIRECT :
				   PCI_ACS_P2PDMA_DIRECT;
}
EXPORT_SYMBOL_IF_KUNIT(pci_acs_p2pdma_decision);

static enum pci_acs_p2pdma_state
pci_acs_p2pdma_state(struct pci_dev *pdev, struct pci_dev *target)
{
	int pos, egress = 0;
	u16 ctrl;

	pos = pdev->acs_cap;
	if (!pos)
		return PCI_ACS_P2PDMA_DIRECT;

	if (pci_read_config_word(pdev, pos + PCI_ACS_CTRL, &ctrl))
		return PCI_ACS_P2PDMA_NOT_SUPPORTED;

	/* Egress Control is evaluated only where the target is known. */
	if (target && (ctrl & PCI_ACS_EC))
		egress = pci_acs_egress_ctrl_is_set(pdev, target);

	return pci_acs_p2pdma_decision(ctrl, !!target, egress);
}

static void seq_buf_print_bus_devfn(struct seq_buf *buf, struct pci_dev *pdev)
{
	if (!buf)
		return;

	seq_buf_printf(buf, "%s;", pci_name(pdev));
}

static bool cpu_supports_p2pdma(void)
{
#ifdef CONFIG_X86
	struct cpuinfo_x86 *c = &cpu_data(0);

	/* Any AMD CPU whose family ID is Zen or newer supports p2pdma */
	if (c->x86_vendor == X86_VENDOR_AMD && c->x86 >= 0x17)
		return true;
#endif

	return false;
}

static const struct pci_p2pdma_whitelist_entry {
	unsigned short vendor;
	int device;
	enum {
		REQ_SAME_HOST_BRIDGE	= 1 << 0,
	} flags;
} pci_p2pdma_whitelist[] = {
	/* Intel Xeon E5/Core i7 */
	{PCI_VENDOR_ID_INTEL,	0x3c00, REQ_SAME_HOST_BRIDGE},
	{PCI_VENDOR_ID_INTEL,	0x3c01, REQ_SAME_HOST_BRIDGE},
	/* Intel Xeon E7 v3/Xeon E5 v3/Core i7 */
	{PCI_VENDOR_ID_INTEL,	0x2f00, REQ_SAME_HOST_BRIDGE},
	{PCI_VENDOR_ID_INTEL,	0x2f01, REQ_SAME_HOST_BRIDGE},
	/* Intel Skylake-E */
	{PCI_VENDOR_ID_INTEL,	0x2030, 0},
	{PCI_VENDOR_ID_INTEL,	0x2031, 0},
	{PCI_VENDOR_ID_INTEL,	0x2032, 0},
	{PCI_VENDOR_ID_INTEL,	0x2033, 0},
	{PCI_VENDOR_ID_INTEL,	0x2020, 0},
	{PCI_VENDOR_ID_INTEL,	0x09a2, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_DSA_SPR0, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_IAX_SPR0, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_DSA_GNRD, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_DSA_DMR, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_IAA_DMR, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_QAT_4XXX, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_QAT_401XX, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_QAT_402XX, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_QAT_420XX, 0},
	{PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_QAT_6XXX, 0},
	/* Google SoCs. */
	{PCI_VENDOR_ID_GOOGLE,	PCI_ANY_ID, 0},
	{}
};

/*
 * If the first device on host's root bus is either devfn 00.0 or a PCIe
 * Root Port, return it.  Otherwise return NULL.
 *
 * We often use a devfn 00.0 "host bridge" in the pci_p2pdma_whitelist[]
 * (though there is no PCI/PCIe requirement for such a device).  On some
 * platforms, e.g., Intel Skylake, there is no such host bridge device, and
 * pci_p2pdma_whitelist[] may contain a Root Port at any devfn.
 *
 * This function is similar to pci_get_slot(host->bus, 0), but it does
 * not take the pci_bus_sem lock since __host_bridge_whitelist() must not
 * sleep.
 *
 * For this to be safe, the caller should hold a reference to a device on the
 * bridge, which should ensure the host_bridge device will not be freed
 * or removed from the head of the devices list.
 */
static struct pci_dev *pci_host_bridge_dev(struct pci_host_bridge *host)
{
	struct pci_dev *root;

	root = list_first_entry_or_null(&host->bus->devices,
					struct pci_dev, bus_list);

	if (!root)
		return NULL;

	if (root->devfn == PCI_DEVFN(0, 0))
		return root;

	if (pci_pcie_type(root) == PCI_EXP_TYPE_ROOT_PORT)
		return root;

	return NULL;
}

static bool __host_bridge_whitelist(struct pci_host_bridge *host,
				    bool same_host_bridge, bool warn)
{
	struct pci_dev *root = pci_host_bridge_dev(host);
	const struct pci_p2pdma_whitelist_entry *entry;
	unsigned short vendor, device;

	if (!root)
		return false;

	vendor = root->vendor;
	device = root->device;

	for (entry = pci_p2pdma_whitelist; entry->vendor; entry++) {
		if (vendor != entry->vendor)
			continue;

		if (entry->device != PCI_ANY_ID && device != entry->device)
			continue;

		if (entry->flags & REQ_SAME_HOST_BRIDGE && !same_host_bridge)
			return false;

		return true;
	}

	if (warn)
		pci_warn(root, "Host bridge not in P2PDMA whitelist: %04x:%04x\n",
			 vendor, device);

	return false;
}

/*
 * If we can't find a common upstream bridge take a look at the root
 * complex and compare it to a whitelist of known good hardware.
 */
static bool host_bridge_whitelist(struct pci_dev *a, struct pci_dev *b,
				  bool warn)
{
	struct pci_host_bridge *host_a = pci_find_host_bridge(a->bus);
	struct pci_host_bridge *host_b = pci_find_host_bridge(b->bus);

	if (host_a == host_b)
		return __host_bridge_whitelist(host_a, true, warn);

	if (__host_bridge_whitelist(host_a, false, warn) &&
	    __host_bridge_whitelist(host_b, false, warn))
		return true;

	return false;
}

#ifdef CONFIG_ACPI
static int pci_host_bridge_pxm(struct pci_dev *pdev)
{
	struct pci_host_bridge *host = pci_find_host_bridge(pdev->bus);
	struct acpi_device *adev;
	const char *uid_str;
	u32 uid;

	adev = to_acpi_device_node(host->dev.fwnode);
	if (!adev)
		return -ENODEV;

	uid_str = acpi_device_uid(adev);
	if (!uid_str || kstrtou32(uid_str, 0, &uid))
		return -ENODEV;

	return acpi_get_genport_proximity_domain(uid);
}
#else
static int pci_host_bridge_pxm(struct pci_dev *pdev)
{
	return -ENODEV;
}
#endif

/*
 * Check whether platform firmware describes, via an HMAT PCIe P2P Latency and
 * Bandwidth Information Structure, a reachable ordered (non-UIO) P2P path from
 * the client's host bridge to the provider's host bridge. When it does, the
 * platform vouches for cross-host-bridge P2P even if the root complex is not
 * in the static whitelist.
 *
 * The client is the requester of the P2P transactions and the provider is the
 * completer, so the client's host bridge is the HMAT initiator and the
 * provider's host bridge is the HMAT target. UIO-only paths are described by
 * firmware but are not (yet) used to authorize ordered DMA.
 *
 * @coord may be NULL when only the authorization answer is needed.
 */
static bool host_bridge_hmat_p2p(struct pci_dev *provider,
				 struct pci_dev *client,
				 struct access_coordinate *coord)
{
	struct access_coordinate unused;
	int init_pxm, targ_pxm;

	init_pxm = pci_host_bridge_pxm(client);
	targ_pxm = pci_host_bridge_pxm(provider);
	if (init_pxm < 0 || targ_pxm < 0)
		return false;

	return acpi_get_p2p_coordinates(init_pxm, targ_pxm, HMAT_P2P_NON_UIO,
					coord ? coord : &unused) == 0;
}

static unsigned long map_types_idx(struct pci_dev *client)
{
	return (pci_domain_nr(client->bus) << 16) | pci_dev_id(client);
}

static enum pci_p2pdma_map_type
__calc_map_type_and_dist(struct pci_dev *provider, struct pci_dev *client,
		int *dist, bool verbose, struct access_coordinate *hmat_coord);

/*
 * Calculate the P2PDMA mapping type and distance between two PCI devices.
 *
 * If the two devices are the same PCI function, return
 * PCI_P2PDMA_MAP_BUS_ADDR and a distance of 0.
 *
 * If they are two functions of the same device, return
 * PCI_P2PDMA_MAP_BUS_ADDR and a distance of 2 (one hop up to the bridge,
 * then one hop back down to another function of the same device).
 *
 * In the case where two devices are connected to the same PCIe switch,
 * return a distance of 4. This corresponds to the following PCI tree:
 *
 *     -+  Root Port
 *      \+ Switch Upstream Port
 *       +-+ Switch Downstream Port 0
 *       + \- Device A
 *       \-+ Switch Downstream Port 1
 *         \- Device B
 *
 * The distance is 4 because we traverse from Device A to Downstream Port 0
 * to the common Switch Upstream Port, back down to Downstream Port 1 and
 * then to Device B. The mapping type returned depends on the ACS
 * redirection setting of the ports along the path.
 *
 * If ACS redirects traffic on any port in the path, or blocks the direct
 * path or leaves its routing indeterminate, return
 * PCI_P2PDMA_MAP_THRU_HOST_BRIDGE. Otherwise, return
 * PCI_P2PDMA_MAP_BUS_ADDR.
 *
 * Any two devices that have a data path through a host bridge require
 * platform support from the CPU, the host bridge whitelist, or a reachable
 * ordered HMAT path. Return PCI_P2PDMA_MAP_NOT_SUPPORTED when none of those
 * sources permits the path.
 */
VISIBLE_IF_KUNIT enum pci_p2pdma_map_type
calc_map_type_and_dist(struct pci_dev *provider, struct pci_dev *client,
		int *dist, bool verbose)
{
	return __calc_map_type_and_dist(provider, client, dist, verbose, NULL);
}

static enum pci_p2pdma_map_type
__calc_map_type_and_dist(struct pci_dev *provider, struct pci_dev *client,
		int *dist, bool verbose, struct access_coordinate *hmat_coord)
{
	enum pci_p2pdma_map_type map_type = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE;
	struct pci_dev *a = provider, *b = client, *bb, *target;
	struct pci_dev *a_child = NULL, *b_child = NULL;
	struct pci_dev *acs_unsupported = NULL;
	enum pci_acs_p2pdma_state state;
	bool host_bridge_allowed;
	bool hmat_p2p = false;
	struct pci_p2pdma *p2pdma;
	struct seq_buf acs_list;
	int acs_redirect_cnt = 0;
	int dist_a = 0;
	int dist_b = 0;
	char buf[128];

	if (hmat_coord)
		*hmat_coord = (struct access_coordinate) {};

	seq_buf_init(&acs_list, buf, sizeof(buf));

	/*
	 * Note, we don't need to take references to devices returned by
	 * pci_upstream_bridge() seeing we hold a reference to a child
	 * device which will already hold a reference to the upstream bridge.
	 */
	while (a) {
		dist_b = 0;
		b_child = NULL;
		bb = b;

		while (bb) {
			if (a == bb)
				goto check_paths_acs;

			b_child = bb;
			bb = pci_upstream_bridge(bb);
			dist_b++;
		}

		a_child = a;
		a = pci_upstream_bridge(a);
		dist_a++;
	}

	/*
	 * The paths share no upstream bridge, so there is no direct path for
	 * ACS to gate: PCI_P2PDMA_MAP_BUS_ADDR is not reachable here and the
	 * request can only get to the peer through the host bridge.
	 */
	*dist = dist_a + dist_b;
	goto map_through_host_bridge;

check_paths_acs:
	*dist = dist_a + dist_b;
	bb = provider;

	while (bb) {
		target = bb == a_child ? b_child : NULL;
		state = pci_acs_p2pdma_state(bb, target);
		if (state != PCI_ACS_P2PDMA_DIRECT) {
			seq_buf_print_bus_devfn(&acs_list, bb);
			if (state == PCI_ACS_P2PDMA_REDIRECT)
				acs_redirect_cnt++;
			else if (!acs_unsupported)
				acs_unsupported = bb;
		}

		if (a == bb)
			break;

		bb = pci_upstream_bridge(bb);
	}

	bb = client;

	while (bb && a != bb) {
		target = bb == b_child ? a_child : NULL;
		state = pci_acs_p2pdma_state(bb, target);
		if (state != PCI_ACS_P2PDMA_DIRECT) {
			seq_buf_print_bus_devfn(&acs_list, bb);
			if (state == PCI_ACS_P2PDMA_REDIRECT)
				acs_redirect_cnt++;
			else if (!acs_unsupported)
				acs_unsupported = bb;
		}

		bb = pci_upstream_bridge(bb);
	}

	/*
	 * Below a shared upstream bridge, a path that no port redirects or
	 * blocks routes the request directly.
	 */
	if (!acs_unsupported && !acs_redirect_cnt) {
		map_type = PCI_P2PDMA_MAP_BUS_ADDR;
		goto done;
	}

	/*
	 * ACS controls only act on Requests routed peer-to-peer, so a blocked
	 * or indeterminate direct path still leaves the host-bridge route.
	 */
	if (verbose) {
		/* Drop the final semicolon; the list is not empty here. */
		if (!seq_buf_has_overflowed(&acs_list))
			acs_list.buffer[acs_list.len - 1] = '\0';
		if (acs_unsupported)
			pci_warn(client, "ACS leaves no usable direct P2P path to provider %s at %s\n",
				 pci_name(provider), pci_name(acs_unsupported));
		else
			pci_warn(client, "ACS redirect is set between the client and provider (%s)\n",
				 pci_name(provider));
		pci_warn(client, "to disable ACS controls for this path, add the kernel parameter: pci=disable_acs_redir=%s\n",
			 seq_buf_str(&acs_list));
	}

map_through_host_bridge:
	host_bridge_allowed = cpu_supports_p2pdma() ||
			      host_bridge_whitelist(provider, client,
						    false);
	/*
	 * The coordinates are only used to rank providers, which happens in
	 * process context. Skip the firmware lookup on the mapping path once
	 * the CPU or the whitelist has already permitted the path.
	 */
	if (hmat_coord || !host_bridge_allowed)
		hmat_p2p = host_bridge_hmat_p2p(provider, client, hmat_coord);
	if (!host_bridge_allowed && !hmat_p2p) {
		if (verbose) {
			host_bridge_whitelist(provider, client, true);
			pci_warn(client, "cannot be used for peer-to-peer DMA as the client and provider (%s) do not share an upstream bridge, whitelisted host bridge, or HMAT-described P2P path\n",
				 pci_name(provider));
		}
		map_type = PCI_P2PDMA_MAP_NOT_SUPPORTED;
	}
done:
	/*
	 * pci_p2pmem_find_many() reaches this with a provider whose driver may
	 * be unbinding, so the store runs under RCU: pci_p2pdma_release()
	 * clears the pointer and waits for readers before destroying
	 * map_types. See "Lifetime and RCU usage" above.
	 */
	rcu_read_lock();
	p2pdma = rcu_dereference(provider->p2pdma);
	if (p2pdma)
		xa_store(&p2pdma->map_types, map_types_idx(client),
			 xa_mk_value(map_type), GFP_ATOMIC);
	rcu_read_unlock();
	return map_type;
}
EXPORT_SYMBOL_IF_KUNIT(calc_map_type_and_dist);

static int
pci_p2pdma_rank_cmp(const struct pci_p2pdma_rank *a,
		    const struct pci_p2pdma_rank *b)
{
	if (a->type != b->type)
		return a->type < b->type ? -1 : 1;

	if (a->type == PCI_P2PDMA_RANK_HMAT_BANDWIDTH) {
		if (a->bandwidth != b->bandwidth)
			return a->bandwidth > b->bandwidth ? -1 : 1;
		if (a->latency_valid != b->latency_valid)
			return a->latency_valid ? -1 : 1;
	}

	if ((a->type == PCI_P2PDMA_RANK_HMAT_LATENCY ||
	     a->latency_valid) && a->latency != b->latency)
		return a->latency < b->latency ? -1 : 1;

	if (a->distance != b->distance)
		return a->distance < b->distance ? -1 : 1;

	return 0;
}

/*
 * P2P bandwidth is limited by the slowest direction and client path, while
 * the largest latency bounds the worst path. Only compare a metric when every
 * host-bridge path supplies both its read and write values.
 */
static int
pci_p2pdma_rank_many(struct pci_dev *provider, struct device **clients,
		     int num_clients, bool verbose,
		     struct pci_p2pdma_rank *rank)
{
	enum pci_p2pdma_map_type map;
	struct access_coordinate coord;
	bool bandwidth_valid = true;
	bool latency_valid = true;
	bool through_host_bridge = false;
	bool not_supported = false;
	struct pci_dev *pci_client;
	int i, distance;

	*rank = (struct pci_p2pdma_rank) {
		.bandwidth = UINT_MAX,
	};

	if (num_clients == 0)
		return -1;

	for (i = 0; i < num_clients; i++) {
		pci_client = find_parent_pci_dev(clients[i]);
		if (!pci_client) {
			if (verbose)
				dev_warn(clients[i],
					 "cannot be used for peer-to-peer DMA as it is not a PCI device\n");
			return -1;
		}

		map = __calc_map_type_and_dist(provider, pci_client, &distance,
					       verbose, &coord);

		pci_dev_put(pci_client);

		if (map == PCI_P2PDMA_MAP_NOT_SUPPORTED)
			not_supported = true;
		else if (map == PCI_P2PDMA_MAP_THRU_HOST_BRIDGE) {
			through_host_bridge = true;
			if (!coord.read_bandwidth || !coord.write_bandwidth) {
				bandwidth_valid = false;
			} else {
				rank->bandwidth = min(rank->bandwidth,
					min(coord.read_bandwidth,
					    coord.write_bandwidth));
			}

			if (!coord.read_latency || !coord.write_latency) {
				latency_valid = false;
			} else {
				rank->latency = max(rank->latency,
					max(coord.read_latency,
					    coord.write_latency));
			}
		}

		if (not_supported && !verbose)
			break;

		rank->distance += distance;
	}

	if (not_supported)
		return -1;

	if (!through_host_bridge) {
		rank->type = PCI_P2PDMA_RANK_DIRECT;
	} else if (bandwidth_valid) {
		rank->type = PCI_P2PDMA_RANK_HMAT_BANDWIDTH;
		rank->latency_valid = latency_valid;
	} else if (latency_valid) {
		rank->type = PCI_P2PDMA_RANK_HMAT_LATENCY;
		rank->latency_valid = true;
	} else {
		rank->type = PCI_P2PDMA_RANK_DISTANCE;
	}

	return 0;
}

/**
 * pci_p2pdma_distance_many - Determine the cumulative distance between
 *	a p2pdma provider and the clients in use.
 * @provider: p2pdma provider to check against the client list
 * @clients: array of devices to check (NULL-terminated)
 * @num_clients: number of clients in the array
 * @verbose: if true, print warnings for devices when we return -1
 *
 * Returns -1 if any of the clients are not compatible, otherwise returns a
 * positive number where a lower number is the preferable choice. (If there's
 * one client that's the same as the provider it will return 0, which is best
 * choice).
 *
 * "compatible" means the provider and the clients have a direct PCI path or
 * the platform permits the transaction through the host bridge.
 */
int pci_p2pdma_distance_many(struct pci_dev *provider, struct device **clients,
			     int num_clients, bool verbose)
{
	struct pci_p2pdma_rank rank;

	if (pci_p2pdma_rank_many(provider, clients, num_clients, verbose, &rank))
		return -1;

	return rank.distance;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_distance_many);

/**
 * pci_has_p2pmem - check if a given PCI device has published any p2pmem
 * @pdev: PCI device to check
 */
static bool pci_has_p2pmem(struct pci_dev *pdev)
{
	struct pci_p2pdma *p2pdma;
	bool res;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	/*
	 * The callers hand the result to pci_alloc_p2pmem(), so only a
	 * provider backed by a pool is of any use here. pcim_p2pdma_init()
	 * creates providers without one.
	 */
	res = p2pdma && p2pdma->pool && p2pdma->p2pmem_published;
	rcu_read_unlock();

	return res;
}

/**
 * pci_p2pmem_find_many - find a peer-to-peer DMA memory device compatible with
 *	the specified list of clients
 * @clients: array of devices to check (NULL-terminated)
 * @num_clients: number of client devices in the list
 *
 * A provider with direct paths to all clients is preferred. For paths through
 * host bridges, complete ordered HMAT bandwidth is ranked before latency-only
 * data, using the worst client path for each metric. Topology distance breaks
 * performance ties and remains the fallback when HMAT data is incomplete. If
 * multiple providers have an equal rank, one is chosen at random.
 *
 * Returns a pointer to the PCI device with a reference taken (use pci_dev_put
 * to return the reference) or NULL if no compatible device is found. The
 * found provider will also be assigned to the client list.
 */
struct pci_dev *pci_p2pmem_find_many(struct device **clients, int num_clients)
{
	struct pci_dev *pdev = NULL;
	struct pci_p2pdma_rank rank, best_rank;
	struct pci_dev **best_pdevs;
	bool have_best = false;
	int dev_cnt = 0;
	const int max_devs = PAGE_SIZE / sizeof(*best_pdevs);
	int cmp, i;

	best_pdevs = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!best_pdevs)
		return NULL;

	for_each_pci_dev(pdev) {
		if (!pci_has_p2pmem(pdev))
			continue;

		if (pci_p2pdma_rank_many(pdev, clients, num_clients, false,
					  &rank))
			continue;

		if (have_best) {
			cmp = pci_p2pdma_rank_cmp(&rank, &best_rank);
			if (cmp > 0 || (!cmp && dev_cnt >= max_devs))
				continue;
			if (cmp < 0) {
				for (i = 0; i < dev_cnt; i++)
					pci_dev_put(best_pdevs[i]);
				dev_cnt = 0;
				best_rank = rank;
			}
		} else {
			best_rank = rank;
			have_best = true;
		}

		best_pdevs[dev_cnt++] = pci_dev_get(pdev);
	}

	if (dev_cnt)
		pdev = pci_dev_get(best_pdevs[get_random_u32_below(dev_cnt)]);

	for (i = 0; i < dev_cnt; i++)
		pci_dev_put(best_pdevs[i]);

	kfree(best_pdevs);
	return pdev;
}
EXPORT_SYMBOL_GPL(pci_p2pmem_find_many);

/**
 * pci_alloc_p2pmem - allocate peer-to-peer DMA memory
 * @pdev: the device to allocate memory from
 * @size: number of bytes to allocate
 *
 * Returns the allocated memory or NULL on error.
 */
void *pci_alloc_p2pmem(struct pci_dev *pdev, size_t size)
{
	void *ret = NULL;
	struct percpu_ref *ref;
	struct pci_p2pdma *p2pdma;

	/*
	 * Pairs with synchronize_rcu() in pci_p2pdma_release() to
	 * ensure pdev->p2pdma is non-NULL for the duration of the
	 * read-lock.
	 */
	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (unlikely(!p2pdma))
		goto out;

	ret = (void *)gen_pool_alloc_owner(p2pdma->pool, size, (void **) &ref);
	if (!ret)
		goto out;

	if (unlikely(!percpu_ref_tryget_live_rcu(ref))) {
		gen_pool_free(p2pdma->pool, (unsigned long) ret, size);
		ret = NULL;
	}
out:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(pci_alloc_p2pmem);

/**
 * pci_free_p2pmem - free peer-to-peer DMA memory
 * @pdev: the device the memory was allocated from
 * @addr: address of the memory that was allocated
 * @size: number of bytes that were allocated
 */
void pci_free_p2pmem(struct pci_dev *pdev, void *addr, size_t size)
{
	struct percpu_ref *ref;
	struct pci_p2pdma *p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);

	gen_pool_free_owner(p2pdma->pool, (uintptr_t)addr, size,
			(void **) &ref);
	percpu_ref_put(ref);
}
EXPORT_SYMBOL_GPL(pci_free_p2pmem);

/**
 * pci_p2pmem_virt_to_bus - return the PCI bus address for a given virtual
 *	address obtained with pci_alloc_p2pmem()
 * @pdev: the device the memory was allocated from
 * @addr: address of the memory that was allocated
 */
pci_bus_addr_t pci_p2pmem_virt_to_bus(struct pci_dev *pdev, void *addr)
{
	struct pci_p2pdma *p2pdma;

	if (!addr)
		return 0;

	p2pdma = rcu_dereference_protected(pdev->p2pdma, 1);
	if (!p2pdma)
		return 0;

	/*
	 * Note: when we added the memory to the pool we used the PCI
	 * bus address as the physical address. So gen_pool_virt_to_phys()
	 * actually returns the bus address despite the misleading name.
	 */
	return gen_pool_virt_to_phys(p2pdma->pool, (unsigned long)addr);
}
EXPORT_SYMBOL_GPL(pci_p2pmem_virt_to_bus);

/**
 * pci_p2pmem_alloc_sgl - allocate peer-to-peer DMA memory in a scatterlist
 * @pdev: the device to allocate memory from
 * @nents: the number of SG entries in the list
 * @length: number of bytes to allocate
 *
 * Return: %NULL on error or &struct scatterlist pointer and @nents on success
 */
struct scatterlist *pci_p2pmem_alloc_sgl(struct pci_dev *pdev,
					 unsigned int *nents, u32 length)
{
	struct scatterlist *sg;
	void *addr;

	sg = kmalloc_obj(*sg);
	if (!sg)
		return NULL;

	sg_init_table(sg, 1);

	addr = pci_alloc_p2pmem(pdev, length);
	if (!addr)
		goto out_free_sg;

	sg_set_buf(sg, addr, length);
	*nents = 1;
	return sg;

out_free_sg:
	kfree(sg);
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_p2pmem_alloc_sgl);

/**
 * pci_p2pmem_free_sgl - free a scatterlist allocated by pci_p2pmem_alloc_sgl()
 * @pdev: the device to allocate memory from
 * @sgl: the allocated scatterlist
 */
void pci_p2pmem_free_sgl(struct pci_dev *pdev, struct scatterlist *sgl)
{
	struct scatterlist *sg;
	int count;

	for_each_sg(sgl, sg, INT_MAX, count) {
		if (!sg)
			break;

		pci_free_p2pmem(pdev, sg_virt(sg), sg->length);
	}
	kfree(sgl);
}
EXPORT_SYMBOL_GPL(pci_p2pmem_free_sgl);

/**
 * pci_p2pmem_publish - publish the peer-to-peer DMA memory for use by
 *	other devices with pci_p2pmem_find()
 * @pdev: the device with peer-to-peer DMA memory to publish
 * @publish: set to true to publish the memory, false to unpublish it
 *
 * Published memory can be used by other PCI device drivers for
 * peer-2-peer DMA operations. Non-published memory is reserved for
 * exclusive use of the device driver that registers the peer-to-peer
 * memory.
 */
void pci_p2pmem_publish(struct pci_dev *pdev, bool publish)
{
	struct pci_p2pdma *p2pdma;

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);
	if (p2pdma)
		p2pdma->p2pmem_published = publish;
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(pci_p2pmem_publish);

/**
 * pci_p2pdma_map_type - Determine the mapping type for P2PDMA transfers
 * @provider: P2PDMA provider structure
 * @dev: Target device for the transfer
 *
 * Determines how peer-to-peer DMA transfers should be mapped between
 * the provider and the target device. The mapping type indicates whether
 * the transfer can be done directly through PCI switches or must go
 * through the host bridge.
 */
enum pci_p2pdma_map_type pci_p2pdma_map_type(struct p2pdma_provider *provider,
					     struct device *dev)
{
	enum pci_p2pdma_map_type type = PCI_P2PDMA_MAP_NOT_SUPPORTED;
	struct pci_dev *pdev = to_pci_dev(provider->owner);
	struct pci_dev *client;
	struct pci_p2pdma *p2pdma;
	int dist;

	if (!pdev->p2pdma)
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;

	if (!dev_is_pci(dev))
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;

	client = to_pci_dev(dev);

	rcu_read_lock();
	p2pdma = rcu_dereference(pdev->p2pdma);

	if (p2pdma)
		type = xa_to_value(xa_load(&p2pdma->map_types,
					   map_types_idx(client)));
	rcu_read_unlock();

	if (type == PCI_P2PDMA_MAP_UNKNOWN)
		return calc_map_type_and_dist(pdev, client, &dist, true);

	return type;
}

void __pci_p2pdma_update_state(struct pci_p2pdma_map_state *state,
		struct device *dev, struct page *page)
{
	struct pci_p2pdma_pagemap *p2p_pgmap = to_p2p_pgmap(page_pgmap(page));

	if (state->mem == p2p_pgmap->mem)
		return;

	state->mem = p2p_pgmap->mem;
	state->map = pci_p2pdma_map_type(p2p_pgmap->mem, dev);
}

/**
 * pci_p2pdma_enable_store - parse a configfs/sysfs attribute store
 *		to enable p2pdma
 * @page: contents of the value to be stored
 * @p2p_dev: returns the PCI device that was selected to be used
 *		(if one was specified in the stored value)
 * @use_p2pdma: returns whether to enable p2pdma or not
 *
 * Parses an attribute value to decide whether to enable p2pdma.
 * The value can select a PCI device (using its full BDF device
 * name) or a boolean (in any format kstrtobool() accepts). A false
 * value disables p2pdma, a true value expects the caller
 * to automatically find a compatible device and specifying a PCI device
 * expects the caller to use the specific provider.
 *
 * pci_p2pdma_enable_show() should be used as the show operation for
 * the attribute.
 *
 * Returns 0 on success
 */
int pci_p2pdma_enable_store(const char *page, struct pci_dev **p2p_dev,
			    bool *use_p2pdma)
{
	struct device *dev;

	dev = bus_find_device_by_name(&pci_bus_type, NULL, page);
	if (dev) {
		*use_p2pdma = true;
		*p2p_dev = to_pci_dev(dev);

		if (!pci_has_p2pmem(*p2p_dev)) {
			pci_err(*p2p_dev,
				"PCI device has no peer-to-peer memory: %s\n",
				page);
			pci_dev_put(*p2p_dev);
			return -ENODEV;
		}

		return 0;
	} else if ((page[0] == '0' || page[0] == '1') && !iscntrl(page[1])) {
		/*
		 * If the user enters a PCI device that  doesn't exist
		 * like "0000:01:00.1", we don't want kstrtobool to think
		 * it's a '0' when it's clearly not what the user wanted.
		 * So we require 0's and 1's to be exactly one character.
		 */
	} else if (!kstrtobool(page, use_p2pdma)) {
		return 0;
	}

	pr_err("No such PCI device: %.*s\n", (int)strcspn(page, "\n"), page);
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(pci_p2pdma_enable_store);

/**
 * pci_p2pdma_enable_show - show a configfs/sysfs attribute indicating
 *		whether p2pdma is enabled
 * @page: contents of the stored value
 * @p2p_dev: the selected p2p device (NULL if no device is selected)
 * @use_p2pdma: whether p2pdma has been enabled
 *
 * Attributes that use pci_p2pdma_enable_store() should use this function
 * to show the value of the attribute.
 *
 * Returns 0 on success
 */
ssize_t pci_p2pdma_enable_show(char *page, struct pci_dev *p2p_dev,
			       bool use_p2pdma)
{
	if (!use_p2pdma)
		return sprintf(page, "0\n");

	if (!p2p_dev)
		return sprintf(page, "1\n");

	return sprintf(page, "%s\n", pci_name(p2p_dev));
}
EXPORT_SYMBOL_GPL(pci_p2pdma_enable_show);
