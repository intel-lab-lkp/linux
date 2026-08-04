// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 NVIDIA Corporation & Affiliates */
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
#include <linux/list.h>
#include <linux/memregion.h>
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

static int setup_hw_decoder(struct cxl_decoder_settings *settings,
			    void __iomem *hdm)
{
	int id = settings->id;
	u64 target_or_skip;
	u64 base, size;
	u32 ctrl;

	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
	if (ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED)
		return -EBUSY;
	if (ctrl & CXL_HDM_DECODER0_CTRL_COMMIT)
		return -ETIMEDOUT;
	if (ctrl & CXL_HDM_DECODER0_CTRL_COMMIT_ERROR)
		return -EIO;
	cxld_set_interleave(settings, &ctrl);
	cxld_set_type(settings, &ctrl);
	base = settings->hpa_range.start;
	size = range_len(&settings->hpa_range);
	target_or_skip = settings->target_or_skip;

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

	return 0;
}

int cxl_commit_start(struct cxl_decoder_settings *settings, void __iomem *hdm)
{
	lockdep_assert_held(&cxl_rwsem.dpa);
	return setup_hw_decoder(settings, hdm);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_commit_start, "cxl_core");

int cxl_commit_wait(struct cxl_decoder_settings *settings, void __iomem *hdm)
{
	int rc;

	rc = cxld_await_commit(hdm, settings->id);
	if (rc)
		return rc;

	return 0;
}
EXPORT_SYMBOL_FOR_MODULES(cxl_commit_wait, "cxl_core");

int cxl_hdm_decode_decoder(struct cxl_decoder_settings *settings, int id,
			   u32 ctrl, u64 base, u64 size, u64 target_or_skip,
			   bool *committed)
{
	bool enabled = FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl);
	int rc;

	*settings = (struct cxl_decoder_settings) {
		.id = id,
		.target_or_skip = target_or_skip,
		.target_type = FIELD_GET(CXL_HDM_DECODER0_CTRL_HOSTONLY, ctrl) ?
			       CXL_DECODER_HOSTONLYMEM : CXL_DECODER_DEVMEM,
	};

	if (committed)
		*committed = enabled;
	if (!enabled)
		size = 0;
	if (base == U64_MAX || size == U64_MAX ||
	    (size && base > U64_MAX - (size - 1)))
		return -ENXIO;
	if (enabled && !size)
		return -ENXIO;

	settings->hpa_range = (struct range) {
		.start = base,
		.end = base + size - 1,
	};
	if (enabled) {
		settings->flags = CXL_DECODER_F_ENABLE;
		if (ctrl & CXL_HDM_DECODER0_CTRL_LOCK)
			settings->flags |= CXL_DECODER_F_LOCK;
	}

	rc = eiw_to_ways(FIELD_GET(CXL_HDM_DECODER0_CTRL_IW_MASK, ctrl),
			 &settings->interleave_ways);
	if (rc)
		return rc;

	return eig_to_granularity(FIELD_GET(CXL_HDM_DECODER0_CTRL_IG_MASK,
				    ctrl),
				  &settings->interleave_granularity);
}
EXPORT_SYMBOL_FOR_MODULES(cxl_hdm_decode_decoder, "cxl_core");

struct cxl_hdm_decoder_state {
	u32 ctrl;
	u32 base_low;
	u32 base_high;
	u32 size_low;
	u32 size_high;
	u32 target_low;
	u32 target_high;
};

static void cxl_pci_hdm_info_free(struct cxl_hdm_info *info)
{
	if (!info)
		return;

	kfree(info->decoder_state);
	kfree(info);
}

void pci_cxl_hdm_release(struct pci_dev *pdev)
{
	struct cxl_hdm_info *info;

	scoped_guard(rwsem_write, &cxl_rwsem.dpa) {
		info = pdev->hdm;
		pdev->hdm = NULL;
	}

	cxl_pci_hdm_info_free(info);
}

