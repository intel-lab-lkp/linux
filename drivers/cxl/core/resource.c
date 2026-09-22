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
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/memregion.h>
#include <linux/overflow.h>
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

static int cxld_await_uncommit(void __iomem *hdm, int id)
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
		if (!FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
			return 0;
		fsleep(1000);
	}

	return -ETIMEDOUT;
}

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

int cxl_commit_wait(void __iomem *hdm, struct cxl_decoder_settings *settings)
{
	return cxld_await_commit(hdm, settings->config.id);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_commit_wait, "cxl_core");

int cxl_hdm_unpack_decoder(struct cxl_decoder_settings *settings, int id,
			   u32 ctrl, u64 base, u64 size,
			   u64 target_or_skip_reg_val)
{
	bool committed = FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl);
	enum cxl_decoder_type target_type;
	int interleave_granularity;
	int interleave_ways;
	unsigned long flags = 0;
	struct range hpa_range;
	int rc;

	if (!committed)
		size = 0;
	if (base == U64_MAX || size == U64_MAX ||
	    (size && base > U64_MAX - (size - 1)))
		return -ENXIO;

	hpa_range = (struct range) {
		.start = base,
		.end = base + size - 1,
	};
	target_type = FIELD_GET(CXL_HDM_DECODER0_CTRL_HOSTONLY, ctrl) ?
		      CXL_DECODER_HOSTONLYMEM : CXL_DECODER_DEVMEM;
	if (committed) {
		flags |= CXL_DECODER_F_ENABLE;
		if (ctrl & CXL_HDM_DECODER0_CTRL_LOCK)
			flags |= CXL_DECODER_F_LOCK;
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
		return -ENOTTY;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CAP, &cap);
	if (rc)
		return pcibios_err_to_errno(rc);
	if (!(cap & PCI_DVSEC_CXL_MEM_CAPABLE) ||
	    !FIELD_GET(PCI_DVSEC_CXL_HDM_COUNT, cap))
		return -ENOTTY;

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

static int __cxl_pci_hdm_read_info(struct pci_dev *pdev,
				   struct cxl_register_map *map, int dvsec,
				   struct cxl_hdm_info **out)
{
	struct cxl_reg_map *hdm_map = &map->component_map.hdm_decoder;
	struct cxl_hdm_info *info = NULL;
	resource_size_t hdm_offset;
	void __iomem *hdm;
	int decoder_count;
	size_t required;
	u16 dvsec_ctrl;
	int rc;

	rc = cxl_setup_regs(map);
	if (rc)
		return rc;
	if (!hdm_map->valid)
		return -ENODEV;
	if (hdm_map->offset > map->max_size ||
	    hdm_map->size > map->max_size - hdm_map->offset)
		return -ENXIO;
	if (check_add_overflow(map->bar_offset,
			       (resource_size_t)hdm_map->offset, &hdm_offset))
		return -EOVERFLOW;

	hdm = ioremap(map->resource + hdm_map->offset, hdm_map->size);
	if (!hdm)
		return -ENOMEM;

	decoder_count = cxl_hdm_decoder_count(readl(hdm + CXL_HDM_DECODER_CAP_OFFSET));
	if (decoder_count < 0) {
		rc = decoder_count;
		goto out_unmap;
	}

	required = CXL_HDM_DECODER0_TL_HIGH(decoder_count - 1) +
		   sizeof(u32);
	if (required > hdm_map->size) {
		pci_err(pdev,
			"CXL HDM decoder count exceeds mapped register block\n");
		rc = -ENXIO;
		goto out_unmap;
	}

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL,
				  &dvsec_ctrl);
	if (rc) {
		rc = pcibios_err_to_errno(rc);
		goto out_unmap;
	}

	info = kzalloc(struct_size(info, settings, decoder_count), GFP_KERNEL);
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
			goto out_unmap;
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
		goto out_unmap;
	}

	*out = info;
	info = NULL;
	rc = 0;

