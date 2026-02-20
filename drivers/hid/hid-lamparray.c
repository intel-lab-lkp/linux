// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hid-lamparray.c - HID LampArray helper module (single-zone RGB)
 *
 * Helper module for HID drivers supporting devices that expose a
 * Lighting and Illumination (LampArray) application collection
 * (usage page 0x59).
 *
 * The module provides a minimal integration with the LED subsystem
 * and treats the device as a single zone: all lamps share one RGB
 * value and a global brightness level. It does not implement multi-
 * zone layouts or hardware effects.
 *
 *
 * If enabled, one multicolor LED class device is registered under
 * /sys/class/leds/<HID-ID> to expose the single-zone RGB control.
 *
 * The use_leds_uapi sysfs attribute is attached directly to the HID device
 * under /sys/bus/hid/devices/<HID-ID>/use_leds_uapi.Writing 0 to use_leds_uapi
 * unregisters the LED class device. The last state is kept cached. Writing 1
 * registers it again and restores the cached state to hardware.
 *
 * State is cached as last known RGB + brightness. Setting sends a HID
 * SET_REPORT. Getting issues a HID GET_REPORT and updates the cache on
 * mismatch. Since the device is handled as single-zone, readback only queries
 * lamp 0 when a lamp range is available.
 *
 * The module does not bind to devices on its own. Instead, a HID
 * driver may query support via lamparray_is_supported_device() after
 * hid_parse() and create an instance using lamparray_register().
 *
 * Copyright (C) 2026 Tim Guttzeit <tgu@tuxedocomputers.com>
 */

#include "hid-lamparray.h"
#include <linux/module.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/leds.h>
#include <linux/led-class-multicolor.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/bitops.h>
#include <linux/xarray.h>

/* Constants */

/* HID usages (LampArray, etc.) */
#define HID_LIGHTING_ILLUMINATION_USAGE_PAGE	0x0059

#define HID_LAIP_LAMP_COUNT			0x0003
#define HID_LAIP_LAMP_ID			0x0021
#define HID_LAIP_RED_UPDATE_CHANNEL		0x0051
#define HID_LAIP_GREEN_UPDATE_CHANNEL		0x0052
#define HID_LAIP_BLUE_UPDATE_CHANNEL		0x0053
#define HID_LAIP_INTENSITY_UPDATE_CHANNEL	0x0054
#define HID_LAIP_LAMP_ID_START			0x0061
#define HID_LAIP_LAMP_ID_END			0x0062
#define HID_LAIP_AUTONOMOUS_MODE		0x0071

#define HID_APPLICATION_COLLECTION_USAGE_TYPE	0x0001

/* Device state */

struct lamparray_quirks {
	unsigned long flags;
	int fixed_lamp_count;
};

#define LAMPARRAY_QUIRK_LAMPCOUNT_FIXED BIT(0)

struct lamparray_quirk_entry {
	u16 vendor;
	u16 product;
	unsigned long flags;
	int fixed_lamp_count;
};

static const struct lamparray_quirk_entry lamparray_quirk_table[] = {
	{ 0xcafe, 0x4005, LAMPARRAY_QUIRK_LAMPCOUNT_FIXED, 12 },
	{}
};

static const struct lamparray_quirk_entry *
lamparray_lookup_quirks(struct hid_device *hdev)
{
	const struct lamparray_quirk_entry *e;

	for (e = lamparray_quirk_table; e->vendor; e++) {
		if (hdev->vendor == e->vendor && hdev->product == e->product)
			return e;
	}
	return NULL;
}

struct lamparray_device {
	const struct lamparray_quirk_entry *quirks;

	struct hid_device *hdev;
	struct hid_report *update_report;

	struct hid_field *red_field;
	int red_index;
	struct hid_field *green_field;
	int green_index;
	struct hid_field *blue_field;
	int blue_index;
	struct hid_field *intensity_field;
	int intensity_index;

	struct hid_report *autonomous_report;
	struct hid_field *autonomous_field;

	struct hid_field *range_start_field;
	int range_start_index;

	struct hid_field *range_end_field;
	int range_end_index;

	struct hid_field *lamp_count_field;
	int lamp_count;
	int lamp_count_index;

	struct led_classdev_mc mc_cdev;
	struct mc_subled subleds[3];

	struct mutex lock; /* protects cached state and HID access */

