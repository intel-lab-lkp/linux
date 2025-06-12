// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, Christoph Hellwig.
 * Copyright (c) 2015, Intel Corporation.
 */
#include <linux/platform_device.h>
#include <linux/memory_hotplug.h>
#include <linux/libnvdimm.h>
#include <linux/module.h>
#include <linux/numa.h>
#include <linux/moduleparam.h>
#include <linux/parser.h>
#include <linux/string.h>
#include <linux/xarray.h>

#define MAX_PMEM_ARGUMENTS 32

static char *pmem[MAX_PMEM_ARGUMENTS];
static int pmem_count;

static int pmem_param_set(const char *arg, const struct kernel_param *kp)
{
	int rc;
	struct kernel_param kp_new;

	kp_new.name = kp->name;
	kp_new.arg = &pmem[pmem_count];
	rc = param_set_charp(arg, &kp_new);
	if (rc)
		return rc;
	++pmem_count;
	return 0;
}

static void pmem_param_free(void *arg)
{
	int i;

	for (i = 0; i < pmem_count; ++i)
		param_free_charp(&pmem[i]);

	pmem_count = 0;
}

static const struct kernel_param_ops pmem_param_ops = {
	.set =		pmem_param_set,
	.free =		pmem_param_free,
};
module_param_cb(pmem, &pmem_param_ops, NULL, 0);

struct pmem_entry {
	unsigned long region_size;
	bool treat_as_devdax;
	unsigned long align;
};

static int parse_one_optional_pmem_param(struct pmem_entry *entry, char *p)
{
	int token;
	char *parse_end;
	substring_t args[MAX_OPT_ARGS];

	enum {
		OPT_MODE_DEVDAX,
		OPT_MODE_FSDAX,
		OPT_ALIGN,
		OPT_ERR,
	};

	static const match_table_t tokens = {
		{OPT_MODE_DEVDAX, "mode=devdax"},
		{OPT_MODE_FSDAX, "mode=fsdax"},
		{OPT_ALIGN, "align=%s"},
		{OPT_ERR, NULL}
	};

	token = match_token(p, tokens, args);
	switch (token) {
	case OPT_MODE_DEVDAX:
		entry->treat_as_devdax = true;
		break;
	case OPT_MODE_FSDAX:
		break;
	case OPT_ALIGN:
		entry->align = memparse(args[0].from, &parse_end);
		if (parse_end == args[0].from || parse_end != args[0].to) {
			pr_err("Can't parse pmem align: %s\n", args[0].from);
			return -EINVAL;
		}
		break;
	default:
		pr_warn("Unexpected parameter: %s\n", p);
	}

	return 0;
}

static int parse_one_pmem_arg(struct xarray *xarray, char *whole_arg)
{
	int rc = -EINVAL;
	char *whole_arg_copy, *char_iter, *p, *oldp;
	unsigned long start;
	struct pmem_entry *entry = kzalloc(sizeof(*entry), GFP_KERNEL);

	if (!entry)
		return -ENOMEM;

	whole_arg_copy = kstrdup(whole_arg, GFP_KERNEL);
	if (!whole_arg_copy) {
		rc = -ENOMEM;
		goto err_str;
	}

	char_iter = whole_arg_copy;

	p = strsep(&char_iter, ",");
	oldp = p;
	start = memparse(p, &p);
	if (p == oldp || p == NULL) {
		pr_err("Can't parse pmem start: %s\n", oldp);
		goto err;
	}

	p = strsep(&char_iter, ",");
	oldp = p;
	entry->region_size = memparse(p, &p);
	if (p == oldp) {
		pr_err("Can't parse pmem region size: %s\n", oldp);
		goto err;
	}

	while ((p = strsep(&char_iter, ",")) != NULL) {
		rc = parse_one_optional_pmem_param(entry, p);
		if (rc)
			goto err;
	}

	rc = xa_err(xa_store(xarray, start, entry, GFP_KERNEL));
	if (rc)
		pr_err("Failed to store 0x%lx in xarray, error %d\n", start, rc);

err:
	kfree(whole_arg_copy);
err_str:
	if (rc)
		kfree(entry);
	return rc;
}