static bool cxl_pci_bar_usable(struct pci_dev *pdev, int bar)
{
	struct resource *res = &pdev->resource[bar];

	if (!pci_resource_len(pdev, bar))
		return false;
	if (res->flags & (IORESOURCE_UNSET | IORESOURCE_DISABLED))
		return false;
	if (resource_type(res) != IORESOURCE_MEM)
		return false;
	if (!res->start || !res->end)
		return false;

	return true;
}

static int cxl_pci_hdm_find_bar(struct pci_dev *pdev, resource_size_t hdm_start,
				resource_size_t hdm_size, int *bar,
				resource_size_t *offset)
{
	resource_size_t hdm_end;

	if (!hdm_size)
		return -EINVAL;

	hdm_end = hdm_start + hdm_size - 1;
	if (hdm_end < hdm_start)
		return -EINVAL;

	for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
		struct resource *res = &pdev->resource[i];

		if (!cxl_pci_bar_usable(pdev, i))
			continue;
		if (hdm_start < res->start || hdm_end > res->end)
			continue;

		if (bar)
			*bar = i;
		if (offset)
			*offset = hdm_start - res->start;
		return 0;
	}

	return -ENODEV;
}

static void __iomem *cxl_pci_hdm_map(struct pci_dev *pdev,
				     struct cxl_register_map *map,
				     struct cxl_hdm_info *info)
{
	struct cxl_reg_map *hdm_map = &map->component_map.hdm_decoder;
	resource_size_t hdm_start;
	void __iomem *hdm;
	int rc;

	hdm_start = map->resource + hdm_map->offset;
	info->hdm_size = hdm_map->size;

	rc = cxl_pci_hdm_find_bar(pdev, hdm_start, info->hdm_size,
				  &info->hdm_bar, &info->hdm_offset);
	if (rc)
		return ERR_PTR(rc);

	hdm = ioremap(hdm_start, info->hdm_size);
	if (!hdm) {
		pci_err(pdev, "failed to map CXL HDM decoder registers\n");
		return ERR_PTR(-ENOMEM);
	}

	return hdm;
}

static void cxl_pci_hdm_read_decoder_state(struct cxl_hdm_decoder_state *state,
					   void __iomem *hdm, int id)
{
	state->ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
	state->base_low = readl(hdm + CXL_HDM_DECODER0_BASE_LOW_OFFSET(id));
	state->base_high = readl(hdm + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(id));
	state->size_low = readl(hdm + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(id));
	state->size_high = readl(hdm + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(id));
	state->target_low = readl(hdm + CXL_HDM_DECODER0_TL_LOW(id));
	state->target_high = readl(hdm + CXL_HDM_DECODER0_TL_HIGH(id));
}

