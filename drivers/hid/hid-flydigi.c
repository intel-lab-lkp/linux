// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Flydigi Vader 5 Pro HID driver
 *
 * Exposes two evdev nodes with matching uniq values:
 * - Gamepad with force feedback
 * - Motion sensors (accelerometer + gyroscope)
 *
 * The protocol details and report layout are based on upstream SDL's
 * Flydigi HID backend.
 */

#include <linux/cleanup.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "hid-ids.h"

#define FLYDIGI_REPORT_SIZE		32

#define FLYDIGI_MAGIC1			0x5a
#define FLYDIGI_MAGIC2			0xa5
#define FLYDIGI_CMD_INFO		0x01
#define FLYDIGI_CMD_STATUS		0x10
#define FLYDIGI_CMD_STATUS_UPDATE	0x11
#define FLYDIGI_CMD_HAPTIC		0x12
#define FLYDIGI_CMD_ACQUIRE		0x1c
#define FLYDIGI_CMD_INPUT		0xef

#define FLYDIGI_ACQUIRE_PERIOD_MS	30000

/* SDL identifies this class as 4096 counts per g and 2000 dps full-scale. */
#define FLYDIGI_ACCEL_RES_PER_G	4096
#define FLYDIGI_GYRO_RES_PER_DPS	16

/* Protocol packet offsets (payload starts at magic byte 0x5a). */
#define FLYDIGI_OFF_LX			3
#define FLYDIGI_OFF_LY			5
#define FLYDIGI_OFF_RX			7
#define FLYDIGI_OFF_RY			9
#define FLYDIGI_OFF_DPAD_ABXY		11
#define FLYDIGI_OFF_MISC_BTNS		12
#define FLYDIGI_OFF_EXTRA_BTNS		13
#define FLYDIGI_OFF_SYSTEM_BTNS	14
#define FLYDIGI_OFF_LT			15
#define FLYDIGI_OFF_RT			16
#define FLYDIGI_OFF_GYRO_X		17
#define FLYDIGI_OFF_GYRO_Z		19
#define FLYDIGI_OFF_GYRO_Y		21
#define FLYDIGI_OFF_ACCEL_X		23
#define FLYDIGI_OFF_ACCEL_Z		25
#define FLYDIGI_OFF_ACCEL_Y		27

struct flydigi_device {
	struct hid_device *hdev;
	struct input_dev *gamepad;
	struct input_dev *sensors;

	struct mutex output_mutex;
	spinlock_t lock;
	u8 output_buf[FLYDIGI_REPORT_SIZE];
	u8 output_report_id;

	struct work_struct rumble_work;
	struct delayed_work acquire_work;

	u16 strong;
	u16 weak;
	bool gamepad_open;
	bool removed;
};

/**
 * flydigi_send_output - send one vendor packet through the selected report
 * @fd: controller state
 * @payload: protocol bytes without the HID report ID prefix
 * @payload_len: number of bytes in @payload
 *
 * Vader firmware variants expose either numbered or unnumbered output reports.
 * Descriptor probing selects one report ID at probe time, and all later writes
 * use that framing so the same packet builders work for both variants.
 *
 * Return: number of bytes transferred, or a negative error code.
 */
static int flydigi_send_output(struct flydigi_device *fd, const u8 *payload,
			       size_t payload_len)
{
	u8 *buf = fd->output_buf;
	size_t copy_len;
	bool no_output_report;
	int ret;

	copy_len = min(payload_len, (size_t)FLYDIGI_REPORT_SIZE - 1);

	no_output_report = !fd->hdev->ll_driver ||
			   !fd->hdev->ll_driver->output_report;

	{
		guard(mutex)(&fd->output_mutex);

		memset(buf, 0, FLYDIGI_REPORT_SIZE);
		buf[0] = fd->output_report_id;
		memcpy(&buf[1], payload, copy_len);
		ret = hid_hw_output_report(fd->hdev, buf, FLYDIGI_REPORT_SIZE);
		if (ret == -EOPNOTSUPP || (ret < 0 && no_output_report)) {
			ret = hid_hw_raw_request(fd->hdev, fd->output_report_id, buf,
						 FLYDIGI_REPORT_SIZE, HID_OUTPUT_REPORT,
						 HID_REQ_SET_REPORT);
		}
	}

	if (ret < 0 && ret != -ENODEV)
		hid_dbg(fd->hdev, "output command failed: %d\n", ret);

	return ret;
}