out_unmap:
	kfree(info);
	iounmap(hdm);
	return rc;
}

static int cxl_pci_hdm_read_info(struct pci_dev *pdev,
				 struct cxl_register_map *map, int dvsec,
				 struct cxl_hdm_info **out)
{
	bool restore_command = false;
	u16 command;
	int rc, rc2;

	guard(pci_dev)(pdev);

	rc = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (rc)
		return pcibios_err_to_errno(rc);

	if (!(command & PCI_COMMAND_MEMORY)) {
		rc = pci_write_config_word(pdev, PCI_COMMAND,
					   command | PCI_COMMAND_MEMORY);
		if (rc)
			return pcibios_err_to_errno(rc);
		restore_command = true;
	}

	rc = __cxl_pci_hdm_read_info(pdev, map, dvsec, out);
	if (!restore_command)
		return rc;

	if (!rc) {
		rc = pci_write_config_word(pdev, PCI_COMMAND, command);
		return pcibios_err_to_errno(rc);
	}

	rc2 = pci_write_config_word(pdev, PCI_COMMAND, command);
	if (rc2)
		pci_err(pdev,
			"failed to restore PCI_COMMAND after CXL HDM cache init: %d\n",
			pcibios_err_to_errno(rc2));

	return rc;
}

static int __pci_cxl_hdm_cache_init(struct pci_dev *pdev)
{
	struct cxl_hdm_info *info __free(kfree) = NULL;
	struct cxl_register_map map = { };
	struct resource *bar;
	int dvsec;
	int rc;

	scoped_guard(rwsem_read, &cxl_rwsem.dpa)
		if (pdev->hdm)
			return 0;

	dvsec = cxl_pci_hdm_dvsec(pdev);
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

	rc = cxl_pci_hdm_read_info(pdev, &map, dvsec, &info);
	if (rc)
		return rc;

	guard(rwsem_write)(&cxl_rwsem.dpa);
	if (!pdev->hdm)
		pdev->hdm = no_free_ptr(info);

	return 0;
}

void pci_cxl_hdm_cache_init(struct pci_dev *pdev)
{
	int rc;

	rc = __pci_cxl_hdm_cache_init(pdev);
	if (rc && rc != -ENOTTY && rc != -ENODEV)
		pci_dbg(pdev, "CXL HDM cache init failed: %d\n", rc);
}
EXPORT_SYMBOL_FOR_MODULES(pci_cxl_hdm_cache_init, "cxl_core");

void pci_cxl_hdm_cache_release(struct pci_dev *pdev)
{
	struct cxl_hdm_info *info;

	guard(rwsem_write)(&cxl_rwsem.dpa);
	info = pdev->hdm;
	pdev->hdm = NULL;
	kfree(info);
}

static void __iomem *cxl_pci_hdm_ioremap_current(struct pci_dev *pdev,
						 int bar,
						 resource_size_t offset,
						 resource_size_t size)
{
	struct resource *res;
	resource_size_t hdm_start;
	void __iomem *hdm;

	if (bar < 0 || bar >= PCI_STD_NUM_BARS || !size)
		return ERR_PTR(-EINVAL);

	res = &pdev->resource[bar];
	if (!pci_resource_len(pdev, bar) ||
	    (res->flags & (IORESOURCE_UNSET | IORESOURCE_DISABLED)) ||
	    resource_type(res) != IORESOURCE_MEM || !res->start || !res->end)
		return ERR_PTR(-ENODEV);
	if (offset > resource_size(res) || size > resource_size(res) - offset)
		return ERR_PTR(-ENODEV);
	if (check_add_overflow(res->start, offset, &hdm_start))
		return ERR_PTR(-EOVERFLOW);

	hdm = ioremap(hdm_start, size);
	if (!hdm) {
		pci_err(pdev, "failed to remap CXL HDM decoder registers\n");
		return ERR_PTR(-ENOMEM);
	}

	return hdm;
}

