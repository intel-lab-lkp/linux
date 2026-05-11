// SPDX-License-Identifier: GPL-2.0

/*
 * Hyper-V IOMMU driver.
 *
 * Copyright (C) 2019, 2024-2026 Microsoft, Inc.
 */

#define pr_fmt(fmt) "Hyper-V pvIOMMU: " fmt
#define dev_fmt(fmt) pr_fmt(fmt)

#include <linux/iommu.h>
#include <linux/pci.h>
#include <linux/dma-map-ops.h>
#include <linux/generic_pt/iommu.h>
#include <linux/pci-ats.h>

#include <asm/iommu.h>
#include <asm/hypervisor.h>
#include <asm/mshyperv.h>

#include "iommu.h"
#include "../iommu-pages.h"

struct hv_iommu_dev *hv_iommu_device;

/*
 * Identity and blocking domains are static singletons: identity is a 1:1
 * passthrough with no page table, blocking rejects all DMA. Neither holds
 * per-IOMMU state, so one instance suffices even with multiple vIOMMUs.
 */
static struct hv_iommu_domain hv_identity_domain;
static struct hv_iommu_domain hv_blocking_domain;
static const struct iommu_domain_ops hv_iommu_identity_domain_ops;
static const struct iommu_domain_ops hv_iommu_blocking_domain_ops;
static struct iommu_ops hv_iommu_ops;
static LIST_HEAD(hv_iommu_pci_bus_list);
static DEFINE_SPINLOCK(hv_iommu_pci_bus_lock);

#define hv_iommu_present(iommu_cap) (iommu_cap & HV_IOMMU_CAP_PRESENT)
#define hv_iommu_s1_domain_supported(iommu_cap) (iommu_cap & HV_IOMMU_CAP_S1)
#define hv_iommu_5lvl_supported(iommu_cap) (iommu_cap & HV_IOMMU_CAP_S1_5LVL)
#define hv_iommu_ats_supported(iommu_cap) (iommu_cap & HV_IOMMU_CAP_ATS)

