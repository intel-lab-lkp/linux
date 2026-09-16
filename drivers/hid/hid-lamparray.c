// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hid-lamparray.c - HID LampArray helper module (single-zone RGB)
 *
 * Helper module for HID drivers supporting devices that expose a Lighting and
 * Illumination (LampArray) application collection (usage page 0x59).
 *
 * The module provides a minimal integration with the LED subsystem and treats
 * the device as a single zone: all lamps share one RGB value and a global
 * brightness level. It does not implement multi-zone layouts or hardware
 * effects.
 *
 * If enabled and a device supporting LampArray is found, one multicolor LED
 * class device is registered under /sys/class/leds/rgb:<function> to expose the
 * single-zone RGB control.
 *
 * The use_leds_uapi sysfs attribute is attached directly to the HID device
 * under /sys/bus/hid/devices/<HID-ID>/use_leds_uapi. Writing 0 to use_leds_uapi
 * unregisters the LED class device. The last state is kept cached. Writing 1
 * registers it again and restores the cached state to hardware. State is cached
 * as last known RGB + brightness.
 *
 * The module does not bind to devices on its own. Instead, a HID driver may
 * query support via lamparray_is_supported_device() after hid_parse() and
 * create an instance using lamparray_register().
 *
 * Copyright (C) 2026 Tim Guttzeit <tgu@tuxedocomputers.com>
 * Copyright (C) 2026 Aaron Erhardt <aer@tuxedocomputers.com>
 */

#include <linux/device.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/hid.h>
#include <linux/leds.h>
#include <linux/sysfs.h>
#include <linux/hid-lamparray.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/container_of.h>
#include <linux/led-class-multicolor.h>
#include <linux/xarray.h>

/* Constants */

/* HID usages (LampArray, etc.) */
#define HID_LIGHTING_ILLUMINATION_USAGE_PAGE	0x0059

/* HID usage types */
#define HID_APPLICATION_COLLECTION_USAGE_TYPE	0x0001
#define HID_LAMPARRAY_ATTRIBUTES_REPORT		0x0002
#define HID_LAMP_ATTRIBUTES_RESPONSE_REPORT	0x0022
#define HID_LAMP_RANGE_UPDATE_REPORT		0x0060
#define HID_LAMPARRAY_CONTROL_REPORT		0x0070

/* HID attributes */
#define HID_LAIP_LAMP_COUNT			0x0003
#define HID_LAIP_LAMPARRAY_KIND			0x0007
#define HID_LAIP_RED_LEVEL_COUNT		0x0028
#define HID_LAIP_GREEN_LEVEL_COUNT		0x0029
#define HID_LAIP_BLUE_LEVEL_COUNT		0x002a
#define HID_LAIP_INTENSITY_LEVEL_COUNT		0x002b
#define HID_LAIP_RED_UPDATE_CHANNEL		0x0051
#define HID_LAIP_GREEN_UPDATE_CHANNEL		0x0052
#define HID_LAIP_BLUE_UPDATE_CHANNEL		0x0053
#define HID_LAIP_INTENSITY_UPDATE_CHANNEL	0x0054
#define HID_LAIP_LAMP_ID_START			0x0061
#define HID_LAIP_LAMP_ID_END			0x0062
#define HID_LAIP_AUTONOMOUS_MODE		0x0071

/* LampArrayKind values */
#define HID_LAMPARRAY_KIND_KEYBOARD		0x0001

/* Helper struct for fields and their indices */
struct hid_field_value {
	struct hid_field *field;
	int index;
};

/* Helper struct for color fields */
struct lamparray_color_fields {
	struct hid_field_value red;
	struct hid_field_value green;
	struct hid_field_value blue;
	struct hid_field_value intensity;
};

/* Device state */
struct lamparray_device {
	struct hid_device *hdev;

	struct lamparray_color_fields color_levels;
	struct lamparray_color_fields color_update;

	struct hid_field_value autonomous_field;
	struct hid_field_value range_start;
	struct hid_field_value range_end;
	struct hid_field_value lamp_count;
	struct hid_field_value lamparray_kind;

	u16 lamp_count_value;
	u32 lamparray_kind_value;

	struct led_classdev_mc mc_cdev;
	struct mc_subled subleds[3];

	struct mutex dev_lock; /* Protects cached state and HID access */

	u8 max_r;
	u8 max_g;
	u8 max_b;
	u8 max_brightness;

