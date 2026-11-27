// SPDX-License-Identifier: GPL-2.0
/*
 * HID driver for the keyboard backlight of Lenovo Legion laptops.
 *
 * The ITE 8258 keyboard controller (048d:c197) is the built-in keyboard and
 * carries Lenovo's vendor lighting protocol on feature report 0x07 (up to
 * 960 bytes): a request (struct legion_msg), answered by reading the report
 * back with a GET_FEATURE. Requests and answers may be shorter than the
 * report; the answer is ready as soon as the request has been sent.
 *
 * The backlight brightness is op 0xCE (set) / 0xCD (get), 0..9, stored in the
 * controller; it applies to every LED of the machine (keys, front strip, rear
 * vents, lid logo). The firmware handles Fn+Up / Fn+Down itself and reports
 * the new level on vendor input report 5 (struct legion_event). It reports
 * every other set the same way, also the host's and unchanged levels.
 *
 * The colours are the controller's own effects. The LEDs can also be handed
 * to the HID LampArray of the "Lenovo Lighting" device (048d:c193), which
 * this driver leaves to others.
 */

#include <linux/build_bug.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/leds.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/byteorder.h>
#include <asm/rwonce.h>

#include "hid-ids.h"

#define LEGION_REPORT_ID		0x07

#define LEGION_OP_GET_BRIGHTNESS	0xcd
#define LEGION_OP_SET_BRIGHTNESS	0xce
#define LEGION_MAX_BRIGHTNESS		9

#define LEGION_EVENT_REPORT_ID		0x05
#define LEGION_EVENT_BRIGHTNESS		0x02	/* set by the host, Fn+Up, Fn+Space */
#define LEGION_EVENT_BRIGHTNESS_DOWN	0x03	/* Fn+Down */

/* Request, and its answer: the value is params[0]. */
struct legion_msg {
	u8 report_id;
	u8 op;
	__le16 len;			/* of params */
	u8 params[];
} __packed;

#define LEGION_REPORT_PARAM_SIZE	956
#define LEGION_REPORT_SIZE		960
static_assert(struct_size_t(struct legion_msg, params, LEGION_REPORT_PARAM_SIZE) ==
	      LEGION_REPORT_SIZE);

struct legion_event {
	u8 report_id;
	u8 event;
	u8 level;			/* brightness after the change */
} __packed;

struct legion_kbd {
	struct hid_device *hdev;
	struct mutex lock;		/* one request/answer exchange at a time */
	struct led_classdev led;
	u8 level;			/* last level set, read or reported */
};

/*
 * Send one request. When @answer is non-NULL, read the controller's answer
 * and return its value there.
 */
