// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Generic System Framebuffers
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
 */

/*
 * Simple-Framebuffer support
 * Create a platform-device for any available boot framebuffer. The
 * simple-framebuffer platform device is already available on DT systems, so
 * this module parses the global "screen_info" object and creates a suitable
 * platform device compatible with the "simple-framebuffer" DT object. If
 * the framebuffer is incompatible, we instead create a legacy
 * "vesa-framebuffer", "efi-framebuffer" or "platform-framebuffer" device and
 * pass the screen_info as platform_data. This allows legacy drivers
 * to pick these devices up without messing with simple-framebuffer drivers.
 * The global "screen_info" is still valid at all times.
 *
 * If CONFIG_SYSFB_SIMPLEFB is not selected, never register "simple-framebuffer"
 * platform devices, but only use legacy framebuffer devices for
 * backwards compatibility.
 *
 * TODO: We set the dev_id field of all platform-devices to 0. This allows
 * other OF/DT parsers to create such devices, too. However, they must
 * start at offset 1 for this to work.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/platform_data/simplefb.h>
#include <linux/platform_device.h>
#include <linux/screen_info.h>
#include <linux/sysfb.h>

static struct platform_device *pd;
static DEFINE_MUTEX(disable_lock);
static bool disabled;
static bool quirks_applied;

static struct device *sysfb_parent_dev(const struct screen_info *si);

static bool sysfb_unregister(void)
{
	if (IS_ERR_OR_NULL(pd))
		return false;

	platform_device_unregister(pd);
	pd = NULL;

	return true;
}

/**
 * sysfb_disable() - disable the Generic System Framebuffers support
 * @dev:	the device to check if non-NULL
 *
 * This disables the registration of system framebuffer devices that match the
 * generic drivers that make use of the system framebuffer set up by firmware.
 *
 * It also unregisters a device if this was already registered by sysfb_init().
 *
 * Context: The function can sleep. A @disable_lock mutex is acquired to serialize
 *          against sysfb_init(), that registers a system framebuffer device.
 */
void sysfb_disable(struct device *dev)
{
	struct screen_info *si = &screen_info;
	struct device *parent;

	mutex_lock(&disable_lock);
	parent = sysfb_parent_dev(si);
	if (!dev || !parent || dev == parent) {
		sysfb_unregister();
		disabled = true;
	}
	mutex_unlock(&disable_lock);
}
EXPORT_SYMBOL_GPL(sysfb_disable);

/* Caller must hold disable_lock */
static int __sysfb_create_device(bool restore)
{
	struct screen_info *si = &screen_info;
	struct device *parent;
	unsigned int type;
	struct simplefb_platform_data mode;
	const char *name;
	bool compatible;
	int ret = 0;

	if (!IS_ERR_OR_NULL(pd))
		return 0;

	/*
	 * If quirks haven't been applied yet, sysfb_init() hasn't run.
	 * Don't create the device now - let sysfb_init() do it after
	 * applying the necessary fixups and quirks. We can't call
	 * sysfb_apply_efi_quirks() here because it's __init.
	 */
	if (!quirks_applied)
		return 0;

	parent = sysfb_parent_dev(si);
	if (IS_ERR(parent))
		return PTR_ERR(parent);

	type = screen_info_video_type(si);

	/* try to create a simple-framebuffer device */
	compatible = sysfb_parse_mode(si, &mode);
	if (compatible) {
		pd = sysfb_create_simplefb(si, &mode, parent);
		if (!IS_ERR(pd)) {
			if (restore)
				pr_info("sysfb: restored simple-framebuffer device\n");
			goto put_device;
		}
	}

	/* if the FB is incompatible, create a legacy framebuffer device */
	switch (type) {
	case VIDEO_TYPE_EGAC:
		name = "ega-framebuffer";
		break;
	case VIDEO_TYPE_VGAC:
		name = "vga-framebuffer";
		break;
	case VIDEO_TYPE_VLFB:
		name = "vesa-framebuffer";
		break;
	case VIDEO_TYPE_EFI:
		name = "efi-framebuffer";
		break;
	default:
		name = "platform-framebuffer";
		break;
	}

	pd = platform_device_alloc(name, 0);
	if (!pd) {
		ret = -ENOMEM;
		goto put_device;
	}

	pd->dev.parent = parent;

	sysfb_set_efifb_fwnode(pd);

	ret = platform_device_add_data(pd, si, sizeof(*si));
	if (ret)
		goto err;

	ret = platform_device_add(pd);
	if (ret)
		goto err;

	if (restore)
		pr_info("sysfb: restored %s device\n", name);
	goto put_device;
err:
	platform_device_put(pd);
	pd = NULL;
put_device:
	put_device(parent);
	return ret;
}

/**
 * sysfb_restore() - restore the Generic System Framebuffer
 *
 * This function re-enables the Generic System Framebuffers support and
 * re-creates the platform device that was previously unregistered by
 * sysfb_disable(). This is intended for use by DRM drivers that need to
 * restore the fallback framebuffer when their probe fails after having
 * called aperture_remove_conflicting_devices() or similar.
 *
 * Context: The function can sleep. A @disable_lock mutex is acquired.
 *
 * Returns:
 * 0 on success, or a negative errno value otherwise.
 */
int sysfb_restore(void)
{
	int ret;

	mutex_lock(&disable_lock);
	disabled = false;
	ret = __sysfb_create_device(true);
	mutex_unlock(&disable_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(sysfb_restore);

/**
 * sysfb_handles_screen_info() - reports if sysfb handles the global screen_info
 *
 * Callers can use sysfb_handles_screen_info() to determine whether the Generic
 * System Framebuffers (sysfb) can handle the global screen_info data structure
 * or not. Drivers might need this information to know if they have to setup the
 * system framebuffer, or if they have to delegate this action to sysfb instead.
 *
 * Returns:
 * True if sysfb handles the global screen_info data structure.
 */
bool sysfb_handles_screen_info(void)
{
	const struct screen_info *si = &screen_info;

	return !!screen_info_video_type(si);
}
EXPORT_SYMBOL_GPL(sysfb_handles_screen_info);

#if defined(CONFIG_PCI)
static bool sysfb_pci_dev_is_enabled(struct pci_dev *pdev)
{
	/*
	 * TODO: Try to integrate this code into the PCI subsystem
	 */
	int ret;
	u16 command;

	ret = pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (ret != PCIBIOS_SUCCESSFUL)
		return false;
	if (!(command & PCI_COMMAND_MEMORY))
		return false;
	return true;
}
#else
static bool sysfb_pci_dev_is_enabled(struct pci_dev *pdev)
{
	return false;
}
#endif

static struct device *sysfb_parent_dev(const struct screen_info *si)
{
	struct pci_dev *pdev;

	pdev = screen_info_pci_dev(si);
	if (IS_ERR(pdev)) {
		return ERR_CAST(pdev);
	} else if (pdev) {
		if (!sysfb_pci_dev_is_enabled(pdev)) {
			pci_dev_put(pdev);
			return ERR_PTR(-ENODEV);
		}
		return &pdev->dev;
	}

	return NULL;
}

static __init int sysfb_init(void)
{
	int ret = 0;

	screen_info_apply_fixups();
	sysfb_apply_efi_quirks();

	mutex_lock(&disable_lock);
	quirks_applied = true;
	if (!disabled)
		ret = __sysfb_create_device(false);
	mutex_unlock(&disable_lock);

	return ret;
}

/* must execute after PCI subsystem for EFI quirks */
device_initcall(sysfb_init);
