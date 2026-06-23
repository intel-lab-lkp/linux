// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 NVIDIA Corporation. All rights reserved. */
#include <linux/delay.h>
#include <linux/bug.h>
#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include <cxlpci.h>

#include "cxl.h"
#include "core.h"

struct cxl_rwsem cxl_rwsem = {
	.region = __RWSEM_INITIALIZER(cxl_rwsem.region),
	.dpa = __RWSEM_INITIALIZER(cxl_rwsem.dpa),
};
EXPORT_SYMBOL_FOR_MODULES(cxl_rwsem, "cxl_core");

static void cxld_set_interleave(struct cxl_decoder_settings *settings, u32 *ctrl)
{
	u16 eig;
	u8 eiw;

	/*
	 * Input validation ensures these warns never fire, but otherwise
	 * suppress uninitialized variable usage warnings.
	 */
	if (WARN_ONCE(ways_to_eiw(settings->interleave_ways, &eiw),
		      "invalid interleave_ways: %d\n",
		      settings->interleave_ways))
		return;
	if (WARN_ONCE(granularity_to_eig(settings->interleave_granularity, &eig),
		      "invalid interleave_granularity: %d\n",
		      settings->interleave_granularity))
		return;

	u32p_replace_bits(ctrl, eig, CXL_HDM_DECODER0_CTRL_IG_MASK);
	u32p_replace_bits(ctrl, eiw, CXL_HDM_DECODER0_CTRL_IW_MASK);
	*ctrl |= CXL_HDM_DECODER0_CTRL_COMMIT;
}

static void cxld_set_type(struct cxl_decoder_settings *settings, u32 *ctrl)
{
	u32p_replace_bits(ctrl,
			  !!(settings->target_type == CXL_DECODER_HOSTONLYMEM),
			  CXL_HDM_DECODER0_CTRL_HOSTONLY);
}

/*
 * Per CXL 2.0 8.2.5.12.20 Committing Decoder Programming, hardware must set
 * committed or error within 10ms, but just be generous with 20ms to account for
 * clock skew and other marginal behavior.
 */
#define COMMIT_TIMEOUT_MS 20
static int cxld_await_commit(void __iomem *hdm, int id)
{
	u32 ctrl;
	int i;

	for (i = 0; i < COMMIT_TIMEOUT_MS; i++) {
		ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
		if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMIT_ERROR, ctrl)) {
			ctrl &= ~CXL_HDM_DECODER0_CTRL_COMMIT;
			writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
			return -EIO;
		}
		if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
			return 0;
		fsleep(1000);
	}

	return -ETIMEDOUT;
}

static void setup_hw_decoder(struct cxl_decoder_settings *settings,
			     void __iomem *hdm)
{
	int id = settings->id;
	u64 target_or_skip;
	u64 base, size;
	u32 ctrl;

	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
	cxld_set_interleave(settings, &ctrl);
	cxld_set_type(settings, &ctrl);
	base = settings->hpa_range.start;
	size = range_len(&settings->hpa_range);
	target_or_skip = settings->targets;

	writel(upper_32_bits(base), hdm + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(id));
	writel(lower_32_bits(base), hdm + CXL_HDM_DECODER0_BASE_LOW_OFFSET(id));
	writel(upper_32_bits(size), hdm + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(id));
	writel(lower_32_bits(size), hdm + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(id));
	/* Target-list and endpoint-skip registers alias the same slot. */
	writel(upper_32_bits(target_or_skip),
	       hdm + CXL_HDM_DECODER0_TL_HIGH(id));
	writel(lower_32_bits(target_or_skip),
	       hdm + CXL_HDM_DECODER0_TL_LOW(id));

	writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
}

int cxl_commit(struct cxl_decoder_settings *settings, void __iomem *hdm)
{
	int rc;

	scoped_guard(rwsem_read, &cxl_rwsem.dpa) {
		setup_hw_decoder(settings, hdm);
	}

	rc = cxld_await_commit(hdm, settings->id);
	if (rc)
		return rc;

	settings->flags |= CXL_DECODER_F_ENABLE;

	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_commit, "cxl_core");

#define CXL_HDM_DECODER_MAX_COUNT 32

