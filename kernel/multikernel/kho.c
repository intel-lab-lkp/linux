// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Multikernel Technologies, Inc. All rights reserved
 *
 * Multikernel KHO (Kexec HandOver)
 *
 * Provides KHO support for preserving and restoring multikernel instance
 * device trees across kexec boundaries using shared memory.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/multikernel.h>
#include <linux/io.h>
#ifdef CONFIG_KEXEC_HANDOVER
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include "internal.h"

#define PROP_SUB_FDT "fdt"
#endif

#ifdef CONFIG_KEXEC_HANDOVER

/**
 * mk_kexec_notifier() - Multikernel kexec notifier callback for DTB preservation
 * @nb: Notifier block
 * @action: Notifier action (unused)
 * @data: Multikernel kexec data
 *
 * Called by multikernel kexec subsystem during kexec to preserve multikernel DTBs
 * in shared memory for the target kernel. This uses a simplified interface
 * compared to the full KHO system.
 */
static int mk_kexec_notifier(struct notifier_block *nb, unsigned long action, void *data)
{
	struct mk_kexec_data {
		struct kimage *image;
		void *fdt;
		int mk_id;
	} *mk_data = data;

	struct mk_instance *instance;
	int ret = 0;

	pr_info("Preserving multikernel DTB for instance %d\n", mk_data->mk_id);

	/* Find the target multikernel instance */
	instance = mk_instance_find(mk_data->mk_id);
	if (!instance) {
		pr_err("Target multikernel instance %d not found\n", mk_data->mk_id);
		return NOTIFY_STOP;
	}

	if (!instance->dtb_data || instance->dtb_size == 0) {
		pr_err("Target multikernel instance %d has no DTB data - did you write to device_tree file?\n", mk_data->mk_id);
		mk_instance_put(instance);
		return NOTIFY_STOP;
	}

	ret |= fdt_begin_node(mk_data->fdt, "multikernel");
	ret |= fdt_property(mk_data->fdt, "dtb-data", instance->dtb_data, instance->dtb_size);
	ret |= fdt_end_node(mk_data->fdt);

	if (ret) {
		pr_err("Failed to add DTB for instance %d to FDT: %d\n", mk_data->mk_id, ret);
		mk_instance_put(instance);
		return notifier_from_errno(ret);
	}

	pr_info("Preserved DTB for instance %d (%zu bytes)\n", mk_data->mk_id, instance->dtb_size);
	mk_instance_put(instance);
	return NOTIFY_OK;
}

/* Multikernel kexec notifier block */
static struct notifier_block mk_kexec_nb = {
	.notifier_call = mk_kexec_notifier,
};

/**
 * mk_dt_extract_instance_info() - Extract instance ID and name from DTB
 * @dtb_data: Device tree blob data
 * @dtb_size: Size of DTB data
 * @instance_id: Output parameter for instance ID
 * @instance_name: Output parameter for instance name (caller must free)
 *
 * Parses the DTB to find the first instance in the instances node and
 * extracts its ID and name.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int mk_dt_extract_instance_info(const void *dtb_data, size_t dtb_size,
				       int *instance_id, const char **instance_name)
{
	const void *fdt = dtb_data;
	int instances_node, instance_node;
	const fdt32_t *id_prop;
	const char *name;

	if (!dtb_data || !instance_id || !instance_name) {
		return -EINVAL;
	}

	/* Find /instances node */
	instances_node = fdt_path_offset(fdt, "/instances");
	if (instances_node < 0) {
		pr_err("No /instances node found in device tree\n");
		return -ENOENT;
	}

	/* Find the first instance child node */
	instance_node = fdt_first_subnode(fdt, instances_node);
	if (instance_node < 0) {
		pr_err("No instance found in /instances node\n");
		return -ENOENT;
	}

	/* Get the instance name (node name) */
	name = fdt_get_name(fdt, instance_node, NULL);
	if (!name) {
		pr_err("Failed to get instance name\n");
		return -EINVAL;
	}

	/* Get the instance ID property */
	id_prop = fdt_getprop(fdt, instance_node, "id", NULL);
	if (!id_prop) {
		pr_err("No 'id' property found in instance '%s'\n", name);
		return -ENOENT;
	}

	*instance_id = fdt32_to_cpu(*id_prop);
	*instance_name = name;

	return 0;
}

/**
 * mk_kho_restore_dtbs() - Restore DTB from KHO shared memory
 *
 * Called during multikernel initialization in the spawned kernel to restore
 * the single DTB that was preserved by the host kernel via KHO. The spawned
 * kernel receives exactly one DTB and parses the instance ID from it.
 *
 * Returns: 0 on success, negative error code on failure
 */
