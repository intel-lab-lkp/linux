// SPDX-License-Identifier: GPL-2.0
/*
 * HID driver for the keyboard lighting of Lenovo Legion laptops (2025).
 *
 * Two USB devices form the lighting:
 *
 * - The ITE 8258 keyboard controller (048d:c197) is the built-in keyboard and
 *   carries Lenovo's vendor lighting protocol: feature report 0x07, 960 bytes,
 *   request "07 op len_lo len_hi params...", answer read back with a
 *   GET_FEATURE as "07 op len_lo len_hi value..." once the controller has
 *   prepared it (up to ~50 ms). Brightness is op 0xCE (set) / 0xCD (get),
 *   0..9. Op 0xAA selects who drives the LEDs: 01 the controller's own
 *   effects, 00 the LampArray below. Both are stored in the controller. The
 *   firmware handles Fn+Up / Fn+Down and reports the new level on vendor
 *   input report 5: "05 02 B" (raised) / "05 03 B" (lowered).
 *
 * - "Lenovo Lighting" (048d:c193) interface 1 is a standard HID LampArray
 *   covering every LED (keys, front strip, rear vents, lid logo). It only
 *   shows anything once the controller hands the LEDs over (0xAA 00), its
 *   colours are scaled by the controller's brightness, and after resume from
 *   S3 it comes back reset, in autonomous mode.
 *
 * Userspace sees one multicolor LED: brightness is the controller's, the
 * colour goes to every lamp. Its lighting_mode attribute says who drives the
 * LEDs: "firmware" (the controller's own effects; the driver leaves the LEDs
 * alone, so a userspace tool may drive the LampArray itself) or "color" (the
 * driver shows multi_intensity on every lamp). It starts as "firmware";
 * writing a new colour to multi_intensity switches to "color".
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "hid-ids.h"

/* Keyboard controller, vendor protocol */
#define LEGION_REPORT_ID		0x07
#define LEGION_REPORT_SIZE		960
#define LEGION_ANSWER_DELAY_MS		100

#define LEGION_OP_GET_BRIGHTNESS	0xcd
#define LEGION_OP_SET_BRIGHTNESS	0xce
#define LEGION_OP_LIGHTING_OWNER	0xaa
#define LEGION_OWNER_LAMPARRAY		0x00
#define LEGION_OWNER_FIRMWARE		0x01
#define LEGION_MAX_BRIGHTNESS		9

#define LEGION_EVENT_REPORT_ID		0x05
#define LEGION_EVENT_BRIGHTNESS_UP	0x02
#define LEGION_EVENT_BRIGHTNESS_DOWN	0x03

/* LampArray (HID Usage Tables, Lighting and Illumination page 0x59) */
#define LAMPARRAY_USAGE			0x00590001
#define LAMPARRAY_ATTRIBUTES_ID		0x01
#define LAMPARRAY_ATTRIBUTES_SIZE	23
#define LAMPARRAY_RANGE_UPDATE_ID	0x05
#define LAMPARRAY_RANGE_UPDATE_SIZE	10
#define LAMPARRAY_CONTROL_ID		0x06
#define LAMPARRAY_CONTROL_SIZE		2
#define LAMPARRAY_UPDATE_COMPLETE	0x01

struct legion_lamps;

struct legion_kbd {
	struct hid_device *hdev;
	struct usb_bus *bus;		/* pairs it with its LampArray */
	struct list_head node;		/* in legion_kbds */
	struct mutex lock;		/* one request/answer exchange at a time */
	u8 *buf;			/* LEGION_REPORT_SIZE, DMA-safe */
	struct led_classdev_mc mc;
	struct mc_subled subleds[3];
	struct work_struct hw_changed_work;
	unsigned int hw_brightness;	/* last level reported by the firmware */
	spinlock_t event_lock;		/* protects ready */
	bool ready;			/* LED registered: events may notify it */