	u8 last_r;
	u8 last_g;
	u8 last_b;
	u8 last_brightness;

	struct mutex sysfs_lock; /* Protects sysfs LED (de-)initialization */

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

/*
 * Mapping for hid_device pointers to their lamparray data.
 * Since there is not guarantee of how the driver using this library
 * will use its drvdata, the only safe way to retrieve the lamparray
 * data from a HID device pointer is using this mapping.
 */
static DEFINE_XARRAY(lamparray_by_hdev);

/* HID helper functions */

static int get_field_value(struct hid_field_value *field_value)
{
	return field_value->field->value[field_value->index];
}

static u8 get_field_value_as_u8(struct hid_field_value *field_value)
{
	return clamp_val(get_field_value(field_value), 0, U8_MAX);
}

static void set_field_value(struct hid_field_value *field_value, int value)
{
	field_value->field->value[field_value->index] = value;
}

static bool lamparray_color_fields_is_complete(struct lamparray_color_fields *color_fields)
{
	return color_fields->red.field && color_fields->green.field &&
	       color_fields->blue.field && color_fields->intensity.field;
}

static int lamparray_read_attributes_report(struct lamparray_device *ldev)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_report *report;
	int ret;
	size_t len;

	if (!ldev->lamp_count.field) {
		hid_warn(hdev, "No LampCount field found\n");
		return -ENODEV;
	}

	if (!ldev->lamparray_kind.field) {
		hid_warn(hdev, "No LampArrayKind field found\n");
		return -ENODEV;
	}

	report = ldev->lamp_count.field->report;

	if (!report) {
		hid_warn(hdev, "LampCount field has no report\n");
		return -ENODEV;
	}

	len = hid_report_len(report);
	u8 *buf __free(kfree) = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	guard(mutex)(&ldev->dev_lock);

	/* Update values */
	ret = hid_hw_raw_request(hdev, report->id, buf, len,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_warn(hdev, "Failed to request LampArrayAttributesReport: %d\n", ret);
		return ret;
	}

	ret = hid_report_raw_event(hdev, HID_FEATURE_REPORT, buf, len, ret, 0);
	if (ret < 0) {
		hid_warn(hdev, "Failed to parse LampArrayAttributesReport: %d\n", ret);
		return ret;
	}

	ldev->lamp_count_value = get_field_value(&ldev->lamp_count);
	if (!ldev->lamp_count_value) {
		hid_warn(hdev, "LampCount is 0 (invalid)\n");
		return -EINVAL;
	}

	ldev->lamparray_kind_value = get_field_value(&ldev->lamparray_kind);
	return 0;
}

static int lamparray_parse_update_report(struct lamparray_device *ldev)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_report_enum *re;
	struct hid_report *report;
	struct hid_field *field;
	int i, j;
	int ret = 0;

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
				u32 collection_idx = field->usage[j].collection_index;
				u32 collection_usage = hdev->collection[collection_idx].usage;

				u16 page = (usage & HID_USAGE_PAGE) >> 16;
				u16 id = usage & HID_USAGE;
				u16 collection_usage_id = collection_usage & U16_MAX;

				if (page != HID_LIGHTING_ILLUMINATION_USAGE_PAGE)
					continue;

				if (collection_usage_id == HID_LAMPARRAY_ATTRIBUTES_REPORT) {
					switch (id) {
					case HID_LAIP_LAMP_COUNT:
						ldev->lamp_count.field = field;
						ldev->lamp_count.index = j;
						break;
					case HID_LAIP_LAMPARRAY_KIND:
						ldev->lamparray_kind.field = field;
						ldev->lamparray_kind.index = j;
						break;
					}
				} else if (collection_usage_id ==
					   HID_LAMP_ATTRIBUTES_RESPONSE_REPORT) {
					switch (id) {
					case HID_LAIP_RED_LEVEL_COUNT:
						ldev->color_levels.red.field = field;
						ldev->color_levels.red.index = j;
						break;
					case HID_LAIP_GREEN_LEVEL_COUNT:
						ldev->color_levels.green.field = field;
						ldev->color_levels.green.index = j;
						break;
					case HID_LAIP_BLUE_LEVEL_COUNT:
						ldev->color_levels.blue.field = field;
						ldev->color_levels.blue.index = j;
						break;
					case HID_LAIP_INTENSITY_LEVEL_COUNT:
						ldev->color_levels.intensity.field = field;
						ldev->color_levels.intensity.index = j;
						break;
					}
				} else if (collection_usage_id == HID_LAMP_RANGE_UPDATE_REPORT) {
					switch (id) {
					case HID_LAIP_RED_UPDATE_CHANNEL:
						ldev->color_update.red.field = field;
						ldev->color_update.red.index = j;
						break;
					case HID_LAIP_GREEN_UPDATE_CHANNEL:
						ldev->color_update.green.field = field;
						ldev->color_update.green.index = j;
						break;
					case HID_LAIP_BLUE_UPDATE_CHANNEL:
						ldev->color_update.blue.field = field;
						ldev->color_update.blue.index = j;
						break;
					case HID_LAIP_INTENSITY_UPDATE_CHANNEL:
						ldev->color_update.intensity.field = field;
						ldev->color_update.intensity.index = j;
						break;
					case HID_LAIP_LAMP_ID_START:
						ldev->range_start.field = field;
						ldev->range_start.index = j;
						break;
					case HID_LAIP_LAMP_ID_END:
						ldev->range_end.field = field;
						ldev->range_end.index = j;
						break;
					default:
						break;
					}
				} else if (collection_usage_id == HID_LAMPARRAY_CONTROL_REPORT &&
					   id == HID_LAIP_AUTONOMOUS_MODE) {
					ldev->autonomous_field.field = field;
					ldev->autonomous_field.index = j;
				}
			}
		}
	}

	if (!ldev->autonomous_field.field ||
	    !lamparray_color_fields_is_complete(&ldev->color_update))
		ret = -ENODEV;

	return ret;
}