static void cxl_pci_hdm_clear(void *data)
{
	struct pci_dev *pdev = data;

	WRITE_ONCE(pdev->hdm, NULL);
}

static void cxl_pci_hdm_unmap(struct pci_dev *pdev,
			      struct cxl_component_regs *regs,
			      struct cxl_register_map *map)
{
	struct cxl_reg_map *hdm_map = &map->component_map.hdm_decoder;

	if (!regs->hdm_decoder)
		return;

	devm_iounmap(&pdev->dev, regs->hdm_decoder);
	devm_release_mem_region(&pdev->dev, map->resource + hdm_map->offset,
				hdm_map->size);
}

static int cxl_pci_hdm_read_decoder(struct pci_dev *pdev,
				    struct cxl_decoder_settings *settings,
				    void __iomem *hdm, int id)
{
	u64 target_or_skip, base, size;
	u32 ctrl, lo, hi;
	int rc;

	*settings = (struct cxl_decoder_settings) {
		.id = id,
	};

	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
	if (!(ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED))
		return 0;

	lo = readl(hdm + CXL_HDM_DECODER0_BASE_LOW_OFFSET(id));
	hi = readl(hdm + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(id));
	base = ((u64)hi << 32) | lo;

	lo = readl(hdm + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(id));
	hi = readl(hdm + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(id));
	size = ((u64)hi << 32) | lo;

	if (!size || base == U64_MAX || size == U64_MAX ||
	    base > U64_MAX - (size - 1)) {
		pci_err(pdev, "CXL HDM decoder %d has invalid range\n", id);
		return -ENXIO;
	}

	lo = readl(hdm + CXL_HDM_DECODER0_TL_LOW(id));
	hi = readl(hdm + CXL_HDM_DECODER0_TL_HIGH(id));
	target_or_skip = ((u64)hi << 32) | lo;

	settings->hpa_range = (struct range) {
		.start = base,
		.end = base + size - 1,
	};
	settings->targets = target_or_skip;
	settings->target_type = FIELD_GET(CXL_HDM_DECODER0_CTRL_HOSTONLY, ctrl) ?
				CXL_DECODER_HOSTONLYMEM : CXL_DECODER_DEVMEM;
	settings->flags = CXL_DECODER_F_ENABLE;
	if (ctrl & CXL_HDM_DECODER0_CTRL_LOCK)
		settings->flags |= CXL_DECODER_F_LOCK;

	rc = eiw_to_ways(FIELD_GET(CXL_HDM_DECODER0_CTRL_IW_MASK, ctrl),
			 &settings->interleave_ways);
	if (rc)
		return rc;

	return eig_to_granularity(FIELD_GET(CXL_HDM_DECODER0_CTRL_IG_MASK,
					    ctrl),
				  &settings->interleave_granularity);
}

static int cxl_pci_hdm_capable(struct pci_dev *pdev)
{
	u16 cap;
	int dvsec;
	int rc;

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec)
		return -ENOTTY;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CAP, &cap);
	if (rc)
		return pcibios_err_to_errno(rc);

	if (!(cap & PCI_DVSEC_CXL_MEM_CAPABLE))
		return -ENOTTY;

	return 0;
}