int hv_iommu_register_pci_bus(int pci_domain_nr, u32 logical_dev_id_prefix)
{
	struct hv_pci_busdata *bus, *new;
	int ret = 0;

	if (no_iommu || !iommu_detected)
		return 0;

	new = kzalloc_obj(*new, GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	spin_lock(&hv_iommu_pci_bus_lock);
	list_for_each_entry(bus, &hv_iommu_pci_bus_list, list) {
		if (bus->pci_domain_nr != pci_domain_nr)
			continue;

		if (bus->logical_dev_id_prefix != logical_dev_id_prefix) {
			pr_err("stale registration for PCI domain %d (old prefix 0x%08x, new 0x%08x)\n",
			       pci_domain_nr, bus->logical_dev_id_prefix,
			       logical_dev_id_prefix);
			ret = -EEXIST;
		}

		goto out_free;
	}

	new->pci_domain_nr = pci_domain_nr;
	new->logical_dev_id_prefix = logical_dev_id_prefix;
	list_add(&new->list, &hv_iommu_pci_bus_list);
	spin_unlock(&hv_iommu_pci_bus_lock);
	return 0;

out_free:
	spin_unlock(&hv_iommu_pci_bus_lock);
	kfree(new);
	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(hv_iommu_register_pci_bus, "pci-hyperv");

void hv_iommu_unregister_pci_bus(int pci_domain_nr)
{
	struct hv_pci_busdata *bus, *tmp;

	spin_lock(&hv_iommu_pci_bus_lock);
	list_for_each_entry_safe(bus, tmp, &hv_iommu_pci_bus_list, list) {
		if (bus->pci_domain_nr == pci_domain_nr) {
			list_del(&bus->list);
			kfree(bus);
			break;
		}
	}
	spin_unlock(&hv_iommu_pci_bus_lock);
}
EXPORT_SYMBOL_FOR_MODULES(hv_iommu_unregister_pci_bus, "pci-hyperv");

/*
 * Look up the logical device ID for a vPCI device. Returns 0 on success
 * with *logical_id filled in; -ENODEV if no entry registered for this
 * device's vPCI bus.
 */
static int hv_iommu_lookup_logical_dev_id(struct pci_dev *pdev, u64 *logical_id)
{
	struct hv_pci_busdata *bus;
	int domain = pci_domain_nr(pdev->bus);
	int ret = -ENODEV;

	spin_lock(&hv_iommu_pci_bus_lock);
	list_for_each_entry(bus, &hv_iommu_pci_bus_list, list) {
		if (bus->pci_domain_nr == domain) {
			*logical_id = (u64)bus->logical_dev_id_prefix |
				      PCI_FUNC(pdev->devfn);
			ret = 0;
			break;
		}
	}
	spin_unlock(&hv_iommu_pci_bus_lock);
	return ret;
}

static int hv_create_device_domain(struct hv_iommu_domain *hv_domain, u32 domain_stage)
{
	int ret;
	u64 status;
	unsigned long flags;
	struct hv_input_create_device_domain *input;

	ret = ida_alloc_range(&hv_iommu_device->domain_ids,
			hv_iommu_device->first_domain, hv_iommu_device->last_domain,
			GFP_KERNEL);
	if (ret < 0)
		return ret;

	hv_domain->device_domain.partition_id = HV_PARTITION_ID_SELF;
	hv_domain->device_domain.domain_id.type = domain_stage;
	hv_domain->device_domain.domain_id.id = ret;
	hv_domain->hv_iommu = hv_iommu_device;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));
	input->device_domain = hv_domain->device_domain;
	input->create_device_domain_flags.forward_progress_required = 1;
	input->create_device_domain_flags.inherit_owning_vtl = 0;
	status = hv_do_hypercall(HVCALL_CREATE_DEVICE_DOMAIN, input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status)) {
		pr_err("HVCALL_CREATE_DEVICE_DOMAIN failed, status %lld\n", status);
		ida_free(&hv_iommu_device->domain_ids, hv_domain->device_domain.domain_id.id);
	}

	return hv_result_to_errno(status);
}

static void hv_delete_device_domain(struct hv_iommu_domain *hv_domain)
{
	u64 status;
	unsigned long flags;
	struct hv_input_delete_device_domain *input;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));
	input->device_domain = hv_domain->device_domain;
	status = hv_do_hypercall(HVCALL_DELETE_DEVICE_DOMAIN, input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_DELETE_DEVICE_DOMAIN failed, status %lld\n", status);

	ida_free(&hv_domain->hv_iommu->domain_ids, hv_domain->device_domain.domain_id.id);
}

static bool hv_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	case IOMMU_CAP_DEFERRED_FLUSH:
		return true;
	default:
		return false;
	}
}

static void hv_flush_device_domain(struct hv_iommu_domain *hv_domain)
{
	u64 status;
	unsigned long flags;
	struct hv_input_flush_device_domain *input;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));
	input->device_domain = hv_domain->device_domain;
	status = hv_do_hypercall(HVCALL_FLUSH_DEVICE_DOMAIN, input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_FLUSH_DEVICE_DOMAIN failed, status %lld\n", status);
}

static void hv_iommu_detach_dev(struct iommu_domain *domain, struct device *dev)
{
	u64 status;
	unsigned long flags;
	struct hv_input_detach_device_domain *input;
	struct pci_dev *pdev;
	struct hv_iommu_domain *hv_domain = to_hv_iommu_domain(domain);
	struct hv_iommu_endpoint *vdev = dev_iommu_priv_get(dev);

	/* See the attach function, only PCI devices for now */
	if (!dev_is_pci(dev) || vdev->hv_domain != hv_domain)
		return;

	pdev = to_pci_dev(dev);

	dev_dbg(dev, "detaching from domain %d\n", hv_domain->device_domain.domain_id.id);

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));
	input->partition_id = HV_PARTITION_ID_SELF;
	if (hv_iommu_lookup_logical_dev_id(pdev, &input->device_id.as_uint64)) {
		local_irq_restore(flags);
		dev_warn(&pdev->dev, "no IOMMU registration for vPCI bus on detach\n");
		return;
	}
	status = hv_do_hypercall(HVCALL_DETACH_DEVICE_DOMAIN, input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_DETACH_DEVICE_DOMAIN failed, status %lld\n", status);

	hv_flush_device_domain(hv_domain);

	vdev->hv_domain = NULL;
}