static int lamparray_hw_set_autonomous(struct lamparray_device *ldev,
				       bool enable)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_field *field = ldev->autonomous_field.field;

	if (!field)
		return -ENODEV;

	guard(mutex)(&ldev->dev_lock);
	set_field_value(&ldev->autonomous_field, !!enable);
	hid_hw_request(hdev, field->report, HID_REQ_SET_REPORT);

	return 0;
}

/* Make sure to hold dev lock to prevent concurrent access. */
static int lamparray_hw_set_state(struct lamparray_device *ldev, u8 r, u8 g,
				  u8 b, u8 intensity)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_report *report;

	if (!lamparray_color_fields_is_complete(&ldev->color_update))
		return -ENODEV;

	if (ldev->range_start.field && ldev->range_end.field) {
		set_field_value(&ldev->range_start, 0);
		set_field_value(&ldev->range_end, ldev->lamp_count_value - 1);
	}

	set_field_value(&ldev->color_update.red, r);
	set_field_value(&ldev->color_update.green, g);
	set_field_value(&ldev->color_update.blue, b);
	set_field_value(&ldev->color_update.intensity, intensity);

	report = ldev->color_update.red.field->report;
	hid_hw_request(hdev, report, HID_REQ_SET_REPORT);

	return 0;
}

/*
 * Simple helper to read the color information of the first lamp.
 * This does not read the state of the whole lamp array since this driver only
 * exposes one LED anyway, so one color is sufficient here for now.
 */
static int lamparray_get_lamp_attributes(struct lamparray_device *ldev)
{
	struct hid_device *hdev = ldev->hdev;
	struct hid_report *report;
	int ret;
	size_t len;

	if (!lamparray_color_fields_is_complete(&ldev->color_levels))
		return -ENODEV;

	/*
	 * All four fields must share the same report since the
	 * attributes are fetched with a single GET_REPORT below.
	 */
	report = ldev->color_levels.red.field->report;
	if (!report ||
	    ldev->color_levels.green.field->report != report ||
	    ldev->color_levels.blue.field->report != report ||
	    ldev->color_levels.intensity.field->report != report)
		return -ENODEV;

	len = hid_report_len(report);
	u8 *buf __free(kfree) = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	guard(mutex)(&ldev->dev_lock);

	/* Update values */
	ret = hid_hw_raw_request(hdev, report->id, buf, len,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_warn(hdev, "Failed to request LampAttributesResponseReport: %d\n", ret);
		return ret;
	}

	ret = hid_report_raw_event(hdev, HID_FEATURE_REPORT, buf, len, ret, 0);
	if (ret < 0) {
		hid_warn(hdev, "Failed to parse LampAttributesResponseReport: %d\n", ret);
		return ret;
	}

	ldev->max_r = get_field_value_as_u8(&ldev->color_levels.red);
	ldev->max_g = get_field_value_as_u8(&ldev->color_levels.green);
	ldev->max_b = get_field_value_as_u8(&ldev->color_levels.blue);
	ldev->max_brightness = get_field_value_as_u8(&ldev->color_levels.intensity);

	if (!ldev->max_r || !ldev->max_g || !ldev->max_b || !ldev->max_brightness) {
		hid_warn(hdev, "LampArray device has no color levels\n");
		return -EINVAL;
	}

	return 0;
}