int pci_cxl_hdm_init(struct pci_dev *pdev)
{
	struct cxl_decoder_settings *settings;
	struct cxl_component_regs regs = { 0 };
	struct cxl_register_map map = { 0 };
	struct cxl_hdm_info *info;
	bool allocated_info = false;
	int decoder_count;
	u16 command;
	int rc;

	info = READ_ONCE(pdev->hdm);
	if (info && info->regs.hdm_decoder)
		return 0;

	rc = cxl_pci_hdm_capable(pdev);
	if (rc)
		return rc;

	rc = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (rc)
		return pcibios_err_to_errno(rc);

	if (!(command & PCI_COMMAND_MEMORY))
		return -ENOTTY;

	if (!info) {
		info = devm_kzalloc(&pdev->dev,
				    struct_size(info, settings,
						CXL_HDM_DECODER_MAX_COUNT),
				    GFP_KERNEL);
		if (!info)
			return -ENOMEM;
		allocated_info = true;
	}

	rc = cxl_find_regblock(pdev, CXL_REGLOC_RBI_COMPONENT, &map);
	if (rc)
		return rc;

	rc = cxl_setup_regs(&map);
	if (rc)
		return rc;

	if (!map.component_map.hdm_decoder.valid) {
		rc = -ENODEV;
		return rc;
	}

	rc = cxl_map_component_regs(&map, &regs, BIT(CXL_CM_CAP_CAP_ID_HDM));
	if (rc)
		return rc;

	decoder_count = cxl_hdm_decoder_count(readl(regs.hdm_decoder +
						    CXL_HDM_DECODER_CAP_OFFSET));
	if (decoder_count < 0) {
		rc = decoder_count;
		goto out_unmap;
	}

	if (decoder_count > CXL_HDM_DECODER_MAX_COUNT) {
		rc = -ENXIO;
		goto out_unmap;
	}

	if (info->decoder_count && info->decoder_count != decoder_count) {
		rc = -ENXIO;
		goto out_unmap;
	}

	info->decoder_count = decoder_count;
	info->regs = regs;

	settings = info->settings;
	for (int i = 0; i < info->decoder_count; i++) {
		rc = cxl_pci_hdm_read_decoder(pdev, &settings[i],
					      regs.hdm_decoder, i);
		if (rc)
			goto out_unmap;
	}

	WRITE_ONCE(pdev->hdm, info);
	if (allocated_info) {
		rc = devm_add_action(&pdev->dev, cxl_pci_hdm_clear, pdev);
		if (rc) {
			WRITE_ONCE(pdev->hdm, NULL);
			goto out_unmap;
		}
	}
	return 0;

out_unmap:
	cxl_pci_hdm_unmap(pdev, &regs, &map);
	return rc;
}

/*
 * CXL r4.0 sec 9.7.2 defines the reset completion timeout encodings.
 * Sec 9.7.3 leaves config-space access behavior undefined for 100 ms after
 * initiating CXL Reset, then limits software to CXL Status2 access until
 * reset completion, timeout, or error.
 */
#define CXL_RESET_RRS_WAIT_MS 100
#define CXL_RESET_STATUS_POLL_MS 20
static const u32 cxl_reset_timeout_ms[] = {
	10, 100, 1000, 10000, 100000,
};

#define CXL_CACHE_WBI_TIMEOUT_US 100000
#define CXL_CACHE_WBI_POLL_US 100

static int cxl_reset_dvsec(struct pci_dev *pdev)
{
	int dvsec, rc;
	u16 cap;

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec)
		return -ENOTTY;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CAP, &cap);
	if (rc)
		return pcibios_err_to_errno(rc);

	if ((cap & (PCI_DVSEC_CXL_CACHE_CAPABLE |
		    PCI_DVSEC_CXL_MEM_CAPABLE)) !=
	    (PCI_DVSEC_CXL_CACHE_CAPABLE | PCI_DVSEC_CXL_MEM_CAPABLE))
		return -ENOTTY;

	if (!(cap & PCI_DVSEC_CXL_RST_CAPABLE))
		return -ENOTTY;

	return dvsec;
}

static int cxl_reset_update_ctrl2(struct pci_dev *pdev, int dvsec, u16 set,
				  u16 clear)
{
	u16 ctrl2;
	int rc;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL2, &ctrl2);
	if (rc)
		return pcibios_err_to_errno(rc);

	ctrl2 |= set;
	ctrl2 &= ~clear;

	rc = pci_write_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL2, ctrl2);
	if (rc)
		return pcibios_err_to_errno(rc);

	return 0;
}

static int cxl_reset_enable_cache(struct pci_dev *pdev, int dvsec)
{
	return cxl_reset_update_ctrl2(pdev, dvsec, 0,
				      PCI_DVSEC_CXL_DISABLE_CACHING);
}

