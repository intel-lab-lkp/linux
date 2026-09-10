// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for the Valve Index headset
 */

#include <linux/hid.h>
#include <linux/module.h>

#include "hid-ids.h"

#define VALVE_INDEX_REBOOT_REPORT_ID	0x16
#define VALVE_INDEX_REBOOT_CMD		0x01
#define VALVE_INDEX_REPORT_SIZE		64

static bool valve_index_has_reboot_report(struct hid_device *hdev)
{
	struct hid_report *report;

	/*
	 * The reboot command is a vendor protocol carried in the unnumbered
	 * 64-byte output report of the headset's third interface; the first
	 * data byte is the command id.  Report 0x16 is only declared as a
	 * feature report and is not what the command is sent as.
	 */
	report = hdev->report_enum[HID_OUTPUT_REPORT].report_id_hash[0];

	return report && hid_report_len(report) == VALVE_INDEX_REPORT_SIZE;
}

static void valve_index_reboot(struct hid_device *hdev, bool wake)
{
	u8 *report;
	int ret;

	if (!valve_index_has_reboot_report(hdev))
		return;

	/* USB transfer buffers must be DMA-able, so not on the stack. */
	report = kzalloc(VALVE_INDEX_REPORT_SIZE, GFP_KERNEL);
	if (!report)
		return;
	report[0] = VALVE_INDEX_REBOOT_REPORT_ID;
	report[1] = VALVE_INDEX_REBOOT_CMD;

	if (wake) {
		ret = hid_hw_power(hdev, PM_HINT_FULLON);
		if (ret < 0) {
			hid_warn(hdev, "failed to resume headset for reboot: %d\n",
				 ret);
			goto out;
		}
	}

	/* Use the same interrupt-out then SET_REPORT fallback as hidraw. */
	ret = hid_hw_output_report(hdev, report, VALVE_INDEX_REPORT_SIZE);
	if (ret == -ENOSYS)
		ret = hid_hw_raw_request(hdev, report[0], report,
					 VALVE_INDEX_REPORT_SIZE,
					 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		hid_warn(hdev, "failed to reboot headset: %d\n", ret);
	else if (ret != VALVE_INDEX_REPORT_SIZE)
		hid_warn(hdev, "short headset reboot report: %d\n", ret);

	if (wake)
		hid_hw_power(hdev, PM_HINT_NORMAL);
out:
	kfree(report);
}

/*
 * The suspend and shutdown hooks only cover orderly power transitions.  After
 * a crash, a hard reset or a power cut the headset is left in the state where
 * its EDID no longer reads, and nothing recovers it until the next orderly
 * transition.  Expose the reboot command as a write-only "reboot" attribute
 * on the HID device so userspace can recover it, for instance from a udev
 * rule that fires only when the connector reports no EDID.  Writing to an
 * interface that does not carry the reboot report returns -ENODEV.
 */
static ssize_t reboot_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;
	if (!val)
		return count;
	if (!valve_index_has_reboot_report(hdev))
		return -ENODEV;

	valve_index_reboot(hdev, true);

	return count;
}
static DEVICE_ATTR_WO(reboot);

static struct attribute *valve_index_attrs[] = {
	&dev_attr_reboot.attr,
	NULL
};
ATTRIBUTE_GROUPS(valve_index);

/*
 * The headset's EDID service is lost when the host disables the DisplayPort
 * PHY during system suspend, so it needs the reboot on the way out of
 * suspend.  Doing it on the way in does not work: the headset dropping off
 * USB is a remote-wakeup event from its hub and aborts the suspend.
 */
static int valve_index_resume(struct hid_device *hdev)
{
	valve_index_reboot(hdev, false);

	return 0;
}

static void valve_index_shutdown(struct hid_device *hdev)
{
	valve_index_reboot(hdev, true);
}

static const struct hid_device_id valve_index_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_VALVE,
			 USB_DEVICE_ID_VALVE_INDEX_HEADSET) },
	{ }
};
MODULE_DEVICE_TABLE(hid, valve_index_devices);

static struct hid_driver valve_index_driver = {
	.name = "valve-index",
	.id_table = valve_index_devices,
	.resume = valve_index_resume,
	.reset_resume = valve_index_resume,
	.shutdown = valve_index_shutdown,
	.driver.dev_groups = valve_index_groups,
};
module_hid_driver(valve_index_driver);

MODULE_AUTHOR("Mario Limonciello <mario.limonciello@amd.com>");
MODULE_DESCRIPTION("HID driver for Valve Index headset");
MODULE_LICENSE("GPL");
