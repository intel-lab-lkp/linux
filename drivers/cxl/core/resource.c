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
	int rc, rc2;

	rc = cxl_reset_disable_cache(pdev, dvsec, cap);
	if (rc)
		goto out_enable_cache;

	if (!pci_wait_for_pending_transaction(pdev)) {
		pci_err(pdev, "timed out waiting for pending transactions\n");
		rc = -ETIMEDOUT;
		goto out_enable_cache;
	}

	rc = pci_dev_reset_iommu_prepare(pdev);
	if (rc) {
		pci_err(pdev, "failed to stop IOMMU for CXL reset: %d\n", rc);
		goto out_enable_cache;
	}

	/* Clear Memory Clear again even if this command write reports failure. */
	rc = cxl_reset_update_ctrl2_no_replay(pdev, dvsec,
					      PCI_DVSEC_CXL_INIT_CXL_RST |
					      PCI_DVSEC_CXL_RST_MEM_CLR_EN, 0);
	if (rc)
		goto out_clear_memory;

	rc = cxl_reset_wait_done(pdev, dvsec, cap);

out_clear_memory:
	rc2 = cxl_reset_update_ctrl2_no_replay(
		pdev, dvsec, 0, PCI_DVSEC_CXL_RST_MEM_CLR_EN);
	if (rc2)
		pci_err(pdev, "failed to clear CXL Reset Memory Clear: %d\n",
			rc2);
	rc = rc ?: rc2;

	pci_dev_reset_iommu_done(pdev);

out_enable_cache:
	/*
	 * DISABLE_CACHING was the first preparation step. Restore the original
	 * cache policy last, after reset exclusion has ended.
	 */
	rc2 = cxl_reset_update_ctrl2_no_replay(
		pdev, dvsec, 0, PCI_DVSEC_CXL_DISABLE_CACHING);
	if (rc2)
		pci_err(pdev, "failed to re-enable CXL caching: %d\n", rc2);
	rc = rc ?: rc2;

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

	if (probe)
		return 0;

	return cxl_reset_execute(pdev, dvsec, cap);
}