	u8 last_r;
	u8 last_g;
	u8 last_b;
	enum led_brightness last_brightness;

	bool use_leds_uapi;
	bool led_registered;
};

/*
 * Opaque handle exposed to callers via the header.
 * Keep the actual state in lamparray_device, but return a stable pointer.
 */
struct lamparray {
	struct lamparray_device ldev;
};

static DEFINE_XARRAY(lamparray_by_hdev);

/* HID helper functions */

#ifdef DEBUG
static void lamparray_dump_reports(struct hid_device *hdev)
{
	struct hid_report_enum *re;
	struct hid_report *report;
	int i, j, report_type;

	for (report_type = 0; report_type < HID_REPORT_TYPES; report_type++) {
		re = &hdev->report_enum[report_type];
		hid_dbg(hdev, "Dumping reports for report type %d",
			report_type);
		list_for_each_entry(report, &re->report_list, list) {
			hid_dbg(hdev,
				"lamparray: report id=%u type=%d maxfield=%u\n",
				report->id, report->type, report->maxfield);

			for (i = 0; i < report->maxfield; i++) {
				struct hid_field *field = report->field[i];

				for (j = 0; j < field->maxusage; j++) {
					u32 usage = field->usage[j].hid;
					u16 page = usage >> 16;
					u16 id = usage & 0xFFFF;

					hid_dbg(hdev,
						"lamparray: report %u field %d usage[%d]: page=0x%04x id=0x%04x\n",
						report->id, i, j, page, id);
				}
			}
		}
	}
}
#else
static inline void lamparray_dump_reports(struct hid_device *hdev)
{}
#endif

static int lamparray_read_lamp_count(struct lamparray_device *ldev)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_report *report;

	if (ldev->quirks &&
	    (ldev->quirks->flags & LAMPARRAY_QUIRK_LAMPCOUNT_FIXED)) {
		ldev->lamp_count = ldev->quirks->fixed_lamp_count;
		hid_dbg(hdev, "LampCount from quirk: %d\n", ldev->lamp_count);
		return 0;
	}
	if (!ldev->lamp_count_field) {
		hid_warn(hdev, "No LampCount field found\n");
		return -ENODEV;
	}

	report = ldev->lamp_count_field->report;

	if (!report) {
		hid_warn(hdev, "LampCount field has no report\n");
		return -ENODEV;
	}
	hid_hw_request(hdev, report, HID_REQ_GET_REPORT);
	ldev->lamp_count =
		ldev->lamp_count_field->value[ldev->lamp_count_index];

	hid_dbg(hdev, "LampCount from device: %d\n", ldev->lamp_count);

	if (ldev->lamp_count <= 0) {
		hid_warn(hdev, "LampCount is %d (invalid)\n", ldev->lamp_count);
		return -EINVAL;
	}

	return 0;
}

static int lamparray_parse_update_report(struct lamparray_device *ldev)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_report_enum *re;
	struct hid_report *report;
	struct hid_field *field;
	int i, j;

	lamparray_dump_reports(hdev);

	re = &hdev->report_enum[HID_FEATURE_REPORT];

	list_for_each_entry(report, &re->report_list, list) {
		for (i = 0; i < report->maxfield; i++) {
			field = report->field[i];
			if (!field)
				continue;

			if (!field->usage || !field->maxusage)
				continue;

			for (j = 0; j < field->maxusage; j++) {
				u32 usage = field->usage[j].hid;
				u16 page = usage >> 16;
				u16 id = usage & 0xffff;

				if (page !=
				    HID_LIGHTING_ILLUMINATION_USAGE_PAGE)
					continue;
				switch (id) {
				case HID_LAIP_LAMP_COUNT:
					ldev->lamp_count_field = field;
					ldev->lamp_count_index = j;
					break;
				case HID_LAIP_RED_UPDATE_CHANNEL:
					ldev->update_report = report;
					ldev->red_field = field;
					ldev->red_index = j;
					break;
				case HID_LAIP_GREEN_UPDATE_CHANNEL:
					ldev->update_report = report;
					ldev->green_field = field;
					ldev->green_index = j;
					break;
				case HID_LAIP_BLUE_UPDATE_CHANNEL:
					ldev->update_report = report;
					ldev->blue_field = field;
					ldev->blue_index = j;
					break;
				case HID_LAIP_INTENSITY_UPDATE_CHANNEL:
					ldev->update_report = report;
					ldev->intensity_field = field;
					ldev->intensity_index = j;
					break;
				case HID_LAIP_LAMP_ID_START:
					ldev->range_start_field = field;
					ldev->range_start_index = j;
					break;
				case HID_LAIP_LAMP_ID_END:
					ldev->range_end_field = field;
					ldev->range_end_index = j;
					break;
				case HID_LAIP_AUTONOMOUS_MODE:
					ldev->autonomous_field = field;
					ldev->autonomous_report = report;
					break;
				default:
					break;
				}
			}
		}
	}

	if (!ldev->update_report || !ldev->red_field || !ldev->green_field ||
	    !ldev->blue_field || !ldev->autonomous_report || !ldev->autonomous_field)
		return -ENODEV;

	return 0;
}

