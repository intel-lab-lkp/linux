// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Multikernel Technologies, Inc. All rights reserved
 *
 * Multikernel device tree support
 *
 * Provides device tree parsing and validation for multikernel instances.
 * Designed to be extensible for future enhancements like CPU affinity,
 * I/O resource allocation, NUMA topology, etc.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/sizes.h>
#include <linux/cpumask.h>
#include <linux/multikernel.h>

/**
 * Configuration initialization and cleanup
 */
void mk_dt_config_init(struct mk_dt_config *config)
{
	memset(config, 0, sizeof(*config));
	config->version = MK_DT_CONFIG_CURRENT;
	config->memory_size = 0;

	/* Initialize CPU mask */
	if (!alloc_cpumask_var(&config->cpus, GFP_KERNEL)) {
		pr_warn("Failed to allocate CPU mask, CPU assignment disabled\n");
		config->cpus_valid = false;
	} else {
		cpumask_clear(config->cpus);
		config->cpus_valid = true;
	}
}

void mk_dt_config_free(struct mk_dt_config *config)
{
	if (!config)
		return;

	/* Free CPU mask */
	if (config->cpus_valid) {
		free_cpumask_var(config->cpus);
		config->cpus_valid = false;
	}

	/* Reset memory size */
	config->memory_size = 0;

	/* Note: We don't free dtb_data here as it's managed by the caller */
}

/**
 * Function prototypes
 */
static int mk_dt_parse_memory(const void *fdt, int chosen_node,
			      struct mk_dt_config *config);
static int mk_dt_parse_cpus(const void *fdt, int chosen_node,
			    struct mk_dt_config *config);
static int mk_dt_validate_memory(const struct mk_dt_config *config);
static int mk_dt_validate_cpus(const struct mk_dt_config *config);

/**
 * Memory region parsing
 */
static int mk_dt_parse_memory(const void *fdt, int chosen_node,
			      struct mk_dt_config *config)
{
	const fdt32_t *prop;
	int len;
	size_t total_size = 0;

	/* Look for memory-bytes property */
	prop = fdt_getprop(fdt, chosen_node, MK_DT_RESOURCE_MEMORY, &len);
	if (!prop) {
		pr_debug("No %s property found\n", MK_DT_RESOURCE_MEMORY);
		return 0; /* Not an error - property is optional */
	}

	if (len != 4) {
		pr_err("Invalid %s property length: %d (must be 4 bytes for single u32 size)\n",
		       MK_DT_RESOURCE_MEMORY, len);
		return -EINVAL;
	}

	/* Parse single memory size value */
	total_size = fdt32_to_cpu(prop[0]);
	if (total_size == 0) {
		pr_err("Invalid memory size 0 in %s\n", MK_DT_RESOURCE_MEMORY);
		return -EINVAL;
	}

	/* Validate size alignment */
	if (total_size & (PAGE_SIZE - 1)) {
		pr_err("Memory size 0x%zx not page-aligned\n", total_size);
		return -EINVAL;
	}

	config->memory_size = total_size;
	pr_info("Successfully parsed memory size: %zu bytes (%zu MB)\n",
		total_size, total_size >> 20);
	return 0;
}

/**
 * CPU resource parsing
 */
static int mk_dt_parse_cpus(const void *fdt, int chosen_node,
			    struct mk_dt_config *config)
{
	const fdt32_t *prop;
	int len, i, cpu_count;

	if (!config->cpus_valid) {
		pr_debug("CPU mask allocation failed, skipping CPU parsing\n");
		return 0;
	}

	/* Look for cpus property */
	prop = fdt_getprop(fdt, chosen_node, MK_DT_RESOURCE_CPUS, &len);
	if (!prop) {
		pr_debug("No %s property found\n", MK_DT_RESOURCE_CPUS);
		return 0; /* Not an error - property is optional */
	}

	if (len % 4 != 0) {
		pr_err("Invalid %s property length: %d (must be multiple of 4)\n",
		       MK_DT_RESOURCE_CPUS, len);
		return -EINVAL;
	}

	cpu_count = len / 4; /* Each CPU is a u32 value */
	if (cpu_count == 0) {
		pr_err("Empty CPU list in %s\n", MK_DT_RESOURCE_CPUS);
		return -EINVAL;
	}

	pr_debug("Parsing %d CPUs\n", cpu_count);

	/* Clear the CPU mask first */
	cpumask_clear(config->cpus);

