// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 NVIDIA Corporation. All rights reserved. */
#include <linux/delay.h>
#include <linux/bug.h>
#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/ioport.h>
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
