// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * Author: Daniel Lezcano <daniel.lezcano@oss.qualcomm.com>
 *
 * Powercap hierarchy description test module
 */
#include <linux/powercap.h>

struct pch_test_data {
	int value;
};

static struct powercap_node __initdata pch_test_nodes[] = {
	[0] = { .name = "package",
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[1] = { .name = "cluster0", .parent = &pch_test_nodes[0],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[2] = { .name = "cluster1", .parent = &pch_test_nodes[0],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[3] = { .name = "cluster2", .parent = &pch_test_nodes[0],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[4] = { .name = "cpu0", .parent = &pch_test_nodes[1],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[5] = { .name = "cpu1", .parent = &pch_test_nodes[1],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[6] = { .name = "cpu2", .parent = &pch_test_nodes[1],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[7] = { .name = "cpu3", .parent = &pch_test_nodes[1],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[8] = { .name = "cpu4", .parent = &pch_test_nodes[2],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[9] = { .name = "cpu5", .parent = &pch_test_nodes[2],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[10] = { .name = "cpu6", .parent = &pch_test_nodes[2],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[11] = { .name = "cpu7", .parent = &pch_test_nodes[2],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[12] = { .name = "cpu8", .parent = &pch_test_nodes[3],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[13] = { .name = "cpu9", .parent = &pch_test_nodes[3],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[14] = { .name = "cpu10", .parent = &pch_test_nodes[3],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
	[15] = { .name = "cpu11", .parent = &pch_test_nodes[3],
		.data = &(struct pch_test_data) { .value = 0xDEADBEEF } },
};

static struct powercap_hierarchy __initdata pch_test_hierarchy = {
	.nodes = pch_test_nodes,
	.nr_nodes = ARRAY_SIZE(pch_test_nodes),
};

static struct powercap_hierarchy *hierarchy;

static struct powercap_control_type *pct;

struct pch_test {
	struct powercap_zone zone;
};

static struct pch_test *to_pch_test(struct powercap_zone *pcz)
{
	return container_of(pcz, struct pch_test, zone);
}

static int pch_test_get_max_power_range_uw(struct powercap_zone *pcz,
					   u64 *power_uw)
{
	*power_uw = 0xBADC0FFEE;
	return 0;
}

static int pch_test_get_power_uw(struct powercap_zone *pcz,
				 u64 *power_uw)
{
	*power_uw = 0xC0FFEE;
	return 0;
}

static int pch_test_release(struct powercap_zone *pcz)
{
	kfree(to_pch_test(pcz));
	return 0;
}

static const struct powercap_zone_ops pch_test_ops = {
	.get_max_power_range_uw = pch_test_get_max_power_range_uw,
	.get_power_uw = pch_test_get_power_uw,
	.release = pch_test_release,
};

static int pch_test_set_power_limit_uw(struct powercap_zone *pcz,
				       int cid, u64 power_uw)
{
	return 0;
}

static int pch_test_get_power_limit_uw(struct powercap_zone *pcz,
				       int cid, u64 *power_uw)
{
	*power_uw = 0xDEADC0DE;
	return 0;
}

static int pch_test_set_time_window_us(struct powercap_zone *pcz,
				       int cid, u64 power_uw)
{
	return 0;
}

static int pch_test_get_time_window_us(struct powercap_zone *pcz,
				       int cid, u64 *power_uw)
{
	*power_uw = 0xDEADC0DE;
	return 0;
}

static int pch_test_get_max_power_uw(struct powercap_zone *pcz,
				     int cid, u64 *power_uw)
{
	*power_uw = 0xDEADC0DE;
	return 0;
}

static const char *pch_test_get_name(struct powercap_zone *pcz, int cid)
{
	return "my constraint name";
}

static const struct powercap_zone_constraint_ops pch_test_constraint_ops = {
	.set_power_limit_uw = pch_test_set_power_limit_uw,
	.get_power_limit_uw = pch_test_get_power_limit_uw,
	.set_time_window_us = pch_test_set_time_window_us,
	.get_time_window_us = pch_test_get_time_window_us,
	.get_max_power_uw = pch_test_get_max_power_uw,
	.get_name = pch_test_get_name,
};

static struct powercap_zone *pch_test_create(struct powercap_control_type *pct,
					     const char *name, void *data,
					     struct powercap_zone *parent)
{
	struct pch_test_data *pcht_data = data;
	struct pch_test *pcht;
	struct powercap_zone *pcz;

	if (!pct) {
		pr_err("Invalid NULL controller type\n");
		return ERR_PTR(-EINVAL);
	}

	if (!name) {
		pr_err("Invalid NULL name\n");
		return ERR_PTR(-EINVAL);
	}

	if (pcht_data->value != 0xDEADBEEF) {
		pr_err("Invalid pcht data != 0xDEADBEEF");
		return ERR_PTR(-EINVAL);
	}

	pcht = kzalloc_obj(*pcht);
	if (!pcht)
		return ERR_PTR(-ENOMEM);

	pcz = powercap_register_zone(&pcht->zone, pct, name, parent,
				     &pch_test_ops, 1, &pch_test_constraint_ops);
	if (IS_ERR(pcz)) {
		pr_err("Failed to register powercap zone '%s': %ld\n",
		       name, PTR_ERR(pcz));
	}

	return pcz;
}

static void pch_test_destroy(struct powercap_control_type *pct,
			     struct powercap_zone *zone,
			     void *data)
{
	struct pch_test_data *pcht_data = data;

	if (!pct) {
		pr_err("Invalid NULL controller type\n");
		return;
        }

	if (!zone) {
		pr_err("Invalid NULL zone\n");
		return;
        }

	if (pcht_data->value != 0xDEADBEEF) {
		pr_err("Invalid pcht data != 0xDEADBEEF");
		return;
        }

	powercap_unregister_zone(pct, zone);
}

static int __init pch_test_init(void)
{
	int ret;

	hierarchy = powercap_hierarchy_dup(&pch_test_hierarchy);
	if (IS_ERR(hierarchy)) {
		ret = PTR_ERR(hierarchy);
		pr_err("Failed to dup the hierarchy: %d\n", ret);
		return ret;
	}

	pct = powercap_register_control_type(NULL, "powercap-test", NULL);
	if (IS_ERR(pct)) {
		ret = PTR_ERR(pct);
		pr_err("Failed to register control type: %d\n", ret);
		goto out_free_hierarchy;
	}

	ret = powercap_hierarchy_create(pct, hierarchy, pch_test_create, pch_test_destroy);
	if (ret) {
		pr_err("Failed to create the hierarchy: %d\n", ret);
		goto out_unregister_pct;
	}

	return 0;

out_unregister_pct:
	powercap_unregister_control_type(pct);
out_free_hierarchy:
	powercap_hierarchy_free(hierarchy);
	return ret;
}
module_init(pch_test_init);

static void __exit pch_test_exit(void)
{
	powercap_hierarchy_destroy(pct, hierarchy, pch_test_destroy);
	powercap_hierarchy_free(hierarchy);
	powercap_unregister_control_type(pct);
}
module_exit(pch_test_exit);

MODULE_DESCRIPTION("Powercap hierarchy test driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Lezcano <daniel.lezcano@oss.qualcomm.com");

