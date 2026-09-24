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

/*
 * We will not claim these PCI devices. Eg hypervisor debugger is using it
 * for a dynamic debug session. They cannot be enumerated under static ACPI
 * device scope.
 */
static char *hv_skip_pci_devs;
static int __init hv_iommu_setup_skip(char *str)
{
	hv_skip_pci_devs = str;
	return 1;
}
/* Eg: hv_iommu_skip=(SSSS:BB:DD.F)(SSSS:BB:DD.F) */
__setup("hv_iommu_skip=", hv_iommu_setup_skip);

static ulong hv_iommu_max_iova;
static struct iommu_domain_ops hv_paging_domain_ops;

/* IOMMU device that we export to the world. MSHV supports max of one */
static struct iommu_device hv_virt_iommu;

struct hv_domain {
	struct iommu_domain iommu_dom;
	u32 domid_num;			      /* as opposed to domain_id.type */
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
 * domain. It has two types:
 *   S2 default: access to entire root partition memory. This for the host
 *		 maps to IOMMU_DOMAIN_IDENTITY in the iommu subsystem, and
 *		 is called HV_DEVICE_DOMAIN_ID_S2_DEFAULT in the hypervisor.
 *   S2 NULL: Blocks everything, ie, IOMMU_DOMAIN_BLOCKED in linux.
 */

/*
 * Create dummy domains to correspond to hypervisor prebuilt default identity
 * and null domains (dummy because we do not make hypercalls to create them).
 */
static struct hv_domain hv_def_identity_dom;
static struct hv_domain hv_def_blocked_dom;

static bool hv_special_domain(struct hv_domain *hvdom)
{
	return hvdom == &hv_def_identity_dom || hvdom == &hv_def_blocked_dom;
}

#define HV_IOMMU_PGSIZES SZ_4K	      /* for now, to be enhanced */

static DEFINE_IDA(hv_iommu_domids);   /* unique numeric ids for new domains */

static bool hv_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	default:
		return false;
	}
}

static int hv_iommu_add_tree_mapping(struct hv_domain *hvdom, ulong iova,
				     phys_addr_t paddr, size_t size, u32 flags)
{
	ulong irqflags;
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

/* If size == 0, then last = ULONG_MAX. With iova 0, will remove everything */
static size_t hv_iommu_del_tree_mappings(struct hv_domain *hvdom, ulong iova,
					 size_t size)
{
	ulong flags;
	size_t unmapped = 0;
	ulong last = iova + size - 1;
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
	ulong flags;

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

	if (!dev_is_pci(dev))
		return NULL;

	hvdom = kzalloc_obj(struct hv_domain);
	if (hvdom == NULL)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&hvdom->mappings_lock);
	hvdom->mappings_tree = RB_ROOT_CACHED;

	rc = ida_alloc_range(&hv_iommu_domids,
			     HV_DEVICE_DOMAIN_ID_S2_DEFAULT + 1,
			     HV_DEVICE_DOMAIN_ID_S2_NULL - 1, GFP_KERNEL);
	if (rc < 0)
		goto out_err;

	hvdom->domid_num = rc;
	hvdom->iommu_dom.pgsize_bitmap = HV_IOMMU_PGSIZES;
	hvdom->iommu_dom.geometry.aperture_start = 0;
	hvdom->iommu_dom.geometry.aperture_end = hv_iommu_max_iova;
	hvdom->iommu_dom.geometry.force_aperture = true;
	hvdom->iommu_dom.ops = &hv_paging_domain_ops;

	rc = hv_iommu_create_hyp_devdom(hvdom);
	if (rc)
		goto out_err1;

	return &hvdom->iommu_dom;

out_err1:
	ida_free(&hv_iommu_domids, hvdom->domid_num);
out_err:
	kfree(hvdom);
	return ERR_PTR(rc);
}

static void hv_iommu_domain_free(struct iommu_domain *immdom)
{
	ulong flags;
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

	if (hv_result_success(status))
		ida_free(&hv_iommu_domids, hvdom->domid_num);
	else
		hv_status_err(status, "\n");

	kfree(hvdom);
}

/*
 * Attach a device to the default domain, or the null domain, or to a domain
 * previously created in the hypervisor.
 */