static int cxl_pci_hdm_read_decoder(struct pci_dev *pdev,
				    struct cxl_hdm_decoder_state *state,
				    struct cxl_decoder_settings *settings,
				    void __iomem *hdm, int id)
{
	u64 target_or_skip, base, size;
	int rc;

	cxl_pci_hdm_read_decoder_state(state, hdm, id);

	base = ((u64)state->base_high << 32) | state->base_low;
	size = ((u64)state->size_high << 32) | state->size_low;
	target_or_skip = ((u64)state->target_high << 32) | state->target_low;

	rc = cxl_hdm_decode_decoder(settings, id, state->ctrl, base, size,
				    target_or_skip, NULL);
	if (rc) {
		pci_err(pdev, "CXL HDM decoder %d has invalid configuration: %d\n",
			id, rc);
		return rc;
	}
	return 0;
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

static int cxl_pci_hdm_read_info(struct pci_dev *pdev,
				 struct cxl_register_map *map,
				 struct cxl_hdm_info *info)
{
	struct cxl_decoder_settings *settings;
	void __iomem *hdm;
	int decoder_count;
	int rc;

	rc = cxl_setup_regs(map);
	if (rc)
		return rc;

	if (!map->component_map.hdm_decoder.valid)
		return -ENODEV;

	hdm = cxl_pci_hdm_map(pdev, map, info);
	if (IS_ERR(hdm))
		return PTR_ERR(hdm);

	decoder_count = cxl_hdm_decoder_count(readl(hdm +
						    CXL_HDM_DECODER_CAP_OFFSET));
	if (decoder_count < 0) {
		rc = decoder_count;
		goto out_unmap;
	}

	if (decoder_count > CXL_HDM_DECODER_MAX_COUNT) {
		rc = -ENXIO;
		goto out_unmap;
	}

	info->decoder_count = decoder_count;
	info->global_ctrl = readl(hdm + CXL_HDM_DECODER_CTRL_OFFSET);
	info->decoder_state = kcalloc(decoder_count,
				      sizeof(*info->decoder_state),
				      GFP_KERNEL);
	if (!info->decoder_state) {
		rc = -ENOMEM;
		goto out_unmap;
	}

	settings = info->settings;
	for (int i = 0; i < info->decoder_count; i++) {
		rc = cxl_pci_hdm_read_decoder(pdev, &info->decoder_state[i],
					      &settings[i], hdm, i);
		if (rc)
			goto out_unmap;
	}

	rc = 0;
out_unmap:
	iounmap(hdm);
	return rc;
}

static int __pci_cxl_hdm_init(struct pci_dev *pdev)
{
	struct cxl_register_map map = { 0 };
	struct cxl_hdm_info *info;
	bool restore_command;
	u16 command;
	int rc;

	down_read(&cxl_rwsem.dpa);
	if (pdev->hdm) {
		up_read(&cxl_rwsem.dpa);
		return 0;
	}
	up_read(&cxl_rwsem.dpa);

	rc = cxl_pci_hdm_capable(pdev);
	if (rc)
		return rc;

	rc = cxl_find_regblock(pdev, CXL_REGLOC_RBI_COMPONENT, &map);
	if (rc)
		return rc;

	rc = cxl_pci_hdm_find_bar(pdev, map.resource, map.max_size, NULL, NULL);
	if (rc)
		return rc;

	info = kzalloc_obj(*info, GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	rc = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (rc) {
		rc = pcibios_err_to_errno(rc);
		goto out_free_info;
	}

	restore_command = !(command & PCI_COMMAND_MEMORY);
	if (restore_command) {
		rc = pci_write_config_word(pdev, PCI_COMMAND,
					   command | PCI_COMMAND_MEMORY);
		if (rc) {
			rc = pcibios_err_to_errno(rc);
			goto out_free_info;
		}
	}

	rc = cxl_pci_hdm_read_info(pdev, &map, info);

	if (restore_command) {
		int rc2 = pci_write_config_word(pdev, PCI_COMMAND, command);

		if (rc2) {
			rc2 = pcibios_err_to_errno(rc2);
			pci_err(pdev,
				"failed to restore PCI_COMMAND after CXL HDM cache init: %d\n",
				rc2);
			if (!rc)
				rc = rc2;
		}
	}

	if (rc)
		goto out_free_info;

	down_write(&cxl_rwsem.dpa);
	if (!pdev->hdm) {
		pdev->hdm = info;
		info = NULL;
	}
	up_write(&cxl_rwsem.dpa);

	cxl_pci_hdm_info_free(info);
	return 0;

out_free_info:
	cxl_pci_hdm_info_free(info);
	return rc;
}

void pci_cxl_hdm_init(struct pci_dev *pdev)
{
	int rc;

	rc = __pci_cxl_hdm_init(pdev);
	if (rc && rc != -ENOTTY && rc != -ENODEV)
		pci_dbg(pdev, "CXL HDM cache init failed: %d\n", rc);
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
	struct resource *res;
};

struct cxl_hdm_range_context {
	struct list_head ranges;
};

static void cxl_hdm_range_context_init(struct cxl_hdm_range_context *ctx)
{
	INIT_LIST_HEAD(&ctx->ranges);
}

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

static int cxl_hdm_range_add(struct cxl_hdm_range_context *ctx,
			     struct pci_dev *pdev, const struct range *hpa_range)
{
	struct cxl_hdm_range *range;

	if (hpa_range->end < hpa_range->start)
		return -EINVAL;

	list_for_each_entry(range, &ctx->ranges, list)
		if (range->hpa_range.start == hpa_range->start &&
		    range->hpa_range.end == hpa_range->end)
			return 0;

	range = kzalloc_obj(*range);
	if (!range)
		return -ENOMEM;

	range->pdev = pdev;
	range->hpa_range = *hpa_range;
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
		struct cxl_decoder_settings *settings = &info->settings[i];

		if (!(settings->flags & CXL_DECODER_F_ENABLE))
			continue;

		if (settings->flags & CXL_DECODER_F_NORMALIZED_ADDRESSING) {
			pci_err(pdev,
				"CXL reset does not support normalized address decoders\n");
			return -EOPNOTSUPP;
		}

		rc = cxl_hdm_range_add(ctx, pdev, &settings->hpa_range);
		if (rc)
			return rc;
	}

	return 0;
}

static int cxl_hdm_range_len(struct pci_dev *pdev,
			     const struct range *hpa_range, u64 *len)
{
	if (hpa_range->end < hpa_range->start)
		return -EINVAL;

	if (hpa_range->start > RESOURCE_SIZE_MAX ||
	    hpa_range->end > RESOURCE_SIZE_MAX) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds resource address size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	*len = range_len(hpa_range);
	if (!*len || *len > RESOURCE_SIZE_MAX) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds resource size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	if (*len > SIZE_MAX) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds cache flush size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	return 0;
}

static int cxl_hdm_range_request(struct cxl_hdm_range *range)
{
	struct pci_dev *pdev = range->pdev;
	const struct range *hpa_range = &range->hpa_range;
	u64 len;
	int rc;

	rc = cxl_hdm_range_len(pdev, hpa_range, &len);
	if (rc)
		return rc;

	range->res = request_mem_region(hpa_range->start, len, "cxl_reset");
	if (!range->res) {
		pci_err(pdev,
			"cannot reset while CXL memory range is busy [%#llx-%#llx]\n",
			hpa_range->start, hpa_range->end);
		return -EBUSY;
	}

	return 0;
}

static int cxl_hdm_ranges_request(struct cxl_hdm_range_context *ctx)
{
	struct cxl_hdm_range *range;
	int rc;

	lockdep_assert_held_write(&cxl_rwsem.region);

	list_for_each_entry(range, &ctx->ranges, list) {
		rc = cxl_hdm_range_request(range);
		if (rc)
			return rc;
	}

	return 0;
}

static int cxl_hdm_range_flush_cache(struct cxl_hdm_range *range)
{
	struct pci_dev *pdev = range->pdev;
	const struct range *hpa_range = &range->hpa_range;
	u64 len;
	int rc;

	rc = cxl_hdm_range_len(pdev, hpa_range, &len);
	if (rc)
		return rc;

	rc = cpu_cache_invalidate_memregion(hpa_range->start, len);
	if (rc)
		pci_err(pdev,
			"failed to invalidate CPU cache [%#llx-%#llx]: %d\n",
			hpa_range->start, hpa_range->end, rc);

	return rc;
}

static int cxl_hdm_ranges_flush_cpu_caches(struct cxl_hdm_range_context *ctx,
					   struct pci_dev *pdev)
{
	struct cxl_hdm_range *range;
	int rc;

	if (list_empty(&ctx->ranges))
		return 0;

	if (!cpu_cache_has_invalidate_memregion()) {
		pci_warn(pdev,
			 "CPU cache synchronization unavailable; continuing without cache invalidation\n");
		return 0;
	}

	list_for_each_entry(range, &ctx->ranges, list) {
		rc = cxl_hdm_range_flush_cache(range);
		if (rc)
			return rc;
	}

	return 0;
}

static int cxl_hdm_ranges_prepare(struct cxl_hdm_range_context *ctx,
				  struct pci_dev *pdev)
{
	int rc;

	lockdep_assert_held_write(&cxl_rwsem.region);

	rc = cxl_hdm_ranges_collect(ctx, pdev);
	if (rc)
		return rc;

	rc = cxl_hdm_ranges_request(ctx);
	if (rc)
		return rc;

	return cxl_hdm_ranges_flush_cpu_caches(ctx, pdev);
}

static int cxl_reset_dvsec(struct pci_dev *pdev, u16 *cap_out)
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

	if (!(cap & PCI_DVSEC_CXL_CACHE_CAPABLE) ||
	    !(cap & PCI_DVSEC_CXL_MEM_CAPABLE))
		return -ENOTTY;

	if (!(cap & PCI_DVSEC_CXL_RST_CAPABLE))
		return -ENOTTY;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL, &ctrl);
	if (rc)
		return pcibios_err_to_errno(rc);

	if (!(ctrl & PCI_DVSEC_CXL_CACHE_ENABLE) ||
	    !(ctrl & PCI_DVSEC_CXL_MEM_ENABLE))
		return -ENOTTY;

	*cap_out = cap;
	return dvsec;
}