static bool flydigi_desc_has_seq(const u8 *rdesc, unsigned int rsize,
				 const u8 *seq, size_t seq_size)
{
	size_t i;

	if (!rdesc || !rsize || !seq || !seq_size || rsize < seq_size)
		return false;

	for (i = 0; i <= rsize - seq_size; i++) {
		if (!memcmp(&rdesc[i], seq, seq_size))
			return true;
	}

	return false;
}

/**
 * flydigi_detect_protocol_descriptor - identify protocol interface layout
 * @hdev: HID device being probed
 * @output_report_id: resolved output report ID for protocol writes
 *
 * The controller exports multiple interfaces and not all of them transport the
 * extended Flydigi protocol. We key off descriptor signatures instead of fixed
 * interface numbers so the driver keeps working across firmware revisions.
 *
 * Return: true when the interface looks like the Flydigi protocol endpoint.
 */
static bool flydigi_detect_protocol_descriptor(struct hid_device *hdev,
					      u8 *output_report_id)
{
	const u8 *rdesc = hdev->dev_rdesc;
	unsigned int rsize = hdev->dev_rsize;
	static const u8 usage_ffa0[] = { 0x06, 0xa0, 0xff };
	static const u8 report_id_21[] = { 0x85, 0x21, 0x95, 0x1f };
	static const u8 report_32_in[] = { 0x95, 0x20, 0x81 };
	static const u8 report_32_out[] = { 0x95, 0x20, 0x91 };

	if (!flydigi_desc_has_seq(rdesc, rsize, usage_ffa0, sizeof(usage_ffa0)))
		return false;

	if (flydigi_desc_has_seq(rdesc, rsize, report_id_21,
				 sizeof(report_id_21))) {
		*output_report_id = 0x21;
		return true;
	}

	if (flydigi_desc_has_seq(rdesc, rsize, report_32_in,
				 sizeof(report_32_in)) &&
	    flydigi_desc_has_seq(rdesc, rsize, report_32_out,
				 sizeof(report_32_out))) {
		*output_report_id = 0x00;
		return true;
	}

	return false;
}

static int flydigi_get_interface_number(struct hid_device *hdev)
{
	struct usb_interface *intf;
	struct usb_host_interface *alt;

	if (!hid_is_usb(hdev))
		return -ENODEV;

	intf = to_usb_interface(hdev->dev.parent);
	if (!intf)
		return -ENODEV;

	alt = intf->cur_altsetting;
	if (!alt)
		return -ENODEV;

	return alt->desc.bInterfaceNumber;
}

/**
 * flydigi_send_acquire - request ownership of the controller protocol stream
 * @fd: controller state
 *
 * The firmware requires an explicit acquire token and periodic refreshes before
 * delivering full input and motion data.
 *
 * Return: number of bytes transferred, or a negative error code.
 */
static int flydigi_send_acquire(struct flydigi_device *fd)
{
	const u8 payload[31] = {
		FLYDIGI_MAGIC1,
		FLYDIGI_MAGIC2,
		FLYDIGI_CMD_ACQUIRE,
		23,
		1,
		'S', 'D', 'L'
	};

	return flydigi_send_output(fd, payload, sizeof(payload));
}

static int flydigi_send_info_request(struct flydigi_device *fd)
{
	const u8 payload[] = {
		FLYDIGI_MAGIC1,
		FLYDIGI_MAGIC2,
		FLYDIGI_CMD_INFO,
		2,
		0,
	};

	return flydigi_send_output(fd, payload, sizeof(payload));
}

static int flydigi_send_status_request(struct flydigi_device *fd)
{
	const u8 payload[] = {
		FLYDIGI_MAGIC1,
		FLYDIGI_MAGIC2,
		FLYDIGI_CMD_STATUS,
	};

	return flydigi_send_output(fd, payload, sizeof(payload));
}

static int flydigi_send_rumble(struct flydigi_device *fd, u8 low, u8 high)
{
	const u8 payload[] = {
		FLYDIGI_MAGIC1,
		FLYDIGI_MAGIC2,
		FLYDIGI_CMD_HAPTIC,
		6,
		low,
		high,
		0,
		0,
		0,
	};

	return flydigi_send_output(fd, payload, sizeof(payload));
}

