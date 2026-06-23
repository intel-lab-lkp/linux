// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 NVIDIA Corporation. All rights reserved. */
#include <linux/bitmap.h>
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

static int cxld_await_uncommit(void __iomem *hdm, int id)
{
	u32 ctrl;
	int i;

	for (i = 0; i < COMMIT_TIMEOUT_MS; i++) {
		ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
		if (!FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
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
	ctrl &= ~(CXL_HDM_DECODER0_CTRL_COMMIT |
		  CXL_HDM_DECODER0_CTRL_COMMIT_ERROR);
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
	info->global_ctrl = readl(regs.hdm_decoder +
				  CXL_HDM_DECODER_CTRL_OFFSET);

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

static int cxl_hdm_decoder_uncommit(struct pci_dev *pdev, void __iomem *hdm,
				    int id, bool *locked_committed)
{
	u32 ctrl;
	int rc;

	*locked_committed = false;
	ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
	if (ctrl & CXL_HDM_DECODER0_CTRL_LOCK) {
		if (ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED) {
			pci_dbg(pdev,
				"CXL HDM decoder %d retained locked committed state\n",
				id);
			*locked_committed = true;
			return 0;
		}

		pci_err(pdev, "CXL HDM decoder %d is locked\n", id);
		return -EBUSY;
	}

	if (!(ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED))
		return 0;

	ctrl &= ~CXL_HDM_DECODER0_CTRL_COMMIT;
	writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));

	rc = cxld_await_uncommit(hdm, id);
	if (rc)
		pci_err(pdev, "CXL HDM decoder %d uncommit failed: %d\n",
			id, rc);

	return rc;
}

static int cxl_restore_hdm_decoder(struct pci_dev *pdev,
				   struct cxl_decoder_settings *settings,
				   void __iomem *hdm)
{
	bool locked_committed;
	int rc;

	if (!(settings->flags & CXL_DECODER_F_ENABLE))
		return 0;

	rc = cxl_hdm_decoder_uncommit(pdev, hdm, settings->id,
				      &locked_committed);
	if (rc)
		return rc;
	if (locked_committed)
		return 0;

	rc = cxl_commit(settings, hdm);
	if (rc)
		pci_err(pdev, "CXL HDM decoder %d restore failed: %d\n",
			settings->id, rc);

	return rc;
}

static int cxl_restore_hdm(struct pci_dev *pdev)
{
	struct cxl_hdm_info *info = READ_ONCE(pdev->hdm);
	void __iomem *hdm;
	int first_rc = 0;

	if (!info)
		return 0;

	hdm = info->regs.hdm_decoder;
	if (!hdm) {
		pci_err(pdev, "CXL HDM decoder registers unavailable\n");
		return -ENXIO;
	}

	/*
	 * Restore global HDM control before per-decoder commit. PCI config
	 * state has been restored for MMIO access, but IOMMU reset blocks
	 * remain active until HDM restore completes.
	 */
	writel(info->global_ctrl, hdm + CXL_HDM_DECODER_CTRL_OFFSET);

	for (int i = 0; i < info->decoder_count; i++) {
		struct cxl_decoder_settings *settings = &info->settings[i];
		int rc;

		rc = cxl_restore_hdm_decoder(pdev, settings, hdm);
		if (rc && !first_rc)
			first_rc = rc;
	}

	return first_rc;
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

/* CXL r4.0 sec 8.1.4 defines 256 bits of Non-CXL Function Map. */
#define CXL_RESET_MAX_FUNCTIONS 256
#define CXL_RESET_FUNCTION_MAP_REGS (CXL_RESET_MAX_FUNCTIONS / 32)
#define CXL_RESET_SIBLINGS_INIT 8

struct cxl_reset_sibling {
	struct pci_dev *pdev;
	bool has_mem;
};

struct cxl_reset_context {
	struct pci_dev *target;
	struct cxl_reset_sibling *siblings;
	int nr_siblings;
	int nr_siblings_locked;
	int nr_siblings_prepared;
	bool target_prepared;
	int sibling_capacity;
};

struct cxl_reset_walk_context {
	struct cxl_reset_context *ctx;
	DECLARE_BITMAP(non_cxl_func_map, CXL_RESET_MAX_FUNCTIONS);
	bool ari;
	int rc;
};

struct cxl_hdm_range {
	struct list_head list;
	struct pci_dev *pdev;
	struct range hpa_range;
	struct resource *res;
};

struct cxl_hdm_range_context {
	struct list_head ranges;
};

static void cxl_reset_context_init(struct cxl_reset_context *ctx,
				   struct pci_dev *pdev)
{
	*ctx = (struct cxl_reset_context) {
		.target = pdev,
	};
}

static void cxl_reset_context_destroy(struct cxl_reset_context *ctx)
{
	for (int i = 0; i < ctx->nr_siblings; i++)
		pci_dev_put(ctx->siblings[i].pdev);
	kfree(ctx->siblings);
}

static void cxl_reset_read_non_cxl_func_map(struct pci_dev *pdev,
					    unsigned long *map)
{
	u32 words[CXL_RESET_FUNCTION_MAP_REGS];
	int dvsec, reg;

	bitmap_zero(map, CXL_RESET_MAX_FUNCTIONS);

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_FUNCTION_MAP);
	if (!dvsec)
		return;

	for (reg = 0; reg < CXL_RESET_FUNCTION_MAP_REGS; reg++) {
		int rc;

		rc = pci_read_config_dword(pdev,
					   dvsec + PCI_DVSEC_CXL_FUNCTION_MAP_REG +
					   reg * sizeof(u32), &words[reg]);
		if (rc) {
			pci_warn(pdev,
				 "failed to read Non-CXL Function Map; treating all siblings as CXL\n");
			bitmap_zero(map, CXL_RESET_MAX_FUNCTIONS);
			return;
		}
	}

	bitmap_from_arr32(map, words, CXL_RESET_MAX_FUNCTIONS);
}

static int cxl_reset_func_map_bit(struct pci_dev *sibling, bool ari)
{
	if (ari)
		return sibling->devfn;

	/*
	 * Without ARI, the Function Map is organized as 32 device slots per
	 * conventional 3-bit function number.
	 */
	return PCI_FUNC(sibling->devfn) * 32 + PCI_SLOT(sibling->devfn);
}

static int cxl_reset_read_cxl_cap(struct pci_dev *pdev, u16 *cap)
{
	int dvsec, rc;

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec)
		return -ENODEV;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CAP, cap);
	if (rc) {
		rc = pcibios_err_to_errno(rc);
		pci_warn(pdev, "failed to read CXL capability: %d\n", rc);
		return rc;
	}

	return 0;
}

