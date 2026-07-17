// SPDX-License-Identifier: GPL-2.0
/*
 * Arm CLA driver - device topology initialization
 *
 * A CLA domain is a group of devices that work together and cannot be isolated
 * from each other. They are owned by a single user at a time.
 *
 * Copyright 2026 Arm Limited.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/of.h>

#include "arm-cla.h"

DEFINE_XARRAY_ALLOC(cla_domains);
unsigned int cla_nr_domains;

/*
 * Some CPUs may not have a CLA. So cla_lut_cpu (indexed by CPU) may be sparse,
 * but cla_lut_pg (indexed by page offset in mmap'ed file) is a contiguous array
 * of all devices, of size cla_nr_devs, sorted by domain.
 */
struct cla_dev **cla_lut_cpu;
struct cla_dev **cla_lut_pg;
unsigned int cla_nr_devs;

static struct cla_domain *cla_domain_alloc(struct cla_dev *dev, unsigned int id)
{
	int ret;
	struct cla_domain *domain;

	domain = kzalloc_obj(*domain);
	if (!domain)
		return ERR_PTR(-ENOMEM);

	domain->id = id;
	ret = xa_insert(&cla_domains, id, domain, GFP_KERNEL);
	if (ret < 0)
		goto err_free_domain;

	domain->nr_devs = 1;
	domain->devs = kzalloc_obj(*domain->devs);
	if (!domain->devs) {
		ret = -ENOMEM;
		goto err_free_id;
	}
	domain->devs[0] = dev;

	return domain;

err_free_id:
	xa_erase(&cla_domains, domain->id);
err_free_domain:
	kfree(domain);
	return ERR_PTR(ret);
}

/**
 * cla_dev_domain_get - get or create a CLA domain for a device
 * @dev: CLA device
 *
 * Return: CLA domain pointer on success, or an ERR_PTR() on failure.
 */
struct cla_domain *cla_dev_domain_get(struct cla_dev *dev)
{
	int ret;
	unsigned int domain_id;
	struct cla_domain *domain;

	/* Domain ID is provided by firmware */
	ret = of_property_read_u32(dev->dev->of_node, "domain", &domain_id);
	if (WARN_ON(ret))
		return ERR_PTR(-EINVAL);

	domain = xa_load(&cla_domains, domain_id);
	if (domain) {
		domain->nr_devs++;
		domain->devs = krealloc_array(domain->devs, domain->nr_devs,
					      sizeof(*domain->devs), GFP_KERNEL);
		if (!domain->devs)
			return ERR_PTR(-ENOMEM);
		domain->devs[domain->nr_devs - 1] = dev;
		return domain;
	}

	domain = cla_domain_alloc(dev, domain_id);
	if (IS_ERR(domain))
		return domain;

	cla_nr_domains = max(domain_id + 1, cla_nr_domains);

	return domain;
}

/**
 * cla_domains_finalise - build CLA device lookup tables
 *
 * Return: 0 on success, or a negative error code.
 */
int __init cla_domains_finalise(void)
{
	int ret = -ENOMEM;
	unsigned int i, j;
	unsigned int pg_offset = 0;

	cla_lut_cpu = kzalloc_objs(*cla_lut_cpu, nr_cpu_ids);
	if (!cla_lut_cpu)
		goto err_free;

	cla_lut_pg = kzalloc_objs(*cla_lut_pg, cla_nr_devs);
	if (!cla_lut_pg)
		goto err_free;

	ret = -EINVAL;

	/* Populate the lookup tables. */
	for (i = 0; i < cla_nr_domains; i++) {
		struct cla_domain *domain = xa_load(&cla_domains, i);

		/* The user API requires sequential domain IDs */
		if (WARN_ON(!domain))
			goto err_free;

		domain->pg_offset = pg_offset;

		for (j = 0; j < domain->nr_devs; j++) {
			struct cla_dev *dev = domain->devs[j];

			if (WARN_ON(dev->cpu >= nr_cpu_ids) ||
			    WARN_ON(pg_offset >= cla_nr_devs))
				goto err_free;
			WARN_ON(cla_lut_cpu[dev->cpu]);
			WARN_ON(cla_lut_pg[pg_offset]);

			cla_lut_cpu[dev->cpu] = dev;
			cla_lut_pg[pg_offset] = dev;
			dev->pg_offset = pg_offset;
			pg_offset++;
		}
	}

	return 0;

err_free:
	kfree(cla_lut_cpu);
	kfree(cla_lut_pg);
	cla_lut_cpu = NULL;
	cla_lut_pg = NULL;
	return ret;
}

static void cla_domain_free(struct cla_domain *domain)
{
	kfree(domain->devs);
	xa_erase(&cla_domains, domain->id);
	kfree(domain);
}

/**
 * cla_domains_free - free CLA domains and lookup tables
 */
void cla_domains_free(void)
{
	unsigned long id;
	struct cla_domain *domain;

	kfree(cla_lut_cpu);
	kfree(cla_lut_pg);
	cla_lut_cpu = NULL;
	cla_lut_pg = NULL;
	xa_for_each(&cla_domains, id, domain)
		cla_domain_free(domain);
	cla_nr_domains = 0;
}