static int hv_iommu_attach_dev(struct iommu_domain *domain, struct device *dev,
			       struct iommu_domain *old)
{
	u64 status;
	unsigned long flags;
	struct pci_dev *pdev;
	struct hv_input_attach_device_domain *input;
	struct hv_iommu_endpoint *vdev = dev_iommu_priv_get(dev);
	struct hv_iommu_domain *hv_domain = to_hv_iommu_domain(domain);
	int ret;

	/* Only allow PCI devices for now */
	if (!dev_is_pci(dev))
		return -EINVAL;

	if (vdev->hv_domain == hv_domain)
		return 0;

	if (vdev->hv_domain)
		hv_iommu_detach_dev(&vdev->hv_domain->domain, dev);

	pdev = to_pci_dev(dev);
	dev_dbg(dev, "attaching to domain %d\n",
		hv_domain->device_domain.domain_id.id);

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));
	input->device_domain = hv_domain->device_domain;
	ret = hv_iommu_lookup_logical_dev_id(pdev, &input->device_id.as_uint64);
	if (ret) {
		local_irq_restore(flags);
		dev_err(&pdev->dev, "no IOMMU registration for vPCI bus\n");
		return ret;
	}
	status = hv_do_hypercall(HVCALL_ATTACH_DEVICE_DOMAIN, input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_ATTACH_DEVICE_DOMAIN failed, status %lld\n", status);
	else
		vdev->hv_domain = hv_domain;

	return hv_result_to_errno(status);
}

static int hv_iommu_get_logical_device_property(struct device *dev,
					u32 code,
					struct hv_output_get_logical_device_property *property)
{
	u64 status, lid;
	unsigned long flags;
	int ret;
	struct hv_input_get_logical_device_property *input;
	struct hv_output_get_logical_device_property *output;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	output = *this_cpu_ptr(hyperv_pcpu_input_arg) + sizeof(*input);
	memset(input, 0, sizeof(*input));
	input->partition_id = HV_PARTITION_ID_SELF;
	ret = hv_iommu_lookup_logical_dev_id(to_pci_dev(dev), &lid);
	if (ret) {
		local_irq_restore(flags);
		return ret;
	}
	input->logical_device_id = lid;
	input->code = code;
	status = hv_do_hypercall(HVCALL_GET_LOGICAL_DEVICE_PROPERTY, input, output);
	*property = *output;

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_GET_LOGICAL_DEVICE_PROPERTY failed, status %lld\n", status);

	return hv_result_to_errno(status);
}

static struct iommu_device *hv_iommu_probe_device(struct device *dev)
{
	struct pci_dev *pdev;
	struct hv_iommu_endpoint *vdev;
	struct hv_output_get_logical_device_property device_iommu_property = {0};

	if (!dev_is_pci(dev))
		return ERR_PTR(-ENODEV);

	pdev = to_pci_dev(dev);

	if (hv_iommu_get_logical_device_property(dev,
						 HV_LOGICAL_DEVICE_PROPERTY_PVIOMMU,
						 &device_iommu_property) ||
	    !(device_iommu_property.device_iommu & HV_DEVICE_IOMMU_ENABLED))
		return ERR_PTR(-ENODEV);

	vdev = kzalloc_obj(*vdev, GFP_KERNEL);
	if (!vdev)
		return ERR_PTR(-ENOMEM);

	vdev->dev = dev;
	vdev->hv_iommu = hv_iommu_device;
	dev_iommu_priv_set(dev, vdev);

