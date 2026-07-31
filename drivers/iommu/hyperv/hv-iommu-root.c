// SPDX-License-Identifier: GPL-2.0
/*
 * Hyper-V root vIOMMU driver.
 * Copyright (C) 2026, Microsoft, Inc.
 */
#include <linux/pci.h>
#include <linux/dma-map-ops.h>
#include <linux/interval_tree.h>
#include <linux/hyperv.h>
#include <asm/iommu.h>
#include <asm/mshyperv.h>
#include "../dma-iommu.h"

/* We will not claim these PCI devices, eg hypervisor needs it for debugger */
static char *pci_devs_to_skip;
static int __init hv_iommu_setup_skip(char *str)
{
	pci_devs_to_skip = str;

	return 0;
}
/* Eg: hv_iommu_skip=(SSSS:BB:DD.F)(SSSS:BB:DD.F) */
__setup("hv_iommu_skip=", hv_iommu_setup_skip);

/* IOMMU device that we export to the world. HyperV supports max of one */
static struct iommu_device hv_virt_iommu;

struct hv_domain {
	struct iommu_domain iommu_dom;
	u32 domid_num;			      /* as opposed to domain_id.type */
	u64 partid;			      /* partition id */
	spinlock_t mappings_lock;	      /* protects mappings_tree */
	struct rb_root_cached mappings_tree;  /* iova to pa lookup tree */
};

#define to_hv_domain(d) container_of(d, struct hv_domain, iommu_dom)

struct hv_iommu_mapping {
	phys_addr_t paddr;
	struct interval_tree_node iova;
	u32 flags;
};

/*
 * By default, during boot the hypervisor creates one Stage 2 (S2) default
 * domain. Stage 2 means that the page table is controlled by the hypervisor.
 * It has two types:
 *   S2 default: access to entire root partition memory. This for us easily
 *		 maps to IOMMU_DOMAIN_IDENTITY in the iommu subsystem, and
 *		 is called HV_DEVICE_DOMAIN_ID_S2_DEFAULT in the hypervisor.
 *   S2 NULL: Blocks everything except RMRR
 */

/*
 * Create dummy domains to correspond to hypervisor prebuilt default identity
 * and null domains (dummy because we do not make hypercalls to create them).
 */
static struct hv_domain hv_def_identity_dom;
static struct hv_domain hv_null_dom;

static bool hv_special_domain(struct hv_domain *hvdom)
{
	return hvdom == &hv_def_identity_dom || hvdom == &hv_null_dom;
}

struct iommu_domain_geometry default_geometry = (struct iommu_domain_geometry) {
	.aperture_start = 0,
	.aperture_end = -1UL,
	.force_aperture = true,
};

#define HV_IOMMU_PGSIZES SZ_4K	/* for now, to be enhanced */

static atomic_t hv_unique_id;		/* unique numeric id for a new domain */

/*
 * If the current thread is a VMM thread, return the partition id of the VM it
 * is managing, else return HV_PARTITION_ID_INVALID.
 */
static u64 hv_get_current_partid(void)
{
	u64 (*fn)(void);
	u64 ptid;

	fn = symbol_get(mshv_current_partid);
	if (!fn)
		return HV_PARTITION_ID_INVALID;

	ptid = fn();
	symbol_put(mshv_current_partid);

	return ptid;
}

static bool hv_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	default:
		return false;
	}
}

static int hv_iommu_add_tree_mapping(struct hv_domain *hvdom,
				     unsigned long iova, phys_addr_t paddr,
				     size_t size, u32 flags)
{
	unsigned long irqflags;
	struct hv_iommu_mapping *mapping;

	mapping = kzalloc_obj(struct hv_iommu_mapping, GFP_ATOMIC);
	if (!mapping)
		return -ENOMEM;

	mapping->paddr = paddr;
	mapping->iova.start = iova;
	mapping->iova.last = iova + size - 1;
	mapping->flags = flags;

	spin_lock_irqsave(&hvdom->mappings_lock, irqflags);
	interval_tree_insert(&mapping->iova, &hvdom->mappings_tree);
	spin_unlock_irqrestore(&hvdom->mappings_lock, irqflags);

	return 0;
}

