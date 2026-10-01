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

/*
 * CXL r4.0 sec 9.7.2 defines the reset completion timeout encodings.
 * Sec 9.7.3 leaves config-space access behavior undefined for 100 ms after
 * initiating CXL Reset, then limits software to CXL Status2 access until
 * reset completion, timeout, or error.
 */
#define CXL_RESET_RRS_WAIT_MS 100
#define CXL_RESET_STATUS_POLL_MS 20
#define CXL_RESET_MAX_WAIT_MS 20000
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
	struct cxl_hdm_range *range, *next, *new_range;
	int rc;

	rc = cxl_hdm_range_validate(pdev, hpa_range);
	if (rc)
		return rc;

	list_for_each_entry(range, &ctx->ranges, list)
		if (range_contains(&range->hpa_range, hpa_range))
			return 0;

	new_range = kzalloc_obj(*new_range);
	if (!new_range)
		return -ENOMEM;

	new_range->pdev = pdev;
	new_range->hpa_range = *hpa_range;
	new_range->len = range_len(hpa_range);

	list_for_each_entry_safe(range, next, &ctx->ranges, list) {
		if (range_contains(hpa_range, &range->hpa_range)) {
			list_del(&range->list);
			kfree(range);
		}
	}
	list_add_tail(&new_range->list, &ctx->ranges);

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

		/* A committed zero-size decoder maps no HPA. */
		if (!(config->flags & CXL_DECODER_F_ENABLE) ||
		    !range_len(&config->hpa_range))
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

	lockdep_assert_held_write(&cxl_rwsem.region);

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

static int cxl_reset_restore_cache_policy(struct pci_dev *pdev, int dvsec,
					  u16 saved_ctrl2)
{
	int rc;

	rc = cxl_reset_update_ctrl2_no_replay(pdev, dvsec,
					      saved_ctrl2 & PCI_DVSEC_CXL_DISABLE_CACHING,
					      PCI_DVSEC_CXL_DISABLE_CACHING);
	if (rc)
		pci_err(pdev, "failed to restore CXL cache policy: %d\n", rc);

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
		pci_warn(pdev,
			 "unknown CXL reset timeout encoding %d; using %u ms\n",
			 idx, CXL_RESET_MAX_WAIT_MS);
		idx = ARRAY_SIZE(cxl_reset_timeout_ms) - 1;
	}

	/* Allow one retry after the mandatory 100 ms quiet period. */
	timeout_ms = clamp_t(u32, cxl_reset_timeout_ms[idx],
			     CXL_RESET_RRS_WAIT_MS + CXL_RESET_STATUS_POLL_MS,
			     CXL_RESET_MAX_WAIT_MS);
	msleep(CXL_RESET_RRS_WAIT_MS);
	deadline = jiffies + msecs_to_jiffies(timeout_ms -
					      CXL_RESET_RRS_WAIT_MS);

	for (;;) {
		rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_STATUS2,
					  &status2);
		if (rc)
			return pcibios_err_to_errno(rc);
		if (status2 != U16_MAX) {
			if (status2 & PCI_DVSEC_CXL_RST_ERR)
				return -EIO;
			if (status2 & PCI_DVSEC_CXL_RST_DONE)
				return 0;
		}

		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;

		msleep(CXL_RESET_STATUS_POLL_MS);
	}
}

static int cxl_clear_memory(struct pci_dev *pdev, int dvsec, bool initiate)
{
	if (initiate)
		return cxl_reset_update_ctrl2_no_replay(pdev, dvsec,
						      PCI_DVSEC_CXL_INIT_CXL_RST |
						      PCI_DVSEC_CXL_RST_MEM_CLR_EN,
						      0);

	return cxl_reset_update_ctrl2_no_replay(pdev, dvsec, 0,
					      PCI_DVSEC_CXL_RST_MEM_CLR_EN);
}

static int __cxl_reset_execute(struct pci_dev *pdev, int dvsec, u16 cap,
			      struct cxl_hdm_range_context *range_ctx)
{
	int rc, rc2;

	if (!pci_wait_for_pending_transaction(pdev)) {
		pci_err(pdev, "timed out waiting for pending transactions\n");
		return -ETIMEDOUT;
	}

	rc = pci_dev_reset_iommu_prepare(pdev);
	if (rc) {
		pci_err(pdev, "failed to stop IOMMU for CXL reset: %d\n", rc);
		return rc;
	}

	rc = cxl_clear_memory(pdev, dvsec, true);
	if (!rc)
		rc = cxl_reset_wait_done(pdev, dvsec, cap);

	/* Always clear Memory Clear; successful cleanup must not hide reset failure. */
	rc2 = cxl_clear_memory(pdev, dvsec, false);
	if (rc2)
		pci_err(pdev, "failed to clear CXL Reset Memory Clear: %d\n", rc2);
	rc = rc ?: rc2;

	/* Evict lines fetched during reset before ending DMA exclusion. */
	rc2 = cxl_hdm_ranges_invalidate(range_ctx);
	rc = rc ?: rc2;

	pci_dev_reset_iommu_done(pdev);
	return rc;
}

static int cxl_reset_execute(struct pci_dev *pdev, int dvsec, u16 cap,
			     struct cxl_hdm_range_context *range_ctx)
{
	u16 saved_ctrl2;
	int rc, rc2;

	rc = pci_read_config_word(pdev, dvsec + PCI_DVSEC_CXL_CTRL2, &saved_ctrl2);
	if (rc)
		rc = pcibios_err_to_errno(rc);
	else if (PCI_POSSIBLE_ERROR(saved_ctrl2))
		rc = -ENODEV;
	if (rc) {
		cxl_hdm_range_context_destroy(range_ctx);
		return rc;
	}

	rc = cxl_reset_disable_cache(pdev, dvsec, cap);
	if (!rc)
		rc = __cxl_reset_execute(pdev, dvsec, cap, range_ctx);
	/* Restore cache policy after any attempt to disable caching. */
	rc2 = cxl_reset_restore_cache_policy(pdev, dvsec, saved_ctrl2);
	cxl_hdm_range_context_destroy(range_ctx);
	return rc ?: rc2;
}

int cxl_reset_function(struct pci_dev *pdev, bool probe)
{
	struct cxl_hdm_range_context range_ctx;
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

	INIT_LIST_HEAD(&range_ctx.ranges);

	scoped_guard(rwsem_write, &cxl_rwsem.region) {
		rc = cxl_hdm_ranges_prepare(&range_ctx, pdev);
		if (rc) {
			cxl_hdm_range_context_destroy(&range_ctx);
			return rc;
		}

		/* cxl_reset_execute() releases the ranges on success and failure. */
		rc = cxl_reset_execute(pdev, dvsec, cap, &range_ctx);
	}

	return rc;
}
