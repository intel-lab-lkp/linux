// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Error Source Table Support
 *
 * Copyright (c) 2024, Alibaba Group.
 */

#include <linux/xarray.h>
#include <linux/platform_device.h>
#include <linux/acpi_aest.h>

#include "init.h"

#include <ras/ras_event.h>

#undef pr_fmt
#define pr_fmt(fmt) "ACPI AEST: " fmt

static struct xarray *aest_array;

static void __init aest_init_interface(struct acpi_aest_hdr *hdr,
				       struct acpi_aest_node *node)
{
	struct acpi_aest_node_interface_header *interface;

	interface = ACPI_ADD_PTR(struct acpi_aest_node_interface_header, hdr,
				 hdr->node_interface_offset);

	node->type = hdr->type;
	node->interface_hdr = interface;

	switch (interface->group_format) {
	case ACPI_AEST_NODE_GROUP_FORMAT_4K: {
		struct acpi_aest_node_interface_4k *interface_4k =
			(struct acpi_aest_node_interface_4k *)(interface + 1);

		node->common = &interface_4k->common;
		node->record_implemented =
			(unsigned long *)&interface_4k->error_record_implemented;
		node->status_reporting =
			(unsigned long *)&interface_4k->error_status_reporting;
		node->addressing_mode =
			(unsigned long *)&interface_4k->addressing_mode;
		break;
	}
	case ACPI_AEST_NODE_GROUP_FORMAT_16K: {
		struct acpi_aest_node_interface_16k *interface_16k =
			(struct acpi_aest_node_interface_16k *)(interface + 1);

		node->common = &interface_16k->common;
		node->record_implemented =
			(unsigned long *)interface_16k->error_record_implemented;
		node->status_reporting =
			(unsigned long *)interface_16k->error_status_reporting;
		node->addressing_mode =
			(unsigned long *)interface_16k->addressing_mode;
		break;
	}
	case ACPI_AEST_NODE_GROUP_FORMAT_64K: {
		struct acpi_aest_node_interface_64k *interface_64k =
			(struct acpi_aest_node_interface_64k *)(interface + 1);

		node->common = &interface_64k->common;
		node->record_implemented =
			(unsigned long *)interface_64k->error_record_implemented;
		node->status_reporting =
			(unsigned long *)interface_64k->error_status_reporting;
		node->addressing_mode =
			(unsigned long *)interface_64k->addressing_mode;
		break;
	}
	default:
		pr_err("invalid group format: %d\n", interface->group_format);
	}

	node->interrupt = ACPI_ADD_PTR(struct acpi_aest_node_interrupt_v2,
					hdr, hdr->node_interrupt_offset);

	node->interrupt_count = hdr->node_interrupt_count;
}

static int __init acpi_aest_init_node_common(struct acpi_aest_hdr *aest_hdr,
					struct acpi_aest_node *node)
{
	int ret;
	struct aest_hnode *hnode;
	u64 error_device_id;

	aest_init_interface(aest_hdr, node);

	error_device_id = node->common->error_node_device;

	hnode = xa_load(aest_array, error_device_id);
	if (!hnode) {
		hnode = kmalloc(sizeof(*hnode), GFP_KERNEL);
		if (!hnode) {
			ret = -ENOMEM;
			goto free;
		}
		INIT_LIST_HEAD(&hnode->list);
		hnode->uid = error_device_id;
		hnode->count = 0;
		hnode->type = node->type;
		xa_store(aest_array, error_device_id, hnode, GFP_KERNEL);
	}

	list_add_tail(&node->list, &hnode->list);
	hnode->count++;

	return 0;

free:
	kfree(node);
	return ret;
}

static int __init
acpi_aest_init_node_default(struct acpi_aest_hdr *aest_hdr)
{
	struct acpi_aest_node *node;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->spec_pointer = ACPI_ADD_PTR(void, aest_hdr,
					aest_hdr->node_specific_offset);

	return acpi_aest_init_node_common(aest_hdr, node);
}

static int __init
acpi_aest_init_processor_node(struct acpi_aest_hdr *aest_hdr)
{
	struct acpi_aest_node *node;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->spec_pointer = ACPI_ADD_PTR(void, aest_hdr,
					aest_hdr->node_specific_offset);

	node->processor_spec_pointer = ACPI_ADD_PTR(void, node->spec_pointer,
					sizeof(struct acpi_aest_processor));

	return acpi_aest_init_node_common(aest_hdr, node);
}

static int __init acpi_aest_init_node(struct acpi_aest_hdr *header)
{
	switch (header->type) {
	case ACPI_AEST_PROCESSOR_ERROR_NODE:
		return acpi_aest_init_processor_node(header);
	case ACPI_AEST_VENDOR_ERROR_NODE:
	case ACPI_AEST_SMMU_ERROR_NODE:
	case ACPI_AEST_GIC_ERROR_NODE:
	case ACPI_AEST_PCIE_ERROR_NODE:
	case ACPI_AEST_PROXY_ERROR_NODE:
	case ACPI_AEST_MEMORY_ERROR_NODE:
		return acpi_aest_init_node_default(header);
	default:
		pr_err("acpi table header type is invalid: %d\n", header->type);
		return -EINVAL;
	}

	return 0;
}

