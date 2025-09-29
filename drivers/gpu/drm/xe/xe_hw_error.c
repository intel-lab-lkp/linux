// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <linux/atomic.h>
#include <linux/fault-inject.h>
#include <linux/find.h>
#include <linux/types.h>

#include <drm/drm_managed.h>
#include <drm/drm_ras.h>

#include "regs/xe_gsc_regs.h"
#include "regs/xe_hw_error_regs.h"
#include "regs/xe_irq_regs.h"

#include "xe_device.h"
#include "xe_hw_error.h"
#include "xe_mmio.h"
#include "xe_survivability_mode.h"

#define  HEC_UNCORR_FW_ERR_BITS 4
extern struct fault_attr inject_csc_hw_error;

/* Error categories reported by hardware */
enum hardware_error {
	HARDWARE_ERROR_CORRECTABLE = 0,
	HARDWARE_ERROR_NONFATAL = 1,
	HARDWARE_ERROR_FATAL = 2,
	HARDWARE_ERROR_MAX,
};

static const char * const hec_uncorrected_fw_errors[] = {
	"Fatal",
	"CSE Disabled",
	"FD Corruption",
	"Data Corruption"
};

static const char *hw_error_to_str(const enum hardware_error hw_err)
{
	switch (hw_err) {
	case HARDWARE_ERROR_CORRECTABLE:
		return "CORRECTABLE";
	case HARDWARE_ERROR_NONFATAL:
		return "NONFATAL";
	case HARDWARE_ERROR_FATAL:
		return "FATAL";
	default:
		return "UNKNOWN";
	}
}

struct error_info {
	const char *name;
	atomic64_t counter;
};

#define ERR_INFO(_bit, _name) \
	[__ffs(_bit)] = { .name = _name, .counter = ATOMIC64_INIT(0) }

static struct error_info dev_err_stat_nonfatal_reg[] = {
	ERR_INFO(XE_GT_ERROR, "GT Error"),
	ERR_INFO(XE_SGGI_DATA_PARITY_ERROR, "SGGI Data Parity Error"),
	ERR_INFO(XE_DISPLAY_ERROR, "Display Error"),
	ERR_INFO(XE_SGDI_DATA_PARITY_ERROR, "SGDI Data Parity Error"),
	ERR_INFO(XE_GSC_ERROR, "GSC Error"),
	ERR_INFO(XE_SGLI_DATA_PARITY_ERROR, "SGLI Data Parity Error"),
	ERR_INFO(XE_SGUNIT_ERROR, "SG Unit Error"),
	ERR_INFO(XE_SGCI_DATA_PARITY_ERROR, "SGCI Data Parity Error"),
	ERR_INFO(XE_SOC_ERROR, "SoC Error"),
	ERR_INFO(XE_CSC_ERROR, "CSC Error"),
	ERR_INFO(XE_MERT_ERROR, "MERT Error"),
	ERR_INFO(XE_SGMI_DATA_PARITY_ERROR, "SGMI Data Parity Error"),
};

static struct error_info dev_err_stat_correctable_reg[] = {
	ERR_INFO(XE_GT_ERROR, "GT Error"),
	ERR_INFO(XE_DISPLAY_ERROR, "Display Error"),
	ERR_INFO(XE_GSC_ERROR, "GSC Error"),
	ERR_INFO(XE_SGUNIT_ERROR, "SG Unit Error"),
	ERR_INFO(XE_SOC_ERROR, "SoC Error"),
	ERR_INFO(XE_CSC_ERROR, "CSC Error"),
};

static int hw_query_error_counter(struct error_info *error_list,
				  u32 error_id, const char **name, u32 *val)
{
	*name = error_list[error_id].name;
	*val = atomic64_read(&error_list[error_id].counter);

	return 0;
}

static int query_error_counter_non_fatal(struct drm_ras_node *ep,
					 u32 error_id,
					 const char **name,
					 u32 *val)
{
	if (error_id >= ARRAY_SIZE(dev_err_stat_nonfatal_reg))
		return -EINVAL;

	if (!(DEV_ERR_STAT_NONFATAL_VALID_MASK & BIT(error_id)) ||
	    !dev_err_stat_nonfatal_reg[error_id].name)
		return -ENOENT;

	return hw_query_error_counter(dev_err_stat_nonfatal_reg,
				      error_id, name, val);
}