/* Helper functions */

static int lamparray_restore_state(struct lamparray_device *ldev)
{
	u8 r, g, b;
	int ret;
	enum led_brightness brightness;

	if (!ldev->use_leds_uapi)
		return 0;

	guard(mutex)(&ldev->dev_lock);

	r = ldev->last_r;
	g = ldev->last_g;
	b = ldev->last_b;
	brightness = ldev->last_brightness;

	ldev->mc_cdev.subled_info[0].intensity = r;
	ldev->mc_cdev.subled_info[1].intensity = g;
	ldev->mc_cdev.subled_info[2].intensity = b;
	ldev->mc_cdev.led_cdev.brightness = brightness;

	/*
	 * Quirk for devices ignoring intensity values, see
	 * lamparray_led_brightness_set for more information.
	 */
	if (brightness)
		ret = lamparray_hw_set_state(ldev, r, g, b, brightness);
	else
		ret = lamparray_hw_set_state(ldev, 0, 0, 0, 0);

	return ret;
}

/* LEDs API */

static int lamparray_led_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct lamparray_device *ldev =
		container_of_const(mc, struct lamparray_device, mc_cdev);
	u8 r, g, b;
	int ret;

	/*
	 * Brightness is handled by the LampArray device if supported,
	 * so we can pass the raw intensity values.
	 */
	r = mc->subled_info[0].intensity;
	g = mc->subled_info[1].intensity;
	b = mc->subled_info[2].intensity;
	mc->led_cdev.brightness = brightness;

	guard(mutex)(&ldev->dev_lock);

	/*
	 * Some devices completely ignore the intensity (brightness)
	 * value and instead rely on zeros for each RGB channel to turn
	 * the LEDs off (e.g. the ASUS Prime B650-Plus). Since it doesn't
	 * hurt spec-compliant devices to quirk this behavior, we turn
	 * off the RGB channels unconditionally when the brightness is
	 * zero.
	 */
	if (brightness)
		ret = lamparray_hw_set_state(ldev, r, g, b, brightness);
	else
		ret = lamparray_hw_set_state(ldev, 0, 0, 0, 0);

	if (ret) {
		hid_err(ldev->hdev, "Failed to send LampArray update: %d\n",
			ret);
		return ret;
	}

	ldev->last_r = r;
	ldev->last_g = g;
	ldev->last_b = b;
	ldev->last_brightness = brightness;

	return 0;
}

static enum led_brightness
lamparray_led_brightness_get(struct led_classdev *cdev)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct lamparray_device *ldev =
	    container_of_const(mc, struct lamparray_device, mc_cdev);

	return ldev->last_brightness;
}