static void flydigi_rumble_worker(struct work_struct *work)
{
	struct flydigi_device *fd = container_of(work, struct flydigi_device,
						 rumble_work);
	u16 strong;
	u16 weak;
	bool removed;

	{
		guard(spinlock_irqsave)(&fd->lock);

		strong = fd->strong;
		weak = fd->weak;
		removed = fd->removed;
	}

	if (removed)
		return;

	flydigi_send_rumble(fd, strong >> 8, weak >> 8);
}

/**
 * flydigi_acquire_worker - keep the protocol session alive while opened
 * @work: delayed work item
 *
 * The device times out its acquired state. Refreshing acquire/info/status keeps
 * the stream active and prevents stale state after userspace idles briefly.
 */
static void flydigi_acquire_worker(struct work_struct *work)
{
	struct flydigi_device *fd = container_of(to_delayed_work(work),
						 struct flydigi_device,
						 acquire_work);
	bool open;
	bool removed;

	{
		guard(spinlock_irqsave)(&fd->lock);

		open = fd->gamepad_open;
		removed = fd->removed;
	}

	if (removed || !open)
		return;

	flydigi_send_acquire(fd);
	flydigi_send_info_request(fd);
	flydigi_send_status_request(fd);
	schedule_delayed_work(&fd->acquire_work,
			      msecs_to_jiffies(FLYDIGI_ACQUIRE_PERIOD_MS));
}

static int flydigi_play_effect(struct input_dev *dev, void *data,
			      struct ff_effect *effect)
{
	struct flydigi_device *fd = input_get_drvdata(dev);

	if (effect->type != FF_RUMBLE)
		return 0;

	{
		guard(spinlock_irqsave)(&fd->lock);

		if (!fd->removed) {
			fd->strong = effect->u.rumble.strong_magnitude;
			fd->weak = effect->u.rumble.weak_magnitude;
			schedule_work(&fd->rumble_work);
		}
	}

	return 0;
}

static int flydigi_gamepad_open(struct input_dev *dev)
{
	struct flydigi_device *fd = input_get_drvdata(dev);

	{
		guard(spinlock_irqsave)(&fd->lock);

		fd->gamepad_open = true;
	}

	schedule_delayed_work(&fd->acquire_work, 0);

	return 0;
}

static void flydigi_gamepad_close(struct input_dev *dev)
{
	struct flydigi_device *fd = input_get_drvdata(dev);

	cancel_delayed_work_sync(&fd->acquire_work);

	{
		guard(spinlock_irqsave)(&fd->lock);

		fd->gamepad_open = false;
		fd->strong = 0;
		fd->weak = 0;
	}

	schedule_work(&fd->rumble_work);
}

static void flydigi_map_hat(u8 value, int *x, int *y)
{
	switch (value & 0x0f) {
	case 0x01:
		*x = 0;
		*y = -1;
		break;
	case 0x03:
		*x = 1;
		*y = -1;
		break;
	case 0x02:
		*x = 1;
		*y = 0;
		break;
	case 0x06:
		*x = 1;
		*y = 1;
		break;
	case 0x04:
		*x = 0;
		*y = 1;
		break;
	case 0x0c:
		*x = -1;
		*y = 1;
		break;
	case 0x08:
		*x = -1;
		*y = 0;
		break;
	case 0x09:
		*x = -1;
		*y = -1;
		break;
	default:
		*x = 0;
		*y = 0;
		break;
	}
}

/**
 * flydigi_report_gamepad - translate one protocol input packet to evdev keys
 * @fd: controller state
 * @data: protocol packet starting at magic byte 0x5a
 *
 * Keep this mapping aligned with SDL's Flydigi backend so userspace sees the
 * same logical layout across hidraw and evdev paths.
 */
