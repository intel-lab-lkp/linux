// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux hwmon driver for ARCTIC Fan Controller
 *
 * USB Custom HID device with 10 fan channels.
 * Exposes fan RPM (input) and PWM (0-255) via hwmon. Device pushes IN reports
 * at ~1 Hz; no GET_REPORT. OUT reports set PWM duty (bytes 1-10, 0-100%).
 * PWM is manual-only: the device does not change duty autonomously, only
 * when it receives an OUT report from the host.
 */

#include <linux/cache.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>

#define ARCTIC_VID			0x3904
#define ARCTIC_PID			0xF001
#define ARCTIC_NUM_FANS			10
#define ARCTIC_OUTPUT_REPORT_ID		0x01
#define ARCTIC_REPORT_LEN		32
#define ARCTIC_RPM_OFFSET		11	/* bytes 11-30: 10 x uint16 LE */
/* ACK report: device sends Report ID 0x02, 2 bytes (ID + status) after applying OUT report */
#define ARCTIC_ACK_REPORT_ID		0x02
#define ARCTIC_ACK_REPORT_LEN		2
/*
 * Time to wait for ACK report after send.
 * Measured over 500 iterations: max ~563 ms. Keep 1 s as margin.
 */
#define ARCTIC_ACK_TIMEOUT_MS		1000

struct arctic_fan_data {
	struct hid_device *hdev;
	spinlock_t in_report_lock;	/* protects fan_rpm[], ack_status, write_pending */
	struct completion in_report_received; /* ACK (ID 0x02) received in raw_event */
	int ack_status;			/* 0 = OK, negative errno on device error */
	bool write_pending;		/* true while an OUT report ACK is in flight */
	u32 fan_rpm[ARCTIC_NUM_FANS];
	u8 pwm_duty[ARCTIC_NUM_FANS];	/* 0-255 matching sysfs range; converted to 0-100 on send */
	/*
	 * OUT report buffer. Cache-line aligned so it occupies its own cache
	 * line, preventing DMA cache-coherency issues with adjacent fields
	 * (fan_rpm[], pwm_duty[]) on non-coherent architectures.
	 * Embedded in the devm_kzalloc'd struct so it is heap-allocated and
	 * passes usb_hcd_map_urb_for_dma(). Serialized by the hwmon core.
	 */
	u8 buf[ARCTIC_REPORT_LEN] ____cacheline_aligned;
};

/*
 * Parse RPM values from the periodic status report (10 x uint16 LE at rpm_off).
 * pwm_duty is not updated from the report: the device is manual-only, so the
 * host cache is the authoritative source for PWM.
 * Called from raw_event which may run in IRQ context; must not sleep.
 */
static void arctic_fan_parse_report(struct arctic_fan_data *priv, u8 *buf,
				    int len, int rpm_off)
{
	unsigned long flags;
	int i;

	if (len < rpm_off + 20)
		return;

	spin_lock_irqsave(&priv->in_report_lock, flags);
	for (i = 0; i < ARCTIC_NUM_FANS; i++)
		priv->fan_rpm[i] = get_unaligned_le16(&buf[rpm_off + i * 2]);
	spin_unlock_irqrestore(&priv->in_report_lock, flags);
}

/*
 * raw_event: IN reports.
 *
 * Status report: Report ID 0x01, 32 bytes:
 *   byte 0 = report ID, bytes 1-10 = PWM 0-100%, bytes 11-30 = 10 x RPM uint16 LE.
 *   Device pushes these at ~1 Hz; no GET_REPORT.
 *
 * ACK report: Report ID 0x02, 2 bytes:
 *   byte 0 = 0x02, byte 1 = status (0x00 = OK, 0x01 = ERROR).
 *   Sent once after accepting and applying an OUT report (ID 0x01).
 */
static int arctic_fan_raw_event(struct hid_device *hdev,
				struct hid_report *report, u8 *data, int size)
{
	struct arctic_fan_data *priv = hid_get_drvdata(hdev);
	unsigned long flags;

	hid_dbg(hdev, "arctic_fan: raw_event id=%u size=%d\n", report->id, size);

	if (report->id == ARCTIC_ACK_REPORT_ID && size == ARCTIC_ACK_REPORT_LEN) {
		spin_lock_irqsave(&priv->in_report_lock, flags);
		/*
		 * Only deliver if a write is in flight. This prevents a
		 * late-arriving ACK from a signal-interrupted write from
		 * erroneously satisfying a subsequent write's completion wait.
		 */
		if (priv->write_pending) {
			priv->ack_status = data[1] == 0x00 ? 0 : -EIO;
			complete(&priv->in_report_received);
		}
		spin_unlock_irqrestore(&priv->in_report_lock, flags);
		return 0;
	}

	if (report->id != ARCTIC_OUTPUT_REPORT_ID || size != ARCTIC_REPORT_LEN) {
		hid_dbg(hdev, "arctic_fan: raw_event id=%u size=%d ignored\n",
			report->id, size);
		return 0;
	}

	arctic_fan_parse_report(priv, data, size, ARCTIC_RPM_OFFSET);
	return 0;
}

static umode_t arctic_fan_is_visible(const void *data,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel)
{
	if (type == hwmon_fan && attr == hwmon_fan_input)
		return 0444;
	if (type == hwmon_pwm && attr == hwmon_pwm_input)
		return 0644;
	return 0;
}

static int arctic_fan_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct arctic_fan_data *priv = dev_get_drvdata(dev);
	unsigned long flags;

	if (type == hwmon_fan && attr == hwmon_fan_input) {
		spin_lock_irqsave(&priv->in_report_lock, flags);
		*val = priv->fan_rpm[channel];
		spin_unlock_irqrestore(&priv->in_report_lock, flags);
		return 0;
	}
	if (type == hwmon_pwm && attr == hwmon_pwm_input) {
		/* pwm_duty is modified only in write(), which the hwmon core serializes */
		*val = priv->pwm_duty[channel];
		return 0;
	}
	return -EINVAL;
}