static int hv_iommu_att_dev2dom(struct hv_domain *hvdom, struct pci_dev *pdev)
{
	ulong flags;
	u64 status;
	struct hv_input_attach_device_domain *input;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));

	/* For null domain, hvdom->domid_num == HV_DEVICE_DOMAIN_ID_S2_NULL */
	input->device_domain.partition_id = HV_PARTITION_ID_SELF;
	input->device_domain.domain_id.type = HV_DEVICE_DOMAIN_TYPE_S2;
	input->device_domain.domain_id.id = hvdom->domid_num;

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

	pdev = to_pci_dev(dev);

	rc = hv_iommu_att_dev2dom(hvdom_new, pdev);
	if (rc)
		WARN(1, "Failed to attach pdev:%s\n", pci_name(pdev));

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

static size_t hv_iommu_hyp_unmap_pages(struct iommu_domain *immdom, ulong iova,
				       size_t size)
{
	u64 status;
	struct hv_domain *hvdom = to_hv_domain(immdom);
	size_t tot_done = 0;
	ulong npages = size >> HV_HYP_PAGE_SHIFT;

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

static size_t hv_iommu_unmap_pages(struct iommu_domain *immdom, ulong iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	struct hv_domain *hvdom = to_hv_domain(immdom);
	size_t unmapped_sz, size = pgsize * pgcount;

	unmapped_sz = hv_iommu_del_tree_mappings(hvdom, iova, size);
	if (unmapped_sz != size)
		pr_err("%s: could not delete exact mappings (%lx:%lx/%lx)\n",
		       __func__, iova, unmapped_sz, size);

	size = hv_iommu_hyp_unmap_pages(immdom, iova, unmapped_sz);
	if (size != unmapped_sz)
		WARN(1, "Failed to unmap exact sizes(%lx/%lx)\n", unmapped_sz,
		     size);

	return size;
}

/* Return: must return exact status from the hypercall without changes */
static u64 hv_iommu_map_pgs(struct hv_domain *hvdom, ulong iova,
			    phys_addr_t paddr, ulong npages, u32 map_flags)
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

	if (!hv_result_success(status))
		hv_status_err(status, "npgs:%lx paddr:%llx iova:%lx mf:0x%x\n",
			      npages, paddr, iova, map_flags);
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
	u64 status = 0;
	ulong npages, sav_iova = iova, done = 0;
	struct hv_domain *hvdom = to_hv_domain(immdom);
	phys_addr_t sav_paddr = paddr;
	size_t done_size, size = pgsize * pgcount;

	map_flags = HV_MAP_GPA_READABLE;	/* required */
	map_flags |= prot & IOMMU_WRITE ? HV_MAP_GPA_WRITABLE : 0;

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

		if (hv_result_needs_memory(status)) {
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

	done_size = done << HV_HYP_PAGE_SHIFT;

	if (mapped)
		*mapped = done_size;

	if (done_size == 0)
		return hv_result_to_errno(status);

	ret = hv_iommu_add_tree_mapping(hvdom, sav_iova, sav_paddr, done_size,
					map_flags);
	if (ret) {
		size = hv_iommu_hyp_unmap_pages(immdom, sav_iova, done_size);
		if (size != done_size)
			WARN(1, "Failed to unmap exact sizes(%lx/%lx)\n",
			     done_size, size);
		if (mapped)
			*mapped = done_size - size;

		return ret;
	}

	return 0;
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

	if (hv_skip_pci_devs && *hv_skip_pci_devs) {
		int rc, parsed, segment, bus, slot, func;
		int pos = 0;
		struct pci_dev *pdev = to_pci_dev(dev);

		do {
			parsed = 0;

			rc = sscanf(hv_skip_pci_devs + pos, " (%x:%x:%x.%x) %n",
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

		} while (hv_skip_pci_devs[pos]);
	}

	return &hv_virt_iommu;
}

static struct iommu_group *hv_iommu_device_group(struct device *dev)
{
	return pci_device_group(dev);
}

static void hv_iommu_get_resv_regions(struct device *dev,
				      struct list_head *head)
{
	struct iommu_resv_region *reg;

	/* reserve the entire LAPIC region */
	reg = iommu_alloc_resv_region(0xfee00000, SZ_1M, 0, IOMMU_RESV_MSI,
				      GFP_KERNEL);
	if (reg)
		list_add_tail(&reg->list, head);
}

static struct iommu_domain_ops hv_paging_domain_ops = {
	.attach_dev = hv_iommu_attach_dev,
	.map_pages = hv_iommu_map_pages,
	.unmap_pages = hv_iommu_unmap_pages,
	.iova_to_phys = hv_iommu_iova_to_phys,
	.free = hv_iommu_domain_free,
};

static struct iommu_ops hv_iommu_ops = {
	.capable	    = hv_iommu_capable,
	.domain_alloc_paging	= hv_iommu_domain_alloc_paging,
	.probe_device	    = hv_iommu_probe_device,
	.device_group	    = hv_iommu_device_group,
	.get_resv_regions   = hv_iommu_get_resv_regions,
	.owner		    = THIS_MODULE,
	.identity_domain    = &hv_def_identity_dom.iommu_dom,
	.blocked_domain     = &hv_def_blocked_dom.iommu_dom,
	.release_domain     = &hv_def_blocked_dom.iommu_dom,
};

static const struct iommu_domain_ops hv_special_domain_ops = {
	.attach_dev = hv_iommu_attach_dev,
};

static void __init hv_initialize_special_domains(void)
{
	hv_def_identity_dom.iommu_dom.type = IOMMU_DOMAIN_IDENTITY;
	hv_def_identity_dom.iommu_dom.ops = &hv_special_domain_ops;
	hv_def_identity_dom.iommu_dom.owner = &hv_iommu_ops;
	hv_def_identity_dom.domid_num = HV_DEVICE_DOMAIN_ID_S2_DEFAULT; /* 0 */

	hv_def_blocked_dom.iommu_dom.type = IOMMU_DOMAIN_BLOCKED;
	hv_def_blocked_dom.iommu_dom.ops = &hv_special_domain_ops;
	hv_def_blocked_dom.iommu_dom.owner = &hv_iommu_ops;
	hv_def_blocked_dom.domid_num = HV_DEVICE_DOMAIN_ID_S2_NULL; /* INTMAX */
}

static int hv_iommu_get_caps(struct hv_output_get_iommu_capabilities *caps)
{
	u64 status;
	unsigned long flags;
	struct hv_input_get_iommu_capabilities *input;
	struct hv_output_get_iommu_capabilities *output;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	output = *this_cpu_ptr(hyperv_pcpu_output_arg);
	memset(input, 0, sizeof(*input));
	input->partition_id = HV_PARTITION_ID_SELF;
	status = hv_do_hypercall(HVCALL_GET_IOMMU_CAPABILITIES, input, output);
	*caps = *output;

	local_irq_restore(flags);

	if (!hv_result_success(status))
		hv_status_err(status, "\n");

	return hv_result_to_errno(status);
}

static int __init hv_iommu_init(void)
{
	int rc;
	struct iommu_device *iommup = &hv_virt_iommu;
	struct hv_output_get_iommu_capabilities caps;

	if (!hv_is_hyperv_initialized())
		return -ENODEV;

	rc = hv_iommu_get_caps(&caps);
	if (rc)
		return rc;

	hv_iommu_max_iova = ((ulong)1 << caps.max_iova_width) - 1;

	rc = iommu_device_sysfs_add(iommup, NULL, NULL, "%s", "hyperv-iommu");
	if (rc) {
		pr_err("Hyper-V: iommu_device_sysfs_add failed: %d\n", rc);
		return rc;
	}

	/* This must come before iommu_device_register() because the latter
	 * calls into the hooks.
	 */
	hv_initialize_special_domains();

	rc = iommu_device_register(iommup, &hv_iommu_ops, NULL);
	if (rc) {
		pr_err("Hyper-V: iommu_device_register failed: %d\n", rc);
		goto err_sysfs_remove;
	}

	pr_info("Hyper-V IOMMU initialized\n");

	return 0;

err_sysfs_remove:
	iommu_device_sysfs_remove(iommup);
	return rc;
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