static int cxl_hdm_decoder_uncommit(struct pci_dev *pdev, void __iomem *hdm,
				    int id, bool *retained)
{
	u32 ctrl;
	int rc;

	*retained = false;
	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
	if (PCI_POSSIBLE_ERROR(ctrl))
		return -EIO;
	if (ctrl & CXL_HDM_DECODER0_CTRL_COMMIT_ERROR) {
		pci_err(pdev, "CXL HDM decoder %d has commit error set\n", id);
		return -EIO;
	}

	if (ctrl & CXL_HDM_DECODER0_CTRL_LOCK) {
		if (ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED) {
			*retained = true;
			return 0;
		}

		pci_err(pdev, "CXL HDM decoder %d is locked and uncommitted\n",
			id);
		return -EIO;
	}

	if (!(ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED))
		return 0;

	ctrl &= ~CXL_HDM_DECODER0_CTRL_COMMIT;
	writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));

	rc = cxld_await_uncommit(hdm, id);
	if (rc)
		pci_err(pdev, "CXL HDM decoder %d uncommit failed: %d\n", id,
			rc);

	return rc;
}

static bool cxl_hdm_decoder_settings_match(
	const struct cxl_decoder_settings *saved,
	const struct cxl_decoder_settings *active)
{
	const struct cxl_decoder_config *a = &saved->config;
	const struct cxl_decoder_config *b = &active->config;

	return a->id == b->id &&
	       a->hpa_range.start == b->hpa_range.start &&
	       a->hpa_range.end == b->hpa_range.end &&
	       a->interleave_ways == b->interleave_ways &&
	       a->interleave_granularity == b->interleave_granularity &&
	       a->target_type == b->target_type &&
	       (a->flags & CXL_DECODER_F_RESET_MASK) ==
		(b->flags & CXL_DECODER_F_RESET_MASK) &&
	       saved->target_or_skip_reg_val == active->target_or_skip_reg_val;
}

static int cxl_restore_hdm_decoder_commit(
	void __iomem *hdm, struct cxl_decoder_settings *settings)
{
	guard(rwsem_read)(&cxl_rwsem.dpa);

	cxl_commit_start(hdm, settings);
	return cxl_commit_wait(hdm, settings);
}

static int cxl_restore_hdm_decoder(struct pci_dev *pdev, void __iomem *hdm,
				   struct cxl_decoder_settings *settings)
{
	struct cxl_decoder_config *config = &settings->config;
	bool retained;
	u32 ctrl;
	int rc;

	rc = cxl_hdm_decoder_uncommit(pdev, hdm, config->id, &retained);
	if (rc)
		return rc;

	if (retained) {
		struct cxl_decoder_settings active;

		rc = cxl_pci_hdm_read_decoder(pdev, &active, hdm,
					      config->id);
		if (rc)
			return rc;
		if (!cxl_hdm_decoder_settings_match(settings, &active)) {
			pci_err(pdev,
				"CXL HDM decoder %d retained unexpected locked state\n",
				config->id);
			return -EIO;
		}
		return 0;
	}

	if (!(config->flags & CXL_DECODER_F_ENABLE))
		return 0;

	rc = cxl_restore_hdm_decoder_commit(hdm, settings);
	if (rc) {
		pci_err(pdev, "CXL HDM decoder %d restore failed: %d\n",
			config->id, rc);
		return rc;
	}

	if (!(config->flags & CXL_DECODER_F_LOCK))
		return 0;

	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(config->id));
	if (PCI_POSSIBLE_ERROR(ctrl))
		return -EIO;
	ctrl |= CXL_HDM_DECODER0_CTRL_LOCK;
	writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(config->id));

	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(config->id));
	if (PCI_POSSIBLE_ERROR(ctrl) ||
	    !(ctrl & CXL_HDM_DECODER0_CTRL_LOCK)) {
		pci_err(pdev, "CXL HDM decoder %d failed to restore lock\n",
			config->id);
		return -EIO;
	}

	return 0;
}