	if (hv_iommu_ats_supported(hv_iommu_device->cap) &&
	    pci_ats_supported(pdev))
		pci_enable_ats(pdev, __ffs(hv_iommu_device->pgsize_bitmap));

	return &vdev->hv_iommu->iommu;
}

static void hv_iommu_release_device(struct device *dev)
{
	struct hv_iommu_endpoint *vdev = dev_iommu_priv_get(dev);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (pdev->ats_enabled)
		pci_disable_ats(pdev);

	dev_iommu_priv_set(dev, NULL);
	set_dma_ops(dev, NULL);

	kfree(vdev);
}

static struct iommu_group *hv_iommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	else
		return generic_device_group(dev);
}

static int hv_configure_device_domain(struct hv_iommu_domain *hv_domain, u32 domain_type)
{
	u64 status;
	unsigned long flags;
	struct pt_iommu_x86_64_hw_info pt_info;
	struct hv_input_configure_device_domain *input;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));
	input->device_domain = hv_domain->device_domain;
	input->settings.flags.blocked = (domain_type == IOMMU_DOMAIN_BLOCKED);
	input->settings.flags.translation_enabled = (domain_type != IOMMU_DOMAIN_IDENTITY);

	if (domain_type & __IOMMU_DOMAIN_PAGING) {
		pt_iommu_x86_64_hw_info(&hv_domain->pt_iommu_x86_64, &pt_info);
		input->settings.page_table_root = pt_info.gcr3_pt;
		input->settings.flags.first_stage_paging_mode =
			pt_info.levels == 5;
	}
	status = hv_do_hypercall(HVCALL_CONFIGURE_DEVICE_DOMAIN, input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_CONFIGURE_DEVICE_DOMAIN failed, status %lld\n", status);

	return hv_result_to_errno(status);
}

static int __init hv_initialize_static_domains(void)
{
	int ret;
	struct hv_iommu_domain *hv_domain;

	/* Default stage-1 identity domain */
	hv_domain = &hv_identity_domain;

	ret = hv_create_device_domain(hv_domain, HV_DEVICE_DOMAIN_TYPE_S1);
	if (ret)
		return ret;

	ret = hv_configure_device_domain(hv_domain, IOMMU_DOMAIN_IDENTITY);
	if (ret)
		goto delete_identity_domain;

	hv_domain->domain.type = IOMMU_DOMAIN_IDENTITY;
	hv_domain->domain.ops = &hv_iommu_identity_domain_ops;
	hv_domain->domain.owner = &hv_iommu_ops;
	hv_domain->domain.geometry = hv_iommu_device->geometry;
	hv_domain->domain.pgsize_bitmap = hv_iommu_device->pgsize_bitmap;

	/* Default stage-1 blocked domain */
	hv_domain = &hv_blocking_domain;

	ret = hv_create_device_domain(hv_domain, HV_DEVICE_DOMAIN_TYPE_S1);
	if (ret)
		goto delete_identity_domain;

	ret = hv_configure_device_domain(hv_domain, IOMMU_DOMAIN_BLOCKED);
	if (ret)
		goto delete_blocked_domain;

	hv_domain->domain.type = IOMMU_DOMAIN_BLOCKED;
	hv_domain->domain.ops = &hv_iommu_blocking_domain_ops;
	hv_domain->domain.owner = &hv_iommu_ops;
	hv_domain->domain.geometry = hv_iommu_device->geometry;
	hv_domain->domain.pgsize_bitmap = hv_iommu_device->pgsize_bitmap;

	return 0;

delete_blocked_domain:
	hv_delete_device_domain(&hv_blocking_domain);
delete_identity_domain:
	hv_delete_device_domain(&hv_identity_domain);
	return ret;
}

#define INTERRUPT_RANGE_START	(0xfee00000)
#define INTERRUPT_RANGE_END	(0xfeefffff)
static void hv_iommu_get_resv_regions(struct device *dev,
		struct list_head *head)
{
	struct iommu_resv_region *region;