	/* Protected by legion_pair_lock */
	struct legion_lamps *lamps;
	u8 rgb[3];			/* last colour seen in multi_intensity */
	bool color_set;			/* lighting_mode is "color" */
	bool lamparray_owns;		/* we switched the controller to 0xAA 00 */
};

struct legion_lamps {
	struct hid_device *hdev;
	struct usb_bus *bus;
	struct list_head node;		/* in legion_lamps_list */
	struct mutex lock;		/* one report at a time */
	u8 *buf;			/* LAMPARRAY_ATTRIBUTES_SIZE, DMA-safe */
	u16 lamp_count;

	struct legion_kbd *kbd;		/* protected by legion_pair_lock */
};

static DEFINE_MUTEX(legion_pair_lock);
static LIST_HEAD(legion_kbds);
static LIST_HEAD(legion_lamps_list);

static struct usb_bus *legion_usb_bus(struct hid_device *hdev)
{
	return interface_to_usbdev(to_usb_interface(hdev->dev.parent))->bus;
}

/* Keyboard controller -------------------------------------------------- */

/*
 * Send one request. When @answer is non-NULL, wait for the controller to
 * prepare its answer and return its value byte (byte 4) there.
 */
static int legion_exchange(struct legion_kbd *kbd, u8 op, const u8 *params,
			   u8 len, u8 *answer)
{
	struct hid_device *hdev = kbd->hdev;
	u8 *buf = kbd->buf;
	int ret;

	lockdep_assert_held(&kbd->lock);

	memset(buf, 0, LEGION_REPORT_SIZE);
	buf[0] = LEGION_REPORT_ID;
	buf[1] = op;
	buf[2] = len;			/* payload length, little endian */
	buf[3] = 0;
	memcpy(&buf[4], params, len);

	ret = hid_hw_raw_request(hdev, LEGION_REPORT_ID, buf, LEGION_REPORT_SIZE,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;
	if (!answer)
		return 0;

	msleep(LEGION_ANSWER_DELAY_MS);

	memset(buf, 0, LEGION_REPORT_SIZE);
	ret = hid_hw_raw_request(hdev, LEGION_REPORT_ID, buf, LEGION_REPORT_SIZE,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;
	if (ret < 5 || buf[0] != LEGION_REPORT_ID || buf[1] != op) {
		hid_warn(hdev, "unexpected answer to 0x%02x: %*ph\n", op,
			 min(ret, 8), buf);
		return -EIO;
	}

	*answer = buf[4];
	return 0;
}

static int legion_set_owner(struct legion_kbd *kbd, u8 owner)
{
	const u8 params[2] = { owner, 0 };
	int ret;

	mutex_lock(&kbd->lock);
	ret = legion_exchange(kbd, LEGION_OP_LIGHTING_OWNER, params,
			      sizeof(params), NULL);
	mutex_unlock(&kbd->lock);

	return ret;
}

/* LampArray ------------------------------------------------------------ */

static int lamps_set_feature(struct legion_lamps *lamps, const u8 *data,
			     size_t size)
{
	int ret;

	lockdep_assert_held(&lamps->lock);

	memcpy(lamps->buf, data, size);
	ret = hid_hw_raw_request(lamps->hdev, data[0], lamps->buf, size,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	return ret < 0 ? ret : 0;
}

static int lamps_set_autonomous(struct legion_lamps *lamps, bool autonomous)
{
	const u8 report[LAMPARRAY_CONTROL_SIZE] = {
		LAMPARRAY_CONTROL_ID, autonomous,
	};
	int ret;

	mutex_lock(&lamps->lock);
	ret = lamps_set_feature(lamps, report, sizeof(report));
	mutex_unlock(&lamps->lock);

	return ret;
}

/* Host control, then every lamp in one colour. */
static int lamps_set_color(struct legion_lamps *lamps, const u8 *rgb)
{
	u16 last = lamps->lamp_count - 1;
	const u8 control[LAMPARRAY_CONTROL_SIZE] = { LAMPARRAY_CONTROL_ID, 0 };
	const u8 update[LAMPARRAY_RANGE_UPDATE_SIZE] = {
		LAMPARRAY_RANGE_UPDATE_ID, LAMPARRAY_UPDATE_COMPLETE,
		0, 0,				/* first lamp */
		last & 0xff, last >> 8,		/* last lamp */
		rgb[0], rgb[1], rgb[2],
		1,				/* intensity: on */
	};
	int ret;

	mutex_lock(&lamps->lock);
	ret = lamps_set_feature(lamps, control, sizeof(control));
	if (!ret)
		ret = lamps_set_feature(lamps, update, sizeof(update));
	mutex_unlock(&lamps->lock);

	return ret;
}

/* With legion_pair_lock held: show kbd->rgb, handing the LEDs over first. */
static int legion_apply_color(struct legion_kbd *kbd)
{
	int ret;

	lockdep_assert_held(&legion_pair_lock);

	if (!kbd->lamps)
		return 0;	/* applied when the LampArray binds */

	if (!kbd->lamparray_owns) {
		ret = legion_set_owner(kbd, LEGION_OWNER_LAMPARRAY);
		if (ret)
			return ret;
		kbd->lamparray_owns = true;
	}

	return lamps_set_color(kbd->lamps, kbd->rgb);
}

/* With legion_pair_lock held: give the LEDs back to the controller. */
static void legion_hand_back(struct legion_kbd *kbd)
{
	lockdep_assert_held(&legion_pair_lock);

	if (!kbd->lamparray_owns)
		return;

	if (kbd->lamps)
		lamps_set_autonomous(kbd->lamps, true);
	legion_set_owner(kbd, LEGION_OWNER_FIRMWARE);
	kbd->lamparray_owns = false;
}

/* LED ------------------------------------------------------------------ */

static int legion_brightness_set(struct led_classdev *led,
				 enum led_brightness value)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(led);
	struct legion_kbd *kbd = container_of(mc, struct legion_kbd, mc);
	u8 level = min_t(unsigned int, value, LEGION_MAX_BRIGHTNESS);
	u8 rgb[3];
	int i, ret;

	mutex_lock(&kbd->lock);
	ret = legion_exchange(kbd, LEGION_OP_SET_BRIGHTNESS, &level, 1, NULL);
	mutex_unlock(&kbd->lock);
	if (ret)
		return ret;

	/* A multi_intensity write ends up here too: apply a changed colour. */
	for (i = 0; i < 3; i++)
		rgb[i] = READ_ONCE(mc->subled_info[i].intensity);

	mutex_lock(&legion_pair_lock);
	if (memcmp(rgb, kbd->rgb, sizeof(rgb))) {
		memcpy(kbd->rgb, rgb, sizeof(rgb));
		kbd->color_set = true;
		ret = legion_apply_color(kbd);
	}
	mutex_unlock(&legion_pair_lock);

	return ret;
}

static enum led_brightness legion_brightness_get(struct led_classdev *led)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(led);
	struct legion_kbd *kbd = container_of(mc, struct legion_kbd, mc);
	u8 zero = 0, value = 0;
	int ret;

	mutex_lock(&kbd->lock);
	ret = legion_exchange(kbd, LEGION_OP_GET_BRIGHTNESS, &zero, 1, &value);
	mutex_unlock(&kbd->lock);

	if (ret)
		return led->brightness;	/* keep the last known level */

	return min_t(unsigned int, value, LEGION_MAX_BRIGHTNESS);
}

static void legion_hw_changed_work(struct work_struct *work)
{
	struct legion_kbd *kbd = container_of(work, struct legion_kbd,
					      hw_changed_work);

	led_classdev_notify_brightness_hw_changed(&kbd->mc.led_cdev,
						  READ_ONCE(kbd->hw_brightness));
}

static int legion_raw_event(struct hid_device *hdev, struct hid_report *report,
			    u8 *data, int size)
{
	struct legion_kbd *kbd;
	unsigned long flags;

	if (hdev->product != USB_DEVICE_ID_ITE_LENOVO_LEGION_GEN10_KBD)
		return 0;

	kbd = hid_get_drvdata(hdev);
	if (!kbd || size < 3 || data[0] != LEGION_EVENT_REPORT_ID)
		return 0;

	if (data[1] != LEGION_EVENT_BRIGHTNESS_UP &&
	    data[1] != LEGION_EVENT_BRIGHTNESS_DOWN)
		return 0;

	spin_lock_irqsave(&kbd->event_lock, flags);
	if (kbd->ready) {
		WRITE_ONCE(kbd->hw_brightness,
			   min_t(unsigned int, data[2], LEGION_MAX_BRIGHTNESS));
		schedule_work(&kbd->hw_changed_work);
	}
	spin_unlock_irqrestore(&kbd->event_lock, flags);

	return 0;	/* let hid-core handle the report as usual */
}

static struct legion_kbd *led_dev_to_kbd(struct device *dev)
{
	struct led_classdev *led = dev_get_drvdata(dev);

	return container_of(lcdev_to_mccdev(led), struct legion_kbd, mc);
}

static ssize_t lighting_mode_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct legion_kbd *kbd = led_dev_to_kbd(dev);
	bool color;

	mutex_lock(&legion_pair_lock);
	color = kbd->color_set;
	mutex_unlock(&legion_pair_lock);

	return sysfs_emit(buf, "%s\n", color ? "color" : "firmware");
}

