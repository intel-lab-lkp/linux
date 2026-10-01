// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 NVIDIA Corporation & Affiliates */
#include <linux/delay.h>
#include <linux/bug.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include <cxl/pci.h>

#include "cxl.h"
#include "core.h"

struct cxl_rwsem cxl_rwsem = {
	.region = __RWSEM_INITIALIZER(cxl_rwsem.region),
	.dpa = __RWSEM_INITIALIZER(cxl_rwsem.dpa),
};
EXPORT_SYMBOL_FOR_MODULES(cxl_rwsem, "cxl_core");

static void cxld_set_interleave(struct cxl_decoder_config *config, u32 *ctrl)
{
	u16 eig;
	u8 eiw;

	/*
	 * Input validation ensures these warns never fire, but otherwise
	 * suppress unititalized variable usage warnings.
	 */
	if (WARN_ONCE(ways_to_eiw(config->interleave_ways, &eiw),
		      "invalid interleave_ways: %d\n", config->interleave_ways))
		return;
	if (WARN_ONCE(granularity_to_eig(config->interleave_granularity, &eig),
		      "invalid interleave_granularity: %d\n",
		      config->interleave_granularity))
		return;

	u32p_replace_bits(ctrl, eig, CXL_HDM_DECODER0_CTRL_IG_MASK);
	u32p_replace_bits(ctrl, eiw, CXL_HDM_DECODER0_CTRL_IW_MASK);
	*ctrl |= CXL_HDM_DECODER0_CTRL_COMMIT;
}

static void cxld_set_type(struct cxl_decoder_config *config, u32 *ctrl)
{
	u32p_replace_bits(ctrl,
			  !!(config->target_type == CXL_DECODER_HOSTONLYMEM),
			  CXL_HDM_DECODER0_CTRL_HOSTONLY);
}

/*
 * Per CXL 2.0 8.2.5.12.20 Committing Decoder Programming, hardware must set
 * committed or error within 10ms, but just be generous with 20ms to account for
 * clock skew and other marginal behavior
 */
#define COMMIT_TIMEOUT_MS 20
int cxld_await_commit(void __iomem *hdm, int id)
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
EXPORT_SYMBOL_FOR_MODULES(cxld_await_commit, "cxl_core");

static void setup_hw_decoder(void __iomem *hdm,
			     struct cxl_decoder_settings *settings)
{
	struct cxl_decoder_config *config = &settings->config;
	int id = config->id;
	u64 target_or_skip_reg_val;
	u64 base, size;
	u32 ctrl;

	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
	cxld_set_interleave(config, &ctrl);
	cxld_set_type(config, &ctrl);
	base = config->hpa_range.start;
	size = range_len(&config->hpa_range);
	target_or_skip_reg_val = settings->target_or_skip_reg_val;

	writel(upper_32_bits(base), hdm + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(id));
	writel(lower_32_bits(base), hdm + CXL_HDM_DECODER0_BASE_LOW_OFFSET(id));
	writel(upper_32_bits(size), hdm + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(id));
	writel(lower_32_bits(size), hdm + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(id));
	/* Target-list and endpoint-skip registers alias the same slot. */
	writel(upper_32_bits(target_or_skip_reg_val), hdm + CXL_HDM_DECODER0_TL_HIGH(id));
	writel(lower_32_bits(target_or_skip_reg_val), hdm + CXL_HDM_DECODER0_TL_LOW(id));

	writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
}

void cxl_commit_start(void __iomem *hdm, struct cxl_decoder_settings *settings)
{
	lockdep_assert_held(&cxl_rwsem.dpa);
	setup_hw_decoder(hdm, settings);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_commit_start, "cxl_core");

int cxl_hdm_unpack_decoder(struct cxl_decoder_settings *settings, int id,
			   u32 ctrl, u64 base, u64 size,
			   u64 target_or_skip_reg_val)
{
	bool committed = FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl);
	enum cxl_decoder_type target_type = 0;
	int interleave_granularity;
	int interleave_ways;
	unsigned long flags = 0;
	struct range hpa_range;
	u64 end;
	int rc;

	if (!committed)
		size = 0;
	if (base == U64_MAX || size == U64_MAX)
		return -ENXIO;
	if (size && check_add_overflow(base, size - 1, &end))
		return -ENXIO;
	if (!size)
		end = base - 1;

	hpa_range = (struct range) {
		.start = base,
		.end = end,
	};

	if (committed) {
		flags |= CXL_DECODER_F_ENABLE;
		if (ctrl & CXL_HDM_DECODER0_CTRL_LOCK)
			flags |= CXL_DECODER_F_LOCK;
		if (FIELD_GET(CXL_HDM_DECODER0_CTRL_HOSTONLY, ctrl))
			target_type = CXL_DECODER_HOSTONLYMEM;
		else
			target_type = CXL_DECODER_DEVMEM;
	}

	rc = eiw_to_ways(FIELD_GET(CXL_HDM_DECODER0_CTRL_IW_MASK, ctrl),
			 &interleave_ways);
	if (rc)
		return rc;
	rc = eig_to_granularity(FIELD_GET(CXL_HDM_DECODER0_CTRL_IG_MASK, ctrl),
				&interleave_granularity);
	if (rc)
		return rc;

	*settings = (struct cxl_decoder_settings) {
		.config = {
			.id = id,
			.hpa_range = hpa_range,
			.interleave_ways = interleave_ways,
			.interleave_granularity = interleave_granularity,
			.target_type = target_type,
			.flags = flags,
		},
		.target_or_skip_reg_val = target_or_skip_reg_val,
	};

	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_hdm_unpack_decoder, "cxl_core");

