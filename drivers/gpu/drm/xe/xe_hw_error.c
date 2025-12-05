// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <linux/bitmap.h>
#include <linux/fault-inject.h>

#include "regs/xe_gsc_regs.h"
#include "regs/xe_hw_error_regs.h"
#include "regs/xe_irq_regs.h"

#include "xe_device.h"
#include "xe_drm_ras.h"
#include "xe_hw_error.h"
#include "xe_mmio.h"
#include "xe_survivability_mode.h"

#define  HEC_UNCORR_FW_ERR_BITS 4
#define XE_RAS_REG_SIZE 32

extern struct fault_attr inject_csc_hw_error;
static const char * const error_severity[] = DRM_XE_RAS_ERROR_SEVERITY_NAMES;

static const char * const hec_uncorrected_fw_errors[] = {
	"Fatal",
	"CSE Disabled",
	"FD Corruption",
	"Data Corruption"
};

#define ERR_INDEX(_bit, index) \
	[__ffs(_bit)] = index

static const unsigned long xe_hw_error_map[] = {
	ERR_INDEX(XE_GT_ERROR, DRM_XE_RAS_ERROR_CORE_COMPUTE),
};

enum gt_vector_regs {
	ERR_STAT_GT_VECTOR0 = 0,
	ERR_STAT_GT_VECTOR1,
	ERR_STAT_GT_VECTOR2,
	ERR_STAT_GT_VECTOR3,
	ERR_STAT_GT_VECTOR4,
	ERR_STAT_GT_VECTOR5,
	ERR_STAT_GT_VECTOR6,
	ERR_STAT_GT_VECTOR7,
	ERR_STAT_GT_VECTOR_MAX,
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

static void csc_hw_error_handler(struct xe_tile *tile,
				 const enum drm_xe_ras_error_severity severity)
{
	const char *severity_str = error_severity[severity];
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
				    tile->id, severity_str);
		return;
	}

	if (err_src & UNCORR_FW_REPORTED_ERR) {
		fw_err = xe_mmio_read32(mmio, HEC_UNCORR_FW_ERR_DW0(base));
		for_each_set_bit(err_bit, &fw_err, HEC_UNCORR_FW_ERR_BITS) {
			drm_err_ratelimited(&xe->drm, HW_ERR
					    "%s: HEC Uncorrected FW %s error reported, bit[%d] is set\n",
					     severity_str, hec_uncorrected_fw_errors[err_bit],
					     err_bit);

			schedule_work(&tile->csc_hw_error_work);
		}
	}

	xe_mmio_write32(mmio, HEC_UNCORR_ERR_STATUS(base), err_src);
}

static void log_hw_error(struct xe_tile *tile, const char *name,
			 const enum drm_xe_ras_error_severity severity)
{
	const char *severity_str = error_severity[severity];
	struct xe_device *xe = tile_to_xe(tile);

	if (severity == DRM_XE_RAS_ERROR_FATAL)
		drm_err_ratelimited(&xe->drm, "%s %s error detected\n", name, severity_str);
	else
		drm_warn(&xe->drm, "%s %s error detected\n", name, severity_str);
}

static void
log_gt_err(struct xe_tile *tile, const char *name, int i, u32 err,
	   const enum drm_xe_ras_error_severity severity)
{
	const char *severity_str = error_severity[severity];
	struct xe_device *xe = tile_to_xe(tile);

	if (severity == DRM_XE_RAS_ERROR_FATAL)
		drm_err_ratelimited(&xe->drm, "%s %s error detected, ERROR_STAT_GT_VECTOR%d:0x%08x\n",
				    name, severity_str, i, err);
	else
		drm_warn(&xe->drm, "%s %s error detected, ERROR_STAT_GT_VECTOR%d:0x%08x\n",
			 name, severity_str, i, err);
}