/* If size == 0, then last = ULONG_MAX to remove everything with iova 0 */
static size_t hv_iommu_del_tree_mappings(struct hv_domain *hvdom,
					 unsigned long iova, size_t size)
{
	unsigned long flags;
	size_t unmapped = 0;
	unsigned long last = iova + size - 1;
	struct hv_iommu_mapping *mapping = NULL;
	struct interval_tree_node *node, *next;

	spin_lock_irqsave(&hvdom->mappings_lock, flags);
	next = interval_tree_iter_first(&hvdom->mappings_tree, iova, last);
	while (next) {
		node = next;
		mapping = container_of(node, struct hv_iommu_mapping, iova);
		next = interval_tree_iter_next(node, iova, last);

		/* Splitting of a mapping is not supported at present */
		if (mapping->iova.start < iova)
			break;

		unmapped += mapping->iova.last - mapping->iova.start + 1;

		interval_tree_remove(node, &hvdom->mappings_tree);
		kfree(mapping);
	}
	spin_unlock_irqrestore(&hvdom->mappings_lock, flags);

	return unmapped;
}

/* Create a new device domain in the hypervisor */
static int hv_iommu_create_hyp_devdom(struct hv_domain *hvdom)
{
	u64 status;
	struct hv_input_device_domain *ddp;
	struct hv_input_create_device_domain *input;
	unsigned long flags;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));

	ddp = &input->device_domain;
	ddp->partition_id = HV_PARTITION_ID_SELF;
	ddp->domain_id.type = HV_DEVICE_DOMAIN_TYPE_S2;
	ddp->domain_id.id = hvdom->domid_num;

	input->create_device_domain_flags.forward_progress_required = 1;
	input->create_device_domain_flags.inherit_owning_vtl = 0;

	status = hv_do_hypercall(HVCALL_CREATE_DEVICE_DOMAIN, input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status))
		hv_status_err(status, "\n");

	return hv_result_to_errno(status);
}

static struct iommu_domain *hv_iommu_domain_alloc_paging(struct device *dev)
{
	struct hv_domain *hvdom;
	int rc;
	u32 unique_id;
	u64 ptid = hv_get_current_partid();

	if (ptid == HV_PARTITION_ID_INVALID)
		return NULL;

	hvdom = kzalloc_obj(struct hv_domain);
	if (hvdom == NULL)
		return NULL;

	spin_lock_init(&hvdom->mappings_lock);
	hvdom->mappings_tree = RB_ROOT_CACHED;

	unique_id = (u32)atomic_inc_return(&hv_unique_id);
	if (unique_id == HV_DEVICE_DOMAIN_ID_S2_NULL)	/* ie, UINTMAX */
		goto out_err;

	hvdom->domid_num = unique_id;
	hvdom->partid = ptid;
	hvdom->iommu_dom.geometry = default_geometry;
	hvdom->iommu_dom.pgsize_bitmap = HV_IOMMU_PGSIZES;

	rc = hv_iommu_create_hyp_devdom(hvdom);
	if (rc)
		goto out_err;

	return &hvdom->iommu_dom;

out_err:
	kfree(hvdom);
	return NULL;
}

static void hv_iommu_domain_free(struct iommu_domain *immdom)
{
	unsigned long flags;
	u64 status;
	struct hv_input_delete_device_domain *input;
	struct hv_input_device_domain *ddp;
	struct hv_domain *hvdom = to_hv_domain(immdom);

	if (hv_special_domain(hvdom))
		return;

	/* Cleanup any remaining. 0 for size results in ULONG_MAX as the last */
	hv_iommu_del_tree_mappings(hvdom, 0, 0);

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	ddp = &input->device_domain;
	memset(input, 0, sizeof(*input));

	ddp->partition_id = HV_PARTITION_ID_SELF;
	ddp->domain_id.type = HV_DEVICE_DOMAIN_TYPE_S2;
	ddp->domain_id.id = hvdom->domid_num;

	status = hv_do_hypercall(HVCALL_DELETE_DEVICE_DOMAIN, input,
				 NULL);
	local_irq_restore(flags);

	if (!hv_result_success(status))
		hv_status_err(status, "\n");

	kfree(hvdom);
}