static bool cxl_reset_hdm_available(struct pci_dev *pdev)
{
	struct cxl_hdm_info *info;

	/*
	 * pdev->hdm is owned by the PCI device and released with pci_dev, so
	 * reset-method probes and reset requests can test availability without
	 * a CXL driver bound to the device.
	 */
	guard(rwsem_read)(&cxl_rwsem.dpa);
	info = pdev->hdm;
	return info && info->hdm_size;
}

#define CXL_RESET_CTRL2_CMD_MASK \
	(PCI_DVSEC_CXL_INIT_CACHE_WBI | PCI_DVSEC_CXL_INIT_CXL_RST)

static int cxl_reset_read_ctrl2(struct pci_dev *pdev, int dvsec, u16 *ctrl2)
{
	int rc;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL2, ctrl2);
	if (rc)
		return pcibios_err_to_errno(rc);

	*ctrl2 &= ~CXL_RESET_CTRL2_CMD_MASK;
	return 0;
}

static int cxl_reset_write_ctrl2(struct pci_dev *pdev, int dvsec, u16 ctrl2)
{
	int rc;

	rc = pci_write_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL2, ctrl2);
	if (rc)
		return pcibios_err_to_errno(rc);

	return 0;
}

static int cxl_reset_set_ctrl2(struct pci_dev *pdev, int dvsec, u16 set)
{
	u16 ctrl2;
	int rc;

	rc = cxl_reset_read_ctrl2(pdev, dvsec, &ctrl2);
	if (rc)
		return rc;

	ctrl2 |= set;
	return cxl_reset_write_ctrl2(pdev, dvsec, ctrl2);
}

