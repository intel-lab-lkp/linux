// SPDX-License-Identifier: GPL-2.0
/*
 * AMD PROM21 xHCI Hwmon Implementation
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

#define PCI_DEVICE_ID_AMD_PROM21_XHCI 0x43fd

#define PROM21_INDEX 0x3000
#define PROM21_DATA 0x3008
#define PROM21_TEMP_REG 0x0001e520

#define PROM21_HWMON_NAME "prom21_hwmon"
#define PROM21_TEMP_LABEL "xHCI"

struct prom21_hwmon {
	struct pci_dev *pdev;
	struct device *hwmon_dev;
	void __iomem *regs;
	bool removing;
	struct mutex lock; /* protects removing and the index/data registers */
};

static bool allow_pm_switch = true;
module_param(allow_pm_switch, bool, 0444);
MODULE_PARM_DESC(allow_pm_switch,
		 "Allow temperature reads to wake the xHCI PCI device");

static void prom21_hwmon_invalidate(struct prom21_hwmon *hwmon)
{
	mutex_lock(&hwmon->lock);
	hwmon->removing = true;
	mutex_unlock(&hwmon->lock);
}

static int prom21_hwmon_pm_get(struct prom21_hwmon *hwmon, bool *pm_ref)
{
	struct device *dev = &hwmon->pdev->dev;
	int ret;

	*pm_ref = false;

	/*
	 * PROM21 temperature register access does not return a valid value while
	 * the parent xHCI PCI function is suspended. By default, resume the xHC
	 * before touching MMIO. If allow_pm_switch=N, do not resume the xHC from
	 * a hwmon sysfs read; only read when runtime PM reports it as active, or
	 * when runtime PM is disabled and the device is not marked as suspended.
	 */
	if (allow_pm_switch) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			return ret;

		*pm_ref = true;
		return 0;
	}

	ret = pm_runtime_get_if_active(dev);
	if (ret > 0) {
		*pm_ref = true;
		return 0;
	}

	if (!ret || pm_runtime_status_suspended(dev))
		return -EAGAIN;

	if (ret == -EINVAL)
		return 0;

	return ret;
}

/*
 * This is not a pure MMIO read. The PROM21 vendor data register is selected
 * by temporarily writing PROM21_TEMP_REG to the vendor index register. Keep
 * the sequence short and restore the previous index before returning.
 */
static int prom21_hwmon_read_temp_raw_restore_index(struct prom21_hwmon *hwmon,
						    u8 *raw)
{
	struct device *dev = &hwmon->pdev->dev;
	bool pm_ref;
	u32 index;
	u32 data;
	int ret;

	/*
	 * The xHCI PCI remove path destroys the auxiliary device before HCD
	 * teardown. Keep runtime PM and MMIO inside the critical section so a
	 * sysfs read cannot use the vendor register pair after remove starts.
	 */
	mutex_lock(&hwmon->lock);
	if (hwmon->removing) {
		mutex_unlock(&hwmon->lock);
		return -ENODEV;
	}

	ret = prom21_hwmon_pm_get(hwmon, &pm_ref);
	if (ret) {
		mutex_unlock(&hwmon->lock);
		return ret;
	}

	index = readl(hwmon->regs + PROM21_INDEX);
	/* Select the PROM21 temperature register through the vendor index. */
	writel(PROM21_TEMP_REG, hwmon->regs + PROM21_INDEX);
	data = readl(hwmon->regs + PROM21_DATA);
	/* Restore the previous vendor index register value. */
	writel(index, hwmon->regs + PROM21_INDEX);
	readl(hwmon->regs + PROM21_INDEX);

	if (pm_ref) {
		/*
		 * Use autosuspend so repeated sysfs reads do not suspend the
		 * controller immediately after each successful register access.
		 */
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
	}
	mutex_unlock(&hwmon->lock);

	*raw = data & 0xff;
	if (!*raw || *raw == 0xff)
		return -ENODATA;

	return 0;
}