static int cxl_snapshot_hdm(struct pci_dev *pdev,
			    struct cxl_hdm_info **snapshot)
{
	struct cxl_hdm_info *info;
	size_t size;

	guard(rwsem_read)(&cxl_rwsem.dpa);
	info = pdev->hdm;
	if (!info)
		return -ENXIO;
	if (info->decoder_count <= 0)
		return -EINVAL;

	size = struct_size(info, settings, info->decoder_count);
	if (size == SIZE_MAX)
		return -EOVERFLOW;
	*snapshot = kmemdup(info, size, GFP_KERNEL);
	if (!*snapshot)
		return -ENOMEM;

	return 0;
}

static void cxl_restore_pci_state_for_hdm_restore(struct pci_dev *pdev,
						  u16 *command)
{
	u32 saved_config = pdev->saved_config_space[PCI_COMMAND / 4];
	u32 restore_config;

	device_lock_assert(&pdev->dev);
	*command = saved_config & 0xffff;

	/*
	 * pci_restore_state() is the available interface for restoring BARs and
	 * related PCI state needed to reach HDM MMIO. Temporarily alter its saved
	 * command value so that it enables MSE but cannot enable BME or INTx
	 * before CXL state restoration completes. Preserve the original saved
	 * value for the outer PCI reset path.
	 */
	restore_config = saved_config | PCI_COMMAND_MEMORY |
			 PCI_COMMAND_INTX_DISABLE;
	restore_config &= ~PCI_COMMAND_MASTER;

	pdev->saved_config_space[PCI_COMMAND / 4] = restore_config;
	pci_restore_state(pdev);
	pdev->saved_config_space[PCI_COMMAND / 4] = saved_config;
}

static int cxl_restore_dvsec_ctrl(struct pci_dev *pdev, u16 saved_ctrl)
{
	int dvsec;
	int rc;

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec)
		return -ENOTTY;

	rc = pci_write_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL,
				   saved_ctrl);
	if (rc)
		return pcibios_err_to_errno(rc);

	return 0;
}

static int cxl_restore_state(struct pci_dev *pdev,
			     struct cxl_hdm_info *snapshot)
{
	void __iomem *hdm;
	int count;
	int rc;

	hdm = cxl_pci_hdm_ioremap_current(pdev, snapshot->hdm_bar,
					  snapshot->hdm_offset,
					  snapshot->hdm_size);
	if (IS_ERR(hdm)) {
		pci_err(pdev, "failed to map CXL HDM state for restore: %ld\n",
			PTR_ERR(hdm));
		return PTR_ERR(hdm);
	}

	count = cxl_hdm_decoder_count(readl(hdm + CXL_HDM_DECODER_CAP_OFFSET));
	if (count != snapshot->decoder_count) {
		pci_err(pdev, "CXL HDM decoder count changed from %d to %d\n",
			snapshot->decoder_count, count);
		rc = -ENXIO;
		goto out_unmap;
	}

	/* Restore global HDM control before committing individual decoders. */
	writel(snapshot->global_ctrl, hdm + CXL_HDM_DECODER_CTRL_OFFSET);

	for (int i = 0; i < snapshot->decoder_count; i++) {
		rc = cxl_restore_hdm_decoder(pdev, hdm,
					     &snapshot->settings[i]);
		if (rc)
			goto out_unmap;
	}

	/* Flush posted HDM writes before restoring protocol enables. */
	readl(hdm + CXL_HDM_DECODER_CTRL_OFFSET);

	if (!snapshot->dvsec_ctrl_valid) {
		pci_err(pdev, "CXL DVSEC control snapshot is invalid\n");
		rc = -ENXIO;
		goto out_unmap;
	}
	rc = cxl_restore_dvsec_ctrl(pdev, snapshot->dvsec_ctrl);
	if (rc)
		pci_err(pdev, "failed to restore CXL DVSEC control: %d\n", rc);

out_unmap:
	iounmap(hdm);
	return rc;
}