static void e820_pmem_remove(struct platform_device *pdev)
{
	struct nvdimm_bus *nvdimm_bus = platform_get_drvdata(pdev);

	nvdimm_bus_unregister(nvdimm_bus);
}

static int register_one_pmem(struct resource *res, struct nvdimm_bus *nvdimm_bus,
			     struct pmem_entry *entry)
{
	struct nd_region_desc ndr_desc;
	int nid = phys_to_target_node(res->start);

	memset(&ndr_desc, 0, sizeof(ndr_desc));
	ndr_desc.res = res;
	ndr_desc.numa_node = numa_map_to_online_node(nid);
	ndr_desc.target_node = nid;
	set_bit(ND_REGION_PAGEMAP, &ndr_desc.flags);
	if (entry && entry->treat_as_devdax) {
		set_bit(ND_REGION_DEVDAX, &ndr_desc.flags);
		ndr_desc.provider_data = (void *)entry->align;
	}
	if (!nvdimm_pmem_region_create(nvdimm_bus, &ndr_desc))
		return -ENXIO;
	return 0;
}

struct walk_data {
	struct xarray *pmem_xarray;
	struct nvdimm_bus *nvdimm_bus;
};

static int e820_handle_one_entry(struct resource *res, void *data)
{
	struct walk_data *walk_data = data;
	struct resource res_local;
	struct pmem_entry *entry;
	unsigned long entry_size = resource_size(res);
	int rc;

	entry = xa_load(walk_data->pmem_xarray, res->start);

	if (!entry)
		return register_one_pmem(res, walk_data->nvdimm_bus, NULL);

	if (entry_size % entry->region_size != 0) {
		pr_err("Entry size %lu is not divisible by region size %lu\n",
		       entry_size, entry->region_size);
		return -EINVAL;
	}

	res_local.start = res->start;
	res_local.end = res->start + entry->region_size - 1;
	while (res_local.end <= res->end) {
		rc = register_one_pmem(&res_local, walk_data->nvdimm_bus, entry);
		if (rc)
			return rc;

		res_local.start += entry->region_size;
		res_local.end += entry->region_size;
	}

	return 0;
}

static void free_pmem_xarray(struct xarray *pmem_xarray)
{
	unsigned long start;
	struct pmem_entry *entry;

	xa_for_each(pmem_xarray, start, entry) {
		kfree(entry);
	}
	xa_destroy(pmem_xarray);
}

static int e820_pmem_probe(struct platform_device *pdev)
{
	static struct nvdimm_bus_descriptor nd_desc;
	struct device *dev = &pdev->dev;
	struct nvdimm_bus *nvdimm_bus;
	struct xarray pmem_xarray;
	struct walk_data walk_data = {.pmem_xarray = &pmem_xarray};
	int rc = -ENXIO;
	int i;

	nd_desc.provider_name = "e820";
	nd_desc.module = THIS_MODULE;
	nvdimm_bus = nvdimm_bus_register(dev, &nd_desc);
	if (!nvdimm_bus)
		goto err;
	platform_set_drvdata(pdev, nvdimm_bus);

	xa_init(&pmem_xarray);
	for (i = 0; i < pmem_count; i++) {
		rc = parse_one_pmem_arg(&pmem_xarray, pmem[i]);
		if (rc != 0 && rc != -EINVAL) {
			free_pmem_xarray(&pmem_xarray);
			goto err;
		}
	}

	walk_data.nvdimm_bus = nvdimm_bus;
	rc = walk_iomem_res_desc(IORES_DESC_PERSISTENT_MEMORY_LEGACY,
		IORESOURCE_MEM, 0, -1, &walk_data, e820_handle_one_entry);
	free_pmem_xarray(&pmem_xarray);
	if (rc)
		goto err;
	return 0;
err:
	nvdimm_bus_unregister(nvdimm_bus);
	dev_err(dev, "failed to register legacy persistent memory ranges\n");
	return rc;
}

static struct platform_driver e820_pmem_driver = {
	.probe = e820_pmem_probe,
	.remove = e820_pmem_remove,
	.driver = {
		.name = "e820_pmem",
	},
};

module_platform_driver(e820_pmem_driver);

MODULE_ALIAS("platform:e820_pmem*");
MODULE_DESCRIPTION("NVDIMM support for e820 type-12 memory");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel Corporation");