static int query_error_counter_correctable(struct drm_ras_node *ep,
					   u32 error_id,
					   const char **name,
					   u32 *val)
{
	if (error_id >= ARRAY_SIZE(dev_err_stat_correctable_reg))
		return -EINVAL;

	if (!(DEV_ERR_STAT_CORRECTABLE_VALID_MASK & BIT(error_id)) ||
	    !dev_err_stat_correctable_reg[error_id].name)
		return -ENOENT;

	return hw_query_error_counter(dev_err_stat_correctable_reg,
				      error_id, name, val);
}

static struct drm_ras_node node_non_fatal = {
	.node_name = "non-fatal",
	.type = DRM_RAS_NODE_TYPE_ERROR_COUNTER,
	.error_counter_range.last = __ffs(XE_SGMI_DATA_PARITY_ERROR),
	.query_error_counter = query_error_counter_non_fatal,
};

static struct drm_ras_node node_correctable = {
	.node_name = "correctable",
	.type = DRM_RAS_NODE_TYPE_ERROR_COUNTER,
	.error_counter_range.last = __ffs(XE_CSC_ERROR),
	.query_error_counter = query_error_counter_correctable,
};

static bool fault_inject_csc_hw_error(void)
{
	return IS_ENABLED(CONFIG_DEBUG_FS) && should_fail(&inject_csc_hw_error, 1);
}

static void csc_hw_error_work(struct work_struct *work)
{
	struct xe_tile *tile = container_of(work, typeof(*tile), csc_hw_error_work);
	struct xe_device *xe = tile_to_xe(tile);
	int ret;

	ret = xe_survivability_mode_runtime_enable(xe);
	if (ret)
		drm_err(&xe->drm, "Failed to enable runtime survivability mode\n");
}

static void csc_hw_error_handler(struct xe_tile *tile, const enum hardware_error hw_err)
{
	const char *hw_err_str = hw_error_to_str(hw_err);
	struct xe_device *xe = tile_to_xe(tile);
	struct xe_mmio *mmio = &tile->mmio;
	u32 base, err_bit, err_src;
	unsigned long fw_err;

	if (xe->info.platform != XE_BATTLEMAGE)
		return;

	base = BMG_GSC_HECI1_BASE;
	lockdep_assert_held(&xe->irq.lock);
	err_src = xe_mmio_read32(mmio, HEC_UNCORR_ERR_STATUS(base));
	if (!err_src) {
		drm_err_ratelimited(&xe->drm, HW_ERR "Tile%d reported HEC_ERR_STATUS_%s blank\n",
				    tile->id, hw_err_str);
		return;
	}

	if (err_src & UNCORR_FW_REPORTED_ERR) {
		fw_err = xe_mmio_read32(mmio, HEC_UNCORR_FW_ERR_DW0(base));
		for_each_set_bit(err_bit, &fw_err, HEC_UNCORR_FW_ERR_BITS) {
			drm_err_ratelimited(&xe->drm, HW_ERR
					    "%s: HEC Uncorrected FW %s error reported, bit[%d] is set\n",
					     hw_err_str, hec_uncorrected_fw_errors[err_bit],
					     err_bit);

			schedule_work(&tile->csc_hw_error_work);
		}
	}

	xe_mmio_write32(mmio, HEC_UNCORR_ERR_STATUS(base), err_src);
}

static void hw_error_counter(struct xe_device *xe,
			     const enum hardware_error hw_err, const u32 err_src)
{
	struct error_info *err_info;
	unsigned long err_bits = err_src;
	unsigned long error;

	if (hw_err == HARDWARE_ERROR_NONFATAL) {
		err_info = dev_err_stat_nonfatal_reg;
	} else if (hw_err == HARDWARE_ERROR_CORRECTABLE) {
		err_info = dev_err_stat_correctable_reg;
	} else {
		drm_err_ratelimited(&xe->drm, HW_ERR
				    "Error from non-supported source, err_src=0x%x\n",
				    err_src);
		return;
	}

	for_each_set_bit(error, &err_bits, 32) {
		atomic64_inc(&err_info[error].counter);
	}
}

