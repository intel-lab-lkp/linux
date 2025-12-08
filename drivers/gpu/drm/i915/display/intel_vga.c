// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/vgaarb.h>

#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include <drm/intel/i915_drm.h>
#include <video/vga.h>

#include "intel_de.h"
#include "intel_display.h"
#include "intel_display_types.h"
#include "intel_vga.h"
#include "intel_vga_regs.h"

static unsigned int intel_gmch_ctrl_reg(struct intel_display *display)
{
	return DISPLAY_VER(display) >= 6 ? SNB_GMCH_CTRL : INTEL_GMCH_CTRL;
}

static bool intel_vga_decode_is_enabled(struct intel_display *display)
{
	struct pci_dev *pdev = to_pci_dev(display->drm->dev);
	u16 gmch_ctrl = 0;

	if (pci_bus_read_config_word(pdev->bus, PCI_DEVFN(0, 0),
				     intel_gmch_ctrl_reg(display), &gmch_ctrl))
		return false;

	return !(gmch_ctrl & INTEL_GMCH_VGA_DISABLE);
}

static i915_reg_t intel_vga_cntrl_reg(struct intel_display *display)
{
	if (display->platform.valleyview || display->platform.cherryview)
		return VLV_VGACNTRL;
	else if (DISPLAY_VER(display) >= 5)
		return CPU_VGACNTRL;
	else
		return VGACNTRL;
}

static bool has_vga_pipe_sel(struct intel_display *display)
{
	if (display->platform.i845g ||
	    display->platform.i865g)
		return false;

	if (display->platform.valleyview ||
	    display->platform.cherryview)
		return true;

	return DISPLAY_VER(display) < 7;
}

static bool intel_pci_set_io_decode(struct pci_dev *pdev, bool enable)
{
	u16 old = 0, cmd;

	pci_read_config_word(pdev, PCI_COMMAND, &old);
	cmd = old & ~PCI_COMMAND_IO;
	if (enable)
		cmd |= PCI_COMMAND_IO;
	pci_write_config_word(pdev, PCI_COMMAND, cmd);

	return old & PCI_COMMAND_IO;
}

static bool intel_vga_get(struct intel_display *display)
{
	struct pci_dev *pdev = to_pci_dev(display->drm->dev);

	/* WaEnableVGAAccessThroughIOPort:ctg+ */

	/*
	 * Bypass the VGA arbiter on the iGPU and just enable
	 * IO decode by hand. This avoids clobbering the VGA
	 * routing for an external GPU when it's the current
	 * VGA device, and thus prevents the all 0xff/white
	 * readout from VGA memory when taking over from vgacon.
	 *
	 * The iGPU has the highest VGA decode priority so it will
	 * grab any VGA IO access when IO decode is enabled, regardless
	 * of how any other VGA routing bits are configured.
	 */
	if (display->platform.dgfx)
		vga_get_uninterruptible(pdev, VGA_RSRC_LEGACY_IO);

	return intel_pci_set_io_decode(pdev, true);
}

static void intel_vga_put(struct intel_display *display, bool io_decode)
{
	struct pci_dev *pdev = to_pci_dev(display->drm->dev);

	/* see intel_vga_get() */
	intel_pci_set_io_decode(pdev, io_decode);

	if (display->platform.dgfx)
		vga_put(pdev, VGA_RSRC_LEGACY_IO);
}