static void gt_handle_errors(struct xe_tile *tile,
			     const enum drm_xe_ras_error_severity severity, u32 error_id)
{
	struct xe_device *xe = tile_to_xe(tile);
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[severity];
	struct xe_mmio *mmio = &tile->mmio;
	unsigned long err_stat = 0;
	int i;

	if (xe->info.platform != XE_PVC)
		return;

	for (i = 0; i < ERR_STAT_GT_VECTOR_MAX; i++) {
		u32 vector, val;

		if (severity == DRM_XE_RAS_ERROR_CORRECTABLE && i >= ERR_STAT_GT_COR_VECTOR_LEN)
			break;

		vector = xe_mmio_read32(mmio, ERR_STAT_GT_VECTOR_REG(severity, i));
		if (!vector)
			continue;

		switch (i) {
		case ERR_STAT_GT_VECTOR0:
		case ERR_STAT_GT_VECTOR1:
			u32 errbit;

			val = hweight32(vector);
			atomic64_add(val, &info[error_id].counter);
			log_gt_err(tile, "Subslice", i, vector, severity);

			if (err_stat)
				break;

			err_stat = xe_mmio_read32(mmio, ERR_STAT_GT_REG(severity));
			for_each_set_bit(errbit, &err_stat, GT_HW_ERROR_MAX_ERR_BITS) {
				if (severity == DRM_XE_RAS_ERROR_CORRECTABLE &&
				    (BIT(errbit) & PVC_COR_ERR_MASK))
					atomic64_inc(&info[error_id].counter);
				if (severity == DRM_XE_RAS_ERROR_FATAL &&
				    (BIT(errbit) & PVC_FAT_ERR_MASK))
					atomic64_inc(&info[error_id].counter);
			}
			if (err_stat)
				xe_mmio_write32(mmio, ERR_STAT_GT_REG(severity), err_stat);
			break;
		case ERR_STAT_GT_VECTOR2:
		case ERR_STAT_GT_VECTOR3:
			val = hweight32(vector);
			atomic64_add(val, &info[error_id].counter);
			log_gt_err(tile, "L3 BANK", i, vector, severity);
			break;
		case ERR_STAT_GT_VECTOR6:
			val = hweight32(vector);
			atomic64_add(val, &info[error_id].counter);
			log_gt_err(tile, "TLB", i, vector, severity);
			break;
		case ERR_STAT_GT_VECTOR7:
			val = hweight32(vector);
			atomic64_add(val, &info[error_id].counter);
			break;
		default:
			log_gt_err(tile, "Undefined", i, vector, severity);
		}

		xe_mmio_write32(mmio, ERR_STAT_GT_VECTOR_REG(severity, i), vector);
	}
}

static void gt_hw_error_handler(struct xe_tile *tile,
				const enum drm_xe_ras_error_severity severity, u32 error_id)
{
	struct xe_device *xe = tile_to_xe(tile);
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[severity];

	switch (severity) {
	case DRM_XE_RAS_ERROR_CORRECTABLE:
		gt_handle_errors(tile, severity, error_id);
		break;
	case DRM_XE_RAS_ERROR_NONFATAL:
		atomic64_inc(&info[error_id].counter);
		log_hw_error(tile, "GT", severity);
		break;
	case DRM_XE_RAS_ERROR_FATAL:
		gt_handle_errors(tile, severity, error_id);
		break;
	default:
		drm_warn(&xe->drm, "Undefined error detected\n");
	}
}

static void hw_error_source_handler(struct xe_tile *tile, enum drm_xe_ras_error_severity severity)
{
	const char *severity_str = error_severity[severity];
	struct xe_device *xe = tile_to_xe(tile);
	struct xe_drm_ras *ras = &xe->ras;
	struct xe_drm_ras_counter *info = ras->info[severity];
	unsigned long flags, err_src;
	u32 err_bit;

	if (!IS_DGFX(xe))
		return;

	spin_lock_irqsave(&xe->irq.lock, flags);
	err_src = xe_mmio_read32(&tile->mmio, DEV_ERR_STAT_REG(severity));
	if (!err_src) {
		drm_err_ratelimited(&xe->drm, HW_ERR "Tile%d reported DEV_ERR_STAT_%s blank!\n",
				    tile->id, severity_str);
		goto unlock;
	}

	if (err_src & XE_CSC_ERROR) {
		csc_hw_error_handler(tile, severity);
		goto clear_reg;
	}

	if (!info) {
		drm_err_ratelimited(&xe->drm, HW_ERR "Errors undefined\n");
		goto clear_reg;
	}

	for_each_set_bit(err_bit, &err_src, XE_RAS_REG_SIZE) {
		u32 error_id = xe_hw_error_map[err_bit];
		const char *name;

		name = info[error_id].name;
		if (!name)
			goto clear_reg;

		if (severity == DRM_XE_RAS_ERROR_FATAL) {
			drm_err_ratelimited(&xe->drm, HW_ERR
					    "TILE%d reported %s %s error, bit[%d] is set\n",
					    tile->id, name, severity_str, err_bit);
		} else {
			drm_warn(&xe->drm, HW_ERR
				 "TILE%d reported %s %s error, bit[%d] is set\n",
				 tile->id, name, severity_str, err_bit);
		}
		if (BIT(err_bit) & XE_GT_ERROR)
			gt_hw_error_handler(tile, severity, error_id);
	}

clear_reg:
	xe_mmio_write32(&tile->mmio, DEV_ERR_STAT_REG(severity), err_src);
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
	u32 hw_err;

	if (fault_inject_csc_hw_error())
		schedule_work(&tile->csc_hw_error_work);

	for (hw_err = 0; hw_err < DRM_XE_RAS_ERROR_SEVERITY_MAX; hw_err++)
		if (master_ctl & ERROR_IRQ(hw_err))
			hw_error_source_handler(tile, hw_err);
}

static int hw_error_info_init(struct xe_device *xe)
{
	int ret;

	if (xe->info.platform != XE_PVC)
		return 0;

	ret = xe_drm_ras_allocate_nodes(xe);
	if (ret)
		return ret;

	return 0;
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

	if (!IS_DGFX(xe) || IS_SRIOV_VF(xe))
		return;

	INIT_WORK(&tile->csc_hw_error_work, csc_hw_error_work);

	hw_error_info_init(xe);
	process_hw_errors(xe);
}