static int lamparray_hw_set_autonomous(struct lamparray_device *ldev,
				       bool enable)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_field *field = ldev->autonomous_field;
	struct hid_report *report = ldev->autonomous_report;

	if (!field || !report)
		return -ENODEV;

	field->value[0] = enable ? 1 : 0;

	hid_hw_request(hdev, report, HID_REQ_SET_REPORT);
	return 0;
}

static int lamparray_hw_set_state(struct lamparray_device *ldev, u8 r, u8 g,
				  u8 b, u8 intensity)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_report *report = ldev->update_report;
	int i, j;

	if (!report || !ldev->red_field || !ldev->green_field ||
	    !ldev->blue_field || !ldev->intensity_field)
		return -ENODEV;

	if (ldev->range_start_field && ldev->range_end_field) {
		ldev->range_start_field->value[ldev->range_start_index] = 0;
		ldev->range_end_field->value[ldev->range_end_index] = ldev->lamp_count - 1;
	}

	for (i = 0; i < report->maxfield; i++) {
		struct hid_field *field = report->field[i];

		if (!field || !field->usage || !field->maxusage)
			continue;

		for (j = 0; j < field->maxusage; j++) {
			u32 usage = field->usage[j].hid;
			u16 page = usage >> 16;
			u16 id = usage & 0xffff;

			if (page != HID_LIGHTING_ILLUMINATION_USAGE_PAGE)
				continue;

			switch (id) {
			case HID_LAIP_RED_UPDATE_CHANNEL:
				field->value[j] = r;
				break;
			case HID_LAIP_GREEN_UPDATE_CHANNEL:
				field->value[j] = g;
				break;
			case HID_LAIP_BLUE_UPDATE_CHANNEL:
				field->value[j] = b;
				break;
			case HID_LAIP_INTENSITY_UPDATE_CHANNEL:
				field->value[j] = intensity;
				break;
			default:
				break;
			}
		}
	}

	hid_hw_request(hdev, report, HID_REQ_SET_REPORT);
	return 0;
}

static int lamparray_hw_get_state(struct lamparray_device *ldev, u8 *r, u8 *g,
				  u8 *b, enum led_brightness *brightness)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_report *report = ldev->update_report;

	if (!report || !ldev->red_field || !ldev->green_field ||
	    !ldev->blue_field || !ldev->intensity_field)
		return -ENODEV;

	if (!r || !g || !b || !brightness)
		return -EINVAL;

	/* Single-zone: Reading lamp 0 only suffices */
	if (ldev->range_start_field && ldev->range_end_field) {
		ldev->range_start_field->value[ldev->range_start_index] = 0;
		ldev->range_end_field->value[ldev->range_end_index] = 0;
	}

	hid_hw_request(hdev, report, HID_REQ_GET_REPORT);

	*r = ldev->red_field->value[ldev->red_index];
	*g = ldev->green_field->value[ldev->green_index];
	*b = ldev->blue_field->value[ldev->blue_index];
	*brightness = ldev->intensity_field->value[ldev->intensity_index];

	return 0;
}

/* Helper functions */

static int lamparray_restore_state(struct lamparray_device *ldev)
{
	u8 r, g, b;
	int ret;
	enum led_brightness brightness;

	mutex_lock(&ldev->lock);

	if (!ldev->use_leds_uapi) {
		mutex_unlock(&ldev->lock);
		return 0;
	}

	r = ldev->last_r;
	g = ldev->last_g;
	b = ldev->last_b;
	brightness = ldev->last_brightness;

	ldev->mc_cdev.subled_info[0].brightness = r;
	ldev->mc_cdev.subled_info[1].brightness = g;
	ldev->mc_cdev.subled_info[2].brightness = b;
	ldev->mc_cdev.led_cdev.brightness = brightness;

	mutex_unlock(&ldev->lock);

	ret = lamparray_hw_set_state(ldev, r, g, b, brightness);
	return ret;
}

