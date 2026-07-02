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
static const struct iommu_domain_ops hv_iommu_identity_domain_ops;
static const struct iommu_domain_ops hv_iommu_blocking_domain_ops;
static struct iommu_ops hv_iommu_ops;

static struct hv_iommu_domain hv_identity_domain = {
	.domain = {
		.type	= IOMMU_DOMAIN_IDENTITY,
		.ops	= &hv_iommu_identity_domain_ops,
		.owner	= &hv_iommu_ops,
	},
};
static struct hv_iommu_domain hv_blocking_domain = {
	.domain = {
		.type	= IOMMU_DOMAIN_BLOCKED,
		.ops	= &hv_iommu_blocking_domain_ops,
		.owner	= &hv_iommu_ops,
	},
};

static inline bool hv_iommu_present(u64 cap)
{
	return cap & HV_IOMMU_CAP_PRESENT;
}

static inline bool hv_iommu_s1_domain_supported(u64 cap)
{
	return cap & HV_IOMMU_CAP_S1;
}

static inline bool hv_iommu_5lvl_supported(u64 cap)
{
	return cap & HV_IOMMU_CAP_S1_5LVL;
}

static inline bool hv_iommu_ats_supported(u64 cap)
{
	return cap & HV_IOMMU_CAP_ATS;
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

static int hv_iommu_attach_dev(struct iommu_domain *domain, struct device *dev,
			       struct iommu_domain *old)
{
	u64 status;
	u32 prefix;
	unsigned long flags;
	struct pci_dev *pdev;
	struct hv_input_attach_device_domain *input;
	struct hv_iommu_endpoint *vdev = dev_iommu_priv_get(dev);
	struct hv_iommu_domain *hv_domain = to_hv_iommu_domain(domain);
	int ret;

	if (vdev->hv_domain == hv_domain)
		return 0;

	pdev = to_pci_dev(dev);
	dev_dbg(dev, "attaching to domain %d\n",
		hv_domain->device_domain.domain_id.id);

	ret = hv_iommu_lookup_logical_dev_id(pci_domain_nr(pdev->bus), &prefix);
	if (ret) {
		dev_err(&pdev->dev, "no IOMMU registration for vPCI bus\n");
		return ret;
	}

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));
	input->device_domain = hv_domain->device_domain;
	input->device_id.as_uint64 = (u64)prefix | PCI_FUNC(pdev->devfn);
	status = hv_do_hypercall(HVCALL_ATTACH_DEVICE_DOMAIN, input, NULL);

	local_irq_restore(flags);

	if (!hv_result_success(status))
		pr_err("HVCALL_ATTACH_DEVICE_DOMAIN failed, status %lld\n", status);
	else
		vdev->hv_domain = hv_domain;

	return hv_result_to_errno(status);
}

static int hv_iommu_blocking_attach_dev(struct iommu_domain *domain,
					struct device *dev,
					struct iommu_domain *old)
{
	int ret = hv_iommu_attach_dev(domain, dev, old);

	/*
	 * Attaching to the blocking domain only asks the hypervisor to
	 * disable translation and IOPF for the device, so it cannot fail
	 * unless there is a driver or hypervisor bug. Return the hypercall
	 * status rather than 0 so that a failure on the DMA ownership claim
	 * path (VFIO/iommufd) fails the claim instead of leaving the device
	 * unblocked. WARN since such a failure indicates a bug.
	 */
	WARN_ON(ret);
	return ret;
}

static int hv_iommu_get_logical_device_property(struct device *dev,
					u32 code,
					struct hv_output_get_logical_device_property *property)
{
	u64 status;
	u32 prefix;
	unsigned long flags;
	int ret;
	struct pci_dev *pdev = to_pci_dev(dev);
	struct hv_input_get_logical_device_property *input;
	struct hv_output_get_logical_device_property *output;

	ret = hv_iommu_lookup_logical_dev_id(pci_domain_nr(pdev->bus), &prefix);
	if (ret)
		return ret;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	output = (struct hv_output_get_logical_device_property *)(input + 1);
	memset(input, 0, sizeof(*input));
	input->partition_id = HV_PARTITION_ID_SELF;
	input->logical_device_id = (u64)prefix | PCI_FUNC(pdev->devfn);
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

	kfree(vdev);
}

static struct iommu_group *hv_iommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))
		return pci_device_group(dev);

	WARN_ON_ONCE(1);
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
	/*
	 * Clearing translation_enabled bypasses translation (DMA uses the GPA
	 * directly), which only suits identity. The hypervisor requires paging
	 * and blocked domains to keep it set.
	 */
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

	/* Default stage-1 blocked domain */
	hv_domain = &hv_blocking_domain;

	ret = hv_create_device_domain(hv_domain, HV_DEVICE_DOMAIN_TYPE_S1);
	if (ret)
		goto delete_identity_domain;

	ret = hv_configure_device_domain(hv_domain, IOMMU_DOMAIN_BLOCKED);
	if (ret)
		goto delete_blocked_domain;

	return 0;

delete_blocked_domain:
	hv_delete_device_domain(&hv_blocking_domain);
delete_identity_domain:
	hv_delete_device_domain(&hv_identity_domain);
	return ret;
}

/* x86 architectural MSI address range */
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

static void hv_iommu_iotlb_sync(struct iommu_domain *domain,
				struct iommu_iotlb_gather *iotlb_gather)
{
	hv_flush_device_domain(to_hv_iommu_domain(domain));

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
	.attach_dev	= hv_iommu_blocking_attach_dev,
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
	if (ret)
		goto err_free;

	hv_domain->pt_iommu.nid = dev_to_node(dev);

	cfg.common.hw_max_vasz_lg2 = hv_iommu_device->max_iova_width;
	cfg.common.hw_max_oasz_lg2 = 52;
	cfg.top_level = (hv_iommu_device->max_iova_width > 48) ? 4 : 3;

	ret = pt_iommu_x86_64_init(&hv_domain->pt_iommu_x86_64, &cfg, GFP_KERNEL);
	if (ret)
		goto err_delete_domain;

	/* Constrain to page sizes the hypervisor supports */
	hv_domain->domain.pgsize_bitmap &= hv_iommu_device->pgsize_bitmap;

	hv_domain->domain.ops = &hv_iommu_paging_domain_ops;

	ret = hv_configure_device_domain(hv_domain, __IOMMU_DOMAIN_PAGING);
	if (ret)
		goto err_pt_deinit;

	return &hv_domain->domain;

err_pt_deinit:
	pt_iommu_deinit(&hv_domain->pt_iommu);
err_delete_domain:
	hv_delete_device_domain(hv_domain);
err_free:
	kfree(hv_domain);
	return ERR_PTR(ret);
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
	output = (struct hv_output_get_iommu_capabilities *)(input + 1);
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
	hv_iommu->pgsize_bitmap = hv_iommu_cap->pgsize_bitmap;
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

	/*
	 * The page table code only maps x86 page sizes (4K/2M/1G); require the
	 * hypervisor to advertise a non-empty subset of exactly those.
	 */
	if (!hv_iommu_cap.pgsize_bitmap ||
	    (hv_iommu_cap.pgsize_bitmap & ~(u64)(SZ_4K | SZ_2M | SZ_1G))) {
		pr_err("unsupported page sizes: pgsize_bitmap=0x%llx\n",
		       hv_iommu_cap.pgsize_bitmap);
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