	region = iommu_alloc_resv_region(INTERRUPT_RANGE_START,
				      INTERRUPT_RANGE_END - INTERRUPT_RANGE_START + 1,
				      0, IOMMU_RESV_MSI, GFP_KERNEL);
	if (!region)
		return;

	list_add_tail(&region->list, head);
}

static void hv_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	hv_flush_device_domain(to_hv_iommu_domain(domain));
}

/* Max number of iova_list entries in a single hypercall input page. */
#define HV_IOMMU_MAX_FLUSH_VA_COUNT \
	((HV_HYP_PAGE_SIZE - sizeof(struct hv_input_flush_device_domain_list)) / \
	 sizeof(union hv_iommu_flush_va))

/* Returned by hv_iommu_fill_iova_list() when the range exceeds the capacity */
#define HV_IOMMU_FLUSH_VA_OVERFLOW	U16_MAX

static inline u16 hv_iommu_fill_iova_list(union hv_iommu_flush_va *iova_list,
					  unsigned long start,
					  unsigned long end)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long end_pfn = PAGE_ALIGN(end) >> PAGE_SHIFT;
	unsigned long nr_pages = end_pfn - start_pfn;
	u16 count = 0;

	while (nr_pages > 0) {
		unsigned long flush_pages;
		int order;
		unsigned long pfn_align;
		unsigned long size_align;

		if (count >= HV_IOMMU_MAX_FLUSH_VA_COUNT) {
			count = HV_IOMMU_FLUSH_VA_OVERFLOW;
			break;
		}

		if (start_pfn)
			pfn_align = __ffs(start_pfn);
		else
			pfn_align = BITS_PER_LONG - 1;

		size_align = __fls(nr_pages);
		order = min(pfn_align, size_align);
		iova_list[count].page_mask_shift = order;
		iova_list[count].page_number = start_pfn;

		flush_pages = 1UL << order;
		start_pfn += flush_pages;
		nr_pages -= flush_pages;
		count++;
	}

	return count;
}

static void hv_flush_device_domain_list(struct hv_iommu_domain *hv_domain,
					struct iommu_iotlb_gather *iotlb_gather)
{
	u64 status;
	u16 count;
	unsigned long flags;
	struct hv_input_flush_device_domain_list *input;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));

	input->device_domain = hv_domain->device_domain;
	input->flags |= HV_FLUSH_DEVICE_DOMAIN_LIST_IOMMU_FORMAT;
	count = hv_iommu_fill_iova_list(input->iova_list,
					iotlb_gather->start,
					iotlb_gather->end);
	if (count == HV_IOMMU_FLUSH_VA_OVERFLOW) {
		/*
		 * Range exceeds hypercall page capacity. Fall back to a full
		 * domain flush.
		 */
		struct hv_input_flush_device_domain *flush_all = (void *)input;

		memset(flush_all, 0, sizeof(*flush_all));
		flush_all->device_domain = hv_domain->device_domain;
		status = hv_do_hypercall(HVCALL_FLUSH_DEVICE_DOMAIN,
					flush_all, NULL);
	} else {
		status = hv_do_rep_hypercall(
				HVCALL_FLUSH_DEVICE_DOMAIN_LIST,
				count, 0, input, NULL);
	}

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_FLUSH_DEVICE_DOMAIN_LIST failed, status %lld\n", status);
}

static void hv_iommu_iotlb_sync(struct iommu_domain *domain,
				struct iommu_iotlb_gather *iotlb_gather)
{
	hv_flush_device_domain_list(to_hv_iommu_domain(domain), iotlb_gather);

	iommu_put_pages_list(&iotlb_gather->freelist);
}

static void hv_iommu_paging_domain_free(struct iommu_domain *domain)
{
	struct hv_iommu_domain *hv_domain = to_hv_iommu_domain(domain);

	/* Free all remaining mappings */
	pt_iommu_deinit(&hv_domain->pt_iommu);

	hv_delete_device_domain(hv_domain);

	kfree(hv_domain);
}