static long prom21_hwmon_raw_to_millicelsius(u8 raw)
{
	/*
	 * No public AMD reference is available for this value.
	 * The scale was derived from observed PROM21 xHCI temperature readings:
	 *  temp[C] = raw * 0.9066 - 78.624
	 */
	return DIV_ROUND_CLOSEST(raw * 9066, 10) - 78624;
}

static umode_t prom21_hwmon_is_visible(const void *drvdata,
				       enum hwmon_sensor_types type, u32 attr,
				       int channel)
{
	if (type != hwmon_temp || channel)
		return 0;

	switch (attr) {
	case hwmon_temp_input:
	case hwmon_temp_label:
		return 0444;
	default:
		return 0;
	}
}

static int prom21_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct prom21_hwmon *hwmon = dev_get_drvdata(dev);
	u8 raw;
	int ret;

	if (type != hwmon_temp || attr != hwmon_temp_input || channel)
		return -EOPNOTSUPP;

	ret = prom21_hwmon_read_temp_raw_restore_index(hwmon, &raw);
	if (ret)
		return ret;

	*val = prom21_hwmon_raw_to_millicelsius(raw);
	return 0;
}

static int prom21_hwmon_read_string(struct device *dev,
				    enum hwmon_sensor_types type, u32 attr,
				    int channel, const char **str)
{
	if (type != hwmon_temp || attr != hwmon_temp_label || channel)
		return -EOPNOTSUPP;

	*str = PROM21_TEMP_LABEL;
	return 0;
}

static const struct hwmon_ops prom21_hwmon_ops = {
	.is_visible = prom21_hwmon_is_visible,
	.read = prom21_hwmon_read,
	.read_string = prom21_hwmon_read_string,
};

static const struct hwmon_channel_info *const prom21_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	NULL,
};

static const struct hwmon_chip_info prom21_hwmon_chip_info = {
	.ops = &prom21_hwmon_ops,
	.info = prom21_hwmon_info,
};

static int prom21_hwmon_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct device *dev = &auxdev->dev;
	struct device *parent = dev->parent;
	struct prom21_hwmon *hwmon;
	struct pci_dev *pdev;
	struct usb_hcd *hcd;
	int ret;

	if (!parent || !dev_is_pci(parent))
		return -ENODEV;

	pdev = to_pci_dev(parent);
	if (pdev->vendor != PCI_VENDOR_ID_AMD ||
	    pdev->device != PCI_DEVICE_ID_AMD_PROM21_XHCI)
		return -ENODEV;

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
		hwmon_device_register_with_info(&pdev->dev, PROM21_HWMON_NAME,
						hwmon, &prom21_hwmon_chip_info,
						NULL);
	if (IS_ERR(hwmon->hwmon_dev))
		return PTR_ERR(hwmon->hwmon_dev);

	return 0;
}

static void prom21_hwmon_remove(struct auxiliary_device *auxdev)
{
	struct prom21_hwmon *hwmon = auxiliary_get_drvdata(auxdev);

	if (hwmon) {
		prom21_hwmon_invalidate(hwmon);
		hwmon_device_unregister(hwmon->hwmon_dev);
	}
}

static const struct auxiliary_device_id prom21_hwmon_id_table[] = {
	{ .name = "xhci_pci.hwmon" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, prom21_hwmon_id_table);

static struct auxiliary_driver prom21_hwmon_driver = {
	.name = "prom21-hwmon",
	.probe = prom21_hwmon_probe,
	.remove = prom21_hwmon_remove,
	.id_table = prom21_hwmon_id_table,
};
module_auxiliary_driver(prom21_hwmon_driver);

MODULE_AUTHOR("Jihong Min <hurryman2212@gmail.com>");
MODULE_DESCRIPTION("AMD PROM21 xHCI hwmon driver");
MODULE_LICENSE("GPL");