static int cxl_reset_clear_ctrl2(struct pci_dev *pdev, int dvsec, u16 clear)
{
	u16 ctrl2;
	int rc;

	rc = cxl_reset_read_ctrl2(pdev, dvsec, &ctrl2);
	if (rc)
		return rc;

	ctrl2 &= ~clear;
	return cxl_reset_write_ctrl2(pdev, dvsec, ctrl2);
}

static int cxl_reset_enable_cache(struct pci_dev *pdev, int dvsec)
{
	return cxl_reset_clear_ctrl2(pdev, dvsec,
				     PCI_DVSEC_CXL_DISABLE_CACHING);
}

static int cxl_reset_initiate(struct pci_dev *pdev, int dvsec)
{
	u16 ctrl2;
	int rc;

	rc = cxl_reset_read_ctrl2(pdev, dvsec, &ctrl2);
	if (rc)
		return rc;

	ctrl2 &= ~PCI_DVSEC_CXL_RST_MEM_CLR_EN;
	ctrl2 |= PCI_DVSEC_CXL_INIT_CXL_RST;
	return cxl_reset_write_ctrl2(pdev, dvsec, ctrl2);
}

static int cxl_reset_wait_cache_wbi(struct pci_dev *pdev, int dvsec)
{
	unsigned long deadline;
	u16 status2;
	int rc;

	rc = cxl_reset_set_ctrl2(pdev, dvsec, PCI_DVSEC_CXL_INIT_CACHE_WBI);
	if (rc)
		return rc;

	deadline = jiffies + usecs_to_jiffies(CXL_CACHE_WBI_TIMEOUT_US);
	do {
		usleep_range(CXL_CACHE_WBI_POLL_US, CXL_CACHE_WBI_POLL_US + 1);

		rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_STATUS2,
					  &status2);
		if (rc)
			return pcibios_err_to_errno(rc);
		if (status2 != U16_MAX && (status2 & PCI_DVSEC_CXL_CACHE_INV))
			return 0;
	} while (time_before(jiffies, deadline));

	return -ETIMEDOUT;
}