static void hw_error_source_handler(struct xe_tile *tile, const enum hardware_error hw_err)
{
	const char *hw_err_str = hw_error_to_str(hw_err);
	struct xe_device *xe = tile_to_xe(tile);
	unsigned long flags;
	u32 err_src;

	if (xe->info.platform != XE_BATTLEMAGE)
		return;

	spin_lock_irqsave(&xe->irq.lock, flags);
	err_src = xe_mmio_read32(&tile->mmio, DEV_ERR_STAT_REG(hw_err));
	if (!err_src) {
		drm_err_ratelimited(&xe->drm, HW_ERR "Tile%d reported DEV_ERR_STAT_%s blank!\n",
				    tile->id, hw_err_str);
		goto unlock;
	}

	if (err_src & XE_CSC_ERROR)
		csc_hw_error_handler(tile, hw_err);

	hw_error_counter(xe, hw_err, err_src);

	xe_mmio_write32(&tile->mmio, DEV_ERR_STAT_REG(hw_err), err_src);

unlock:
	spin_unlock_irqrestore(&xe->irq.lock, flags);
}

/**
 * xe_hw_error_irq_handler - irq handling for hw errors
 * @tile: tile instance
 * @master_ctl: value read from master interrupt register
 *
 * Xe platforms add three error bits to the master interrupt register to support error handling.
 * These three bits are used to convey the class of error FATAL, NONFATAL, or CORRECTABLE.
 * To process the interrupt, determine the source of error by reading the Device Error Source
 * Register that corresponds to the class of error being serviced.
 */
void xe_hw_error_irq_handler(struct xe_tile *tile, const u32 master_ctl)
{
	enum hardware_error hw_err;

	if (fault_inject_csc_hw_error())
		schedule_work(&tile->csc_hw_error_work);

	for (hw_err = 0; hw_err < HARDWARE_ERROR_MAX; hw_err++)
		if (master_ctl & ERROR_IRQ(hw_err))
			hw_error_source_handler(tile, hw_err);
}

/*
 * Process hardware errors during boot
 */
static void process_hw_errors(struct xe_device *xe)
{
	struct xe_tile *tile;
	u32 master_ctl;
	u8 id;

	for_each_tile(tile, xe, id) {
		master_ctl = xe_mmio_read32(&tile->mmio, GFX_MSTR_IRQ);
		xe_hw_error_irq_handler(tile, master_ctl);
		xe_mmio_write32(&tile->mmio, GFX_MSTR_IRQ, master_ctl);
	}
}

static void hw_error_counter_fini(struct drm_device *dev, void *res)
{
	drm_ras_node_unregister(&node_non_fatal);
	drm_ras_node_unregister(&node_correctable);
}

static void hw_error_counter_init(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	char *name;

	name = kasprintf(GFP_KERNEL, "%02x:%02x.%d",
			 pdev->bus->number,
			 PCI_SLOT(pdev->devfn),
			 PCI_FUNC(pdev->devfn));
	if (!name) {
		drm_err(&xe->drm, "Failed to allocate memory for device name for drm_ras\n");
		return;
	}

	node_non_fatal.device_name = name;
	drm_ras_node_register(&node_non_fatal);

	node_correctable.device_name = name;
	drm_ras_node_register(&node_correctable);

	if (drmm_add_action_or_reset(&xe->drm, hw_error_counter_fini, xe))
		drm_err(&xe->drm, "Failed to add action for hw error counter fini\n");
}

/**
 * xe_hw_error_init - Initialize hw errors
 * @xe: xe device instance
 *
 * Initialize and check for errors that occurred during boot
 * prior to driver load
 */
void xe_hw_error_init(struct xe_device *xe)
{
	struct xe_tile *tile = xe_device_get_root_tile(xe);

	if (IS_SRIOV_VF(xe))
		return;

	if (IS_DGFX(xe))
		INIT_WORK(&tile->csc_hw_error_work, csc_hw_error_work);

	process_hw_errors(xe);

	hw_error_counter_init(xe);
}