static const struct iommu_domain_ops hv_iommu_identity_domain_ops = {
	.attach_dev	= hv_iommu_attach_dev,
};

static const struct iommu_domain_ops hv_iommu_blocking_domain_ops = {
	.attach_dev	= hv_iommu_attach_dev,
};

static const struct iommu_domain_ops hv_iommu_paging_domain_ops = {
	.attach_dev	= hv_iommu_attach_dev,
	IOMMU_PT_DOMAIN_OPS(x86_64),
	.flush_iotlb_all = hv_iommu_flush_iotlb_all,
	.iotlb_sync = hv_iommu_iotlb_sync,
	.free = hv_iommu_paging_domain_free,
};

static struct iommu_domain *hv_iommu_domain_alloc_paging(struct device *dev)
{
	int ret;
	struct hv_iommu_domain *hv_domain;
	struct pt_iommu_x86_64_cfg cfg = {};

	hv_domain = kzalloc_obj(*hv_domain, GFP_KERNEL);
	if (!hv_domain)
		return ERR_PTR(-ENOMEM);

	ret = hv_create_device_domain(hv_domain, HV_DEVICE_DOMAIN_TYPE_S1);
	if (ret) {
		kfree(hv_domain);
		return ERR_PTR(ret);
	}

	hv_domain->domain.geometry = hv_iommu_device->geometry;
	hv_domain->pt_iommu.nid = dev_to_node(dev);

	cfg.common.hw_max_vasz_lg2 = hv_iommu_device->max_iova_width;
	cfg.common.hw_max_oasz_lg2 = 52;
	cfg.common.features |= BIT(PT_FEAT_FLUSH_RANGE);
	cfg.top_level = (hv_iommu_device->max_iova_width > 48) ? 4 : 3;

	ret = pt_iommu_x86_64_init(&hv_domain->pt_iommu_x86_64, &cfg, GFP_KERNEL);
	if (ret) {
		hv_delete_device_domain(hv_domain);
		kfree(hv_domain);
		return ERR_PTR(ret);
	}

	/* Constrain to page sizes the hypervisor supports */
	hv_domain->domain.pgsize_bitmap &= hv_iommu_device->pgsize_bitmap;

	hv_domain->domain.ops = &hv_iommu_paging_domain_ops;

	ret = hv_configure_device_domain(hv_domain, __IOMMU_DOMAIN_PAGING);
	if (ret) {
		pt_iommu_deinit(&hv_domain->pt_iommu);
		hv_delete_device_domain(hv_domain);
		kfree(hv_domain);
		return ERR_PTR(ret);
	}

	return &hv_domain->domain;
}

static struct iommu_ops hv_iommu_ops = {
	.capable		  = hv_iommu_capable,
	.domain_alloc_paging	  = hv_iommu_domain_alloc_paging,
	.probe_device		  = hv_iommu_probe_device,
	.release_device		  = hv_iommu_release_device,
	.device_group		  = hv_iommu_device_group,
	.get_resv_regions	  = hv_iommu_get_resv_regions,
	.owner			  = THIS_MODULE,
	.identity_domain	  = &hv_identity_domain.domain,
	.blocked_domain		  = &hv_blocking_domain.domain,
	.release_domain		  = &hv_blocking_domain.domain,
};

static int hv_iommu_detect(struct hv_output_get_iommu_capabilities *hv_iommu_cap)
{
	u64 status;
	unsigned long flags;
	struct hv_input_get_iommu_capabilities *input;
	struct hv_output_get_iommu_capabilities *output;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	output = *this_cpu_ptr(hyperv_pcpu_input_arg) + sizeof(*input);
	memset(input, 0, sizeof(*input));
	input->partition_id = HV_PARTITION_ID_SELF;
	status = hv_do_hypercall(HVCALL_GET_IOMMU_CAPABILITIES, input, output);
	*hv_iommu_cap = *output;

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_GET_IOMMU_CAPABILITIES failed, status %lld\n", status);

	return hv_result_to_errno(status);
}