static void flydigi_report_gamepad(struct flydigi_device *fd, const u8 *data)
{
	struct input_dev *gamepad = fd->gamepad;
	s16 axis;
	int hat_x;
	int hat_y;

	flydigi_map_hat(data[FLYDIGI_OFF_DPAD_ABXY], &hat_x, &hat_y);
	input_report_abs(gamepad, ABS_HAT0X, hat_x);
	input_report_abs(gamepad, ABS_HAT0Y, hat_y);

	input_report_key(gamepad, BTN_SOUTH,
			 data[FLYDIGI_OFF_DPAD_ABXY] & BIT(4));
	input_report_key(gamepad, BTN_EAST,
			 data[FLYDIGI_OFF_DPAD_ABXY] & BIT(5));
	/*
	 * Keep BTN_WEST/BTN_NORTH aligned with printed X/Y legends used by
	 * applications. On-wire encoding for X/Y is swapped on this model.
	 */
	input_report_key(gamepad, BTN_WEST,
			 data[FLYDIGI_OFF_MISC_BTNS] & BIT(0));
	input_report_key(gamepad, BTN_NORTH,
			 data[FLYDIGI_OFF_DPAD_ABXY] & BIT(7));
	input_report_key(gamepad, BTN_SELECT,
			 data[FLYDIGI_OFF_DPAD_ABXY] & BIT(6));
	input_report_key(gamepad, BTN_START,
			 data[FLYDIGI_OFF_MISC_BTNS] & BIT(1));
	input_report_key(gamepad, BTN_TL,
			 data[FLYDIGI_OFF_MISC_BTNS] & BIT(2));
	input_report_key(gamepad, BTN_TR,
			 data[FLYDIGI_OFF_MISC_BTNS] & BIT(3));
	input_report_key(gamepad, BTN_THUMBL,
			 data[FLYDIGI_OFF_MISC_BTNS] & BIT(6));
	input_report_key(gamepad, BTN_THUMBR,
			 data[FLYDIGI_OFF_MISC_BTNS] & BIT(7));

	input_report_key(gamepad, BTN_MODE,
			 data[FLYDIGI_OFF_SYSTEM_BTNS] & BIT(3));

	/*
	 * SDL's generic Linux evdev mapper recognizes paddles specifically on
	 * BTN_TRIGGER_HAPPY5..8, so keep M1..M4 on that range for Steam/SDL.
	 */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY1,
			 data[FLYDIGI_OFF_EXTRA_BTNS] & BIT(0)); /* C */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY2,
			 data[FLYDIGI_OFF_EXTRA_BTNS] & BIT(1)); /* Z */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY5,
			 data[FLYDIGI_OFF_EXTRA_BTNS] & BIT(2)); /* M1 */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY6,
			 data[FLYDIGI_OFF_EXTRA_BTNS] & BIT(3)); /* M2 */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY7,
			 data[FLYDIGI_OFF_EXTRA_BTNS] & BIT(4)); /* M3 */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY8,
			 data[FLYDIGI_OFF_EXTRA_BTNS] & BIT(5)); /* M4 */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY9,
			 data[FLYDIGI_OFF_EXTRA_BTNS] & BIT(6)); /* LM */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY10,
			 data[FLYDIGI_OFF_EXTRA_BTNS] & BIT(7)); /* RM */
	input_report_key(gamepad, BTN_TRIGGER_HAPPY11,
			 data[FLYDIGI_OFF_SYSTEM_BTNS] & BIT(0)); /* Circle */

	axis = (s16)get_unaligned_le16(&data[FLYDIGI_OFF_LX]);
	input_report_abs(gamepad, ABS_X, axis);

	axis = -(s16)get_unaligned_le16(&data[FLYDIGI_OFF_LY]);
	if (axis <= -32768)
		axis = 32767;
	input_report_abs(gamepad, ABS_Y, axis);

	axis = (s16)get_unaligned_le16(&data[FLYDIGI_OFF_RX]);
	input_report_abs(gamepad, ABS_RX, axis);

	axis = -(s16)get_unaligned_le16(&data[FLYDIGI_OFF_RY]);
	if (axis <= -32768)
		axis = 32767;
	input_report_abs(gamepad, ABS_RY, axis);

	input_report_abs(gamepad, ABS_Z,
			 ((int)data[FLYDIGI_OFF_LT] * 257) - 32768);
	input_report_abs(gamepad, ABS_RZ,
			 ((int)data[FLYDIGI_OFF_RT] * 257) - 32768);

	input_sync(gamepad);
}

/**
 * flydigi_report_sensors - publish IMU samples from one protocol packet
 * @fd: controller state
 * @data: protocol packet starting at magic byte 0x5a
 *
 * Motion data is exposed as a dedicated evdev node so userspace can consume
 * gyro/accelerometer data independently of gamepad button polling.
 */