static int cxl_pci_hdm_dvsec(struct pci_dev *pdev)
{
	u16 cap;
	int dvsec;
	int rc;

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec)
		return -ENXIO;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CAP, &cap);
	if (rc)
		return pcibios_err_to_errno(rc);
	if (!(cap & PCI_DVSEC_CXL_MEM_CAPABLE) ||
	    !FIELD_GET(PCI_DVSEC_CXL_HDM_COUNT, cap))
		return -ENXIO;

	return dvsec;
}

static int cxl_pci_hdm_read_decoder(struct pci_dev *pdev,
				    struct cxl_decoder_settings *settings,
				    void __iomem *hdm, int id)
{
	u64 target_or_skip, base, size;
	u32 ctrl;
	int rc;

	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
	base = ((u64)readl(hdm + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(id)) << 32) |
	       readl(hdm + CXL_HDM_DECODER0_BASE_LOW_OFFSET(id));
	size = ((u64)readl(hdm + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(id)) << 32) |
	       readl(hdm + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(id));
	target_or_skip =
		((u64)readl(hdm + CXL_HDM_DECODER0_TL_HIGH(id)) << 32) |
		readl(hdm + CXL_HDM_DECODER0_TL_LOW(id));

	rc = cxl_hdm_unpack_decoder(settings, id, ctrl, base, size,
				    target_or_skip);
	if (rc) {
		pci_err(pdev,
			"CXL HDM decoder %d has invalid configuration: %d\n",
			id, rc);
		return rc;
	}

	if (!(settings->config.flags & CXL_DECODER_F_ENABLE))
		*settings = (struct cxl_decoder_settings) {
			.config.id = id,
		};

	return 0;
}

static void __iomem *cxl_pci_hdm_map_regs(struct pci_dev *pdev,
					  struct cxl_register_map *map,
					  resource_size_t *hdm_offset,
					  int *decoder_count)
{
	struct cxl_reg_map *hdm_map = &map->component_map.hdm_decoder;
	void __iomem *hdm;
	size_t required;
	int rc;

	/* cxl_setup_regs() populates hdm_map, including its valid bit. */
	rc = cxl_setup_regs(map);
	if (rc)
		return ERR_PTR(rc);
	if (!hdm_map->valid)
		return ERR_PTR(-ENODEV);
	if (hdm_map->offset > map->max_size ||
	    hdm_map->size > map->max_size - hdm_map->offset)
		return ERR_PTR(-ENXIO);
	if (check_add_overflow(map->bar_offset,
			       (resource_size_t)hdm_map->offset, hdm_offset))
		return ERR_PTR(-EOVERFLOW);

	hdm = ioremap(map->resource + hdm_map->offset, hdm_map->size);
	if (!hdm)
		return ERR_PTR(-ENOMEM);

	*decoder_count =
		cxl_hdm_decoder_count(readl(hdm + CXL_HDM_DECODER_CAP_OFFSET));
	if (*decoder_count < 0) {
		iounmap(hdm);
		return ERR_PTR(*decoder_count);
	}

	/* The last decoder's target-list high register must fit in the mapping. */
	required = CXL_HDM_DECODER0_TL_HIGH(*decoder_count - 1) + sizeof(u32);
	if (required > hdm_map->size) {
		pci_err(pdev,
			"CXL HDM decoder count exceeds mapped register block\n");
		iounmap(hdm);
		return ERR_PTR(-ENXIO);
	}

	return hdm;
}

