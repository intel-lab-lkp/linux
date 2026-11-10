// SPDX-License-Identifier: GPL-2.0
/*
 * HID driver for the keyboard backlight of Lenovo Legion laptops (2025).
 *
 * The ITE 8258 keyboard controller (048d:c197) is the built-in keyboard and
 * carries Lenovo's vendor lighting protocol: feature report 0x07, 960 bytes,
 * a request (struct legion_msg) answered by reading the report back with a
 * GET_FEATURE once the controller has prepared it (up to ~50 ms).
 *
 * The backlight brightness is op 0xCE (set) / 0xCD (get), 0..9, stored in the
 * controller; it applies to every LED of the machine (keys, front strip, rear
 * vents, lid logo). The firmware handles Fn+Up / Fn+Down itself and reports
 * the new level on vendor input report 5 (struct legion_event).
 *
 * The colours are the controller's own effects. The LEDs can also be handed
 * to the HID LampArray of the "Lenovo Lighting" device (048d:c193), which
 * this driver leaves to others.
 */

#include <linux/cleanup.h>
#include <linux/compiler.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/leds.h>
#include <linux/lockdep.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/byteorder.h>

#include "hid-ids.h"

#define LEGION_REPORT_ID		0x07
#define LEGION_REPORT_SIZE		960
#define LEGION_ANSWER_DELAY_MS		100

#define LEGION_OP_GET_BRIGHTNESS	0xcd
#define LEGION_OP_SET_BRIGHTNESS	0xce
#define LEGION_MAX_BRIGHTNESS		9

#define LEGION_EVENT_REPORT_ID		0x05
#define LEGION_EVENT_BRIGHTNESS_UP	0x02
#define LEGION_EVENT_BRIGHTNESS_DOWN	0x03

/* Request, and its answer: the value is params[0]. */
struct legion_msg {
	u8 report_id;
	u8 op;
	__le16 len;			/* of params */
	u8 params[];
} __packed;

struct legion_event {
	u8 report_id;
	u8 event;
	u8 level;			/* brightness after the change */
} __packed;

struct legion_kbd {
	struct hid_device *hdev;
	struct mutex lock;		/* one request/answer exchange at a time */
	struct legion_msg *msg;		/* LEGION_REPORT_SIZE, DMA-safe */
	struct led_classdev led;
};

/*
 * Send one request. When @answer is non-NULL, wait for the controller to
 * prepare its answer and return its value there.
 */
static int legion_exchange(struct legion_kbd *kbd, u8 op, const u8 *params,
			   u16 len, u8 *answer)
{
	struct hid_device *hdev = kbd->hdev;
	struct legion_msg *msg = kbd->msg;
	int ret;

	lockdep_assert_held(&kbd->lock);

	memset(msg, 0, LEGION_REPORT_SIZE);
	msg->report_id = LEGION_REPORT_ID;
	msg->op = op;
	msg->len = cpu_to_le16(len);
	memcpy(msg->params, params, len);

	ret = hid_hw_raw_request(hdev, LEGION_REPORT_ID, (u8 *)msg,
				 LEGION_REPORT_SIZE, HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;
	if (!answer)
		return 0;

	msleep(LEGION_ANSWER_DELAY_MS);

	memset(msg, 0, LEGION_REPORT_SIZE);
	ret = hid_hw_raw_request(hdev, LEGION_REPORT_ID, (u8 *)msg,
				 LEGION_REPORT_SIZE, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;
	if (ret <= (int)sizeof(*msg) || msg->report_id != LEGION_REPORT_ID ||
	    msg->op != op) {
		hid_warn(hdev, "unexpected answer to 0x%02x: %*ph\n", op,
			 min(ret, 8), msg);
		return -EIO;
	}

	*answer = msg->params[0];
	return 0;
}

static int legion_brightness_set(struct led_classdev *led,
				 enum led_brightness value)
{
	struct legion_kbd *kbd = container_of(led, struct legion_kbd, led);
	u8 level = min_t(unsigned int, value, LEGION_MAX_BRIGHTNESS);

	guard(mutex)(&kbd->lock);
	return legion_exchange(kbd, LEGION_OP_SET_BRIGHTNESS, &level,
			       sizeof(level), NULL);
}

static enum led_brightness legion_brightness_get(struct led_classdev *led)
{
	struct legion_kbd *kbd = container_of(led, struct legion_kbd, led);
	u8 zero = 0, value;
	int ret;

	guard(mutex)(&kbd->lock);
	ret = legion_exchange(kbd, LEGION_OP_GET_BRIGHTNESS, &zero,
			      sizeof(zero), &value);
	if (ret)
		return led->brightness;	/* keep the last known level */

	return min_t(unsigned int, value, LEGION_MAX_BRIGHTNESS);
}

static int legion_raw_event(struct hid_device *hdev, struct hid_report *report,
			    u8 *data, int size)
{
	const struct legion_event *ev = (const struct legion_event *)data;
	struct legion_kbd *kbd = hid_get_drvdata(hdev);
	unsigned int level;

	if (size < (int)sizeof(*ev) || ev->report_id != LEGION_EVENT_REPORT_ID)
		return 0;

	if (ev->event != LEGION_EVENT_BRIGHTNESS_UP &&
	    ev->event != LEGION_EVENT_BRIGHTNESS_DOWN)
		return 0;

	/*
	 * hid-core calls this only after probe() has returned and not once
	 * remove() has started, so the LED is registered. The controller has
	 * already changed the level: keep led_cdev.brightness in step with it.
	 */
	level = min_t(unsigned int, ev->level, LEGION_MAX_BRIGHTNESS);
	WRITE_ONCE(kbd->led.brightness, level);
	led_classdev_notify_brightness_hw_changed(&kbd->led, level);

	return 0;	/* let hid-core handle the report as usual */
}

static int legion_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct legion_kbd *kbd;
	int ret;

	/* As hid-generic: one input device per application collection. */
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	kbd = devm_kzalloc_obj(&hdev->dev, *kbd);
	if (!kbd)
		return -ENOMEM;

	kbd->msg = devm_kzalloc(&hdev->dev, LEGION_REPORT_SIZE, GFP_KERNEL);
	if (!kbd->msg)
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
	if (ret)
		goto err_stop;

	kbd->led.name = "legion::kbd_backlight";
	kbd->led.max_brightness = LEGION_MAX_BRIGHTNESS;
	kbd->led.brightness_set_blocking = legion_brightness_set;
	kbd->led.brightness_get = legion_brightness_get;
	/*
	 * LED_RETAIN_AT_SHUTDOWN: unregistering (unload, unbind) must not
	 * switch the backlight off, the controller would store 0.
	 * LED_BRIGHT_HW_CHANGED: Fn+Up/Down are reported to userspace.
	 */
	kbd->led.flags = LED_RETAIN_AT_SHUTDOWN | LED_BRIGHT_HW_CHANGED;

	/*
	 * Not devm: the LED must be gone before remove() stops the device,
	 * devm would release it only after that.
	 */
	ret = led_classdev_register(&hdev->dev, &kbd->led);
	if (ret)
		goto err_close;

	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void legion_remove(struct hid_device *hdev)
{
	struct legion_kbd *kbd = hid_get_drvdata(hdev);

	/* hid-core no longer calls legion_raw_event(): the LED can go. */
	led_classdev_unregister(&kbd->led);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
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
MODULE_DESCRIPTION("Keyboard backlight of Lenovo Legion laptops (2025)");
MODULE_LICENSE("GPL");
