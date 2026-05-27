// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "xe_display.h"
#include "regs/xe_irq_regs.h"

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/intel/display_member.h>
#include <drm/intel/display_parent_interface.h>

#include "intel_audio.h"
#include "intel_bw.h"
#include "intel_display_device.h"
#include "intel_display_driver.h"
#include "intel_display_irq.h"
#include "intel_display_power.h"
#include "intel_dram.h"
#include "intel_fbdev.h"
#include "intel_hdcp.h"
#include "intel_hotplug.h"
#include "intel_opregion.h"
#include "xe_device.h"
#include "xe_display_bo.h"
#include "xe_display_pcode.h"
#include "xe_display_rpm.h"
#include "xe_dsb_buffer.h"
#include "xe_fb_pin.h"
#include "xe_frontbuffer.h"
#include "xe_hdcp_gsc.h"
#include "xe_initial_plane.h"
#include "xe_module.h"
#include "xe_panic.h"
#include "xe_stolen.h"

/* Ensure drm and display members are placed properly. */
INTEL_DISPLAY_MEMBER_STATIC_ASSERT(struct xe_device, drm, display);

/* Xe device functions */

/**
 * xe_display_driver_probe_defer - Detect if we need to wait for other drivers
 *				   early on
 * @pdev: PCI device
 *
 * Note: This is called before xe or display device creation.
 *
 * Returns: true if probe needs to be deferred, false otherwise
 */
bool xe_display_driver_probe_defer(struct pci_dev *pdev)
{
	if (!xe_modparam.probe_display)
		return 0;

	return intel_display_driver_probe_defer(pdev);
}

static void unset_display_features(struct xe_device *xe)
{
	xe->drm.driver_features &= ~XE_DISPLAY_DRIVER_FEATURES;
}

static void xe_display_fini_early(void *arg)
{
	struct xe_device *xe = arg;
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_hpd_cancel_work(display);
	intel_display_driver_remove_nogem(display);
	intel_display_driver_remove_noirq(display);
	intel_opregion_cleanup(display);
	intel_display_power_cleanup(display);
}

int xe_display_init_early(struct xe_device *xe)
{
	struct intel_display *display = xe->display;
	int err;

	if (!xe->info.probe_display)
		return 0;

	/* Fake uncore lock */
	spin_lock_init(&xe->uncore.lock);

	intel_display_driver_early_probe(display);

	intel_display_device_info_runtime_init(display);

	/* Display may have been disabled at runtime init */
	if (!intel_display_device_present(display)) {
		xe->info.probe_display = false;
		unset_display_features(xe);
		return 0;
	}

	/* Early display init.. */
	intel_opregion_setup(display);

	/*
	 * Fill the dram structure to get the system dram info. This will be
	 * used for memory latency calculation.
	 */
	err = intel_dram_detect(display);
	if (err)
		goto err_opregion;

	intel_bw_init_hw(display);

	err = intel_display_driver_probe_noirq(display);
	if (err)
		goto err_opregion;

	err = intel_display_driver_probe_nogem(display);
	if (err)
		goto err_noirq;

	return devm_add_action_or_reset(xe->drm.dev, xe_display_fini_early, xe);
err_noirq:
	intel_display_driver_remove_noirq(display);
	intel_display_power_cleanup(display);
err_opregion:
	intel_opregion_cleanup(display);
	return err;
}

static void xe_display_fini(void *arg)
{
	struct xe_device *xe = arg;
	struct intel_display *display = xe->display;

	intel_hpd_poll_fini(display);
	intel_hdcp_component_fini(display);
	intel_audio_deinit(display);
	intel_display_driver_remove(display);
}

int xe_display_init(struct xe_device *xe)
{
	struct intel_display *display = xe->display;
	int err;

	if (!xe->info.probe_display)
		return 0;

	err = intel_display_driver_probe(display);
	if (err)
		return err;

	return devm_add_action_or_reset(xe->drm.dev, xe_display_fini, xe);
}

void xe_display_register(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_register(display);
	intel_display_power_enable(display);
}

void xe_display_unregister(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_power_disable(display);
	intel_display_driver_unregister(display);
}

/* IRQ-related functions */