static int __init acpi_aest_init_nodes(struct acpi_table_header *aest_table)
{
	struct acpi_aest_hdr *aest_node, *aest_end;
	struct acpi_table_aest *aest;
	int rc;

	aest = (struct acpi_table_aest *)aest_table;
	aest_node = ACPI_ADD_PTR(struct acpi_aest_hdr, aest,
				 sizeof(struct acpi_table_header));
	aest_end = ACPI_ADD_PTR(struct acpi_aest_hdr, aest,
				aest_table->length);

	while (aest_node < aest_end) {
		if (((u64)aest_node + aest_node->length) > (u64)aest_end) {
			pr_warn(FW_WARN "AEST node pointer overflow, bad table.\n");
			return -EINVAL;
		}

		rc = acpi_aest_init_node(aest_node);
		if (rc)
			return rc;

		aest_node = ACPI_ADD_PTR(struct acpi_aest_hdr, aest_node,
					 aest_node->length);
	}

	return 0;
}

static int
acpi_aest_parse_irqs(struct platform_device *pdev, struct acpi_aest_node *anode,
				struct resource *res, int *res_idx, int irqs[2])
{
	int i;
	struct acpi_aest_node_interrupt_v2 *interrupt;
	int trigger, irq;

	for (i = 0; i < anode->interrupt_count; i++) {
		interrupt = &anode->interrupt[i];
		if (irqs[interrupt->type])
			continue;

		trigger = (interrupt->flags & AEST_INTERRUPT_MODE) ?
			ACPI_LEVEL_SENSITIVE : ACPI_EDGE_SENSITIVE;

		irq = acpi_register_gsi(&pdev->dev, interrupt->gsiv, trigger,
						ACPI_ACTIVE_HIGH);
		if (irq <= 0) {
			pr_err("failed to map AEST GSI %d\n", interrupt->gsiv);
			return irq;
		}

		res[*res_idx].start = irq;
		res[*res_idx].end = irq;
		res[*res_idx].flags = IORESOURCE_IRQ;
		res[*res_idx].name = interrupt->type ? "eri" : "fhi";

		(*res_idx)++;

		irqs[interrupt->type] = irq;
	}

	return 0;
}

static int __init acpi_aest_alloc_pdev(void)
{
	int ret, j, size;
	struct aest_hnode *ahnode = NULL;
	unsigned long i;
	struct platform_device *pdev;
	struct acpi_device *companion;
	struct acpi_aest_node *anode;
	char uid[16];
	struct resource *res;

	xa_for_each(aest_array, i, ahnode) {
		int irq[2] = { 0 };

		res = kcalloc(ahnode->count + 2, sizeof(*res), GFP_KERNEL);
		if (!res) {
			ret = -ENOMEM;
			break;
		}

		pdev = platform_device_alloc("AEST", i);
		if (IS_ERR(pdev)) {
			ret = PTR_ERR(pdev);
			break;
		}

		ret = snprintf(uid, sizeof(uid), "%u", (u32)i);
		companion = acpi_dev_get_first_match_dev("ARMHE000", uid, -1);
		if (companion)
			ACPI_COMPANION_SET(&pdev->dev, companion);

		j = 0;
		list_for_each_entry(anode, &ahnode->list, list) {
			if (anode->interface_hdr->type !=
					ACPI_AEST_NODE_SYSTEM_REGISTER) {
				res[j].name = "AEST:RECORD";
				res[j].start = anode->interface_hdr->address;
				size = anode->interface_hdr->error_record_count *
						sizeof(struct ras_ext_regs);
				res[j].end = res[j].start + size;
				res[j].flags = IORESOURCE_MEM;
			}

			ret = acpi_aest_parse_irqs(pdev, anode, res, &j, irq);
			if (ret) {
				platform_device_put(pdev);
				break;
			}
		}

		ret = platform_device_add_resources(pdev, res, j);
		if (ret)
			break;

		ret = platform_device_add_data(pdev, &ahnode, sizeof(ahnode));
		if (ret)
			break;

		ret = platform_device_add(pdev);
		if (ret)
			break;
	}

	kfree(res);
	if (ret)
		platform_device_put(pdev);

	return ret;
}

void __init acpi_aest_init(void)
{
	acpi_status status;
	int ret;
	struct acpi_table_header *aest_table;

	status = acpi_get_table(ACPI_SIG_AEST, 0, &aest_table);
	if (ACPI_FAILURE(status)) {
		if (status != AE_NOT_FOUND) {
			const char *msg = acpi_format_exception(status);

			pr_err("Failed to get table, %s\n", msg);
		}

		return;
	}

	aest_array = kzalloc(sizeof(struct xarray), GFP_KERNEL);
	if (!aest_array)
		return;

	xa_init(aest_array);

	ret = acpi_aest_init_nodes(aest_table);
	if (ret) {
		pr_err("Failed init aest node %d\n", ret);
		goto out;
	}

	ret = acpi_aest_alloc_pdev();
	if (ret)
		pr_err("Failed alloc pdev %d\n", ret);

out:
	acpi_put_table(aest_table);
}
