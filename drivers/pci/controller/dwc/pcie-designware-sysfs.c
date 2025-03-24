// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "pcie-designware.h"

static int dw_pcie_ptm_check_capability(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	pci->ptm_vsec_offset = dw_pcie_find_ptm_capability(pci);

	return pci->ptm_vsec_offset;
}

static int dw_pcie_ptm_context_update_store(void *drvdata, const char *buf)
{
	struct dw_pcie *pci = drvdata;
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

	return 0;
}

static ssize_t dw_pcie_ptm_context_update_show(void *drvdata, char *buf)
{
	struct dw_pcie *pci = drvdata;
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

static int dw_pcie_ptm_context_valid_store(void *drvdata, bool valid)
{
	struct dw_pcie *pci = drvdata;
	u32 val;

	if (valid) {
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val |= PTM_RES_CCONTEXT_VALID;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else {
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val &= ~PTM_RES_CCONTEXT_VALID;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	}

	return 0;
}

static ssize_t dw_pcie_ptm_context_valid_show(void *drvdata, char *buf)
{
	struct dw_pcie *pci = drvdata;
	u32 val;

	val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);

	return sysfs_emit(buf, "%u\n", !!FIELD_GET(PTM_RES_CCONTEXT_VALID, val));
}

static ssize_t dw_pcie_ptm_local_clock_show(void *drvdata, char *buf)
{
	struct dw_pcie *pci = drvdata;
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t dw_pcie_ptm_master_clock_show(void *drvdata, char *buf)
{
	struct dw_pcie *pci = drvdata;
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t dw_pcie_ptm_t1_show(void *drvdata, char *buf)
{
	struct dw_pcie *pci = drvdata;
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t dw_pcie_ptm_t2_show(void *drvdata, char *buf)
{
	struct dw_pcie *pci = drvdata;
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t dw_pcie_ptm_t3_show(void *drvdata, char *buf)
{
	struct dw_pcie *pci = drvdata;
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static ssize_t dw_pcie_ptm_t4_show(void *drvdata, char *buf)
{
	struct dw_pcie *pci = drvdata;
	u32 msb, lsb;

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB));

	return sysfs_emit(buf, "%llu\n", ((u64) msb) << 32 | lsb);
}

static bool dw_pcie_ptm_context_update_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return (pci->mode == DW_PCIE_EP_TYPE) ? true : false;
}

static bool dw_pcie_ptm_context_valid_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return (pci->mode == DW_PCIE_RC_TYPE) ? true : false;
}

static bool dw_pcie_ptm_local_clock_visible(void *drvdata)
{
	/* PTM local clock is always visible */
	return true;
}

static bool dw_pcie_ptm_master_clock_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return (pci->mode == DW_PCIE_EP_TYPE) ? true : false;
}

static bool dw_pcie_ptm_t1_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return (pci->mode == DW_PCIE_EP_TYPE) ? true : false;
}

static bool dw_pcie_ptm_t2_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return (pci->mode == DW_PCIE_RC_TYPE) ? true : false;
}

static bool dw_pcie_ptm_t3_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return (pci->mode == DW_PCIE_RC_TYPE) ? true : false;
}

static bool dw_pcie_ptm_t4_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return (pci->mode == DW_PCIE_EP_TYPE) ? true : false;
}

struct pcie_ptm_ops dw_pcie_ptm_ops = {
	.check_capability = dw_pcie_ptm_check_capability,
	.context_update_store = dw_pcie_ptm_context_update_store,
	.context_update_show = dw_pcie_ptm_context_update_show,
	.context_valid_store = dw_pcie_ptm_context_valid_store,
	.context_valid_show = dw_pcie_ptm_context_valid_show,
	.local_clock_show = dw_pcie_ptm_local_clock_show,
	.master_clock_show = dw_pcie_ptm_master_clock_show,
	.t1_show = dw_pcie_ptm_t1_show,
	.t2_show = dw_pcie_ptm_t2_show,
	.t3_show = dw_pcie_ptm_t3_show,
	.t4_show = dw_pcie_ptm_t4_show,
	.context_update_visible = dw_pcie_ptm_context_update_visible,
	.context_valid_visible = dw_pcie_ptm_context_valid_visible,
	.local_clock_visible = dw_pcie_ptm_local_clock_visible,
	.master_clock_visible = dw_pcie_ptm_master_clock_visible,
	.t1_visible = dw_pcie_ptm_t1_visible,
	.t2_visible = dw_pcie_ptm_t2_visible,
	.t3_visible = dw_pcie_ptm_t3_visible,
	.t4_visible = dw_pcie_ptm_t4_visible,
};

void pcie_designware_sysfs_init(struct dw_pcie *pci,
				    enum dw_pcie_device_mode mode)
{
	pci->mode = mode;
	pcie_ptm_create_sysfs(pci->dev, pci, &dw_pcie_ptm_ops);
}

void pcie_designware_sysfs_exit(struct dw_pcie *pci)
{
	pcie_ptm_destroy_sysfs();
}