static struct cxl_hdm_info *__cxl_pci_hdm_read_info(struct pci_dev *pdev,
						    struct cxl_register_map *map,
						    int dvsec)
{
	struct cxl_reg_map *hdm_map = &map->component_map.hdm_decoder;
	resource_size_t hdm_offset;
	void __iomem *hdm;
	int decoder_count;
	u16 dvsec_ctrl;
	int rc;

	hdm = cxl_pci_hdm_map_regs(pdev, map, &hdm_offset, &decoder_count);
	if (IS_ERR(hdm))
		return ERR_CAST(hdm);

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL,
				  &dvsec_ctrl);
	if (rc) {
		rc = pcibios_err_to_errno(rc);
		goto out_unmap;
	}

	struct cxl_hdm_info *info =
		kzalloc_flex(*info, settings, decoder_count, GFP_KERNEL);
	if (!info) {
		rc = -ENOMEM;
		goto out_unmap;
	}

	info->decoder_count = decoder_count;
	info->hdm_bar = map->bar;
	info->hdm_offset = hdm_offset;
	info->hdm_size = hdm_map->size;
	info->global_ctrl = readl(hdm + CXL_HDM_DECODER_CTRL_OFFSET);
	info->dvsec_ctrl = dvsec_ctrl;
	info->dvsec_ctrl_valid = true;

	for (int i = 0; i < decoder_count; i++) {
		rc = cxl_pci_hdm_read_decoder(pdev, &info->settings[i], hdm, i);
		if (rc)
			goto out_free;
	}

	/*
	 * A changed decoder count means the snapshot no longer describes the
	 * complete hardware state. Reject it rather than publish a partial cache.
	 */
	rc = cxl_hdm_decoder_count(readl(hdm + CXL_HDM_DECODER_CAP_OFFSET));
	if (rc != decoder_count) {
		pci_err(pdev, "CXL HDM decoder count changed from %d to %d\n",
			decoder_count, rc);
		rc = -ENXIO;
		goto out_free;
	}

	iounmap(hdm);
	return info;

out_free:
	kfree(info);
out_unmap:
	iounmap(hdm);
	return ERR_PTR(rc);
}

static struct cxl_hdm_info *cxl_pci_hdm_read_info(struct pci_dev *pdev,
						  struct cxl_register_map *map,
						  int dvsec)
{
	struct cxl_hdm_info *info;
	u16 command;
	int rc;

	guard(pci_dev)(pdev);

	rc = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (rc)
		return ERR_PTR(pcibios_err_to_errno(rc));

	if (command & PCI_COMMAND_MEMORY)
		return __cxl_pci_hdm_read_info(pdev, map, dvsec);

	rc = pci_write_config_word(pdev, PCI_COMMAND,
				   command | PCI_COMMAND_MEMORY);
	if (rc)
		return ERR_PTR(pcibios_err_to_errno(rc));

	info = __cxl_pci_hdm_read_info(pdev, map, dvsec);
	rc = pci_write_config_word(pdev, PCI_COMMAND, command);
	if (!rc)
		return info;

	if (IS_ERR(info)) {
		pci_err(pdev,
			"failed to restore PCI_COMMAND after CXL HDM cache init: %d\n",
			pcibios_err_to_errno(rc));
		return info;
	}

	kfree(info);
	return ERR_PTR(pcibios_err_to_errno(rc));
}

static int __pci_cxl_hdm_cache_init(struct pci_dev *pdev)
{
	struct cxl_register_map map = { };
	struct resource *bar;
	int dvsec;
	int rc;

	/* Avoid duplicate MMIO reads when the cache is already present. */
	scoped_guard(rwsem_read, &cxl_rwsem.dpa)
		if (pdev->hdm)
			return 0;

	dvsec = cxl_pci_hdm_dvsec(pdev);
	if (dvsec == -ENXIO)
		return 0;
	if (dvsec < 0)
		return dvsec;

	rc = cxl_find_regblock(pdev, CXL_REGLOC_RBI_COMPONENT, &map);
	if (rc)
		return rc;
	if (map.bar < 0 || map.bar >= PCI_STD_NUM_BARS)
		return -ENODEV;

	bar = &pdev->resource[map.bar];
	if (!pci_resource_len(pdev, map.bar) ||
	    (bar->flags & (IORESOURCE_UNSET | IORESOURCE_DISABLED)) ||
	    resource_type(bar) != IORESOURCE_MEM || !bar->start || !bar->end)
		return -ENODEV;

	struct cxl_hdm_info *read_info =
		cxl_pci_hdm_read_info(pdev, &map, dvsec);
	if (IS_ERR(read_info))
		return PTR_ERR(read_info);

	struct cxl_hdm_info *info __free(kfree) = read_info;
	guard(rwsem_write)(&cxl_rwsem.dpa);
	/* Another initializer may have published while we read MMIO. */
	if (!pdev->hdm)
		pdev->hdm = no_free_ptr(info);

	return 0;
}

void pci_cxl_hdm_cache_init(struct pci_dev *pdev)
{
	int rc;

	rc = __pci_cxl_hdm_cache_init(pdev);
	if (rc)
		pci_dbg(pdev, "CXL HDM cache init failed: %d\n", rc);
}
EXPORT_SYMBOL_FOR_MODULES(pci_cxl_hdm_cache_init, "cxl_core");

void pci_cxl_hdm_cache_release(struct pci_dev *pdev)
{
	struct cxl_hdm_info *info;

	guard(rwsem_write)(&cxl_rwsem.dpa);
	info = pdev->hdm;
	/* Unpublish before freeing so subsequent readers cannot use stale state. */
	pdev->hdm = NULL;
	kfree(info);
}