static void cxl_reset_save_disabled_state(struct pci_dev *pdev)
{
	u32 *saved_command = &pdev->saved_config_space[PCI_COMMAND / 4];
	int rc;

	rc = pci_write_config_word(pdev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE);
	if (rc)
		pci_err(pdev,
			"failed to disable device after CXL reset failure: %d\n",
			pcibios_err_to_errno(rc));

	rc = pci_save_state(pdev);
	if (rc)
		pci_err(pdev,
			"failed to save disabled state after CXL reset failure: %d\n",
			rc);

	/* Ensure the outer PCI restore cannot re-enable MSE or BME. */
	*saved_command &= GENMASK(31, 16);
	*saved_command |= PCI_COMMAND_INTX_DISABLE;
}

static int cxl_reset_save_restored_state(struct pci_dev *pdev, u16 command)
{
	int rc;

	rc = pci_write_config_word(pdev, PCI_COMMAND, command);
	if (rc) {
		rc = pcibios_err_to_errno(rc);
		pci_err(pdev, "failed to restore PCI command state: %d\n", rc);
		cxl_reset_save_disabled_state(pdev);
		return rc;
	}

	rc = pci_save_state(pdev);
	if (rc) {
		pci_err(pdev, "failed to save restored state after CXL reset: %d\n",
			rc);
		cxl_reset_save_disabled_state(pdev);
	}

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

struct cxl_hdm_range {
	struct list_head list;
	struct pci_dev *pdev;
	struct range hpa_range;
	u64 len;
	struct resource *res;
};

struct cxl_hdm_range_context {
	struct list_head ranges;
};

static void cxl_hdm_range_context_destroy(struct cxl_hdm_range_context *ctx)
{
	struct cxl_hdm_range *range, *next;

	list_for_each_entry_safe(range, next, &ctx->ranges, list) {
		list_del(&range->list);
		if (range->res)
			release_mem_region(range->hpa_range.start,
					   resource_size(range->res));
		kfree(range);
	}
}

/*
 * Bound the range twice: request_mem_region() takes resource_size_t while
 * cpu_cache_invalidate_memregion() takes size_t, and the two differ on
 * 32-bit builds with CONFIG_PHYS_ADDR_T_64BIT. range_len() can also reach
 * RESOURCE_SIZE_MAX + 1 for a full-width range, and wraps to zero when
 * resource_size_t is 64-bit, which the !len test catches.
 */
static int cxl_hdm_range_validate(struct pci_dev *pdev,
				  const struct range *hpa_range)
{
	u64 len = range_len(hpa_range);

	if (!len)
		return -EINVAL;

	if (hpa_range->end > RESOURCE_SIZE_MAX) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds resource address size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	if (len > RESOURCE_SIZE_MAX) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds resource size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	if (len > SIZE_MAX) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds cache flush size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	return 0;
}

static int cxl_hdm_range_add(struct cxl_hdm_range_context *ctx,
			     struct pci_dev *pdev, const struct range *hpa_range)
{
	struct cxl_hdm_range *range;
	int rc;

	rc = cxl_hdm_range_validate(pdev, hpa_range);
	if (rc)
		return rc;

	list_for_each_entry(range, &ctx->ranges, list)
		if (range->hpa_range.start == hpa_range->start &&
		    range->hpa_range.end == hpa_range->end)
			return 0;

	range = kzalloc_obj(*range);
	if (!range)
		return -ENOMEM;

	range->pdev = pdev;
	range->hpa_range = *hpa_range;
	range->len = range_len(hpa_range);
	list_add_tail(&range->list, &ctx->ranges);

	return 0;
}

static int cxl_hdm_ranges_collect(struct cxl_hdm_range_context *ctx,
				  struct pci_dev *pdev)
{
	struct cxl_hdm_info *info;
	int rc;

	guard(rwsem_read)(&cxl_rwsem.dpa);
	info = pdev->hdm;
	if (!info) {
		pci_err(pdev, "CXL HDM decoder state unavailable\n");
		return -ENXIO;
	}

	for (int i = 0; i < info->decoder_count; i++) {
		struct cxl_decoder_config *config = &info->settings[i].config;

		if (!(config->flags & CXL_DECODER_F_ENABLE))
			continue;

		/* A committed zero-size decoder maps no HPA. */
		if (!range_len(&config->hpa_range))
			continue;

		if (config->flags & CXL_DECODER_F_NORMALIZED_ADDRESSING) {
			pci_err(pdev,
				"CXL reset does not support normalized address decoders\n");
			return -EOPNOTSUPP;
		}

		rc = cxl_hdm_range_add(ctx, pdev, &config->hpa_range);
		if (rc)
			return rc;
	}

	return 0;
}

static int cxl_hdm_ranges_request(struct cxl_hdm_range_context *ctx)
{
	struct cxl_hdm_range *range;

	lockdep_assert_held_write(&cxl_rwsem.region);

	list_for_each_entry(range, &ctx->ranges, list) {
		const struct range *hpa_range = &range->hpa_range;

		range->res = request_mem_region(hpa_range->start, range->len,
						"cxl_reset");
		if (!range->res) {
			pci_err(range->pdev,
				"cannot reset while CXL memory range is busy [%#llx-%#llx]\n",
				hpa_range->start, hpa_range->end);
			return -EBUSY;
		}
	}

	return 0;
}

static int cxl_hdm_ranges_invalidate(struct cxl_hdm_range_context *ctx)
{
	struct cxl_hdm_range *range;
	int rc = 0;

	list_for_each_entry(range, &ctx->ranges, list) {
		const struct range *hpa_range = &range->hpa_range;
		int rc2;

		rc2 = cpu_cache_invalidate_memregion(hpa_range->start, range->len);
		if (rc2)
			pci_err(range->pdev,
				"failed to invalidate CPU cache [%#llx-%#llx]: %d\n",
				hpa_range->start, hpa_range->end, rc2);
		rc = rc ?: rc2;
	}

	return rc;
}

static int cxl_hdm_ranges_prepare(struct cxl_hdm_range_context *ctx,
				  struct pci_dev *pdev)
{
	int rc;

	lockdep_assert_held_write(&cxl_rwsem.region);

	if (!cpu_cache_has_invalidate_memregion()) {
		pci_err(pdev, "CPU cache invalidation unavailable\n");
		return -ENXIO;
	}

	rc = cxl_hdm_ranges_collect(ctx, pdev);
	if (rc)
		return rc;

	rc = cxl_hdm_ranges_request(ctx);
	if (rc)
		return rc;

	return cxl_hdm_ranges_invalidate(ctx);
}

static int cxl_hdm_ranges_finish(struct cxl_hdm_range_context *ctx)
{
	int rc;

	lockdep_assert_held_write(&cxl_rwsem.region);

	rc = cxl_hdm_ranges_invalidate(ctx);
	cxl_hdm_range_context_destroy(ctx);

	return rc;
}

#define CXL_RESET_CTRL2_CMD_MASK \
	(PCI_DVSEC_CXL_INIT_CACHE_WBI | PCI_DVSEC_CXL_INIT_CXL_RST)

static int cxl_reset_update_ctrl2_no_replay(struct pci_dev *pdev, int dvsec,
					    u16 set, u16 clear)
{
	u16 ctrl2;
	int rc;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL2, &ctrl2);
	if (rc)
		return pcibios_err_to_errno(rc);
	if (PCI_POSSIBLE_ERROR(ctrl2))
		return -ENODEV;

	/*
	 * INIT_CACHE_WBI and INIT_CXL_RST are commands. Do not replay a command
	 * that remains set in the value read back while updating persistent
	 * Control2 state.
	 */
	ctrl2 &= ~CXL_RESET_CTRL2_CMD_MASK;
	ctrl2 &= ~clear;
	ctrl2 |= set;

	rc = pci_write_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL2, ctrl2);
	if (rc)
		return pcibios_err_to_errno(rc);

	return 0;
}

