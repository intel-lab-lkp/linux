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
	u32 access_map;
	struct {
		const char *name;
		u8 id;
		u64 offset;
		u64 size;
		unsigned int is_readable:1;
		unsigned int is_writable:1;
	} regions[];
};

#define SPI_TRIGGER_REG       0x00000000
#define SPI_VALSIG_REG        0x00000010
#define SPI_ADDRESS_REG       0x00000040
#define SPI_REGION_ID_REG     0x00000044
/*
 * [15:0]-Erase size = 0x0010 4K 0x0080 32K 0x0100 64K
 * [23:16]-Reserved
 * [31:24]-Erase SPI RegionID
 */
#define SPI_ERASE_REG         0x00000048
#define SPI_ACCESS_ERROR_REG  0x00000070
#define SPI_ADDRESS_ERROR_REG 0x00000074

/* Flash Valid Signature */
#define SPI_FLVALSIG          0x0FF0A55A

#define SPI_MAP_ADDR_MASK     0x000000FF
#define SPI_MAP_ADDR_SHIFT    0x00000004

#define REGION_ID_DESCRIPTOR  0
/* Flash Region Base Address */
#define FRBA      0x40
/* Flash Region __n - Flash Descriptor Record */
#define FLREG(__n)  (FRBA + ((__n) * 4))
/*  Flash Map 1 Register */
#define FLMAP1_REG  0x18
#define FLMSTR4_OFFSET 0x00C

#define SPI_ACCESS_ERROR_PCIE_MASK 0x7

static inline void spi_set_region_id(struct intel_dg_spi *spi, u8 region)
{
	iowrite32((u32)region, spi->base + SPI_REGION_ID_REG);
}

static inline u32 spi_error(struct intel_dg_spi *spi)
{
	u32 reg = ioread32(spi->base + SPI_ACCESS_ERROR_REG) &
		  SPI_ACCESS_ERROR_PCIE_MASK;

	/* reset error bits */
	if (reg)
		iowrite32(reg, spi->base + SPI_ACCESS_ERROR_REG);

	return reg;
}

static inline u32 spi_read32(struct intel_dg_spi *spi, u32 address)
{
	void __iomem *base = spi->base;

	iowrite32(address, base + SPI_ADDRESS_REG);

	return ioread32(base + SPI_TRIGGER_REG);
}

static int spi_get_access_map(struct intel_dg_spi *spi)
{
	u32 flmap1;
	u32 fmba;
	u32 fmstr4;
	u32 fmstr4_addr;

	spi_set_region_id(spi, REGION_ID_DESCRIPTOR);

	flmap1 = spi_read32(spi, FLMAP1_REG);
	if (spi_error(spi))
		return -EIO;
	/* Get Flash Master Baser Address (FMBA) */
	fmba = ((flmap1 & SPI_MAP_ADDR_MASK) << SPI_MAP_ADDR_SHIFT);
	fmstr4_addr = fmba + FLMSTR4_OFFSET;

	fmstr4 = spi_read32(spi, fmstr4_addr);
	if (spi_error(spi))
		return -EIO;

	spi->access_map = fmstr4;
	return 0;
}

static bool spi_region_readable(struct intel_dg_spi *spi, u8 region)
{
	if (region < 12)
		return spi->access_map & (1 << (region + 8)); /* [19:8] */
	else
		return spi->access_map & (1 << (region - 12)); /* [3:0] */
}

static bool spi_region_writeable(struct intel_dg_spi *spi, u8 region)
{
	if (region < 12)
		return spi->access_map & (1 << (region + 20)); /* [31:20] */
	else
		return spi->access_map & (1 << (region - 8)); /* [7:4] */
}

static int intel_dg_spi_is_valid(struct intel_dg_spi *spi)
{
	u32 is_valid;

	spi_set_region_id(spi, REGION_ID_DESCRIPTOR);

	is_valid = spi_read32(spi, SPI_VALSIG_REG);
	if (spi_error(spi))
		return -EIO;

	if (is_valid != SPI_FLVALSIG)
		return -ENODEV;

	return 0;
}

static int intel_dg_spi_init(struct intel_dg_spi *spi, struct device *device)
{
	int ret;
	unsigned int i, n;

	/* clean error register, previous errors are ignored */
	spi_error(spi);

	ret = intel_dg_spi_is_valid(spi);
	if (ret) {
		dev_err(device, "The SPI is not valid %d\n", ret);
		return ret;
	}

	if (spi_get_access_map(spi))
		return -EIO;

	for (i = 0, n = 0; i < spi->nregions; i++) {
		u32 address, base, limit, region;
		u8 id = spi->regions[i].id;

		address = FLREG(id);
		region = spi_read32(spi, address);

		base = (region & 0x0000FFFF) << 12;
		limit = (((region & 0xFFFF0000) >> 16) << 12) | 0xFFF;

		dev_dbg(device, "[%d] %s: region: 0x%08X base: 0x%08x limit: 0x%08x\n",
			id, spi->regions[i].name, region, base, limit);

		if (base >= limit || (i > 0 && limit == 0)) {
			dev_dbg(device, "[%d] %s: disabled\n",
				id, spi->regions[i].name);
			spi->regions[i].is_readable = 0;
			continue;
		}

		if (spi->size < limit)
			spi->size = limit;

		spi->regions[i].offset = base;
		spi->regions[i].size = limit - base + 1;
		/* No write access to descriptor; mask it out*/
		spi->regions[i].is_writable = spi_region_writeable(spi, id);

		spi->regions[i].is_readable = spi_region_readable(spi, id);
		dev_dbg(device, "Registered, %s id=%d offset=%lld size=%lld rd=%d wr=%d\n",
			spi->regions[i].name,
			spi->regions[i].id,
			spi->regions[i].offset,
			spi->regions[i].size,
			spi->regions[i].is_readable,
			spi->regions[i].is_writable);

		if (spi->regions[i].is_readable)
			n++;
	}

	dev_dbg(device, "Registered %d regions\n", n);

	/* Need to add 1 to the amount of memory
	 * so it is reported as an even block
	 */
	spi->size += 1;

	return n;
}

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

	ret = intel_dg_spi_init(spi, device);
	if (ret < 0) {
		dev_err(device, "cannot initialize spi\n");
		ret = -ENODEV;
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