/* LEDs API */

static int lamparray_led_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct lamparray_device *ldev =
		container_of(mc, struct lamparray_device, mc_cdev);
	struct lamparray *la = container_of(ldev, struct lamparray, ldev);
	u8 r, g, b;
	int ret;

	if (!la)
		return -ENODEV;
	ldev = &la->ldev;

	ret = led_mc_calc_color_components(mc, brightness);
	if (ret)
		return ret;

	r = mc->subled_info[0].brightness;
	g = mc->subled_info[1].brightness;
	b = mc->subled_info[2].brightness;

	ret = lamparray_hw_set_state(ldev, r, g, b, brightness);
	if (ret)
		hid_err(ldev->hdev, "Failed to send LampArray update: %d\n",
			ret);

	mutex_lock(&ldev->lock);
	ldev->last_r = r;
	ldev->last_g = g;
	ldev->last_b = b;
	ldev->last_brightness = brightness;
	mutex_unlock(&ldev->lock);

	return 0;
}

static enum led_brightness
lamparray_led_brightness_get(struct led_classdev *cdev)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct lamparray_device *ldev =
		container_of(mc, struct lamparray_device, mc_cdev);
	enum led_brightness brightness;
	struct lamparray *la = container_of(ldev, struct lamparray, ldev);
	u8 rr, gg, bb;
	enum led_brightness br;
	int ret;

	/* Default: cache (also used while registering LED classdev) */
	mutex_lock(&ldev->lock);
	brightness = ldev->last_brightness;
	mutex_unlock(&ldev->lock);

	/* Only do HID readback after registration completed */
	if (READ_ONCE(ldev->led_registered)) {
		if (!la)
			return brightness;
		ldev = &la->ldev;

		ret = lamparray_hw_get_state(ldev, &rr, &gg, &bb, &br);
		if (ret) {
			hid_warn(ldev->hdev,
				 "Failed to read LampArray state (%d), using cached brightness %u\n",
				 ret, brightness);
			return brightness;
		}

		mutex_lock(&ldev->lock);
		if (ldev->last_r != rr || ldev->last_g != gg ||
		    ldev->last_b != bb || ldev->last_brightness != br) {
			ldev->last_r = rr;
			ldev->last_g = gg;
			ldev->last_b = bb;
			ldev->last_brightness = br;

			if (ldev->led_registered && ldev->mc_cdev.subled_info) {
				ldev->mc_cdev.subled_info[0].brightness = rr;
				ldev->mc_cdev.subled_info[1].brightness = gg;
				ldev->mc_cdev.subled_info[2].brightness = bb;
			}
		}
		mutex_unlock(&ldev->lock);
		return br;
	}
	return brightness;
}