int __init mk_kho_restore_dtbs(void)
{
	void *dtb_virt;
	int dtb_len;
	int ret;
	struct mk_instance *instance;
	struct mk_dt_config config;
	int instance_id;
	const char *instance_name;
	const void *kho_fdt = NULL;
	phys_addr_t fdt_phys;

	fdt_phys = kho_get_fdt_phys();
	if (!fdt_phys) {
		pr_info("No KHO FDT available for multikernel DTB restoration\n");
		return 0;
	}

	pr_info("Restoring multikernel DTB from KHO (phys: 0x%llx)\n", fdt_phys);

	/* Map the FDT for early boot access */
	kho_fdt = early_memremap(fdt_phys, PAGE_SIZE);
	if (!kho_fdt) {
		pr_err("Failed to map KHO FDT at 0x%llx\n", fdt_phys);
		return -EFAULT;
	}

	int mk_node = fdt_subnode_offset(kho_fdt, 0, "multikernel");
	if (mk_node < 0) {
		pr_info("No multikernel node found in KHO FDT\n");
		ret = 0;
		goto cleanup_fdt;
	}

	const void *dtb_data = fdt_getprop(kho_fdt, mk_node, "dtb-data", &dtb_len);
	if (!dtb_data || dtb_len <= 0) {
		pr_info("No dtb-data property found in multikernel node\n");
		ret = 0;
		goto cleanup_fdt;
	}

	pr_info("Found preserved multikernel DTB (%d bytes)\n", dtb_len);

	/* Validate DTB header */
	ret = fdt_check_header(dtb_data);
	if (ret) {
		pr_err("Invalid DTB header from KHO: %d\n", ret);
		ret = -EINVAL;
		goto cleanup_fdt;
	}

	if (dtb_len > SZ_1M) {
		pr_err("DTB size too large: %d bytes\n", dtb_len);
		ret = -EINVAL;
		goto cleanup_fdt;
	}

	dtb_virt = kmalloc(dtb_len, GFP_KERNEL);
	if (!dtb_virt) {
		pr_err("Failed to allocate memory for DTB (%d bytes)\n", dtb_len);
		ret = -ENOMEM;
		goto cleanup_fdt;
	}
	memcpy(dtb_virt, dtb_data, dtb_len);

	/* Parse DTB to get the actual instance ID and name */
	ret = mk_dt_extract_instance_info(dtb_virt, dtb_len, &instance_id, &instance_name);
	if (ret) {
		pr_err("Failed to extract instance info from DTB: %d\n", ret);
		goto cleanup_dtb;
	}

	pr_info("DTB contains instance ID %d, name '%s'\n", instance_id, instance_name);

	/* Parse DTB configuration */
	mk_dt_config_init(&config);
	ret = mk_dt_parse(dtb_virt, dtb_len, &config);
	if (ret) {
		pr_err("Failed to parse DTB from KHO: %d\n", ret);
		goto config_free;
	}

	/* Create a new instance for this DTB */
	instance = kzalloc(sizeof(*instance), GFP_KERNEL);
	if (!instance) {
		pr_err("Failed to allocate memory for multikernel instance\n");
		ret = -ENOMEM;
		goto config_free;
	}

	/* Initialize instance with parsed data */
	instance->id = instance_id;
	instance->name = kstrdup(instance_name, GFP_KERNEL);
	if (!instance->name) {
		ret = -ENOMEM;
		goto cleanup_instance;
	}

	instance->dtb_data = kmalloc(dtb_len, GFP_KERNEL);
	if (!instance->dtb_data) {
		pr_err("Failed to allocate memory for DTB restoration\n");
		ret = -ENOMEM;
		goto cleanup_instance_name;
	}

	memcpy(instance->dtb_data, dtb_virt, dtb_len);
	instance->dtb_size = dtb_len;

	INIT_LIST_HEAD(&instance->memory_regions);
	INIT_LIST_HEAD(&instance->list);
	kref_init(&instance->refcount);

	ret = mk_instance_reserve_resources(instance, &config);
	if (ret == 0) {
		mk_instance_set_state(instance, MK_STATE_READY);

		mutex_lock(&mk_instance_mutex);
		list_add_tail(&instance->list, &mk_instance_list);
		mutex_unlock(&mk_instance_mutex);

		pr_info("Successfully restored multikernel instance %d ('%s') from KHO (%d bytes)\n",
			instance_id, instance_name, dtb_len);
		mk_dt_config_free(&config);
		kfree(dtb_virt);
		early_memunmap((void *)kho_fdt, PAGE_SIZE);
		return 0;
	} else {
		pr_err("Failed to reserve memory for restored instance: %d\n", ret);
		mk_instance_set_state(instance, MK_STATE_FAILED);
		/* Fall through to cleanup */
	}

cleanup_instance_name:
	kfree(instance->name);
cleanup_instance:
	kfree(instance->dtb_data);
	kfree(instance);
config_free:
	mk_dt_config_free(&config);
cleanup_dtb:
	kfree(dtb_virt);
cleanup_fdt:
	early_memunmap((void *)kho_fdt, PAGE_SIZE);
	return ret;
}

/**
 * mk_kho_init() - Initialize KHO support for multikernel
 *
 * Registers the KHO notifier and attempts to restore DTBs from
 * a previous KHO boot.
 *
 * Returns: 0 on success, negative error code on failure
 */
int __init mk_kho_init(void)
{
	int ret;

	/* Register multikernel kexec notifier for DTB preservation */
	ret = mk_kexec_register_notifier(&mk_kexec_nb);
	if (ret) {
		pr_warn("Failed to register multikernel kexec notifier: %d\n", ret);
		return ret;
	}

	pr_info("Registered multikernel kexec notifier for DTB preservation\n");

	/* Restore DTBs from previous kernel if KHO boot */
	ret = mk_kho_restore_dtbs();
	if (ret) {
		pr_warn("Failed to restore DTBs from KHO: %d\n", ret);
		/* Continue - this is not fatal */
	}

	return 0;
}

/**
 * mk_kho_cleanup() - Cleanup multikernel kexec support
 *
 * Unregisters the multikernel kexec notifier.
 */
void mk_kho_cleanup(void)
{
	mk_kexec_unregister_notifier(&mk_kexec_nb);
	pr_debug("Unregistered multikernel kexec notifier\n");
}

#else /* !CONFIG_KEXEC_HANDOVER */

/* Stub functions when KHO is not enabled */
int __init mk_kho_restore_dtbs(void)
{
	return 0;
}

int __init mk_kho_init(void)
{
	return 0;
}

void mk_kho_cleanup(void)
{
}

#endif /* CONFIG_KEXEC_HANDOVER */