static ssize_t lighting_mode_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct legion_kbd *kbd = led_dev_to_kbd(dev);
	struct led_classdev_mc *mc = &kbd->mc;
	int i, ret = 0;

	mutex_lock(&legion_pair_lock);
	if (sysfs_streq(buf, "color")) {
		for (i = 0; i < 3; i++)
			kbd->rgb[i] = READ_ONCE(mc->subled_info[i].intensity);
		kbd->color_set = true;
		ret = legion_apply_color(kbd);
	} else if (sysfs_streq(buf, "firmware")) {
		kbd->color_set = false;
		legion_hand_back(kbd);
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&legion_pair_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(lighting_mode);

static struct attribute *legion_led_attrs[] = {
	&dev_attr_lighting_mode.attr,
	NULL
};
ATTRIBUTE_GROUPS(legion_led);

/* Probe and remove ----------------------------------------------------- */

static int legion_kbd_probe(struct hid_device *hdev)
{
	struct legion_kbd *kbd;
	struct legion_lamps *lamps;
	int i, ret;

	kbd = devm_kzalloc(&hdev->dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd)
		return -ENOMEM;

	kbd->buf = devm_kzalloc(&hdev->dev, LEGION_REPORT_SIZE, GFP_KERNEL);
	if (!kbd->buf)
		return -ENOMEM;

	kbd->hdev = hdev;
	kbd->bus = legion_usb_bus(hdev);
	mutex_init(&kbd->lock);
	spin_lock_init(&kbd->event_lock);
	INIT_WORK(&kbd->hw_changed_work, legion_hw_changed_work);
	hid_set_drvdata(hdev, kbd);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	/* Keep the interrupt endpoint polled so hotkey reports always arrive. */
	ret = hid_hw_open(hdev);
	if (ret)
		goto err_stop;

	for (i = 0; i < 3; i++) {
		kbd->subleds[i].color_index = LED_COLOR_ID_RED + i;
		kbd->subleds[i].intensity = 255;
		kbd->subleds[i].max_intensity = 255;
		kbd->rgb[i] = 255;
	}
	kbd->mc.subled_info = kbd->subleds;
	kbd->mc.num_colors = 3;
	kbd->mc.led_cdev.name = "legion:rgb:kbd_backlight";
	kbd->mc.led_cdev.color = LED_COLOR_ID_RGB;
	kbd->mc.led_cdev.max_brightness = LEGION_MAX_BRIGHTNESS;
	kbd->mc.led_cdev.brightness_set_blocking = legion_brightness_set;
	kbd->mc.led_cdev.brightness_get = legion_brightness_get;
	/*
	 * LED_RETAIN_AT_SHUTDOWN: unregistering (unload, unbind) must not
	 * switch the backlight off, the controller would store 0.
	 * LED_BRIGHT_HW_CHANGED: Fn+Up/Down are reported to userspace.
	 */
	kbd->mc.led_cdev.flags = LED_RETAIN_AT_SHUTDOWN | LED_BRIGHT_HW_CHANGED;

	ret = led_classdev_multicolor_register(&hdev->dev, &kbd->mc);
	if (ret)
		goto err_close;

	/* Added afterwards: the multicolor class replaces led_cdev.groups. */
	ret = device_add_groups(kbd->mc.led_cdev.dev, legion_led_groups);
	if (ret)
		goto err_unregister;

	spin_lock_irq(&kbd->event_lock);
	kbd->ready = true;
	spin_unlock_irq(&kbd->event_lock);

	mutex_lock(&legion_pair_lock);
	list_add(&kbd->node, &legion_kbds);
	list_for_each_entry(lamps, &legion_lamps_list, node) {
		if (lamps->bus == kbd->bus && !lamps->kbd) {
			lamps->kbd = kbd;
			kbd->lamps = lamps;
			hid_info(hdev, "paired with %s\n", dev_name(&lamps->hdev->dev));
			break;
		}
	}
	mutex_unlock(&legion_pair_lock);

	return 0;

err_unregister:
	led_classdev_multicolor_unregister(&kbd->mc);
err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void legion_kbd_remove(struct hid_device *hdev)
{
	struct legion_kbd *kbd = hid_get_drvdata(hdev);

	/* No new notifications, then wait for a pending one, then the LED. */
	spin_lock_irq(&kbd->event_lock);
	kbd->ready = false;
	spin_unlock_irq(&kbd->event_lock);
	cancel_work_sync(&kbd->hw_changed_work);
	device_remove_groups(kbd->mc.led_cdev.dev, legion_led_groups);
	led_classdev_multicolor_unregister(&kbd->mc);

	mutex_lock(&legion_pair_lock);
	legion_hand_back(kbd);
	if (kbd->lamps)
		kbd->lamps->kbd = NULL;
	list_del(&kbd->node);
	mutex_unlock(&legion_pair_lock);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int legion_lamps_probe(struct hid_device *hdev)
{
	struct legion_lamps *lamps;
	struct legion_kbd *kbd;
	int ret;

	lamps = devm_kzalloc(&hdev->dev, sizeof(*lamps), GFP_KERNEL);
	if (!lamps)
		return -ENOMEM;

	lamps->buf = devm_kzalloc(&hdev->dev, LAMPARRAY_ATTRIBUTES_SIZE,
				  GFP_KERNEL);
	if (!lamps->buf)
		return -ENOMEM;

	lamps->hdev = hdev;
	lamps->bus = legion_usb_bus(hdev);
	mutex_init(&lamps->lock);
	hid_set_drvdata(hdev, lamps);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	ret = hid_hw_raw_request(hdev, LAMPARRAY_ATTRIBUTES_ID, lamps->buf,
				 LAMPARRAY_ATTRIBUTES_SIZE, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 3 || lamps->buf[0] != LAMPARRAY_ATTRIBUTES_ID) {
		hid_err(hdev, "can't read the LampArray attributes: %d\n", ret);
		ret = ret < 0 ? ret : -EIO;
		goto err_stop;
	}
	lamps->lamp_count = get_unaligned_le16(&lamps->buf[1]);
	if (!lamps->lamp_count) {
		ret = -ENODEV;
		goto err_stop;
	}
	hid_info(hdev, "LampArray with %u lamps\n", lamps->lamp_count);

	mutex_lock(&legion_pair_lock);
	list_add(&lamps->node, &legion_lamps_list);
	list_for_each_entry(kbd, &legion_kbds, node) {
		if (kbd->bus == lamps->bus && !kbd->lamps) {
			kbd->lamps = lamps;
			lamps->kbd = kbd;
			hid_info(hdev, "paired with %s\n", dev_name(&kbd->hdev->dev));
			if (kbd->color_set)
				legion_apply_color(kbd);
			break;
		}
	}
	mutex_unlock(&legion_pair_lock);

	return 0;

err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void legion_lamps_remove(struct hid_device *hdev)
{
	struct legion_lamps *lamps = hid_get_drvdata(hdev);

	mutex_lock(&legion_pair_lock);
	if (lamps->kbd) {
		if (lamps->kbd->lamparray_owns)
			lamps_set_autonomous(lamps, true);
		lamps->kbd->lamps = NULL;
	}
	list_del(&lamps->node);
	mutex_unlock(&legion_pair_lock);

	hid_hw_stop(hdev);
}

/* Resumed after a USB reset: the LampArray is back in autonomous mode. */
static int legion_reset_resume(struct hid_device *hdev)
{
	struct legion_lamps *lamps = hid_get_drvdata(hdev);

	if (hdev->product != USB_DEVICE_ID_ITE_LENOVO_LEGION_GEN10_LAMPS || !lamps)
		return 0;

	mutex_lock(&legion_pair_lock);
	if (lamps->kbd && lamps->kbd->lamparray_owns)
		lamps_set_color(lamps, lamps->kbd->rgb);
	mutex_unlock(&legion_pair_lock);

	return 0;
}

static bool legion_is_lamparray(struct hid_device *hdev)
{
	return hdev->maxcollection > 0 &&
	       hdev->collection[0].usage == LAMPARRAY_USAGE;
}

static int legion_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;

	if (!hid_is_usb(hdev))
		return -ENODEV;

	/* As hid-generic: one input device per application collection. */
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	if (id->product == USB_DEVICE_ID_ITE_LENOVO_LEGION_GEN10_KBD)
		return legion_kbd_probe(hdev);

	if (legion_is_lamparray(hdev))
		return legion_lamps_probe(hdev);

	/* Other interfaces of c193: exactly what hid-generic would do. */
	hid_set_drvdata(hdev, NULL);
	return hid_hw_start(hdev, HID_CONNECT_DEFAULT);
}

static void legion_remove(struct hid_device *hdev)
{
	if (hdev->product == USB_DEVICE_ID_ITE_LENOVO_LEGION_GEN10_KBD)
		legion_kbd_remove(hdev);
	else if (hid_get_drvdata(hdev))
		legion_lamps_remove(hdev);
	else
		hid_hw_stop(hdev);
}

static const struct hid_device_id legion_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, USB_DEVICE_ID_ITE_LENOVO_LEGION_GEN10_KBD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, USB_DEVICE_ID_ITE_LENOVO_LEGION_GEN10_LAMPS) },
	{ }
};
MODULE_DEVICE_TABLE(hid, legion_devices);

static struct hid_driver legion_driver = {
	.name = "hid-lenovo-legion",
	.id_table = legion_devices,
	.probe = legion_probe,
	.remove = legion_remove,
	.raw_event = legion_raw_event,
	.reset_resume = pm_ptr(legion_reset_resume),
};
module_hid_driver(legion_driver);

MODULE_AUTHOR("Ciro Pedrini <ciro.pedrini@gmail.com>");
MODULE_DESCRIPTION("Keyboard lighting of Lenovo Legion laptops (2025)");
MODULE_LICENSE("GPL");