static int lamparray_register_led(struct lamparray_device *ldev)
{
	struct device *dev = &ldev->hdev->dev;
	struct led_classdev *cdev = &ldev->mc_cdev.led_cdev;
	u8 r_i, g_i, b_i;
	int ret;

	mutex_lock(&ldev->lock);

	if (ldev->led_registered) {
		mutex_unlock(&ldev->lock);
		return 0;
	}

	if (!cdev->name) {
		cdev->name =
			devm_kasprintf(dev, GFP_KERNEL, "%s", dev_name(dev));
		if (!cdev->name) {
			mutex_unlock(&ldev->lock);
			return -ENOMEM;
		}
	}

	cdev->max_brightness = 255;
	cdev->brightness_set_blocking = lamparray_led_brightness_set;
	cdev->brightness_get = lamparray_led_brightness_get;
	cdev->brightness = ldev->last_brightness;

	ldev->subleds[0].color_index = LED_COLOR_ID_RED;
	ldev->subleds[1].color_index = LED_COLOR_ID_GREEN;
	ldev->subleds[2].color_index = LED_COLOR_ID_BLUE;

	/*
	 * Initialize the color mix (multi_intensity) from the last known HW/init
	 * state so that writing only /brightness scales the expected default color
	 * instead of white.
	 *
	 * If last_brightness is non-zero, treat last_r/g/b as per-channel
	 * brightness and normalize back to intensities (0..255).
	 * If last_brightness is zero, keep last_r/g/b as the intended mix.
	 */
	if (ldev->last_brightness) {
		r_i = (u8)min_t(unsigned int, 255,
				(ldev->last_r * 255u) / ldev->last_brightness);
		g_i = (u8)min_t(unsigned int, 255,
				(ldev->last_g * 255u) / ldev->last_brightness);
		b_i = (u8)min_t(unsigned int, 255,
				(ldev->last_b * 255u) / ldev->last_brightness);
	} else {
		r_i = ldev->last_r;
		g_i = ldev->last_g;
		b_i = ldev->last_b;
	}

	ldev->subleds[0].intensity = r_i;
	ldev->subleds[1].intensity = g_i;
	ldev->subleds[2].intensity = b_i;

	ldev->mc_cdev.subled_info = ldev->subleds;
	ldev->mc_cdev.num_colors = ARRAY_SIZE(ldev->subleds);

	/* Ensure subled_info[].brightness matches intensity + brightness */
	led_mc_calc_color_components(&ldev->mc_cdev, cdev->brightness);

	ldev->mc_cdev.subled_info = ldev->subleds;
	ldev->mc_cdev.num_colors = ARRAY_SIZE(ldev->subleds);

	mutex_unlock(&ldev->lock);

	ret = led_classdev_multicolor_register(dev, &ldev->mc_cdev);
	if (ret)
		return ret;

	mutex_lock(&ldev->lock);
	ldev->led_registered = true;
	mutex_unlock(&ldev->lock);

	return 0;
}

static void lamparray_unregister_led(struct lamparray_device *ldev)
{
	bool was_registered;

	mutex_lock(&ldev->lock);
	was_registered = ldev->led_registered;
	ldev->led_registered = false;
	mutex_unlock(&ldev->lock);

	if (!was_registered)
		return;

	led_classdev_multicolor_unregister(&ldev->mc_cdev);
}

/* Sysfs */

static struct lamparray_device *
lamparray_ldev_from_sysfs_dev(struct device *dev)
{
	struct hid_device *hdev = to_hid_device(dev);

	return xa_load(&lamparray_by_hdev, (unsigned long)hdev);
}

static ssize_t use_leds_uapi_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct lamparray_device *ldev = lamparray_ldev_from_sysfs_dev(dev);

	if (!ldev)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", ldev->use_leds_uapi);
}

static ssize_t use_leds_uapi_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct lamparray_device *ldev = lamparray_ldev_from_sysfs_dev(dev);
	int val;
	int old_val;
	int ret;

	if (!ldev)
		return -ENODEV;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if (val != 0 && val != 1)
		return -EINVAL;

	mutex_lock(&ldev->lock);
	old_val = ldev->use_leds_uapi;

	if (val == old_val) {
		mutex_unlock(&ldev->lock);
		return count;
	}

	ldev->use_leds_uapi = val;
	mutex_unlock(&ldev->lock);

	if (val == 1) {
		ret = lamparray_register_led(ldev);
		if (ret) {
			mutex_lock(&ldev->lock);
			ldev->use_leds_uapi = old_val;
			mutex_unlock(&ldev->lock);
			return ret;
		}
		ret = lamparray_restore_state(ldev);
		if (ret) {
			hid_err(ldev->hdev, "Could not restore state: %d", ret);
			return ret;
		}

	} else {
		lamparray_unregister_led(ldev);
	}

	return count;
}
static DEVICE_ATTR_RW(use_leds_uapi);

static struct attribute *lamparray_attrs[] = {
	&dev_attr_use_leds_uapi.attr,
	NULL,
};

static const struct attribute_group lamparray_attr_group = {
	.attrs = lamparray_attrs,
};

static int lamparray_register_sysfs(struct lamparray_device *ldev)
{
	struct device *dev = &ldev->hdev->dev;
	int ret;

	ret = sysfs_create_group(&dev->kobj, &lamparray_attr_group);
	if (ret)
		hid_err(ldev->hdev,
			"Failed to create lamparray sysfs group: %d\n", ret);

	return ret;
}

static void lamparray_remove_sysfs(struct lamparray_device *ldev)
{
	sysfs_remove_group(&ldev->hdev->dev.kobj, &lamparray_attr_group);
}