static void flydigi_report_sensors(struct flydigi_device *fd, const u8 *data)
{
	struct input_dev *sensors = fd->sensors;

	if (!sensors)
		return;

	input_event(sensors, EV_MSC, MSC_TIMESTAMP,
		    ktime_to_us(ktime_get_boottime()));

	/* Accelerometer */
	input_report_abs(sensors, ABS_X,
			 (s16)get_unaligned_le16(&data[FLYDIGI_OFF_ACCEL_X]));
	input_report_abs(sensors, ABS_Y,
			 (s16)get_unaligned_le16(&data[FLYDIGI_OFF_ACCEL_Y]));
	input_report_abs(sensors, ABS_Z,
			 -(s16)get_unaligned_le16(&data[FLYDIGI_OFF_ACCEL_Z]));

	/* Gyroscope */
	input_report_abs(sensors, ABS_RX,
			 (s16)get_unaligned_le16(&data[FLYDIGI_OFF_GYRO_X]));
	input_report_abs(sensors, ABS_RY,
			 (s16)get_unaligned_le16(&data[FLYDIGI_OFF_GYRO_Y]));
	input_report_abs(sensors, ABS_RZ,
			 -(s16)get_unaligned_le16(&data[FLYDIGI_OFF_GYRO_Z]));

	input_sync(sensors);
}

/**
 * flydigi_raw_event - parse vendor packets delivered by HID core
 * @hdev: HID device
 * @report: HID report metadata
 * @data: raw bytes from device
 * @size: number of bytes in @data
 *
 * The transport may prepend a synthetic report ID byte. Strip it when present,
 * then dispatch by vendor command so gamepad and sensor state stay in sync.
 *
 * Return: always 0 so hidraw still sees the original traffic.
 */
static int flydigi_raw_event(struct hid_device *hdev, struct hid_report *report,
			     u8 *data, int size)
{
	struct flydigi_device *fd = hid_get_drvdata(hdev);
	const u8 *payload = data;
	int len = size;
	(void)report;

	if (!fd || size <= 0)
		return 0;

	if (payload[0] != FLYDIGI_MAGIC1) {
		payload++;
		len--;
	}

	if (len < 31)
		return 0;
	if (payload[0] != FLYDIGI_MAGIC1 || payload[1] != FLYDIGI_MAGIC2) {
		hid_dbg(hdev,
			"non-protocol packet ignored: sz=%d b0=%02x b1=%02x b2=%02x\n",
			len, payload[0], payload[1], payload[2]);
		return 0;
	}

	switch (payload[2]) {
	case FLYDIGI_CMD_INPUT:
		flydigi_report_gamepad(fd, payload);
		flydigi_report_sensors(fd, payload);
		break;
	case FLYDIGI_CMD_INFO:
	case FLYDIGI_CMD_STATUS:
	case FLYDIGI_CMD_ACQUIRE:
	case FLYDIGI_CMD_HAPTIC:
		/* Expected replies/acks to commands sent by this driver. */
		break;
	case FLYDIGI_CMD_STATUS_UPDATE:
		flydigi_send_status_request(fd);
		schedule_delayed_work(&fd->acquire_work, 0);
		break;
	default:
		hid_dbg(hdev, "unhandled command: cmd=%02x sz=%d\n",
			payload[2], len);
		break;
	}

	return 0;
}