static void __init hv_init_iommu_device(struct hv_iommu_dev *hv_iommu,
			struct hv_output_get_iommu_capabilities *hv_iommu_cap)
{
	ida_init(&hv_iommu->domain_ids);

	hv_iommu->cap = hv_iommu_cap->iommu_cap;
	hv_iommu->max_iova_width = hv_iommu_cap->max_iova_width;
	if (!hv_iommu_5lvl_supported(hv_iommu->cap) &&
	    hv_iommu->max_iova_width > 48) {
		pr_info("5-level paging not supported, limiting iova width to 48.\n");
		hv_iommu->max_iova_width = 48;
	}

	hv_iommu->geometry = (struct iommu_domain_geometry) {
		.aperture_start = 0,
		.aperture_end = (((u64)1) << hv_iommu->max_iova_width) - 1,
		.force_aperture = true,
	};

	hv_iommu->first_domain = HV_DEVICE_DOMAIN_ID_DEFAULT + 1;
	hv_iommu->last_domain = HV_DEVICE_DOMAIN_ID_NULL - 1;
	/* Only x86 page sizes (4K/2M/1G) are supported */
	hv_iommu->pgsize_bitmap = hv_iommu_cap->pgsize_bitmap &
				  (SZ_4K | SZ_2M | SZ_1G);
	if (hv_iommu->pgsize_bitmap != hv_iommu_cap->pgsize_bitmap)
		pr_warn("unsupported page sizes masked: 0x%llx -> 0x%llx\n",
			hv_iommu_cap->pgsize_bitmap, hv_iommu->pgsize_bitmap);
	if (!hv_iommu->pgsize_bitmap) {
		pr_warn("no supported page sizes, defaulting to 4K\n");
		hv_iommu->pgsize_bitmap = SZ_4K;
	}
	hv_iommu_device = hv_iommu;
}

int __init hv_iommu_init(void)
{
	int ret = 0;
	struct hv_iommu_dev *hv_iommu = NULL;
	struct hv_output_get_iommu_capabilities hv_iommu_cap = {0};

	if (no_iommu || iommu_detected)
		return -ENODEV;

	if (!hv_is_hyperv_initialized())
		return -ENODEV;

	ret = hv_iommu_detect(&hv_iommu_cap);
	if (ret) {
		pr_err("HVCALL_GET_IOMMU_CAPABILITIES failed: %d\n", ret);
		return -ENODEV;
	}

	if (!hv_iommu_present(hv_iommu_cap.iommu_cap) ||
	    !hv_iommu_s1_domain_supported(hv_iommu_cap.iommu_cap)) {
		pr_err("IOMMU capabilities not sufficient: cap=0x%llx\n",
		       hv_iommu_cap.iommu_cap);
		return -ENODEV;
	}

	iommu_detected = 1;
	pci_request_acs();

	hv_iommu = kzalloc_obj(*hv_iommu, GFP_KERNEL);
	if (!hv_iommu)
		return -ENOMEM;

	hv_init_iommu_device(hv_iommu, &hv_iommu_cap);

	ret = hv_initialize_static_domains();
	if (ret) {
		pr_err("static domains init failed: %d\n", ret);
		goto err_free;
	}

	ret = iommu_device_sysfs_add(&hv_iommu->iommu, NULL, NULL, "%s", "hv-iommu");
	if (ret) {
		pr_err("iommu_device_sysfs_add failed: %d\n", ret);
		goto err_delete_static_domains;
	}

	ret = iommu_device_register(&hv_iommu->iommu, &hv_iommu_ops, NULL);
	if (ret) {
		pr_err("iommu_device_register failed: %d\n", ret);
		goto err_sysfs_remove;
	}

	pr_info("successfully initialized\n");
	return 0;

err_sysfs_remove:
	iommu_device_sysfs_remove(&hv_iommu->iommu);
err_delete_static_domains:
	hv_delete_device_domain(&hv_blocking_domain);
	hv_delete_device_domain(&hv_identity_domain);
err_free:
	kfree(hv_iommu);
	return ret;
}