/* Public API */

bool lamparray_is_supported_device(struct hid_device *hdev)
{
	unsigned int i;

	hid_dbg(hdev, "lamparray: walking %u collections\n",
		hdev->maxcollection);

	for (i = 0; i < hdev->maxcollection; i++) {
		struct hid_collection *col = &hdev->collection[i];
		u16 page = (col->usage & HID_USAGE_PAGE) >> 16;
		u16 code = col->usage & HID_USAGE;

		hid_dbg(hdev,
			"lamparray:  collection[%u]: type=%u level=%u usage=0x%08x page=0x%04x code=0x%04x\n",
			i, col->type, col->level, col->usage, page, code);

		if (col->type == HID_COLLECTION_APPLICATION &&
		    page == HID_LIGHTING_ILLUMINATION_USAGE_PAGE &&
		    code == HID_APPLICATION_COLLECTION_USAGE_TYPE) {
			return true;
		}
	}
	return false;
}
EXPORT_SYMBOL_GPL(lamparray_is_supported_device);

struct lamparray *
lamparray_register(struct hid_device *hdev,
		   const struct lamparray_init_state *led_init_state)
{
	int ret;
	struct lamparray *la;
	struct lamparray_device *ldev;

	if (!hdev)
		return ERR_PTR(-ENODEV);

	la = kzalloc(sizeof(*la), GFP_KERNEL);
	if (!la)
		return ERR_PTR(-ENOMEM);

	ldev = &la->ldev;

	mutex_init(&ldev->lock);
	ldev->hdev = hdev;
	ldev->quirks = lamparray_lookup_quirks(hdev);
	ldev->use_leds_uapi = 1;
	ldev->led_registered = false;
	if (!led_init_state) {
		ldev->last_r = 255;
		ldev->last_g = 255;
		ldev->last_b = 255;
		ldev->last_brightness = LED_OFF;
	} else {
		ldev->last_r = led_init_state->r;
		ldev->last_g = led_init_state->g;
		ldev->last_b = led_init_state->b;
		ldev->last_brightness = led_init_state->brightness;
	}
	ret = lamparray_parse_update_report(ldev);
	if (ret) {
		hid_err(hdev, "No LampArray update report found: %d\n", ret);
		goto err_free;
	}

	ret = lamparray_read_lamp_count(ldev);
	if (ret) {
		hid_err(hdev,
			"Could not determine LampCount. This device needs a quirk for a fixed LampCount: %d\n",
			ret);
		goto err_unregister_led;
	}

	ret = lamparray_register_led(ldev);
	if (ret) {
		hid_warn(hdev, "Failed to register LED UAPI: %d\n", ret);
		mutex_lock(&ldev->lock);
		ldev->use_leds_uapi = 0;
		mutex_unlock(&ldev->lock);
	}

	ret = xa_err(xa_store(&lamparray_by_hdev, (unsigned long)hdev, ldev,
			      GFP_KERNEL));
	if (ret)
		goto err_unregister_led;

	ret = lamparray_register_sysfs(ldev);
	if (ret)
		goto err_xa_erase;

	ret = lamparray_hw_set_autonomous(ldev, false);
	if (ret) {
		hid_err(hdev, "Could not disable autonomous mode: %d", ret);
		goto err_remove_sysfs;
	}

	hid_info(hdev, "LampArray device registered\n");

	ret = lamparray_restore_state(ldev);
	if (ret) {
		hid_err(hdev, "Failed to set standard state: %d", ret);
		goto err_remove_sysfs;
	}
	return la;

err_remove_sysfs:
	lamparray_remove_sysfs(ldev);
err_xa_erase:
	xa_erase(&lamparray_by_hdev, (unsigned long)hdev);
err_unregister_led:
	lamparray_unregister_led(ldev);
err_free:
	kfree(la);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(lamparray_register);

void lamparray_unregister(struct lamparray *la)
{
	struct lamparray_device *ldev;

	if (!la)
		return;

	ldev = &la->ldev;

	lamparray_unregister_led(ldev);
	lamparray_remove_sysfs(ldev);
	xa_erase(&lamparray_by_hdev, (unsigned long)ldev->hdev);

	kfree(la);
}
EXPORT_SYMBOL_GPL(lamparray_unregister);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HID LampArray helper module (single-zone RGB)");