/* Disable the VGA plane that we never use */
void intel_vga_disable(struct intel_display *display)
{
	i915_reg_t vga_reg = intel_vga_cntrl_reg(display);
	bool io_decode;
	u8 msr, sr1;
	u32 tmp;

	if (!intel_vga_decode_is_enabled(display)) {
		drm_dbg_kms(display->drm, "VGA decode is disabled\n");

		/*
		 * On older hardware VGA_DISP_DISABLE defaults to 0, but
		 * it *must* be set or else the pipe will be completely
		 * stuck (at least on g4x).
		 */
		goto reset_vgacntr;
	}

	tmp = intel_de_read(display, vga_reg);

	if ((tmp & VGA_DISP_DISABLE) == 0) {
		enum pipe pipe;

		if (display->platform.cherryview)
			pipe = REG_FIELD_GET(VGA_PIPE_SEL_MASK_CHV, tmp);
		else if (has_vga_pipe_sel(display))
			pipe = REG_FIELD_GET(VGA_PIPE_SEL_MASK, tmp);
		else
			pipe = PIPE_A;

		drm_dbg_kms(display->drm, "Disabling VGA plane on pipe %c\n",
			    pipe_name(pipe));
	} else {
		drm_dbg_kms(display->drm, "VGA plane is disabled\n");

		/*
		 * Unfortunately at least some BIOSes (eg. HSW Lenovo
		 * ThinkCentre E73) set up the VGA registers even when
		 * in UEFI mode with the VGA plane disabled. So we need to
		 * always clean up the mess for iGPUs. For discrete GPUs we
		 * don't really care about the state of the VGA registers
		 * since all VGA accesses can be blocked via the bridge.
		 */
		if (display->platform.dgfx)
			goto reset_vgacntr;
	}

	io_decode = intel_vga_get(display);

	outb(0x01, VGA_SEQ_I);
	sr1 = inb(VGA_SEQ_D);
	outb(sr1 | VGA_SR01_SCREEN_OFF, VGA_SEQ_D);

	msr = inb(VGA_MIS_R);
	/*
	 * VGA_MIS_COLOR controls both GPU level and display engine level
	 * MDA vs. CGA decode logic. But when the register gets reset
	 * (reset value has VGA_MIS_COLOR=0) by the power well, only the
	 * display engine level decode logic gets notified.
	 *
	 * Switch to MDA mode to make sure the GPU level decode logic will
	 * be in sync with the display engine level decode logic after the
	 * power well has been reset. Otherwise the GPU will claim CGA
	 * register accesses but the display engine will not, causing
	 * RMbus NoClaim errors.
	 */
	msr &= ~VGA_MIS_COLOR;
	outb(msr, VGA_MIS_W);

	intel_vga_put(display, io_decode);

	udelay(300);

reset_vgacntr:
	intel_de_write(display, vga_reg, VGA_DISP_DISABLE);
	intel_de_posting_read(display, vga_reg);
}

static int intel_gmch_vga_set_state(struct intel_display *display, bool enable_decode)
{
	struct pci_dev *pdev = to_pci_dev(display->drm->dev);
	u16 gmch_ctrl;

	if (pci_bus_read_config_word(pdev->bus, PCI_DEVFN(0, 0),
				     intel_gmch_ctrl_reg(display), &gmch_ctrl)) {
		drm_err(display->drm, "failed to read control word\n");
		return -EIO;
	}

	if (!!(gmch_ctrl & INTEL_GMCH_VGA_DISABLE) == !enable_decode)
		return 0;

	if (enable_decode)
		gmch_ctrl &= ~INTEL_GMCH_VGA_DISABLE;
	else
		gmch_ctrl |= INTEL_GMCH_VGA_DISABLE;

	if (pci_bus_write_config_word(pdev->bus, PCI_DEVFN(0, 0),
				      intel_gmch_ctrl_reg(display), gmch_ctrl)) {
		drm_err(display->drm, "failed to write control word\n");
		return -EIO;
	}

	return 0;
}

static unsigned int intel_gmch_vga_set_decode(struct pci_dev *pdev, bool enable_decode)
{
	struct intel_display *display = to_intel_display(pdev);

	intel_gmch_vga_set_state(display, enable_decode);

	if (enable_decode)
		return VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM |
		       VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;
	else
		return VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;
}

void intel_vga_register(struct intel_display *display)
{

	struct pci_dev *pdev = to_pci_dev(display->drm->dev);
	int ret;

	/*
	 * If we have > 1 VGA cards, then we need to arbitrate access to the
	 * common VGA resources.
	 *
	 * If we are a secondary display controller (!PCI_DISPLAY_CLASS_VGA),
	 * then we do not take part in VGA arbitration and the
	 * vga_client_register() fails with -ENODEV.
	 */
	ret = vga_client_register(pdev, intel_gmch_vga_set_decode);
	drm_WARN_ON(display->drm, ret && ret != -ENODEV);
}

void intel_vga_unregister(struct intel_display *display)
{
	struct pci_dev *pdev = to_pci_dev(display->drm->dev);

	vga_client_unregister(pdev);
}
