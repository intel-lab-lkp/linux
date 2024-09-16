// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2019-2024, Intel Corporation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/intel_dg_spi_aux.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/types.h>

struct intel_dg_spi {
	struct kref refcnt;
	void __iomem *base;
	size_t size;
	unsigned int nregions;
	struct {
		const char *name;
		u8 id;
		u64 offset;
		u64 size;
	} regions[];
};

static void intel_dg_spi_release(struct kref *kref)
{
	struct intel_dg_spi *spi = container_of(kref, struct intel_dg_spi, refcnt);
	int i;

	pr_debug("freeing spi memory\n");
	for (i = 0; i < spi->nregions; i++)
		kfree(spi->regions[i].name);
	kfree(spi);
}

static int intel_dg_spi_probe(struct auxiliary_device *aux_dev,
			      const struct auxiliary_device_id *aux_dev_id)
{
	struct intel_dg_spi_dev *ispi = auxiliary_dev_to_intel_dg_spi_dev(aux_dev);
	struct device *device;
	struct intel_dg_spi *spi;
	unsigned int nregions;
	unsigned int i, n;
	size_t size;
	char *name;
	size_t name_size;
	int ret;

	device = &aux_dev->dev;

	/* count available regions */
	for (nregions = 0, i = 0; i < INTEL_DG_SPI_REGIONS; i++) {
		if (ispi->regions[i].name)
			nregions++;
	}

	if (!nregions) {
		dev_err(device, "no regions defined\n");
		return -ENODEV;
	}

	size = sizeof(*spi) + sizeof(spi->regions[0]) * nregions;
	spi = kzalloc(size, GFP_KERNEL);
	if (!spi)
		return -ENOMEM;

	kref_init(&spi->refcnt);

	spi->nregions = nregions;
	for (n = 0, i = 0; i < INTEL_DG_SPI_REGIONS; i++) {
		if (ispi->regions[i].name) {
			name_size = strlen(dev_name(&aux_dev->dev)) +
				    strlen(ispi->regions[i].name) + 2; /* for point */
			name = kzalloc(name_size, GFP_KERNEL);
			if (!name)
				continue;
			snprintf(name, name_size, "%s.%s",
				 dev_name(&aux_dev->dev), ispi->regions[i].name);
			spi->regions[n].name = name;
			spi->regions[n].id = i;
			n++;
		}
	}

	spi->base = devm_ioremap_resource(device, &ispi->bar);
	if (IS_ERR(spi->base)) {
		dev_err(device, "mmio not mapped\n");
		ret = PTR_ERR(spi->base);
		goto err;
	}

	dev_set_drvdata(&aux_dev->dev, spi);

	return 0;

err:
	kref_put(&spi->refcnt, intel_dg_spi_release);
	return ret;
}

static void intel_dg_spi_remove(struct auxiliary_device *aux_dev)
{
	struct intel_dg_spi *spi = dev_get_drvdata(&aux_dev->dev);

	if (!spi)
		return;

	dev_set_drvdata(&aux_dev->dev, NULL);

	kref_put(&spi->refcnt, intel_dg_spi_release);
}

static const struct auxiliary_device_id intel_dg_spi_id_table[] = {
	{
		.name = "i915.spi",
	},
	{
		.name = "xe.spi",
	},
	{
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(auxiliary, intel_dg_spi_id_table);

static struct auxiliary_driver intel_dg_spi_driver = {
	.probe  = intel_dg_spi_probe,
	.remove = intel_dg_spi_remove,
	.driver = {
		/* auxiliary_driver_register() sets .name to be the modname */
	},
	.id_table = intel_dg_spi_id_table
};

module_auxiliary_driver(intel_dg_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Intel DGFX SPI driver");
