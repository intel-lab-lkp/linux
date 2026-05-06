// SPDX-License-Identifier: GPL-2.0
/*
 * AMD PROM21 xHCI Hwmon Implementation
 * (only temperature monitoring is supported)
 *
 * This can be effectively used as the alternative chipset temperature monitor.
 *
 * Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
 */

#include <linux/device/devres.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "xhci.h"
#include "xhci-prom21-hwmon.h"

#define XHCI_PROM21_INDEX 0x3000
#define XHCI_PROM21_DATA 0x3008
#define XHCI_PROM21_TEMP_REG 0x0001e520

#define XHCI_PROM21_HWMON_NAME "xhci_prom21"

struct xhci_prom21_hwmon {
	struct pci_dev *pdev;
	void __iomem *regs;
	bool removing;
	struct mutex lock; /* protects removing and the index/data registers */
};

struct xhci_prom21_hwmon_devres {
	struct xhci_prom21_hwmon *hwmon;
};

static void xhci_prom21_hwmon_invalidate(struct xhci_prom21_hwmon *hwmon)
{
	mutex_lock(&hwmon->lock);
	hwmon->removing = true;
	mutex_unlock(&hwmon->lock);
}

static void xhci_prom21_hwmon_devres_release(struct device *dev, void *res)
{
	struct xhci_prom21_hwmon_devres *devres = res;

	/*
	 * devm hwmon unregister runs after this lookup record is released.
	 * Mark the data path closed first so any late sysfs read returns
	 * without touching xHCI MMIO.
	 */
	xhci_prom21_hwmon_invalidate(devres->hwmon);
}

/*
 * This is not a pure MMIO read. The PROM21 vendor data register is selected
 * by temporarily writing XHCI_PROM21_TEMP_REG to the vendor index register.
 * Keep the sequence short and restore the previous index before returning.
 */
static int
xhci_prom21_read_temp_raw_restore_index(struct xhci_prom21_hwmon *hwmon,
					u8 *raw)
{
	struct device *dev = &hwmon->pdev->dev;
	u32 index;
	u32 data;
	int ret;

	/*
	 * xhci_try_prom21_hwmon_invalidate() uses the same lock before HCD
	 * teardown. Keep runtime PM and MMIO inside the critical section so a
	 * sysfs read cannot use the vendor register pair after remove starts.
	 */
	mutex_lock(&hwmon->lock);
	if (hwmon->removing) {
		mutex_unlock(&hwmon->lock);
		return -ENODEV;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		mutex_unlock(&hwmon->lock);
		return ret;
	}

	index = readl(hwmon->regs + XHCI_PROM21_INDEX);
	/* Select the PROM21 temperature register through the vendor index. */
	writel(XHCI_PROM21_TEMP_REG, hwmon->regs + XHCI_PROM21_INDEX);
	data = readl(hwmon->regs + XHCI_PROM21_DATA);
	/* Restore the previous vendor index register value. */
	writel(index, hwmon->regs + XHCI_PROM21_INDEX);
	readl(hwmon->regs + XHCI_PROM21_INDEX);

	/* Let xHCI PCI runtime PM coalesce repeated sysfs polling. */
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	mutex_unlock(&hwmon->lock);

	*raw = data & 0xff;
	if (!*raw || *raw == 0xff)
		return -ENODATA;

	return 0;
}

static long xhci_prom21_raw_to_millicelsius(u8 raw)
{
	/*
	 * No public AMD register reference is available for this value.
	 * The scale was derived from observed PROM21 xHCI temperature readings:
	 *  temp[C] = raw * 0.9066 - 78.624
	 */
	return DIV_ROUND_CLOSEST(raw * 9066, 10) - 78624;
}

static umode_t xhci_prom21_hwmon_is_visible(const void *drvdata,
					    enum hwmon_sensor_types type,
					    u32 attr, int channel)
{
	if (type != hwmon_temp || channel)
		return 0;

	switch (attr) {
	case hwmon_temp_input:
		return 0444;
	default:
		return 0;
	}
}

static int xhci_prom21_hwmon_read(struct device *dev,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel, long *val)
{
	struct xhci_prom21_hwmon *hwmon = dev_get_drvdata(dev);
	u8 raw;
	int ret;

	if (type != hwmon_temp || attr != hwmon_temp_input || channel)
		return -EOPNOTSUPP;

	ret = xhci_prom21_read_temp_raw_restore_index(hwmon, &raw);
	if (ret)
		return ret;

	*val = xhci_prom21_raw_to_millicelsius(raw);
	return 0;
}

static const struct hwmon_ops xhci_prom21_hwmon_ops = {
	.is_visible = xhci_prom21_hwmon_is_visible,
	.read = xhci_prom21_hwmon_read,
};

static const struct hwmon_channel_info *const xhci_prom21_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL,
};

static const struct hwmon_chip_info xhci_prom21_chip_info = {
	.ops = &xhci_prom21_hwmon_ops,
	.info = xhci_prom21_hwmon_info,
};

void xhci_prom21_hwmon_init(struct xhci_hcd *xhci, struct pci_dev *pdev)
{
	struct xhci_prom21_hwmon_devres *devres;
	struct xhci_prom21_hwmon *hwmon;
	struct usb_hcd *hcd = xhci_to_hcd(xhci);
	struct device *dev = &pdev->dev;
	struct device *hwmon_dev;

	if (!hcd->regs || hcd->rsrc_len < XHCI_PROM21_DATA + sizeof(u32)) {
		dev_err(dev,
			"AMD PROM21 hwmon unavailable: invalid MMIO resource\n");
		return;
	}

	hwmon = devm_kzalloc(dev, sizeof(*hwmon), GFP_KERNEL);
	if (!hwmon) {
		/* The allocator reports OOM; add PROM21 device context. */
		dev_err(dev, "AMD PROM21 hwmon state unavailable\n");
		return;
	}

	devres = devres_alloc(xhci_prom21_hwmon_devres_release, sizeof(*devres),
			      GFP_KERNEL);
	if (!devres) {
		dev_err(dev, "AMD PROM21 hwmon devres allocation failed\n");
		return;
	}

	hwmon->pdev = pdev;
	hwmon->regs = hcd->regs;
	mutex_init(&hwmon->lock);

	hwmon_dev = devm_hwmon_device_register_with_info(dev,
							 XHCI_PROM21_HWMON_NAME,
							 hwmon,
							 &xhci_prom21_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev)) {
		devres_free(devres);
		dev_err(dev, "AMD PROM21 hwmon registration failed: %pe\n",
			hwmon_dev);
		return;
	}

	/*
	 * Store a private devres record so the device remove path can find this
	 * state without adding PROM21-specific part to xhci-pci.
	 */
	devres->hwmon = hwmon;
	devres_add(dev, devres);
}
EXPORT_SYMBOL_GPL(xhci_prom21_hwmon_init);

void xhci_try_prom21_hwmon_invalidate(struct pci_dev *pdev)
{
	struct xhci_prom21_hwmon_devres *devres;

	/*
	 * This is called for every xHCI PCI device. Devices without PROM21
	 * hwmon support simply have no matching helper devres entry.
	 */
	devres = devres_find(&pdev->dev, xhci_prom21_hwmon_devres_release, NULL,
			     NULL);
	if (!devres) {
		dev_dbg(&pdev->dev, "AMD PROM21 hwmon state not found\n");
		return;
	}

	xhci_prom21_hwmon_invalidate(devres->hwmon);
}
EXPORT_SYMBOL_GPL(xhci_try_prom21_hwmon_invalidate);