static void hv_iommu_det_dev_from_dom(struct pci_dev *pdev)
{
	u64 status;
	unsigned long flags;
	struct hv_input_detach_device_domain *input;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));

	input->partition_id = HV_PARTITION_ID_SELF;
	input->device_id.as_uint64 = hv_build_devid_type_pci(pdev);
	status = hv_do_hypercall(HVCALL_DETACH_DEVICE_DOMAIN, input, NULL);
	local_irq_restore(flags);

	if (!hv_result_success(status))
		hv_status_err(status, "\n");
}

static void hv_iommu_detach_dev(struct hv_domain *hvdom, struct device *dev)
{
	struct pci_dev *pdev;

	/* See the attach function, only PCI devices for now */
	if (!dev_is_pci(dev))
		return;

	pdev = to_pci_dev(dev);
	hv_iommu_det_dev_from_dom(pdev);
}

/*
 * Attach a device to the default domain, or the null domain, or to a domain
 * previously created in the hypervisor.
 */
static int hv_iommu_att_dev2dom(struct hv_domain *hvdom, struct pci_dev *pdev)
{
	unsigned long flags;
	u64 status;
	struct hv_input_attach_device_domain *input;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));

	/* For null domain, hvdom->domid_num == HV_DEVICE_DOMAIN_ID_S2_NULL */
	input->device_domain.partition_id = HV_PARTITION_ID_SELF;
	input->device_domain.domain_id.type = HV_DEVICE_DOMAIN_TYPE_S2;
	input->device_domain.domain_id.id = hvdom->domid_num;

	/* NB: Upon guest shutdown, device is re-attached to the default domain
	 *     without explicit detach.
	 */
	input->device_id.as_uint64 = hv_build_devid_type_pci(pdev);

	status = hv_do_hypercall(HVCALL_ATTACH_DEVICE_DOMAIN, input, NULL);
	local_irq_restore(flags);

	if (!hv_result_success(status))
		hv_status_err(status, "\n");

	return hv_result_to_errno(status);
}

static int hv_iommu_attach_dev(struct iommu_domain *immdom, struct device *dev,
			       struct iommu_domain *old)
{
	struct pci_dev *pdev;
	int rc;
	struct hv_domain *hvdom_new = to_hv_domain(immdom);
	struct hv_domain *hvdom_prev = to_hv_domain(old);

	/* Only allow PCI devices for now */
	if (!dev_is_pci(dev))
		return -EINVAL;

	pdev = to_pci_dev(dev);

	/* There are no explicit detach calls, hence check if we need to detach
	 * first. Also, in case of guest shutdown, it's the VMM thread that
	 * attaches it back to the hv_def_identity_dom, and hvdom_prev will not
	 * be null then. It is null during boot.
	 */
	if (hvdom_prev && !hv_special_domain(hvdom_prev))
		hv_iommu_detach_dev(hvdom_prev, dev);

	rc = hv_iommu_att_dev2dom(hvdom_new, pdev);

	if (rc == 0)
		dev_iommu_priv_set(dev, hvdom_new);  /* sets "private" field */
	else {
		if (hvdom_prev && !hv_special_domain(hvdom_prev))
			WARN(1, "Failed to attach dev:%s\n", pci_name(pdev));
		dev_iommu_priv_set(dev, NULL);
	}

	return rc;
}