static int cxl_reset_wait_cache_wbi(struct pci_dev *pdev, int dvsec)
{
	unsigned long deadline;
	u16 status2;
	int rc;

	rc = cxl_reset_update_ctrl2_no_replay(pdev, dvsec,
					      PCI_DVSEC_CXL_INIT_CACHE_WBI, 0);
	if (rc)
		return rc;

	deadline = jiffies + usecs_to_jiffies(CXL_CACHE_WBI_TIMEOUT_US);
	while (time_before(jiffies, deadline)) {
		usleep_range(CXL_CACHE_WBI_POLL_US,
			     CXL_CACHE_WBI_POLL_US * 2);

		rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_STATUS2,
					  &status2);
		if (rc)
			return pcibios_err_to_errno(rc);
		if (PCI_POSSIBLE_ERROR(status2))
			return -ENODEV;
		if (status2 & PCI_DVSEC_CXL_CACHE_INV)
			return 0;
	}

	/* Do not miss completion concurrent with the polling deadline. */
	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_STATUS2,
				  &status2);
	if (rc)
		return pcibios_err_to_errno(rc);
	if (PCI_POSSIBLE_ERROR(status2))
		return -ENODEV;
	if (status2 & PCI_DVSEC_CXL_CACHE_INV)
		return 0;

	return -ETIMEDOUT;
}