static int cxl_reset_has_cache_or_mem(struct pci_dev *pdev, bool *has_mem)
{
	u16 cap;
	int rc;

	*has_mem = false;

	rc = cxl_reset_read_cxl_cap(pdev, &cap);
	if (rc == -ENODEV)
		return 0;
	if (rc)
		return rc;

	*has_mem = cap & PCI_DVSEC_CXL_MEM_CAPABLE;
	return !!(cap & (PCI_DVSEC_CXL_CACHE_CAPABLE |
			 PCI_DVSEC_CXL_MEM_CAPABLE));
}

static int cxl_reset_add_sibling(struct cxl_reset_context *ctx,
				 struct pci_dev *sibling, bool has_mem)
{
	if (ctx->nr_siblings >= ctx->sibling_capacity) {
		int capacity = ctx->sibling_capacity ?: CXL_RESET_SIBLINGS_INIT;
		struct cxl_reset_sibling *siblings;

		if (capacity > INT_MAX / 2)
			return -ENOMEM;
		if (ctx->sibling_capacity)
			capacity *= 2;

		siblings = krealloc_array(ctx->siblings, capacity,
					  sizeof(*siblings), GFP_KERNEL);
		if (!siblings)
			return -ENOMEM;

		ctx->siblings = siblings;
		ctx->sibling_capacity = capacity;
	}

	ctx->siblings[ctx->nr_siblings] = (struct cxl_reset_sibling) {
		.pdev = pci_dev_get(sibling),
		.has_mem = has_mem,
	};
	ctx->nr_siblings++;
	return 0;
}