void xe_display_irq_handler(struct xe_device *xe, u32 master_ctl)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	if (master_ctl & DISPLAY_IRQ)
		intel_display_irq_handler(display, NULL);
}

void xe_display_irq_enable(struct xe_device *xe, u32 gu_misc_iir)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	if (gu_misc_iir & GU_MISC_GSE)
		intel_opregion_asle_intr(display);
}

void xe_display_irq_reset(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_irq_reset(display);
}

void xe_display_irq_postinstall(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_irq_postinstall(display);
}

void xe_display_pm_suspend(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_pm_suspend(display);
}

void xe_display_shutdown(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_shutdown(display);
}

void xe_display_pm_runtime_suspend(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_pm_runtime_suspend(display);
}

void xe_display_pm_suspend_late(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_pm_suspend_late(display);
}

void xe_display_pm_runtime_suspend_late(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_pm_runtime_suspend_late(display);
}

void xe_display_shutdown_late(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_shutdown_late(display);
}

void xe_display_pm_resume_early(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_pm_resume_early(display);
}

void xe_display_pm_resume(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_pm_resume(display);
}

void xe_display_pm_runtime_resume(struct xe_device *xe)
{
	struct intel_display *display = xe->display;

	if (!xe->info.probe_display)
		return;

	intel_display_driver_pm_runtime_resume(display);
}


static void display_device_remove(struct drm_device *dev, void *arg)
{
	struct xe_device *xe = arg;

	intel_display_device_remove(xe->display);
	xe->display = NULL;
}

static bool irq_enabled(struct drm_device *drm)
{
	struct xe_device *xe = to_xe_device(drm);

	return atomic_read(&xe->irq.enabled);
}

static void irq_synchronize(struct drm_device *drm)
{
	synchronize_irq(to_pci_dev(drm->dev)->irq);
}

static const struct intel_display_irq_interface xe_display_irq_interface = {
	.enabled = irq_enabled,
	.synchronize = irq_synchronize,
};

static bool d3cold_allowed(struct drm_device *drm)
{
	struct xe_device *xe = to_xe_device(drm);

	return xe->d3cold.allowed;
}

static bool has_auxccs(struct drm_device *drm)
{
	struct xe_device *xe = to_xe_device(drm);

	return xe->info.platform == XE_ALDERLAKE_P;
}

static const struct intel_display_parent_interface parent = {
	.bo = &xe_display_bo_interface,
	.dsb = &xe_display_dsb_interface,
	.fb_pin = &xe_display_fb_pin_interface,
	.frontbuffer = &xe_display_frontbuffer_interface,
	.hdcp = &xe_display_hdcp_interface,
	.initial_plane = &xe_display_initial_plane_interface,
	.irq = &xe_display_irq_interface,
	.panic = &xe_display_panic_interface,
	.pcode = &xe_display_pcode_interface,
	.rpm = &xe_display_rpm_interface,
	.stolen = &xe_display_stolen_interface,
	.d3cold_allowed = d3cold_allowed,
	.has_auxccs = has_auxccs,
};

/**
 * xe_display_probe - probe display and create display struct
 * @xe: XE device instance
 *
 * Initialize all fields used by the display part.
 *
 * TODO: once everything can be inside a single struct, make the struct opaque
 * to the rest of xe and return it to be xe->display.
 *
 * Returns: 0 on success
 */
int xe_display_probe(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct intel_display *display;
	int err;

	if (!xe->info.probe_display)
		goto no_display;

	display = intel_display_device_probe(pdev, &parent);
	if (IS_ERR(display))
		return PTR_ERR(display);

	xe->display = display;

	err = drmm_add_action_or_reset(&xe->drm, display_device_remove, xe);
	if (err)
		return err;

	if (intel_display_device_present(display))
		return 0;

no_display:
	xe->info.probe_display = false;
	unset_display_features(xe);
	return 0;
}

#ifdef CONFIG_DRM_FBDEV_EMULATION
int xe_display_driver_fbdev_probe(struct drm_fb_helper *fbh,
				  struct drm_fb_helper_surface_size *sizes)
{
	return intel_fbdev_driver_fbdev_probe(fbh, sizes);
}
#endif