static u64 hv_iommu_unmap_batch(u32 domid_num, ulong iova, u16 count)
{
	ulong flags;
	struct hv_input_unmap_device_gpa_pages *input;
	u64 status;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));

	input->device_domain.partition_id = HV_PARTITION_ID_SELF;
	input->device_domain.domain_id.type = HV_DEVICE_DOMAIN_TYPE_S2;
	input->device_domain.domain_id.id = domid_num;
	input->target_device_va_base = iova;

	status = hv_do_rep_hypercall(HVCALL_UNMAP_DEVICE_GPA_PAGES, count,
				     0, input, NULL);
	local_irq_restore(flags);

	if (!hv_result_success(status))
		hv_status_err(status, "iova:0x%lx count:0x%x\n", iova, count);

	return status;
}

static size_t hv_iommu_unmap_pages(struct iommu_domain *immdom, ulong iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	unsigned long npages;
	u64 status;
	struct hv_domain *hvdom = to_hv_domain(immdom);
	size_t unmapped, tot_done = 0, size = pgsize * pgcount;

	unmapped = hv_iommu_del_tree_mappings(hvdom, iova, size);
	if (unmapped < size)
		pr_err("%s: could not delete all mappings (%lx:%lx/%lx)\n",
		       __func__, iova, unmapped, size);

	npages = unmapped >> HV_HYP_PAGE_SHIFT;

	while (npages) {
		int done, count = min(npages, HV_REP_COUNT_MAX);

		status = hv_iommu_unmap_batch(hvdom->domid_num, iova, count);

		done = hv_repcomp(status);
		tot_done += done;
		npages -= done;
		iova += done << HV_HYP_PAGE_SHIFT;

		if (!hv_result_success(status))
			break;
	}

	return tot_done << HV_HYP_PAGE_SHIFT;
}

/* Return: must return exact status from the hypercall without changes */
static u64 hv_iommu_map_pgs(struct hv_domain *hvdom,
			    unsigned long iova, phys_addr_t paddr,
			    unsigned long npages, u32 map_flags)
{
	u64 status;
	int i;
	struct hv_input_map_device_gpa_pages *input;
	unsigned long flags, pfn;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));

	input->device_domain.partition_id = HV_PARTITION_ID_SELF;
	input->device_domain.domain_id.type = HV_DEVICE_DOMAIN_TYPE_S2;
	input->device_domain.domain_id.id = hvdom->domid_num;
	input->map_flags = map_flags;
	input->target_device_va_base = iova;

	pfn = paddr >> HV_HYP_PAGE_SHIFT;
	for (i = 0; i < npages; i++, pfn++)
		input->gpa_page_list[i] = pfn;

	status = hv_do_rep_hypercall(HVCALL_MAP_DEVICE_GPA_PAGES, npages, 0,
				     input, NULL);
	local_irq_restore(flags);

	return status;
}

#define HV_MAP_DEVICE_GPA_BATCH_SIZE   \
	((HV_HYP_PAGE_SIZE - sizeof(struct hv_input_map_device_gpa_pages)) \
			/ sizeof(u64))

/*
 * The core VFIO code loops over memory ranges calling this function with the
 * largest pgsize from HV_IOMMU_PGSIZES. cond_resched() is in vfio_iommu_map.
 */