static int cxl_reset_disable_cache(struct pci_dev *pdev, int dvsec, u16 cap)
{
	int rc;

	rc = cxl_reset_update_ctrl2_no_replay(pdev, dvsec,
					      PCI_DVSEC_CXL_DISABLE_CACHING, 0);
	if (rc || !(cap & PCI_DVSEC_CXL_CACHE_WBI_CAPABLE))
		return rc;

	return cxl_reset_wait_cache_wbi(pdev, dvsec);
}

static int cxl_reset_restore_cache_policy(struct pci_dev *pdev, int dvsec)
{
	int rc;

	rc = cxl_reset_update_ctrl2_no_replay(pdev, dvsec, 0,
					      PCI_DVSEC_CXL_DISABLE_CACHING);
	if (rc)
		pci_err(pdev, "failed to re-enable CXL caching: %d\n", rc);

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
	msleep(CXL_RESET_RRS_WAIT_MS);
	deadline = jiffies + msecs_to_jiffies(timeout_ms -
					      CXL_RESET_RRS_WAIT_MS);

	for (;;) {
		rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_STATUS2,
					  &status2);
		if (rc || status2 == U16_MAX)
			goto not_ready;
		if (status2 & PCI_DVSEC_CXL_RST_ERR)
			return -EIO;
		if (status2 & PCI_DVSEC_CXL_RST_DONE)
			return 0;

not_ready:
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;

		msleep(CXL_RESET_STATUS_POLL_MS);
	}
}