static int cxl_reset_collect_sibling(struct pci_dev *sibling, void *data)
{
	struct cxl_reset_walk_context *wctx = data;
	struct cxl_reset_context *ctx = wctx->ctx;
	struct pci_dev *pdev = ctx->target;
	bool has_mem;
	int fn, rc;

	if (sibling == pdev)
		return 0;

	if (sibling->bus != pdev->bus)
		return 0;

	if (!wctx->ari && PCI_SLOT(sibling->devfn) != PCI_SLOT(pdev->devfn))
		return 0;

	fn = cxl_reset_func_map_bit(sibling, wctx->ari);
	if (test_bit(fn, wctx->non_cxl_func_map))
		return 0;

	rc = cxl_reset_has_cache_or_mem(sibling, &has_mem);
	if (rc < 0) {
		wctx->rc = rc;
		return rc;
	}
	if (!rc)
		return 0;

	wctx->rc = cxl_reset_add_sibling(ctx, sibling, has_mem);
	return wctx->rc;
}

static int cxl_reset_collect_siblings(struct cxl_reset_context *ctx)
{
	struct pci_dev *pdev = ctx->target;
	struct cxl_reset_walk_context wctx = {
		.ctx = ctx,
		.ari = pci_ari_enabled(pdev->bus),
	};

	cxl_reset_read_non_cxl_func_map(pdev, wctx.non_cxl_func_map);
	pci_walk_bus(pdev->bus, cxl_reset_collect_sibling, &wctx);

	return wctx.rc;
}

static void cxl_pci_functions_unlock(struct cxl_reset_context *ctx)
{
	while (ctx->nr_siblings_locked) {
		struct pci_dev *sibling;

		sibling = ctx->siblings[--ctx->nr_siblings_locked].pdev;
		pci_dev_unlock(sibling);
	}
}

static int cxl_pci_functions_lock(struct cxl_reset_context *ctx)
{
	for (int i = 0; i < ctx->nr_siblings; i++) {
		struct pci_dev *sibling = ctx->siblings[i].pdev;

		if (!pci_dev_trylock(sibling)) {
			cxl_pci_functions_unlock(ctx);
			return -EAGAIN;
		}

		ctx->nr_siblings_locked++;
	}

	return 0;
}

static void cxl_pci_functions_reset_done(struct cxl_reset_context *ctx)
{
	while (ctx->nr_siblings_prepared) {
		struct pci_dev *sibling;

		sibling = ctx->siblings[--ctx->nr_siblings_prepared].pdev;
		pci_dev_reset_iommu_done(sibling);
		pci_dev_restore(sibling);
	}
}

static int cxl_pci_functions_reset_prepare(struct cxl_reset_context *ctx)
{
	for (int i = 0; i < ctx->nr_siblings_locked; i++) {
		struct pci_dev *sibling = ctx->siblings[i].pdev;
		int rc;

		pci_dev_save_and_disable(sibling);
		if (!pci_wait_for_pending_transaction(sibling))
			pci_err(sibling,
				"timed out waiting for pending transactions\n");

		rc = pci_dev_reset_iommu_prepare(sibling);
		if (rc) {
			pci_err(sibling,
				"failed to stop IOMMU for CXL reset: %d\n",
				rc);
			pci_dev_restore(sibling);
			return rc;
		}

		ctx->nr_siblings_prepared++;
	}

	return 0;
}

static void cxl_pci_target_reset_done(struct cxl_reset_context *ctx)
{
	if (!ctx->target_prepared)
		return;

	pci_dev_reset_iommu_done(ctx->target);
	ctx->target_prepared = false;
}

static int cxl_pci_target_reset_prepare(struct cxl_reset_context *ctx)
{
	struct pci_dev *pdev = ctx->target;
	int rc;

	if (!pci_wait_for_pending_transaction(pdev))
		pci_err(pdev, "timed out waiting for pending transactions\n");

	rc = pci_dev_reset_iommu_prepare(pdev);
	if (rc) {
		pci_err(pdev, "failed to stop IOMMU for CXL reset: %d\n", rc);
		return rc;
	}

	ctx->target_prepared = true;
	return 0;
}

static void cxl_pci_functions_restore_state(struct cxl_reset_context *ctx)
{
	/*
	 * Restore PCI config state first so HDM MMIO is reachable. The final
	 * pci_dev_restore() pass deliberately replays pci_restore_state()
	 * before invoking driver reset_done() callbacks.
	 */
	pci_restore_state(ctx->target);

	for (int i = 0; i < ctx->nr_siblings_prepared; i++)
		pci_restore_state(ctx->siblings[i].pdev);
}