static int flydigi_register_gamepad(struct flydigi_device *fd)
{
	struct hid_device *hdev = fd->hdev;
	struct input_dev *gamepad;
	int ret;

	gamepad = devm_input_allocate_device(&hdev->dev);
	if (!gamepad)
		return -ENOMEM;

	gamepad->name = hdev->name;
	gamepad->phys = hdev->phys;
	gamepad->uniq = hdev->uniq;
	gamepad->id.bustype = hdev->bus;
	gamepad->id.vendor = hdev->vendor;
	gamepad->id.product = hdev->product;
	gamepad->id.version = hdev->version;
	gamepad->open = flydigi_gamepad_open;
	gamepad->close = flydigi_gamepad_close;
	input_set_drvdata(gamepad, fd);

	input_set_capability(gamepad, EV_KEY, BTN_SOUTH);
	input_set_capability(gamepad, EV_KEY, BTN_EAST);
	input_set_capability(gamepad, EV_KEY, BTN_WEST);
	input_set_capability(gamepad, EV_KEY, BTN_NORTH);
	input_set_capability(gamepad, EV_KEY, BTN_SELECT);
	input_set_capability(gamepad, EV_KEY, BTN_START);
	input_set_capability(gamepad, EV_KEY, BTN_MODE);
	input_set_capability(gamepad, EV_KEY, BTN_TL);
	input_set_capability(gamepad, EV_KEY, BTN_TR);
	input_set_capability(gamepad, EV_KEY, BTN_THUMBL);
	input_set_capability(gamepad, EV_KEY, BTN_THUMBR);

	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY1);
	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY2);
	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY5);
	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY6);
	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY7);
	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY8);
	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY9);
	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY10);
	input_set_capability(gamepad, EV_KEY, BTN_TRIGGER_HAPPY11);

	input_set_abs_params(gamepad, ABS_X, -32768, 32767, 0, 0);
	input_set_abs_params(gamepad, ABS_Y, -32768, 32767, 0, 0);
	input_set_abs_params(gamepad, ABS_RX, -32768, 32767, 0, 0);
	input_set_abs_params(gamepad, ABS_RY, -32768, 32767, 0, 0);
	input_set_abs_params(gamepad, ABS_Z, -32768, 32767, 0, 0);
	input_set_abs_params(gamepad, ABS_RZ, -32768, 32767, 0, 0);
	input_set_abs_params(gamepad, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(gamepad, ABS_HAT0Y, -1, 1, 0, 0);

	input_set_capability(gamepad, EV_FF, FF_RUMBLE);
	ret = input_ff_create_memless(gamepad, NULL, flydigi_play_effect);
	if (ret)
		return ret;

	ret = input_register_device(gamepad);
	if (ret)
		return ret;

	fd->gamepad = gamepad;
	return 0;
}

static int flydigi_register_sensors(struct flydigi_device *fd)
{
	struct hid_device *hdev = fd->hdev;
	struct input_dev *sensors;
	char *name;
	int ret;

	sensors = devm_input_allocate_device(&hdev->dev);
	if (!sensors)
		return -ENOMEM;

	name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "%s Motion Sensors",
			      hdev->name);
	if (!name)
		return -ENOMEM;

	sensors->name = name;
	/* Matching phys/uniq helps userspace pair gamepad and IMU endpoints. */
	sensors->phys = hdev->phys;
	sensors->uniq = hdev->uniq;
	sensors->id.bustype = hdev->bus;
	sensors->id.vendor = hdev->vendor;
	sensors->id.product = hdev->product;
	sensors->id.version = hdev->version;
	input_set_drvdata(sensors, fd);

	__set_bit(INPUT_PROP_ACCELEROMETER, sensors->propbit);
	__set_bit(EV_MSC, sensors->evbit);
	__set_bit(MSC_TIMESTAMP, sensors->mscbit);

	input_set_abs_params(sensors, ABS_X, -32768, 32767, 16, 0);
	input_set_abs_params(sensors, ABS_Y, -32768, 32767, 16, 0);
	input_set_abs_params(sensors, ABS_Z, -32768, 32767, 16, 0);
	input_abs_set_res(sensors, ABS_X, FLYDIGI_ACCEL_RES_PER_G);
	input_abs_set_res(sensors, ABS_Y, FLYDIGI_ACCEL_RES_PER_G);
	input_abs_set_res(sensors, ABS_Z, FLYDIGI_ACCEL_RES_PER_G);

	input_set_abs_params(sensors, ABS_RX, -32768, 32767, 16, 0);
	input_set_abs_params(sensors, ABS_RY, -32768, 32767, 16, 0);
	input_set_abs_params(sensors, ABS_RZ, -32768, 32767, 16, 0);
	input_abs_set_res(sensors, ABS_RX, FLYDIGI_GYRO_RES_PER_DPS);
	input_abs_set_res(sensors, ABS_RY, FLYDIGI_GYRO_RES_PER_DPS);
	input_abs_set_res(sensors, ABS_RZ, FLYDIGI_GYRO_RES_PER_DPS);

	ret = input_register_device(sensors);
	if (ret)
		return ret;

	fd->sensors = sensors;
	return 0;
}

/**
 * flydigi_init_uniq - provide a stable unique string for endpoint pairing
 * @hdev: HID device
 *
 * Userspace often merges controller and sensor nodes only when uniq matches.
 * Populate uniq early when firmware does not provide one.
 */