static int hv_iommu_map_pages(struct iommu_domain *immdom, ulong iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int prot, gfp_t gfp, size_t *mapped)
{
	u32 map_flags;
	int ret;
	u64 status;
	unsigned long npages, done = 0;
	struct hv_domain *hvdom = to_hv_domain(immdom);
	size_t size = pgsize * pgcount;

	map_flags = HV_MAP_GPA_READABLE;	/* required */
	map_flags |= prot & IOMMU_WRITE ? HV_MAP_GPA_WRITABLE : 0;

	ret = hv_iommu_add_tree_mapping(hvdom, iova, paddr, size, map_flags);
	if (ret)
		return ret;

	npages = size >> HV_HYP_PAGE_SHIFT;
	while (done < npages) {
		ulong completed, remain = npages - done;

		remain = min(remain, HV_MAP_DEVICE_GPA_BATCH_SIZE);

		status = hv_iommu_map_pgs(hvdom, iova, paddr, remain,
					  map_flags);

		completed = hv_repcomp(status);
		done = done + completed;
		iova = iova + (completed << HV_HYP_PAGE_SHIFT);
		paddr = paddr + (completed << HV_HYP_PAGE_SHIFT);

		if (hv_result(status) == HV_STATUS_INSUFFICIENT_MEMORY) {
			ret = hv_call_deposit_pages(NUMA_NO_NODE,
						    hv_current_partition_id,
						    256);
			if (ret)
				break;
			continue;
		}
		if (!hv_result_success(status))
			break;
	}

	if (!hv_result_success(status)) {
		size_t done_size = done << HV_HYP_PAGE_SHIFT;

		hv_status_err(status, "pgs:%lx/%lx iova:%lx\n",
			      done, npages, iova);
		/*
		 * lookup tree has all mappings [0 - size-1]. Below unmap will
		 * only remove from [0 - done], we need to remove second chunk
		 * [done+1 - size-1].
		 */
		hv_iommu_del_tree_mappings(hvdom, iova, size - done_size);
		hv_iommu_unmap_pages(immdom, iova - done_size, HV_HYP_PAGE_SIZE,
				     done, NULL);
		if (mapped)
			*mapped = 0;
	} else
		if (mapped)
			*mapped = size;

	return hv_result_to_errno(status);
}

static phys_addr_t hv_iommu_iova_to_phys(struct iommu_domain *immdom,
					 dma_addr_t iova)
{
	unsigned long flags;
	struct hv_iommu_mapping *mapping;
	struct interval_tree_node *node;
	u64 paddr = 0;
	struct hv_domain *hvdom = to_hv_domain(immdom);

	spin_lock_irqsave(&hvdom->mappings_lock, flags);
	node = interval_tree_iter_first(&hvdom->mappings_tree, iova, iova);
	if (node) {
		mapping = container_of(node, struct hv_iommu_mapping, iova);
		paddr = mapping->paddr + (iova - mapping->iova.start);
	}
	spin_unlock_irqrestore(&hvdom->mappings_lock, flags);

	return paddr;
}

/*
 * Currently, hypervisor does not provide list of devices it is using
 * dynamically. So use this to allow users to manually specify devices that
 * should be skipped. (eg. hypervisor debugger using some network device).
 */
static struct iommu_device *hv_iommu_probe_device(struct device *dev)
{
	if (!dev_is_pci(dev))
		return ERR_PTR(-ENODEV);

	if (pci_devs_to_skip && *pci_devs_to_skip) {
		int rc, parsed, segment, bus, slot, func;
		int pos = 0;
		struct pci_dev *pdev = to_pci_dev(dev);

		do {
			parsed = 0;

			rc = sscanf(pci_devs_to_skip + pos, " (%x:%x:%x.%x) %n",
				    &segment, &bus, &slot, &func, &parsed);

			if (rc != 4 || parsed <= 0)
				break;

			if (pci_domain_nr(pdev->bus) == segment &&
			    pdev->bus->number == bus &&
			    PCI_SLOT(pdev->devfn) == slot &&
			    PCI_FUNC(pdev->devfn) == func) {

				dev_info(dev, "skipped by Hyper-V IOMMU\n");
				return ERR_PTR(-ENODEV);
			}
			pos += parsed;

		} while (pci_devs_to_skip[pos]);
	}

	/* Device will be explicitly attached to the default domain, so no need
	 * to do dev_iommu_priv_set() here.
	 */

	return &hv_virt_iommu;
}

static void hv_iommu_probe_finalize(struct device *dev)
{
	struct iommu_domain *immdom = iommu_get_domain_for_dev(dev);

	if (immdom && immdom->type == IOMMU_DOMAIN_DMA)
		iommu_setup_dma_ops(dev, immdom);
	else
		set_dma_ops(dev, NULL);
}