/* Make sure to hold sysfs lock to prevent concurrent access. */
static int lamparray_register_led(struct lamparray_device *ldev)
{
	struct device *dev = &ldev->hdev->dev;
	struct led_classdev *cdev = &ldev->mc_cdev.led_cdev;
	int ret;

	if (ldev->led_registered)
		return 0;

	if (!cdev->name) {
		/* Fallback value */
		const char *function = LED_FUNCTION_STATUS;

		/* Some heuristics for choosing a better LED function. */
		if (ldev->lamparray_kind_value == HID_LAMPARRAY_KIND_KEYBOARD)
			function = LED_FUNCTION_KBD_BACKLIGHT;

		cdev->name = kasprintf(GFP_KERNEL, "rgb:%s", function);
		if (!cdev->name)
			return -ENOMEM;
	}

	mutex_lock(&ldev->dev_lock);
	/* Setup */
	cdev->max_brightness = ldev->max_brightness;
	cdev->brightness_set_blocking = lamparray_led_brightness_set;
	cdev->brightness_get = lamparray_led_brightness_get;
	/* Don't do anything automatically on shutdown or suspend. */
	cdev->flags |= LED_RETAIN_AT_SHUTDOWN;

	ldev->subleds[0].color_index = LED_COLOR_ID_RED;
	ldev->subleds[0].max_intensity = ldev->max_r;
	ldev->subleds[1].color_index = LED_COLOR_ID_GREEN;
	ldev->subleds[1].max_intensity = ldev->max_g;
	ldev->subleds[2].color_index = LED_COLOR_ID_BLUE;
	ldev->subleds[2].max_intensity = ldev->max_b;

	/* Set values */
	ldev->subleds[0].intensity = ldev->last_r;
	ldev->subleds[1].intensity = ldev->last_g;
	ldev->subleds[2].intensity = ldev->last_b;
	cdev->brightness = ldev->last_brightness;

	ldev->mc_cdev.subled_info = ldev->subleds;
	ldev->mc_cdev.num_colors = ARRAY_SIZE(ldev->subleds);

	mutex_unlock(&ldev->dev_lock);

	ret = led_classdev_multicolor_register(dev, &ldev->mc_cdev);
	if (ret) {
		hid_warn(ldev->hdev, "Failed to register LED classdev for lamparray\n");
		kfree(cdev->name);
		cdev->name = NULL;
		return ret;
	}

	ldev->led_registered = true;

	return 0;
}

/* Make sure to hold sysfs lock to prevent concurrent access. */
static void lamparray_unregister_led(struct lamparray_device *ldev)
{
	struct led_classdev *cdev = &ldev->mc_cdev.led_cdev;

	if (ldev->led_registered)
		led_classdev_multicolor_unregister(&ldev->mc_cdev);

	ldev->led_registered = false;
	kfree(cdev->name);
	cdev->name = NULL;
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
	bool val, old_val;
	int ret;

	if (!ldev)
		return -ENODEV;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	/* Make sure only one concurrent write is possible */
	guard(mutex)(&ldev->sysfs_lock);
	old_val = ldev->use_leds_uapi;

	if (val == old_val)
		return count;

	ldev->use_leds_uapi = val;

	/*
	 * Take the device out of autonomous mode before exposing the LED
	 * class device, and hand it back afterwards, so that control is
	 * transferred in both directions rather than only at probe.
	 */
	if (val == 1) {
		ret = lamparray_hw_set_autonomous(ldev, false);
		if (ret)
			goto err_revert;
		ret = lamparray_register_led(ldev);
		if (ret) {
			lamparray_hw_set_autonomous(ldev, true);
			goto err_revert;
		}
		ret = lamparray_restore_state(ldev);
		if (ret) {
			hid_warn(ldev->hdev, "Could not restore state: %d\n", ret);
			lamparray_unregister_led(ldev);
			lamparray_hw_set_autonomous(ldev, true);
			goto err_revert;
		}
	} else {
		lamparray_unregister_led(ldev);
		ret = lamparray_hw_set_autonomous(ldev, true);
		if (ret) {
			hid_warn(ldev->hdev, "Could not enable autonomous mode: %d\n", ret);
			return count;
		}
	}

	return count;

err_revert:
	ldev->use_leds_uapi = old_val;
	return ret;
}
static DEVICE_ATTR_RW(use_leds_uapi);

static int lamparray_register_sysfs(struct lamparray_device *ldev)
{
	struct device *dev = &ldev->hdev->dev;
	int ret;

	ret = device_create_file(dev, &dev_attr_use_leds_uapi);
	if (ret)
		hid_err(ldev->hdev,
			"Failed to create lamparray sysfs group: %d\n", ret);

	return ret;
}