static int legion_raw_command(struct legion_kbd *kbd, u8 op, const u8 *params,
			      size_t len, u8 *answer)
{
	size_t get_size = struct_size_t(struct legion_msg, params, 1);
	size_t set_size = struct_size_t(struct legion_msg, params, len);
	struct hid_device *hdev = kbd->hdev;
	int ret;

	if (len > LEGION_REPORT_PARAM_SIZE)
		return -EINVAL;

	/* Not on the stack: usbhid passes the buffer to the USB core for DMA. */
	struct legion_msg *msg __free(kfree) =
		kzalloc_flex(*msg, params, max(len, 1));
	if (!msg)
		return -ENOMEM;

	msg->report_id = LEGION_REPORT_ID;
	msg->op = op;
	msg->len = cpu_to_le16(len);
	memcpy(msg->params, params, len);

	guard(mutex)(&kbd->lock);

	ret = hid_hw_raw_request(hdev, LEGION_REPORT_ID, (u8 *)msg, set_size,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;
	if (!answer)
		return 0;

	memset(msg, 0, get_size);
	ret = hid_hw_raw_request(hdev, LEGION_REPORT_ID, (u8 *)msg, get_size,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;
	if (ret < get_size || msg->report_id != LEGION_REPORT_ID ||
	    msg->op != op) {
		hid_dbg(hdev, "unexpected answer to 0x%02x\n", op);
		return -EIO;
	}

	*answer = msg->params[0];
	return 0;
}

static int legion_simple_command(struct legion_kbd *kbd, u8 op, u8 param,
				 u8 *answer)
{
	return legion_raw_command(kbd, op, &param, sizeof(param), answer);
}

static int legion_brightness_set(struct led_classdev *led,
				 enum led_brightness value)
{
	struct legion_kbd *kbd = container_of(led, struct legion_kbd, led);
	u8 old = READ_ONCE(kbd->level);
	int ret;

	if (value > led->max_brightness)
		return -EINVAL;

	/* Before sending: the report of this set may come before we return. */
	WRITE_ONCE(kbd->level, value);
	ret = legion_simple_command(kbd, LEGION_OP_SET_BRIGHTNESS, value, NULL);
	if (ret)
		WRITE_ONCE(kbd->level, old);

	return ret;
}

static enum led_brightness legion_brightness_get(struct led_classdev *led)
{
	struct legion_kbd *kbd = container_of(led, struct legion_kbd, led);
	u8 value;
	int ret;

	ret = legion_simple_command(kbd, LEGION_OP_GET_BRIGHTNESS, 0, &value);
	if (ret) {
		hid_dbg(kbd->hdev, "reading the brightness failed: %d\n", ret);
		return ret;
	}

	if (value > led->max_brightness) {
		hid_dbg(kbd->hdev, "brightness out of range: %u\n", value);
		return -EIO;
	}

	WRITE_ONCE(kbd->level, value);
	return value;
}

static int legion_raw_event(struct hid_device *hdev, struct hid_report *report,
			    u8 *data, int size)
{
	const struct legion_event *ev = (const struct legion_event *)data;
	struct legion_kbd *kbd = hid_get_drvdata(hdev);

	if (size < sizeof(*ev) || ev->report_id != LEGION_EVENT_REPORT_ID)
		return 0;

	if (ev->event != LEGION_EVENT_BRIGHTNESS &&
	    ev->event != LEGION_EVENT_BRIGHTNESS_DOWN)
		return 0;

	if (ev->level > kbd->led.max_brightness) {
		hid_dbg(hdev, "brightness event out of range: %u\n", ev->level);
		return 0;
	}

	/*
	 * Only a change of the level is the hardware's own: the host's sets
	 * and Fn+Space are reported too, with the level already known.
	 */
	if (ev->level == READ_ONCE(kbd->level))
		return 0;
	WRITE_ONCE(kbd->level, ev->level);

	/*
	 * hid-core calls this only after probe() has returned and not once
	 * remove() has started, so the LED is registered.
	 */
	led_classdev_notify_brightness_hw_changed(&kbd->led, ev->level);

	return 0;	/* let hid-core handle the report as usual */
}

static void legion_stop(void *data)
{
	struct hid_device *hdev = data;

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int legion_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct legion_kbd *kbd;
	int ret;

	/*
	 * Keep the input devices hid-generic creates for this keyboard (which
	 * sets the same quirk): without it the keyboard, its consumer keys and
	 * its radio button would become one input device.
	 */
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	kbd = devm_kzalloc_obj(&hdev->dev, *kbd);
	if (!kbd)
		return -ENOMEM;

	ret = devm_mutex_init(&hdev->dev, &kbd->lock);
	if (ret)
		return ret;

	kbd->hdev = hdev;
	hid_set_drvdata(hdev, kbd);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	/* Keep the interrupt endpoint polled so hotkey reports always arrive. */
	ret = hid_hw_open(hdev);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	ret = devm_add_action_or_reset(&hdev->dev, legion_stop, hdev);
	if (ret)
		return ret;

	kbd->led.name = "legion::kbd_backlight";
	kbd->led.max_brightness = LEGION_MAX_BRIGHTNESS;
	kbd->led.brightness_set_blocking = legion_brightness_set;
	kbd->led.brightness_get = legion_brightness_get;
	kbd->led.flags = LED_RETAIN_AT_SHUTDOWN | LED_BRIGHT_HW_CHANGED;

	return devm_led_classdev_register(&hdev->dev, &kbd->led);
}

static void legion_remove(struct hid_device *hdev)
{
	/*
	 * Done via devm, the LED first. Without .remove, hid-core would stop
	 * the device before releasing devm resources: before the LED is gone,
	 * and twice.
	 */
}

static const struct hid_device_id legion_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, USB_DEVICE_ID_ITE_LENOVO_LEGION_GEN10_KBD) },
	{ }
};
MODULE_DEVICE_TABLE(hid, legion_devices);

static struct hid_driver legion_driver = {
	.name = "hid-lenovo-legion",
	.id_table = legion_devices,
	.probe = legion_probe,
	.remove = legion_remove,
	.raw_event = legion_raw_event,
};
module_hid_driver(legion_driver);

MODULE_AUTHOR("Ciro Pedrini <ciro.pedrini@gmail.com>");
MODULE_DESCRIPTION("Keyboard backlight of Lenovo Legion laptops");
MODULE_LICENSE("GPL");
