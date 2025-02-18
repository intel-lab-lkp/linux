// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/slab.h>

#include "pcie-designware.h"

static ssize_t ptm_context_update_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 val;

	if (sysfs_streq(buf, "auto")) {
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val |= PTM_REQ_AUTO_UPDATE_ENABLED;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else if (sysfs_streq(buf, "manual")) {
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val &= ~PTM_REQ_AUTO_UPDATE_ENABLED;
		val |= PTM_REQ_START_UPDATE;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t ptm_context_update_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 val;

	val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
	if (FIELD_GET(PTM_REQ_AUTO_UPDATE_ENABLED, val))
		return sysfs_emit(buf, "auto\n");

	/*
	 * PTM_REQ_START_UPDATE is a self clearing register bit. So if
	 * PTM_REQ_AUTO_UPDATE_ENABLED is not set, then it implies that
	 * manual update is used.
	 */
	return sysfs_emit(buf, "manual\n");
}

static ssize_t ptm_context_valid_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	unsigned long arg;
	u32 val;

	if (kstrtoul(buf, 0, &arg) < 0)
		return -EINVAL;

	if (!!arg) {
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val |= PTM_RES_CCONTEXT_VALID;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else {
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val &= ~PTM_RES_CCONTEXT_VALID;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	}

	return count;
}

static ssize_t ptm_context_valid_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 val;

	val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);

	return sysfs_emit(buf, "%u\n", !!FIELD_GET(PTM_RES_CCONTEXT_VALID, val));
}

static ssize_t ptm_local_clock_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t ptm_master_clock_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t ptm_t1_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t ptm_t2_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t ptm_t3_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t ptm_t4_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct dw_pcie *pci = dev_get_drvdata(dev);
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static DEVICE_ATTR_RW(ptm_context_update);
static DEVICE_ATTR_RW(ptm_context_valid);
static DEVICE_ATTR_RO(ptm_local_clock);
static DEVICE_ATTR_RO(ptm_master_clock);
static DEVICE_ATTR_RO(ptm_t1);
static DEVICE_ATTR_RO(ptm_t2);
static DEVICE_ATTR_RO(ptm_t3);
static DEVICE_ATTR_RO(ptm_t4);

static struct attribute *ptm_attrs[] = {
	&dev_attr_ptm_context_update.attr,
	&dev_attr_ptm_context_valid.attr,
	&dev_attr_ptm_local_clock.attr,
	&dev_attr_ptm_master_clock.attr,
	&dev_attr_ptm_t1.attr,
	&dev_attr_ptm_t2.attr,
	&dev_attr_ptm_t3.attr,
	&dev_attr_ptm_t4.attr,
	NULL
};

static umode_t ptm_attr_visible(struct kobject *kobj, struct attribute *attr,
				int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct dw_pcie *pci = dev_get_drvdata(dev);

	/* RC only needs local, t2 and t3 clocks and context_valid */
	if ((attr == &dev_attr_ptm_t1.attr && pci->mode == DW_PCIE_RC_TYPE) ||
	    (attr == &dev_attr_ptm_t4.attr && pci->mode == DW_PCIE_RC_TYPE) ||
	    (attr == &dev_attr_ptm_master_clock.attr && pci->mode == DW_PCIE_RC_TYPE) ||
	    (attr == &dev_attr_ptm_context_update.attr && pci->mode == DW_PCIE_RC_TYPE))
		return 0;

	/* EP only needs local, master, t1, and t4 clocks and context_update */
	if ((attr == &dev_attr_ptm_t2.attr && pci->mode == DW_PCIE_EP_TYPE) ||
	    (attr == &dev_attr_ptm_t3.attr && pci->mode == DW_PCIE_EP_TYPE) ||
	    (attr == &dev_attr_ptm_context_valid.attr && pci->mode == DW_PCIE_EP_TYPE))
		return 0;

	return attr->mode;
}

static const struct attribute_group ptm_attr_group = {
	.name = "ptm",
	.attrs = ptm_attrs,
	.is_visible = ptm_attr_visible,
};

static const struct attribute_group *dwc_pcie_attr_groups[] = {
	&ptm_attr_group,
	NULL,
};

static void pcie_designware_sysfs_release(struct device *dev)
{
	kfree(dev);
}

void pcie_designware_sysfs_init(struct dw_pcie *pci,
				    enum dw_pcie_device_mode mode)
{
	struct device *dev;
	int ret;

	/* Check for capabilities before creating sysfs attrbutes */
	ret = dw_pcie_find_ptm_capability(pci);
	if (!ret) {
		dev_dbg(pci->dev, "PTM capability not present\n");
		return;
	}

	pci->ptm_vsec_offset = ret;
	pci->mode = mode;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return;

	device_initialize(dev);
	dev->groups = dwc_pcie_attr_groups;
	dev->release = pcie_designware_sysfs_release;
	dev->parent = pci->dev;
	dev_set_drvdata(dev, pci);

	ret = dev_set_name(dev, "dwc");
	if (ret)
		goto err_free;

	ret = device_add(dev);
	if (ret)
		goto err_free;

	pci->sysfs_dev = dev;

	return;

err_free:
	put_device(dev);
}

void pcie_designware_sysfs_exit(struct dw_pcie *pci)
{
	if (pci->sysfs_dev)
		device_unregister(pci->sysfs_dev);
}