static int cxl_reset_execute(struct pci_dev *pdev, int dvsec, u16 cap)
{
	struct cxl_hdm_info *snapshot = NULL;
	struct cxl_hdm_range_context range_ctx;
	u16 command;
	int rc, rc2;

	down_write(&cxl_rwsem.region);
	INIT_LIST_HEAD(&range_ctx.ranges);

	rc = cxl_hdm_ranges_prepare(&range_ctx, pdev);
	if (rc)
		goto out_destroy_ranges;

	rc = cxl_snapshot_hdm(pdev, &snapshot);
	if (rc)
		goto out_destroy_ranges;

	rc = cxl_reset_disable_cache(pdev, dvsec, cap);
	if (rc)
		goto out_destroy_enable_cache;

	if (!pci_wait_for_pending_transaction(pdev)) {
		pci_err(pdev, "timed out waiting for pending transactions\n");
		rc = -ETIMEDOUT;
		goto out_destroy_enable_cache;
	}

	rc = pci_dev_reset_iommu_prepare(pdev);
	if (rc) {
		pci_err(pdev, "failed to stop IOMMU for CXL reset: %d\n", rc);
		goto out_destroy_enable_cache;
	}

	/* Clear Memory Clear below even if this command write reports failure. */
	rc = cxl_reset_update_ctrl2_no_replay(pdev, dvsec,
					      PCI_DVSEC_CXL_INIT_CXL_RST |
					      PCI_DVSEC_CXL_RST_MEM_CLR_EN, 0);
	if (rc)
		goto out_clear_memory;

	rc = cxl_reset_wait_done(pdev, dvsec, cap);

out_clear_memory:
	rc2 = cxl_reset_update_ctrl2_no_replay(pdev, dvsec, 0,
					       PCI_DVSEC_CXL_RST_MEM_CLR_EN);
	if (rc2)
		pci_err(pdev, "failed to clear CXL Reset Memory Clear: %d\n",
			 rc2);
	rc = rc ?: rc2;
	if (rc)
		goto out_finish_ranges;

	cxl_restore_pci_state_for_hdm_restore(pdev, &command);
	rc = cxl_restore_state(pdev, snapshot);

out_finish_ranges:
	/* Evict lines fetched during reset before ending DMA exclusion. */
	rc2 = cxl_hdm_ranges_finish(&range_ctx);
	rc = rc ?: rc2;
	if (rc)
		goto out_fail_closed;

	pci_dev_reset_iommu_done(pdev);

	rc = cxl_reset_restore_cache_policy(pdev, dvsec);
	if (rc) {
		cxl_reset_save_disabled_state(pdev);
		goto out_unlock;
	}

	rc = cxl_reset_save_restored_state(pdev, command);
	goto out_unlock;

out_fail_closed:
	cxl_reset_save_disabled_state(pdev);
	pci_dev_reset_iommu_done(pdev);
	rc2 = cxl_reset_restore_cache_policy(pdev, dvsec);
	rc = rc ?: rc2;
	goto out_unlock;

out_destroy_enable_cache:
	cxl_hdm_range_context_destroy(&range_ctx);
	rc2 = cxl_reset_restore_cache_policy(pdev, dvsec);
	rc = rc ?: rc2;
	goto out_unlock;

out_destroy_ranges:
	cxl_hdm_range_context_destroy(&range_ctx);

out_unlock:
	kfree(snapshot);
	up_write(&cxl_rwsem.region);
	return rc;
}

int cxl_reset_function(struct pci_dev *pdev, bool probe)
{
	int dvsec, rc;
	u16 cap, ctrl;

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec)
		return -ENOTTY;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CAP, &cap);
	if (rc)
		return pcibios_err_to_errno(rc);
	if (PCI_POSSIBLE_ERROR(cap))
		return -ENODEV;
	if (!(cap & PCI_DVSEC_CXL_CACHE_CAPABLE) ||
	    !(cap & PCI_DVSEC_CXL_MEM_CAPABLE) ||
	    !(cap & PCI_DVSEC_CXL_RST_CAPABLE) ||
	    !(cap & PCI_DVSEC_CXL_RST_MEM_CLR_CAPABLE))
		return -ENOTTY;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL, &ctrl);
	if (rc)
		return pcibios_err_to_errno(rc);
	if (PCI_POSSIBLE_ERROR(ctrl))
		return -ENODEV;
	if (!(ctrl & PCI_DVSEC_CXL_CACHE_ENABLE) ||
	    !(ctrl & PCI_DVSEC_CXL_MEM_ENABLE))
		return -ENOTTY;

	if (pdev->multifunction || pci_num_vf(pdev))
		return -ENOTTY;

	if (probe)
		return 0;

	/* The cache is owned by @pdev and does not require a bound CXL driver. */
	scoped_guard(rwsem_read, &cxl_rwsem.dpa)
		if (!pdev->hdm || !pdev->hdm->hdm_size)
			return -ENOTTY;

	if (!cpu_cache_has_invalidate_memregion())
		return -ENOTTY;

	return cxl_reset_execute(pdev, dvsec, cap);
}