static void lamparray_remove_sysfs(struct lamparray_device *ldev)
{
	device_remove_file(&ldev->hdev->dev, &dev_attr_use_leds_uapi);
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
	struct lamparray_device *ldev;

	if (!hdev)
		return ERR_PTR(-ENODEV);

	struct lamparray *la __free(kfree) = kzalloc_obj(*la, GFP_KERNEL);
	if (!la)
		return ERR_PTR(-ENOMEM);

	ldev = &la->ldev;

	mutex_init(&ldev->dev_lock);
	mutex_init(&ldev->sysfs_lock);
	ldev->hdev = hdev;
	ldev->use_leds_uapi = true;
	ldev->led_registered = false;

	ret = lamparray_parse_update_report(ldev);
	if (ret) {
		hid_err(hdev, "No LampArray update report found: %d\n", ret);
		goto err_free;
	}

	ret = lamparray_read_attributes_report(ldev);
	if (ret) {
		hid_err(hdev,
			"Could not determine LampCount: %d\n",
			ret);
		goto err_free;
	}

	ret = lamparray_get_lamp_attributes(ldev);
	if (ret) {
		hid_err(hdev,
			"Faulty device. Could not query lamp attributes.\n");
		goto err_free;
	}

	/* Use black (all zeros) as default. */
	if (led_init_state) {
		ldev->last_r = min(led_init_state->r, ldev->max_r);
		ldev->last_g = min(led_init_state->g, ldev->max_g);
		ldev->last_b = min(led_init_state->b, ldev->max_b);
		ldev->last_brightness = min(led_init_state->brightness,
					    ldev->max_brightness);
	}

	/* No locking needed, no races are possible before sysfs setup. */
	ret = lamparray_register_led(ldev);
	if (ret) {
		hid_warn(hdev, "Failed to register LED UAPI: %d\n", ret);
		ldev->use_leds_uapi = false;
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
		hid_err(hdev, "Failed to set default state: %d", ret);
		goto err_remove_sysfs;
	}

	return_ptr(la);

err_remove_sysfs:
	lamparray_remove_sysfs(ldev);
err_xa_erase:
	xa_erase(&lamparray_by_hdev, (unsigned long)hdev);
err_unregister_led:
	mutex_lock(&ldev->sysfs_lock);
	lamparray_unregister_led(ldev);
	mutex_unlock(&ldev->sysfs_lock);
err_free:
	mutex_destroy(&ldev->dev_lock);
	mutex_destroy(&ldev->sysfs_lock);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(lamparray_register);

void lamparray_unregister(struct lamparray *la)
{
	struct lamparray_device *ldev;

	if (!la)
		return;

	ldev = &la->ldev;

	lamparray_hw_set_autonomous(ldev, true);

	lamparray_remove_sysfs(ldev);
	xa_erase(&lamparray_by_hdev, (unsigned long)ldev->hdev);

	mutex_lock(&ldev->sysfs_lock);
	lamparray_unregister_led(ldev);
	mutex_unlock(&ldev->sysfs_lock);

	mutex_destroy(&ldev->dev_lock);
	mutex_destroy(&ldev->sysfs_lock);
	kfree(la);
}
EXPORT_SYMBOL_GPL(lamparray_unregister);

/*
 * Blank all lamps on suspend rather than handing control back to the firmware,
 * which may not turn them off in low power states. On an Acer Predator PT14-52T,
 * system power draw during s2idle was ~12.35W with lamps lit, and ~2.84W with
 * them blanked; the lighting accounted for ~77% of the power draw during suspend.
 * Since writing zeroes is well defined on all lamparray devices, always do it.
 * This is ignored if use_leds_uapi is 0; let userspace keep full control.
 */
void lamparray_suspend(struct lamparray *la)
{
	struct lamparray_device *ldev;

	if (!la)
		return;

	ldev = &la->ldev;

	if (!ldev->use_leds_uapi)
		return;

	mutex_lock(&ldev->dev_lock);
	lamparray_hw_set_state(ldev, 0, 0, 0, 0);
	mutex_unlock(&ldev->dev_lock);

	hid_hw_wait(ldev->hdev);
}
EXPORT_SYMBOL_GPL(lamparray_suspend);

void lamparray_resume(struct lamparray *la)
{
	struct lamparray_device *ldev;

	if (!la)
		return;

	ldev = &la->ldev;

	if (!ldev->use_leds_uapi)
		return;

	/*
	 * After a S4 transition, some devices report
	 * AutonomousMode = 0 while still ignoring host lamp updates.
	 * Writing 0 again does nothing; forcing a 1 -> 0
	 * will guarantee the device will update.
	 */
	lamparray_hw_set_autonomous(ldev, true);
	hid_hw_wait(ldev->hdev);
	lamparray_hw_set_autonomous(ldev, false);

	lamparray_restore_state(ldev);
}
EXPORT_SYMBOL_GPL(lamparray_resume);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tim Guttzeit <tgu@tuxedocomputers.com>");
MODULE_AUTHOR("Aaron Erhardt <aer@tuxedocomputers.com>");
MODULE_DESCRIPTION("HID LampArray helper module (single-zone RGB)");