static int cxl_reset_disable_cache(struct pci_dev *pdev, int dvsec, u16 cap)
{
	int rc, rc2;

	rc = cxl_reset_set_ctrl2(pdev, dvsec,
				 PCI_DVSEC_CXL_DISABLE_CACHING);
	if (rc)
		return rc;

	if (!(cap & PCI_DVSEC_CXL_CACHE_WBI_CAPABLE))
		return 0;

	rc = cxl_reset_wait_cache_wbi(pdev, dvsec);
	if (!rc)
		return 0;

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
	bool final = false;
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

	do {
		rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_STATUS2,
					  &status2);
		if (!rc && status2 != U16_MAX) {
			if (status2 & PCI_DVSEC_CXL_RST_ERR)
				return -EIO;

			if (status2 & PCI_DVSEC_CXL_RST_DONE)
				return 0;
		}

		if (time_after_eq(jiffies, deadline)) {
			if (final)
				return -ETIMEDOUT;
			final = true;
			continue;
		}

		msleep(CXL_RESET_STATUS_POLL_MS);
	} while (true);
}

static int cxl_reset_execute(struct pci_dev *pdev, int dvsec, u16 cap)
{
	bool target_prepared = false;
	int rc, rc2;

	rc = cxl_reset_disable_cache(pdev, dvsec, cap);
	if (rc)
		return rc;

	if (!pci_wait_for_pending_transaction(pdev))
		pci_err(pdev, "timed out waiting for pending transactions\n");

	rc = pci_dev_reset_iommu_prepare(pdev);
	if (rc)
		pci_err(pdev, "failed to stop IOMMU for CXL reset: %d\n", rc);
	else
		target_prepared = true;

	if (!rc)
		rc = cxl_reset_initiate(pdev, dvsec);
	if (!rc)
		rc = cxl_reset_wait_done(pdev, dvsec, cap);

	rc2 = cxl_reset_enable_cache(pdev, dvsec);
	if (rc2 && rc)
		pci_warn(pdev, "failed to re-enable CXL caching: %d\n", rc2);
	else if (rc2)
		rc = rc2;

	if (target_prepared)
		pci_dev_reset_iommu_done(pdev);
	return rc;
}

int cxl_reset_function(struct pci_dev *pdev, bool probe)
{
	struct cxl_hdm_range_context range_ctx;
	int dvsec;
	int rc;
	u16 cap;

	dvsec = cxl_reset_dvsec(pdev, &cap);
	if (dvsec < 0)
		return dvsec;

	if (probe)
		return 0;

	if (!cxl_reset_hdm_available(pdev))
		return -ENOTTY;

	cxl_hdm_range_context_init(&range_ctx);

	scoped_guard(rwsem_write, &cxl_rwsem.region) {
		rc = cxl_hdm_ranges_prepare(&range_ctx, pdev);
		if (!rc)
			rc = cxl_reset_execute(pdev, dvsec, cap);
		cxl_hdm_range_context_destroy(&range_ctx);
	}

	return rc;
}