	/* Parse each CPU ID */
	for (i = 0; i < cpu_count; i++) {
		u32 cpu_id = fdt32_to_cpu(prop[i]);

		/* Validate CPU ID */
		if (cpu_id >= nr_cpu_ids) {
		pr_err("Invalid CPU ID %u (max: %u) in %s\n",
		       cpu_id, nr_cpu_ids - 1, MK_DT_RESOURCE_CPUS);
			return -EINVAL;
		}

		/* Check if CPU is online/possible */
		if (!cpu_possible(cpu_id)) {
			pr_err("CPU %u is not possible on this system\n", cpu_id);
			return -EINVAL;
		}

		/* Add to CPU mask */
		cpumask_set_cpu(cpu_id, config->cpus);
		pr_debug("Added CPU %u to multikernel instance\n", cpu_id);
	}

	pr_info("Successfully parsed %d CPUs: %*pbl\n",
		cpu_count, cpumask_pr_args(config->cpus));
	return 0;
}

/**
 * Main device tree parsing function
 */
int mk_dt_parse(const void *dtb_data, size_t dtb_size,
		struct mk_dt_config *config)
{
	const void *fdt = dtb_data;
	int ret;

	if (!dtb_data || !config) {
		pr_err("Invalid parameters to mk_dt_parse\n");
		return -EINVAL;
	}

	/* Validate FDT header */
	ret = fdt_check_header(fdt);
	if (ret) {
		pr_err("Invalid device tree blob: %d\n", ret);
		return -EINVAL;
	}

	/* Verify size matches */
	if (fdt_totalsize(fdt) > dtb_size) {
		pr_err("DTB size mismatch: header=%u, provided=%zu\n",
		       fdt_totalsize(fdt), dtb_size);
		return -EINVAL;
	}

	/* Find /instances node */
	int instances_node = fdt_path_offset(fdt, "/instances");
	if (instances_node < 0) {
		pr_err("No /instances node found in device tree\n");
		return -ENOENT;
	}

	/* Find the first (and should be only) instance */
	int instance_node = fdt_first_subnode(fdt, instances_node);
	if (instance_node < 0) {
		pr_err("No instance found in /instances node\n");
		return -ENOENT;
	}

	/* Find the resources subnode */
	int resources_node = fdt_subnode_offset(fdt, instance_node, "resources");
	if (resources_node < 0) {
		pr_err("No resources node found in instance\n");
		return -ENOENT;
	}

	/* Store raw DTB reference */
	config->dtb_data = (void *)dtb_data;
	config->dtb_size = dtb_size;

	/* Parse memory regions */
	ret = mk_dt_parse_memory(fdt, resources_node, config);
	if (ret) {
		pr_err("Failed to parse memory regions: %d\n", ret);
		mk_dt_config_free(config);
		return ret;
	}

	/* Parse CPU resources */
	ret = mk_dt_parse_cpus(fdt, resources_node, config);
	if (ret) {
		pr_err("Failed to parse CPU resources: %d\n", ret);
		mk_dt_config_free(config);
		return ret;
	}

	pr_info("Successfully parsed multikernel device tree with %zu bytes memory and %d CPUs\n",
		config->memory_size, config->cpus_valid ? cpumask_weight(config->cpus) : 0);
	return 0;
}

/**
 * Configuration validation
 */
int mk_dt_validate(const struct mk_dt_config *config)
{
	int ret;

	if (!config) {
		pr_err("NULL configuration\n");
		return -EINVAL;
	}

	if (config->version != MK_DT_CONFIG_CURRENT) {
		pr_err("Unsupported configuration version: %u\n", config->version);
		return -ENOTSUPP;
	}

	/* Validate memory regions */
	ret = mk_dt_validate_memory(config);
	if (ret)
		return ret;

	/* Validate CPU resources */
	ret = mk_dt_validate_cpus(config);
	if (ret)
		return ret;

	return 0;
}

/**
 * Memory region validation
 */
static int mk_dt_validate_memory(const struct mk_dt_config *config)
{
	struct resource *pool_res;

	/* Get multikernel pool resource for validation */
	pool_res = multikernel_get_pool_resource();
	if (!pool_res && config->memory_size > 0) {
		pr_err("No multikernel pool available for memory allocation\n");
		return -ENODEV;
	}

	/* Validate memory size */
	if (config->memory_size > 0) {
		/* Basic sanity checks */
		if (config->memory_size < PAGE_SIZE) {
			pr_err("Memory size too small: %zu bytes\n", config->memory_size);
			return -EINVAL;
		}

		if (config->memory_size > SZ_1G) {
			pr_warn("Large memory size requested: %zu bytes\n", config->memory_size);
		}

		/* Check if size fits within multikernel pool */
		if (pool_res) {
			resource_size_t pool_size = resource_size(pool_res);
			if (config->memory_size > pool_size) {
				pr_err("Requested memory size %zu bytes exceeds pool size %llu bytes\n",
				       config->memory_size, pool_size);
				return -ERANGE;
			}
		}
	}

	return 0;
}