static int cxl_reset_disable_cache(struct pci_dev *pdev, int dvsec, u16 cap)
{
	int remaining_us = CXL_CACHE_WBI_TIMEOUT_US;
	u16 status2;
	int rc, rc2;

	rc = cxl_reset_update_ctrl2(pdev, dvsec,
				    PCI_DVSEC_CXL_DISABLE_CACHING, 0);
	if (rc)
		return rc;

	if (!(cap & PCI_DVSEC_CXL_CACHE_WBI_CAPABLE))
		return 0;

	rc = cxl_reset_update_ctrl2(pdev, dvsec,
				    PCI_DVSEC_CXL_INIT_CACHE_WBI, 0);
	if (rc)
		goto err_enable_cache;

	do {
		usleep_range(CXL_CACHE_WBI_POLL_US, CXL_CACHE_WBI_POLL_US + 1);
		remaining_us -= CXL_CACHE_WBI_POLL_US;

		rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_STATUS2,
					  &status2);
		if (rc) {
			rc = pcibios_err_to_errno(rc);
			goto err_enable_cache;
		}
	} while (!(status2 & PCI_DVSEC_CXL_CACHE_INV) && remaining_us > 0);

	if (!(status2 & PCI_DVSEC_CXL_CACHE_INV)) {
		rc = -ETIMEDOUT;
		goto err_enable_cache;
	}

	return 0;

err_enable_cache:
	/*
	 * DISABLE_CACHING can be rolled back here. INIT_CACHE_WBI is
	 * self-clearing on completion, so leave any in-flight writeback alone.
	 */
	rc2 = cxl_reset_enable_cache(pdev, dvsec);
	if (rc2)
		pci_warn(pdev, "failed to re-enable CXL caching: %d\n", rc2);
	return rc;
}

static int cxl_reset_wait_done(struct pci_dev *pdev, int dvsec, u16 cap)
{
	unsigned long deadline;
	u32 timeout_ms;
	u16 status2;
	int idx, rc;

	idx = FIELD_GET(PCI_DVSEC_CXL_RST_TIMEOUT, cap);
	if (idx >= ARRAY_SIZE(cxl_reset_timeout_ms)) {
		int last = ARRAY_SIZE(cxl_reset_timeout_ms) - 1;

		pci_warn(pdev,
			 "unknown CXL reset timeout encoding %d; using %u ms\n",
			 idx, cxl_reset_timeout_ms[last]);
		idx = last;
	}

	timeout_ms = max_t(u32, cxl_reset_timeout_ms[idx],
			   CXL_RESET_RRS_WAIT_MS);
	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	msleep(CXL_RESET_RRS_WAIT_MS);

	do {
		rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_STATUS2,
					  &status2);
		if (rc)
			return pcibios_err_to_errno(rc);

		if (status2 & PCI_DVSEC_CXL_RST_ERR)
			return -EIO;

		if (status2 & PCI_DVSEC_CXL_RST_DONE)
			return 0;

		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;

		msleep(CXL_RESET_STATUS_POLL_MS);
	} while (true);
}

static int cxl_reset_execute(struct pci_dev *pdev, int dvsec)
{
	bool cache_disabled = false;
	u16 cap;
	int rc;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CAP, &cap);
	if (rc)
		return pcibios_err_to_errno(rc);

	if (!pci_wait_for_pending_transaction(pdev))
		pci_err(pdev, "timed out waiting for pending transactions\n");

	rc = pci_dev_reset_iommu_prepare(pdev);
	if (rc) {
		pci_err(pdev, "failed to stop IOMMU for CXL reset: %d\n", rc);
		return rc;
	}

	rc = cxl_reset_disable_cache(pdev, dvsec, cap);
	if (rc)
		goto out;
	cache_disabled = true;

	rc = cxl_reset_update_ctrl2(pdev, dvsec, PCI_DVSEC_CXL_INIT_CXL_RST,
				    PCI_DVSEC_CXL_RST_MEM_CLR_EN);
	if (rc)
		goto out;

	rc = cxl_reset_wait_done(pdev, dvsec, cap);
	if (rc)
		goto out;

out:
	if (cache_disabled) {
		int rc2;

		rc2 = cxl_reset_enable_cache(pdev, dvsec);
		if (rc2 && rc)
			pci_warn(pdev, "failed to re-enable CXL caching: %d\n",
				 rc2);
		else if (rc2)
			rc = rc2;
	}

	pci_dev_reset_iommu_done(pdev);
	return rc;
}

int cxl_reset_function(struct pci_dev *pdev, bool probe)
{
	int dvsec;

	dvsec = cxl_reset_dvsec(pdev);
	if (dvsec < 0)
		return dvsec;

	if (probe)
		return 0;

	return cxl_reset_execute(pdev, dvsec);
}