static void flydigi_init_uniq(struct hid_device *hdev)
{
	struct usb_interface *intf;
	struct usb_device *udev;
	char path[64];
	int ret;

	/*
	 * Some Flydigi units expose a static USB serial string, which is not
	 * unique across controllers. Use the USB topology path so userspace can
	 * pair gamepad and sensor nodes per physical device.
	 */
	if (hid_is_usb(hdev)) {
		intf = to_usb_interface(hdev->dev.parent);
		if (intf) {
			udev = interface_to_usbdev(intf);
			if (udev) {
				ret = usb_make_path(udev, path, sizeof(path));
				if (ret > 0) {
					strscpy(hdev->uniq, path, sizeof(hdev->uniq));
					return;
				}
			}
		}
	}

	if (hdev->uniq[0])
		return;

	if (hdev->phys[0]) {
		strscpy(hdev->uniq, hdev->phys, sizeof(hdev->uniq));
		return;
	}

	snprintf(hdev->uniq, sizeof(hdev->uniq), "%04x:%04x",
		 hdev->vendor, hdev->product);
}

static int flydigi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct flydigi_device *fd;
	u8 output_report_id = 0;
	int ifnum;
	int ret;

	ifnum = flydigi_get_interface_number(hdev);
	if (ifnum < 0)
		return -ENODEV;

	if (!flydigi_detect_protocol_descriptor(hdev, &output_report_id))
		return -ENODEV;

	hid_dbg(hdev, "binding protocol interface %d (report id 0x%02x)\n",
		ifnum, output_report_id);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed: %d\n", ret);
		return ret;
	}

	fd = devm_kzalloc(&hdev->dev, sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return -ENOMEM;

	fd->hdev = hdev;
	fd->output_report_id = output_report_id;
	hid_set_drvdata(hdev, fd);

	spin_lock_init(&fd->lock);
	mutex_init(&fd->output_mutex);
	INIT_WORK(&fd->rumble_work, flydigi_rumble_worker);
	INIT_DELAYED_WORK(&fd->acquire_work, flydigi_acquire_worker);

	flydigi_init_uniq(hdev);

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hw start failed: %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hw open failed: %d\n", ret);
		hid_hw_stop(hdev);
		return ret;
	}

	ret = flydigi_register_gamepad(fd);
	if (ret)
		goto err_close;

	ret = flydigi_register_sensors(fd);
	if (ret)
		goto err_close;

	/*
	 * Prime protocol state so input/IMU packets start flowing immediately.
	 * Best-effort: some devices transiently NAK these while initialization
	 * settles, and the periodic acquire worker will retry shortly.
	 */
	(void)flydigi_send_info_request(fd);
	(void)flydigi_send_status_request(fd);
	(void)flydigi_send_acquire(fd);

	return 0;

err_close:
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	cancel_delayed_work_sync(&fd->acquire_work);
	cancel_work_sync(&fd->rumble_work);
	mutex_destroy(&fd->output_mutex);
	return ret;
}

static void flydigi_remove(struct hid_device *hdev)
{
	struct flydigi_device *fd = hid_get_drvdata(hdev);

	{
		guard(spinlock_irqsave)(&fd->lock);

		fd->removed = true;
		fd->strong = 0;
		fd->weak = 0;
	}

	cancel_delayed_work_sync(&fd->acquire_work);
	cancel_work_sync(&fd->rumble_work);

	/* Stop motors on teardown to avoid controllers that keep last FF state. */
	flydigi_send_rumble(fd, 0, 0);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	mutex_destroy(&fd->output_mutex);
}

static const struct hid_device_id flydigi_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_FLYDIGI, USB_DEVICE_ID_FLYDIGI_VADER5) },
	{ }
};
MODULE_DEVICE_TABLE(hid, flydigi_devices);

static struct hid_driver flydigi_driver = {
	.name = "flydigi",
	.id_table = flydigi_devices,
	.probe = flydigi_probe,
	.remove = flydigi_remove,
	.raw_event = flydigi_raw_event,
};
module_hid_driver(flydigi_driver);

MODULE_AUTHOR("Denis Benato <denis.benato@linux.dev>");
MODULE_DESCRIPTION("Flydigi HID driver");
MODULE_LICENSE("GPL");