static int arctic_fan_write(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long val)
{
	struct arctic_fan_data *priv = dev_get_drvdata(dev);
	u8 new_duty = (u8)clamp_val(val, 0, 255);
	unsigned long flags;
	long t;
	int i, ret;

	/*
	 * The hwmon core holds its lock for the duration of this callback,
	 * serializing concurrent writes. priv->buf is heap-allocated (embedded
	 * in the devm_kzalloc'd struct), satisfying usb_hcd_map_urb_for_dma().
	 *
	 * Use new_duty for the target channel and the cached value for all
	 * others. pwm_duty[channel] is committed only after a confirmed ACK so
	 * a failed or timed-out write does not corrupt the cached duty state.
	 */
	priv->buf[0] = ARCTIC_OUTPUT_REPORT_ID;
	for (i = 0; i < ARCTIC_NUM_FANS; i++)
		priv->buf[1 + i] = DIV_ROUND_CLOSEST(
			(unsigned int)(i == channel ? new_duty : priv->pwm_duty[i]) * 100, 255);

	/*
	 * Set write_pending before sending the report and clear it in every
	 * exit path. raw_event() calls complete() only when write_pending is
	 * true, so a stale ACK from a signal-interrupted write cannot
	 * satisfy the next write's completion wait.
	 * Serialized by the hwmon core: only one arctic_fan_write() at a time.
	 * Use irqsave to match the IRQ context in which raw_event may run.
	 */
	spin_lock_irqsave(&priv->in_report_lock, flags);
	priv->ack_status = -ETIMEDOUT;
	priv->write_pending = true;
	reinit_completion(&priv->in_report_received);
	spin_unlock_irqrestore(&priv->in_report_lock, flags);

	ret = hid_hw_output_report(priv->hdev, priv->buf, ARCTIC_REPORT_LEN);
	if (ret < 0) {
		spin_lock_irqsave(&priv->in_report_lock, flags);
		priv->write_pending = false;
		spin_unlock_irqrestore(&priv->in_report_lock, flags);
		return ret;
	}

	t = wait_for_completion_interruptible_timeout(&priv->in_report_received,
						      msecs_to_jiffies(ARCTIC_ACK_TIMEOUT_MS));
	spin_lock_irqsave(&priv->in_report_lock, flags);
	priv->write_pending = false;
	spin_unlock_irqrestore(&priv->in_report_lock, flags);

	if (t < 0)
		return t; /* interrupted by signal */
	if (!t)
		return -ETIMEDOUT;

	/* Commit the new duty only after hardware has acknowledged it */
	priv->pwm_duty[channel] = new_duty;
	return priv->ack_status; /* 0=OK, -EIO=device error */
}

static const struct hwmon_ops arctic_fan_ops = {
	.is_visible = arctic_fan_is_visible,
	.read = arctic_fan_read,
	.write = arctic_fan_write,
};

static const struct hwmon_channel_info *arctic_fan_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
			   HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
			   HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
			   HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info arctic_fan_chip_info = {
	.ops = &arctic_fan_ops,
	.info = arctic_fan_info,
};

static void arctic_fan_hw_stop(void *data)
{
	struct hid_device *hdev = data;

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int arctic_fan_probe(struct hid_device *hdev,
			    const struct hid_device_id *id)
{
	struct arctic_fan_data *priv;
	struct device *hwmon_dev;
	int ret;

	if (!hid_is_usb(hdev))
		return -ENODEV;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	spin_lock_init(&priv->in_report_lock);
	init_completion(&priv->in_report_received);
	hid_set_drvdata(hdev, priv);

	ret = hid_hw_start(hdev, HID_CONNECT_DRIVER);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	/*
	 * Register hardware teardown before hwmon so that devm cleanup runs in
	 * LIFO order: hwmon unregistered first, then hid_hw_close/stop. This
	 * ensures no userspace sysfs write can reach an already stopped device.
	 */
	ret = devm_add_action_or_reset(&hdev->dev, arctic_fan_hw_stop, hdev);
	if (ret)
		return ret;

	/*
	 * Start IO before registering with hwmon. If IO were started after
	 * hwmon registration, a sysfs write arriving in that narrow window
	 * would send an OUT report but the ACK could not be delivered (the HID
	 * core discards events until io_started), causing a spurious timeout.
	 */
	hid_device_io_start(hdev);

	hwmon_dev = devm_hwmon_device_register_with_info(&hdev->dev, "arctic_fan",
							 priv, &arctic_fan_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	return 0;
}

static void arctic_fan_remove(struct hid_device *hdev)
{
	/* devm cleanup (LIFO) handles hid_hw_close/stop after hwmon unregistration */
}

static const struct hid_device_id arctic_fan_id_table[] = {
	{ HID_USB_DEVICE(ARCTIC_VID, ARCTIC_PID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, arctic_fan_id_table);

static struct hid_driver arctic_fan_driver = {
	.name = "arctic_fan",
	.id_table = arctic_fan_id_table,
	.probe = arctic_fan_probe,
	.remove = arctic_fan_remove,
	.raw_event = arctic_fan_raw_event,
};

module_hid_driver(arctic_fan_driver);

MODULE_AUTHOR("Aureo Serrano de Souza <aureo.serrano@arctic.de>");
MODULE_DESCRIPTION("HID hwmon driver for ARCTIC Fan Controller");
MODULE_LICENSE("GPL");