static void hv_iommu_release_device(struct device *dev)
{
	struct hv_domain *hvdom = dev_iommu_priv_get(dev);

	/* Need to detach device from device domain if necessary. */
	if (hvdom)
		hv_iommu_detach_dev(hvdom, dev);

	dev_iommu_priv_set(dev, NULL);
	set_dma_ops(dev, NULL);
}

static struct iommu_group *hv_iommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	else
		return generic_device_group(dev);
}

static int hv_iommu_def_domain_type(struct device *dev)
{
	/* The hypervisor always creates this by default during boot */
	return IOMMU_DOMAIN_IDENTITY;
}

static struct iommu_ops hv_iommu_ops = {
	.capable	    = hv_iommu_capable,
	.domain_alloc_paging	= hv_iommu_domain_alloc_paging,
	.probe_device	    = hv_iommu_probe_device,
	.probe_finalize     = hv_iommu_probe_finalize,
	.release_device     = hv_iommu_release_device,
	.def_domain_type    = hv_iommu_def_domain_type,
	.device_group	    = hv_iommu_device_group,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev   = hv_iommu_attach_dev,
		.map_pages    = hv_iommu_map_pages,
		.unmap_pages  = hv_iommu_unmap_pages,
		.iova_to_phys = hv_iommu_iova_to_phys,
		.free	      = hv_iommu_domain_free,
	},
	.owner		    = THIS_MODULE,
	.identity_domain = &hv_def_identity_dom.iommu_dom,
	.blocked_domain  = &hv_null_dom.iommu_dom,
};

static const struct iommu_domain_ops hv_special_domain_ops = {
	.attach_dev = hv_iommu_attach_dev,
};

static void __init hv_initialize_special_domains(void)
{
	hv_def_identity_dom.iommu_dom.type = IOMMU_DOMAIN_IDENTITY;
	hv_def_identity_dom.iommu_dom.ops = &hv_special_domain_ops;
	hv_def_identity_dom.iommu_dom.owner = &hv_iommu_ops;
	hv_def_identity_dom.iommu_dom.geometry = default_geometry;
	hv_def_identity_dom.domid_num = HV_DEVICE_DOMAIN_ID_S2_DEFAULT; /* 0 */

	hv_null_dom.iommu_dom.type = IOMMU_DOMAIN_BLOCKED;
	hv_null_dom.iommu_dom.ops = &hv_special_domain_ops;
	hv_null_dom.iommu_dom.owner = &hv_iommu_ops;
	hv_null_dom.iommu_dom.geometry = default_geometry;
	hv_null_dom.domid_num = HV_DEVICE_DOMAIN_ID_S2_NULL;  /* INTMAX */
}

static int __init hv_iommu_init(void)
{
	int ret;
	struct iommu_device *iommup = &hv_virt_iommu;

	if (!hv_is_hyperv_initialized())
		return -ENODEV;

	ret = iommu_device_sysfs_add(iommup, NULL, NULL, "%s", "hyperv-iommu");
	if (ret) {
		pr_err("Hyper-V: iommu_device_sysfs_add failed: %d\n", ret);
		return ret;
	}

	/* This must come before iommu_device_register() because the latter
	 * calls into the hooks.
	 */
	hv_initialize_special_domains();

	ret = iommu_device_register(iommup, &hv_iommu_ops, NULL);
	if (ret) {
		pr_err("Hyper-V: iommu_device_register failed: %d\n", ret);
		goto err_sysfs_remove;
	}

	pr_info("Hyper-V IOMMU initialized\n");

	return 0;

err_sysfs_remove:
	iommu_device_sysfs_remove(iommup);
	return ret;
}

void __init hv_iommu_detect(void)
{
	if (no_iommu || iommu_detected || hv_l1vh_partition())
		return;

	if (!(ms_hyperv.misc_features & HV_DEVICE_DOMAIN_AVAILABLE))
		return;

	iommu_detected = 1;
	x86_init.iommu.iommu_init = hv_iommu_init;

	pci_request_acs();
}