/**
 * CPU resource validation
 */
static int mk_dt_validate_cpus(const struct mk_dt_config *config)
{
	int cpu;

	/* Skip validation if CPU assignment is not valid or empty */
	if (!config->cpus_valid || cpumask_empty(config->cpus))
		return 0;

	/* Check that all CPUs are still possible/online */
	for_each_cpu(cpu, config->cpus) {
		if (!cpu_possible(cpu)) {
			pr_err("CPU %d is no longer possible on this system\n", cpu);
			return -EINVAL;
		}

		if (!cpu_online(cpu)) {
			pr_warn("CPU %d is not online, multikernel may fail to start\n", cpu);
		}
	}

	/* Check for reasonable CPU count */
	if (cpumask_weight(config->cpus) > num_online_cpus()) {
		pr_warn("Requested %d CPUs but only %d are online\n",
			cpumask_weight(config->cpus), num_online_cpus());
	}

	/* Ensure we don't assign CPU 0 unless explicitly allowed */
	if (cpumask_test_cpu(0, config->cpus)) {
		pr_warn("CPU 0 assigned to multikernel instance - this may affect system stability\n");
	}

	return 0;
}

/**
 * Resource availability checking
 */
bool mk_dt_resources_available(const struct mk_dt_config *config)
{
	struct resource *pool_res;

	if (!config)
		return false;

	/* Check if multikernel pool is available */
	pool_res = multikernel_get_pool_resource();
	if (!pool_res) {
		pr_debug("No multikernel pool available\n");
		return false;
	}

	/* Check if requested memory size is available */
	if (config->memory_size > 0) {
		resource_size_t pool_size = resource_size(pool_res);
		if (pool_size < config->memory_size) {
			pr_debug("Pool too small: need %zu, have %llu\n",
				 config->memory_size, pool_size);
			return false;
		}
	}

	/* Check CPU availability */
	if (config->cpus_valid && !cpumask_empty(config->cpus)) {
		int cpu;

		for_each_cpu(cpu, config->cpus) {
			if (!cpu_possible(cpu)) {
				pr_debug("CPU %d is not possible\n", cpu);
				return false;
			}
		}
	}

	/* TODO: More sophisticated checking:
	 * - Check for fragmentation
	 * - Honor specific start address requests
	 * - Check for conflicts with existing allocations
	 * - Check for CPU conflicts with other instances
	 */

	return true;
}

/**
 * Property size helper
 */
int mk_dt_get_property_size(const void *dtb_data, size_t dtb_size,
			    const char *property)
{
	const void *fdt = dtb_data;
	int chosen_node;
	const void *prop;
	int len;

	if (!dtb_data || !property)
		return -EINVAL;

	if (fdt_check_header(fdt))
		return -EINVAL;

	chosen_node = fdt_path_offset(fdt, "/chosen");
	if (chosen_node < 0)
		return -ENOENT;

	prop = fdt_getprop(fdt, chosen_node, property, &len);
	if (!prop)
		return -ENOENT;

	return len;
}

/**
 * Debug and information functions
 */
void mk_dt_print_config(const struct mk_dt_config *config)
{
	if (!config) {
		pr_info("Multikernel DT config: (null)\n");
		return;
	}

	pr_info("Multikernel DT config (version %u):\n", config->version);

	if (config->memory_size > 0) {
		pr_info("  Memory size: %zu bytes (%zu MB)\n",
			config->memory_size, config->memory_size >> 20);
	} else {
		pr_info("  Memory size: none specified\n");
	}

	if (config->cpus_valid) {
		if (cpumask_empty(config->cpus)) {
			pr_info("  CPU assignment: none specified\n");
		} else {
			pr_info("  CPU assignment: %*pbl (%d CPUs)\n",
				cpumask_pr_args(config->cpus), cpumask_weight(config->cpus));
		}
	} else {
		pr_info("  CPU assignment: unavailable (allocation failed)\n");
	}

	pr_info("  DTB: %zu bytes\n", config->dtb_size);
}