static int cxl_restore_hdm_decoders(struct cxl_reset_context *ctx)
{
	int first_rc = 0;
	int rc;

	cxl_pci_functions_restore_state(ctx);

	rc = cxl_restore_hdm(ctx->target);
	if (rc && !first_rc)
		first_rc = rc;

	for (int i = 0; i < ctx->nr_siblings_prepared; i++) {
		struct pci_dev *sibling = ctx->siblings[i].pdev;

		rc = cxl_restore_hdm(sibling);
		if (rc && !first_rc)
			first_rc = rc;
	}

	return first_rc;
}

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
	struct cxl_hdm_info *info = READ_ONCE(pdev->hdm);
	int rc;

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
	if (sizeof(resource_size_t) < sizeof(hpa_range->start) &&
	    (hpa_range->start > (resource_size_t)~0ULL ||
	     hpa_range->end > (resource_size_t)~0ULL)) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds resource address size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	if (hpa_range->end < hpa_range->start)
		return -EINVAL;

	if (!hpa_range->start && hpa_range->end == U64_MAX) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds resource size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	*len = range_len(hpa_range);
	if (sizeof(resource_size_t) < sizeof(*len) &&
	    *len > (resource_size_t)~0ULL) {
		pci_err(pdev,
			"CXL reset range [%#llx-%#llx] exceeds resource size\n",
			hpa_range->start, hpa_range->end);
		return -EOVERFLOW;
	}

	if (sizeof(size_t) < sizeof(*len) && *len > SIZE_MAX) {
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
		pci_err(pdev, "failed to synchronize CPU cache state\n");
		return -ENXIO;
	}

	list_for_each_entry(range, &ctx->ranges, list) {
		rc = cxl_hdm_range_flush_cache(range);
		if (rc)
			return rc;
	}

	return 0;
}

static int cxl_hdm_ranges_prepare(struct cxl_hdm_range_context *ctx,
				  struct cxl_reset_context *reset_ctx)
{
	struct pci_dev *pdev = reset_ctx->target;
	int rc;

	lockdep_assert_held_write(&cxl_rwsem.region);

	rc = cxl_hdm_ranges_collect(ctx, pdev);
	if (rc)
		return rc;

	for (int i = 0; i < reset_ctx->nr_siblings; i++) {
		struct cxl_reset_sibling *sibling = &reset_ctx->siblings[i];

		if (!sibling->has_mem)
			continue;

		rc = cxl_hdm_ranges_collect(ctx, sibling->pdev);
		if (rc)
			return rc;
	}

	rc = cxl_hdm_ranges_request(ctx);
	if (rc)
		return rc;

	return cxl_hdm_ranges_flush_cpu_caches(ctx, pdev);
}

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

	rc = cxl_reset_disable_cache(pdev, dvsec, cap);
	if (rc)
		return rc;
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

	return rc;
}

int cxl_reset_function(struct pci_dev *pdev, bool probe)
{
	struct cxl_hdm_range_context range_ctx;
	struct cxl_reset_context ctx;
	int dvsec;
	int rc;

	dvsec = cxl_reset_dvsec(pdev);
	if (dvsec < 0)
		return dvsec;

	if (probe)
		return 0;

	cxl_reset_context_init(&ctx, pdev);
	cxl_hdm_range_context_init(&range_ctx);

	rc = cxl_reset_collect_siblings(&ctx);
	if (rc)
		goto out;

	rc = cxl_pci_functions_lock(&ctx);
	if (rc)
		goto out_unlock;

	rc = cxl_pci_functions_reset_prepare(&ctx);
	if (rc)
		goto out_functions_done;

	rc = cxl_pci_target_reset_prepare(&ctx);
	if (rc)
		goto out_functions_done;

	scoped_guard(rwsem_write, &cxl_rwsem.region) {
		rc = cxl_hdm_ranges_prepare(&range_ctx, &ctx);
		if (!rc)
			rc = cxl_reset_execute(pdev, dvsec);
		if (!rc)
			rc = cxl_restore_hdm_decoders(&ctx);
	}

	cxl_pci_target_reset_done(&ctx);
out_functions_done:
	cxl_pci_functions_reset_done(&ctx);
out_unlock:
	cxl_pci_functions_unlock(&ctx);
out:
	cxl_hdm_range_context_destroy(&range_ctx);
	cxl_reset_context_destroy(&ctx);
	return rc;
}
