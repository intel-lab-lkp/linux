// SPDX-License-Identifier: GPL-2.0
/*
 * AMD Promontory 21 xHCI Hwmon Implementation
 * (only temperature monitoring is supported)
 *
 * This can be effectively used as the alternative chipset temperature monitor.
 *
 * Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#define PROM21_INDEX 0x3000
#define PROM21_DATA 0x3008
#define PROM21_TEMP_REG 0x0001e520

struct prom21_xhci {
	struct pci_dev *pdev;
	struct device *hwmon_dev;
	void __iomem *regs;
	struct mutex lock; /* serializes index/data register access */
};

static int prom21_xhci_pm_get(struct prom21_xhci *hwmon, bool *pm_ref)
{
	struct device *dev = &hwmon->pdev->dev;
	int ret;

	*pm_ref = false;

	/*
	 * PROM21 temperature register access does not return a valid value while
	 * the parent xHCI PCI function is suspended. Do not wake the device from
	 * a hwmon read; only read when runtime PM reports the device as active,
	 * or when runtime PM is disabled and the device is not marked as
	 * suspended.
	 */
	ret = pm_runtime_get_if_active(dev);
	if (ret > 0) {
		*pm_ref = true;
		return 0;
	}

	if (ret == -EINVAL && !pm_runtime_status_suspended(dev))
		return 0;

	if (!ret || pm_runtime_status_suspended(dev))
		return -ENODATA;

	return ret;
}

/*
 * This is not a pure MMIO read. The PROM21 vendor data register is selected
 * by temporarily writing PROM21_TEMP_REG to the vendor index register.
 * Serialize the sequence, keep it short, and restore the previous index before
 * returning so this driver does not leave the vendor index/data register pair
 * in a different state for other possible users.
 */
static int prom21_xhci_read_temp_raw_restore_index(struct prom21_xhci *hwmon,
						   u8 *raw)
{
	struct device *dev = &hwmon->pdev->dev;
	bool pm_ref;
	u32 index;
	u32 data;
	int ret;

	ret = prom21_xhci_pm_get(hwmon, &pm_ref);
	if (ret)
		return ret;

	mutex_lock(&hwmon->lock);
	index = readl(hwmon->regs + PROM21_INDEX);
	/* Select the PROM21 temperature register through the vendor index. */
	writel(PROM21_TEMP_REG, hwmon->regs + PROM21_INDEX);
	data = readl(hwmon->regs + PROM21_DATA);
	/* Restore the previous vendor index register value. */
	writel(index, hwmon->regs + PROM21_INDEX);
	readl(hwmon->regs + PROM21_INDEX);
	mutex_unlock(&hwmon->lock);

	if (pm_ref) {
		/*
		 * Drop only the reference taken by pm_runtime_get_if_active().
		 * Do not mark the device busy or schedule autosuspend from the
		 * hwmon path; sensor polling must not keep the xHCI PCI device
		 * active.
		 */
		pm_runtime_put_noidle(dev);
	}

	*raw = data & 0xff;
	if (!*raw || *raw == 0xff)
		return -ENODATA;

	return 0;
}

static long prom21_xhci_raw_to_millicelsius(u8 raw)
{
	/*
	 * No public AMD reference is available for this value.
	 * The scale was derived from observed PROM21 xHCI temperature readings:
	 *  temp[C] = raw * 0.9066 - 78.624
	 */
	return DIV_ROUND_CLOSEST(raw * 9066, 10) - 78624;
}

static umode_t prom21_xhci_is_visible(const void *drvdata,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel)
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

static int prom21_xhci_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct prom21_xhci *hwmon = dev_get_drvdata(dev);
	u8 raw;
	int ret;

	if (type != hwmon_temp || attr != hwmon_temp_input || channel)
		return -EOPNOTSUPP;

	ret = prom21_xhci_read_temp_raw_restore_index(hwmon, &raw);
	if (ret)
		return ret;

	*val = prom21_xhci_raw_to_millicelsius(raw);
	return 0;
}

static const struct hwmon_ops prom21_xhci_ops = {
	.is_visible = prom21_xhci_is_visible,
	.read = prom21_xhci_read,
};

static const struct hwmon_channel_info *const prom21_xhci_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL,
};

static const struct hwmon_chip_info prom21_xhci_chip_info = {
	.ops = &prom21_xhci_ops,
	.info = prom21_xhci_info,
};

static int prom21_xhci_probe(struct auxiliary_device *auxdev,
			     const struct auxiliary_device_id *id)
{
	struct device *dev = &auxdev->dev;
	struct device *parent = dev->parent;
	struct prom21_xhci *hwmon;
	struct pci_dev *pdev;
	struct usb_hcd *hcd;
	int ret;

	if (!parent || !dev_is_pci(parent))
		return -ENODEV;

	pdev = to_pci_dev(parent);
	hcd = pci_get_drvdata(pdev);
	if (!hcd)
		return dev_err_probe(dev, -ENODEV,
				     "xHCI HCD data unavailable\n");

	if (!hcd->regs || hcd->rsrc_len < PROM21_DATA + sizeof(u32))
		return dev_err_probe(dev, -ENODEV, "invalid MMIO resource\n");

	hwmon = devm_kzalloc(dev, sizeof(*hwmon), GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &hwmon->lock);
	if (ret)
		return ret;

	hwmon->pdev = pdev;
	hwmon->regs = hcd->regs;
	auxiliary_set_drvdata(auxdev, hwmon);

	/*
	 * Use the PCI function as the hwmon parent so user space reports it as
	 * a PCI adapter. Lifetime is still owned by this auxiliary driver;
	 * remove() unregisters the hwmon device before xhci-pci tears down the
	 * HCD.
	 */
	hwmon->hwmon_dev =
		hwmon_device_register_with_info(&pdev->dev, "prom21_xhci",
						hwmon, &prom21_xhci_chip_info,
						NULL);
	if (IS_ERR(hwmon->hwmon_dev))
		return PTR_ERR(hwmon->hwmon_dev);

	return 0;
}

static void prom21_xhci_remove(struct auxiliary_device *auxdev)
{
	struct prom21_xhci *hwmon = auxiliary_get_drvdata(auxdev);

	/*
	 * The PROM21 PCI glue destroys the auxiliary device before HCD teardown.
	 * Unregister the hwmon device here so sysfs removes the attributes,
	 * stops new reads, and drains active hwmon callbacks before the xHCI
	 * MMIO mapping is released.
	 */
	hwmon_device_unregister(hwmon->hwmon_dev);
}

static const struct auxiliary_device_id prom21_xhci_id_table[] = {
	{ .name = "xhci_pci_prom21.hwmon" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, prom21_xhci_id_table);

static struct auxiliary_driver prom21_xhci_driver = {
	.name = "prom21-xhci",
	.probe = prom21_xhci_probe,
	.remove = prom21_xhci_remove,
	.id_table = prom21_xhci_id_table,
};
module_auxiliary_driver(prom21_xhci_driver);

MODULE_AUTHOR("Jihong Min <hurryman2212@gmail.com>");
MODULE_DESCRIPTION("AMD Promontory 21 xHCI temperature sensor driver");
MODULE_LICENSE("GPL");
