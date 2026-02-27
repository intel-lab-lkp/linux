// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Steelseries devices
 *
 *  Copyright (c) 2013 Simon Wood
 *  Copyright (c) 2023 Bastien Nocera
 *  Copyright (c) 2025 Sriman Achanta
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/leds.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <sound/control.h>
#include <sound/core.h>

#include "hid-ids.h"

#define SS_CAP_BATTERY			BIT(0)
#define SS_CAP_CHATMIX			BIT(1)
#define SS_CAP_MIC_MUTE			BIT(2)
#define SS_CAP_BT_ENABLED		BIT(3)
#define SS_CAP_BT_DEVICE_CONNECTED	BIT(4)
#define SS_CAP_EXTERNAL_CONFIG		BIT(5)
#define SS_CAP_SIDETONE			BIT(6)
#define SS_CAP_MIC_VOLUME		BIT(7)
#define SS_CAP_VOLUME_LIMITER		BIT(8)

#define SS_QUIRK_STATUS_SYNC_POLL	BIT(0)

#define SS_SETTING_SIDETONE		0
#define SS_SETTING_MIC_VOLUME		1
#define SS_SETTING_VOLUME_LIMITER	2

struct steelseries_device;

struct steelseries_device_info {
	unsigned long capabilities;
	unsigned long quirks;

	u8 sync_interface;
	u8 async_interface;

	u8 sidetone_max;
	u8 mic_volume_min;
	u8 mic_volume_max;

	int (*request_status)(struct hid_device *hdev);
	void (*parse_status)(struct steelseries_device *sd, u8 *data, int size);

	int (*request_settings)(struct hid_device *hdev);
	void (*parse_settings)(struct steelseries_device *sd, u8 *data, int size);
	int (*write_setting)(struct hid_device *hdev, u8 setting, u8 value);
};

struct steelseries_device {
	struct hid_device *hdev;
	const struct steelseries_device_info *info;

	bool use_async_protocol;

	struct delayed_work status_work;
	struct delayed_work settings_work;

	struct power_supply_desc battery_desc;
	struct power_supply *battery;
	bool headset_connected;
	u8 battery_capacity;
	bool battery_charging;

	struct snd_card *card;
	struct snd_ctl_elem_id chatmix_chat_id;
	struct snd_ctl_elem_id chatmix_game_id;
	struct snd_ctl_elem_id mic_muted_id;
	struct snd_ctl_elem_id sidetone_id;
	struct snd_ctl_elem_id mic_volume_id;
	struct snd_ctl_elem_id volume_limiter_id;
	u8 chatmix_chat;
	u8 chatmix_game;
	bool mic_muted;
	u8 sidetone;
	u8 mic_volume;
	bool volume_limiter;

	bool bt_enabled;
	bool bt_device_connected;

	spinlock_t lock;
	bool removed;
};

#if IS_BUILTIN(CONFIG_LEDS_CLASS) || \
    (IS_MODULE(CONFIG_LEDS_CLASS) && IS_MODULE(CONFIG_HID_STEELSERIES))
#define SRWS1_NUMBER_LEDS 15
struct steelseries_srws1_data {
	__u16 led_state;
	/* the last element is used for setting all leds simultaneously */
	struct led_classdev *led[SRWS1_NUMBER_LEDS + 1];
};
#endif

/* Fixed report descriptor for Steelseries SRW-S1 wheel controller
 *
 * The original descriptor hides the sensitivity and assists dials
 * a custom vendor usage page. This inserts a patch to make them
 * appear in the 'Generic Desktop' usage.
 */

static const __u8 steelseries_srws1_rdesc_fixed[] = {
0x05, 0x01,         /*  Usage Page (Desktop)                */
0x09, 0x08,         /*  Usage (MultiAxis), Changed          */
0xA1, 0x01,         /*  Collection (Application),           */
0xA1, 0x02,         /*      Collection (Logical),           */
0x95, 0x01,         /*          Report Count (1),           */
0x05, 0x01,         /* Changed  Usage Page (Desktop),       */
0x09, 0x30,         /* Changed  Usage (X),                  */
0x16, 0xF8, 0xF8,   /*          Logical Minimum (-1800),    */
0x26, 0x08, 0x07,   /*          Logical Maximum (1800),     */
0x65, 0x14,         /*          Unit (Degrees),             */
0x55, 0x0F,         /*          Unit Exponent (15),         */
0x75, 0x10,         /*          Report Size (16),           */
0x81, 0x02,         /*          Input (Variable),           */
0x09, 0x31,         /* Changed  Usage (Y),                  */
0x15, 0x00,         /*          Logical Minimum (0),        */
0x26, 0xFF, 0x03,   /*          Logical Maximum (1023),     */
0x75, 0x0C,         /*          Report Size (12),           */
0x81, 0x02,         /*          Input (Variable),           */
0x09, 0x32,         /* Changed  Usage (Z),                  */
0x15, 0x00,         /*          Logical Minimum (0),        */
0x26, 0xFF, 0x03,   /*          Logical Maximum (1023),     */
0x75, 0x0C,         /*          Report Size (12),           */
0x81, 0x02,         /*          Input (Variable),           */
0x05, 0x01,         /*          Usage Page (Desktop),       */
0x09, 0x39,         /*          Usage (Hat Switch),         */
0x25, 0x07,         /*          Logical Maximum (7),        */
0x35, 0x00,         /*          Physical Minimum (0),       */
0x46, 0x3B, 0x01,   /*          Physical Maximum (315),     */
0x65, 0x14,         /*          Unit (Degrees),             */
0x75, 0x04,         /*          Report Size (4),            */
0x95, 0x01,         /*          Report Count (1),           */
0x81, 0x02,         /*          Input (Variable),           */
0x25, 0x01,         /*          Logical Maximum (1),        */
0x45, 0x01,         /*          Physical Maximum (1),       */
0x65, 0x00,         /*          Unit,                       */
0x75, 0x01,         /*          Report Size (1),            */
0x95, 0x03,         /*          Report Count (3),           */
0x81, 0x01,         /*          Input (Constant),           */
0x05, 0x09,         /*          Usage Page (Button),        */
0x19, 0x01,         /*          Usage Minimum (01h),        */
0x29, 0x11,         /*          Usage Maximum (11h),        */
0x95, 0x11,         /*          Report Count (17),          */
0x81, 0x02,         /*          Input (Variable),           */
		    /*   ---- Dial patch starts here ----   */
0x05, 0x01,         /*          Usage Page (Desktop),       */
0x09, 0x33,         /*          Usage (RX),                 */
0x75, 0x04,         /*          Report Size (4),            */
0x95, 0x02,         /*          Report Count (2),           */
0x15, 0x00,         /*          Logical Minimum (0),        */
0x25, 0x0b,         /*          Logical Maximum (b),        */
0x81, 0x02,         /*          Input (Variable),           */
0x09, 0x35,         /*          Usage (RZ),                 */
0x75, 0x04,         /*          Report Size (4),            */
0x95, 0x01,         /*          Report Count (1),           */
0x25, 0x03,         /*          Logical Maximum (3),        */
0x81, 0x02,         /*          Input (Variable),           */
		    /*    ---- Dial patch ends here ----    */
0x06, 0x00, 0xFF,   /*          Usage Page (FF00h),         */
0x09, 0x01,         /*          Usage (01h),                */
0x75, 0x04,         /* Changed  Report Size (4),            */
0x95, 0x0D,         /* Changed  Report Count (13),          */
0x81, 0x02,         /*          Input (Variable),           */
0xC0,               /*      End Collection,                 */
0xA1, 0x02,         /*      Collection (Logical),           */
0x09, 0x02,         /*          Usage (02h),                */
0x75, 0x08,         /*          Report Size (8),            */
0x95, 0x10,         /*          Report Count (16),          */
0x91, 0x02,         /*          Output (Variable),          */
0xC0,               /*      End Collection,                 */
0xC0                /*  End Collection                      */
};

#if IS_BUILTIN(CONFIG_LEDS_CLASS) || \
    (IS_MODULE(CONFIG_LEDS_CLASS) && IS_MODULE(CONFIG_HID_STEELSERIES))
static void steelseries_srws1_set_leds(struct hid_device *hdev, __u16 leds)
{
	struct list_head *report_list = &hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	struct hid_report *report = list_entry(report_list->next, struct hid_report, list);
	__s32 *value = report->field[0]->value;

	value[0] = 0x40;
	value[1] = leds & 0xFF;
	value[2] = leds >> 8;
	value[3] = 0x00;
	value[4] = 0x00;
	value[5] = 0x00;
	value[6] = 0x00;
	value[7] = 0x00;
	value[8] = 0x00;
	value[9] = 0x00;
	value[10] = 0x00;
	value[11] = 0x00;
	value[12] = 0x00;
	value[13] = 0x00;
	value[14] = 0x00;
	value[15] = 0x00;

	hid_hw_request(hdev, report, HID_REQ_SET_REPORT);

	/* Note: LED change does not show on device until the device is read/polled */
}

static void steelseries_srws1_led_all_set_brightness(struct led_classdev *led_cdev,
			enum led_brightness value)
{
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hid = to_hid_device(dev);
	struct steelseries_srws1_data *drv_data = hid_get_drvdata(hid);

	if (!drv_data) {
		hid_err(hid, "Device data not found.");
		return;
	}

	if (value == LED_OFF)
		drv_data->led_state = 0;
	else
		drv_data->led_state = (1 << (SRWS1_NUMBER_LEDS + 1)) - 1;

	steelseries_srws1_set_leds(hid, drv_data->led_state);
}

static enum led_brightness steelseries_srws1_led_all_get_brightness(struct led_classdev *led_cdev)
{
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hid = to_hid_device(dev);
	struct steelseries_srws1_data *drv_data;

	drv_data = hid_get_drvdata(hid);

	if (!drv_data) {
		hid_err(hid, "Device data not found.");
		return LED_OFF;
	}

	return (drv_data->led_state >> SRWS1_NUMBER_LEDS) ? LED_FULL : LED_OFF;
}

static void steelseries_srws1_led_set_brightness(struct led_classdev *led_cdev,
			enum led_brightness value)
{
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hid = to_hid_device(dev);
	struct steelseries_srws1_data *drv_data = hid_get_drvdata(hid);
	int i, state = 0;

	if (!drv_data) {
		hid_err(hid, "Device data not found.");
		return;
	}

	for (i = 0; i < SRWS1_NUMBER_LEDS; i++) {
		if (led_cdev != drv_data->led[i])
			continue;

		state = (drv_data->led_state >> i) & 1;
		if (value == LED_OFF && state) {
			drv_data->led_state &= ~(1 << i);
			steelseries_srws1_set_leds(hid, drv_data->led_state);
		} else if (value != LED_OFF && !state) {
			drv_data->led_state |= 1 << i;
			steelseries_srws1_set_leds(hid, drv_data->led_state);
		}
		break;
	}
}

static enum led_brightness steelseries_srws1_led_get_brightness(struct led_classdev *led_cdev)
{
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hid = to_hid_device(dev);
	struct steelseries_srws1_data *drv_data;
	int i, value = 0;

	drv_data = hid_get_drvdata(hid);

	if (!drv_data) {
		hid_err(hid, "Device data not found.");
		return LED_OFF;
	}

	for (i = 0; i < SRWS1_NUMBER_LEDS; i++)
		if (led_cdev == drv_data->led[i]) {
			value = (drv_data->led_state >> i) & 1;
			break;
		}

	return value ? LED_FULL : LED_OFF;
}

static int steelseries_srws1_probe(struct hid_device *hdev,
		const struct hid_device_id *id)
{
	int ret, i;
	struct led_classdev *led;
	struct steelseries_srws1_data *drv_data;
	size_t name_sz;
	char *name;

	drv_data = devm_kzalloc(&hdev->dev, sizeof(*drv_data), GFP_KERNEL);
	if (drv_data == NULL) {
		hid_err(hdev, "can't alloc SRW-S1 memory\n");
		return -ENOMEM;
	}

	hid_set_drvdata(hdev, drv_data);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		goto err;
	}

	if (!hid_validate_values(hdev, HID_OUTPUT_REPORT, 0, 0, 16)) {
		ret = -ENODEV;
		goto err;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		goto err;
	}

	/* register led subsystem */
	drv_data->led_state = 0;
	for (i = 0; i < SRWS1_NUMBER_LEDS + 1; i++)
		drv_data->led[i] = NULL;

	steelseries_srws1_set_leds(hdev, 0);

	name_sz = strlen(hdev->uniq) + 16;

	/* 'ALL', for setting all LEDs simultaneously */
	led = devm_kzalloc(&hdev->dev, sizeof(struct led_classdev)+name_sz, GFP_KERNEL);
	if (!led) {
		hid_err(hdev, "can't allocate memory for LED ALL\n");
		goto out;
	}

	name = (void *)(&led[1]);
	snprintf(name, name_sz, "SRWS1::%s::RPMALL", hdev->uniq);
	led->name = name;
	led->brightness = 0;
	led->max_brightness = 1;
	led->brightness_get = steelseries_srws1_led_all_get_brightness;
	led->brightness_set = steelseries_srws1_led_all_set_brightness;

	drv_data->led[SRWS1_NUMBER_LEDS] = led;
	ret = devm_led_classdev_register(&hdev->dev, led);
	if (ret) {
		hid_err(hdev, "failed to register LED %d. Aborting.\n", SRWS1_NUMBER_LEDS);
		goto out; /* let the driver continue without LEDs */
	}

	/* Each individual LED */
	for (i = 0; i < SRWS1_NUMBER_LEDS; i++) {
		led = devm_kzalloc(&hdev->dev, sizeof(struct led_classdev)+name_sz, GFP_KERNEL);
		if (!led) {
			hid_err(hdev, "can't allocate memory for LED %d\n", i);
			break;
		}

		name = (void *)(&led[1]);
		snprintf(name, name_sz, "SRWS1::%s::RPM%d", hdev->uniq, i+1);
		led->name = name;
		led->brightness = 0;
		led->max_brightness = 1;
		led->brightness_get = steelseries_srws1_led_get_brightness;
		led->brightness_set = steelseries_srws1_led_set_brightness;

		drv_data->led[i] = led;
		ret = devm_led_classdev_register(&hdev->dev, led);

		if (ret) {
			hid_err(hdev, "failed to register LED %d. Aborting.\n", i);
			break;	/* but let the driver continue without LEDs */
		}
	}
out:
	return 0;
err:
	return ret;
}
#endif

static const __u8 *steelseries_srws1_report_fixup(struct hid_device *hdev,
		__u8 *rdesc, unsigned int *rsize)
{
	if (hdev->vendor != USB_VENDOR_ID_STEELSERIES ||
	    hdev->product != USB_DEVICE_ID_STEELSERIES_SRWS1)
		return rdesc;

	if (*rsize >= 115 && rdesc[11] == 0x02 && rdesc[13] == 0xc8
			&& rdesc[29] == 0xbb && rdesc[40] == 0xc5) {
		hid_info(hdev, "Fixing up Steelseries SRW-S1 report descriptor\n");
		*rsize = sizeof(steelseries_srws1_rdesc_fixed);
		return steelseries_srws1_rdesc_fixed;
	}
	return rdesc;
}

/*
 * Headset report helpers
 */

static int steelseries_send_report(struct hid_device *hdev, const u8 *data,
				    int len, enum hid_report_type type)
{
	u8 *buf;
	int ret;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, data[0], buf, len, type,
				 HID_REQ_SET_REPORT);
	kfree(buf);

	if (ret < 0)
		return ret;
	if (ret < len)
		return -EIO;

	return 0;
}

static inline int steelseries_send_feature_report(struct hid_device *hdev,
						   const u8 *data, int len)
{
	return steelseries_send_report(hdev, data, len, HID_FEATURE_REPORT);
}

static inline int steelseries_send_output_report(struct hid_device *hdev,
						  const u8 *data, int len)
{
	return steelseries_send_report(hdev, data, len, HID_OUTPUT_REPORT);
}

/*
 * Headset settings write functions
 */

static int steelseries_arctis_1_write_setting(struct hid_device *hdev,
					      u8 setting, u8 value)
{
	switch (setting) {
	case SS_SETTING_SIDETONE:
		if (value == 0) {
			const u8 data[] = { 0x06, 0x35 };

			return steelseries_send_feature_report(hdev, data,
							       sizeof(data));
		} else {
			const u8 data[] = { 0x06, 0x35, 0x01, 0x00, value };

			return steelseries_send_feature_report(hdev, data,
							       sizeof(data));
		}
	default:
		return -EINVAL;
	}
}

static int steelseries_arctis_9_write_setting(struct hid_device *hdev,
					     u8 setting, u8 value)
{
	switch (setting) {
	case SS_SETTING_SIDETONE: {
		const u8 data[] = { 0x06, 0x00, value + 0xc0 };

		return steelseries_send_feature_report(hdev, data, sizeof(data));
	}
	default:
		return -EINVAL;
	}
}

static int steelseries_arctis_nova_3p_write_setting(struct hid_device *hdev,
						    u8 setting, u8 value)
{
	const u8 save[] = { 0x09 };
	u8 cmd;
	int ret;
	u8 data[2];

	switch (setting) {
	case SS_SETTING_SIDETONE:
		cmd = 0x39;
		break;
	case SS_SETTING_MIC_VOLUME:
		cmd = 0x37;
		break;
	default:
		return -EINVAL;
	}

	data[0] = cmd;
	data[1] = value;

	ret = steelseries_send_feature_report(hdev, data, sizeof(data));
	if (ret)
		return ret;

	return steelseries_send_feature_report(hdev, save, sizeof(save));
}

static int steelseries_arctis_nova_5_write_setting(struct hid_device *hdev,
						   u8 setting, u8 value)
{
	const u8 save[] = { 0x00, 0x09 };
	u8 cmd;
	int ret;
	u8 data[3];

	switch (setting) {
	case SS_SETTING_SIDETONE:
		cmd = 0x39;
		break;
	case SS_SETTING_MIC_VOLUME:
		cmd = 0x37;
		break;
	case SS_SETTING_VOLUME_LIMITER:
		cmd = 0x27;
		break;
	default:
		return -EINVAL;
	}

	data[0] = 0x00;
	data[1] = cmd;
	data[2] = value;

	ret = steelseries_send_output_report(hdev, data, sizeof(data));
	if (ret)
		return ret;

	msleep(10);

	return steelseries_send_output_report(hdev, save, sizeof(save));
}

static int steelseries_arctis_nova_7_write_setting(struct hid_device *hdev,
						   u8 setting, u8 value)
{
	const u8 save[] = { 0x00, 0x09 };
	u8 cmd;
	int ret;
	u8 data[3];

	switch (setting) {
	case SS_SETTING_SIDETONE:
		cmd = 0x39;
		break;
	case SS_SETTING_MIC_VOLUME:
		cmd = 0x37;
		break;
	case SS_SETTING_VOLUME_LIMITER:
		cmd = 0x3a;
		break;
	default:
		return -EINVAL;
	}

	data[0] = 0x00;
	data[1] = cmd;
	data[2] = value;

	ret = steelseries_send_output_report(hdev, data, sizeof(data));
	if (ret)
		return ret;

	msleep(10);

	return steelseries_send_output_report(hdev, save, sizeof(save));
}

static int steelseries_arctis_nova_pro_write_setting(struct hid_device *hdev,
						     u8 setting, u8 value)
{
	const u8 save[] = { 0x06, 0x09 };
	u8 cmd;
	int ret;
	u8 data[3];

	switch (setting) {
	case SS_SETTING_SIDETONE:
		cmd = 0x39;
		break;
	case SS_SETTING_MIC_VOLUME:
		cmd = 0x37;
		break;
	default:
		return -EINVAL;
	}

	data[0] = 0x06;
	data[1] = cmd;
	data[2] = value;

	ret = steelseries_send_output_report(hdev, data, sizeof(data));
	if (ret)
		return ret;

	return steelseries_send_output_report(hdev, save, sizeof(save));
}

/*
 * Headset status request functions
 */

static int steelseries_arctis_1_request_status(struct hid_device *hdev)
{
	const u8 data[] = { 0x06, 0x12 };

	return steelseries_send_feature_report(hdev, data, sizeof(data));
}

static int steelseries_arctis_7_request_status(struct hid_device *hdev)
{
	int ret;
	const u8 connection_data[] = { 0x06, 0x14 };
	const u8 battery_data[] = { 0x06, 0x18 };
	const u8 chatmix_data[] = { 0x06, 0x24 };

	ret = steelseries_send_feature_report(hdev, connection_data, sizeof(connection_data));
	if (ret)
		return ret;

	msleep(10);

	ret = steelseries_send_feature_report(hdev, battery_data, sizeof(battery_data));
	if (ret)
		return ret;

	msleep(10);

	return steelseries_send_feature_report(hdev, chatmix_data, sizeof(chatmix_data));
}

static int steelseries_arctis_9_request_status(struct hid_device *hdev)
{
	const u8 data[] = { 0x00, 0x20 };

	return steelseries_send_feature_report(hdev, data, sizeof(data));
}

static int steelseries_arctis_nova_request_status(struct hid_device *hdev)
{
	const u8 data[] = { 0x00, 0xb0 };

	return steelseries_send_output_report(hdev, data, sizeof(data));
}

static int steelseries_arctis_nova_3p_request_status(struct hid_device *hdev)
{
	const u8 data[] = { 0xb0 };

	return steelseries_send_output_report(hdev, data, sizeof(data));
}

static int steelseries_arctis_nova_pro_request_status(struct hid_device *hdev)
{
	const u8 data[] = { 0x06, 0xb0 };

	return steelseries_send_output_report(hdev, data, sizeof(data));
}

/*
 * Headset battery helpers
 */

static int battery_capacity_to_level(int capacity)
{
	if (capacity >= 50)
		return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	if (capacity >= 20)
		return POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
}

static u8 steelseries_map_capacity(u8 capacity, u8 min_in, u8 max_in)
{
	if (capacity >= max_in)
		return 100;
	if (capacity <= min_in)
		return 0;
	return (capacity - min_in) * 100 / (max_in - min_in);
}

/*
 * Headset status parse functions
 */

static void steelseries_arctis_1_parse_status(struct steelseries_device *sd,
					      u8 *data, int size)
{
	if (size < 4)
		return;

	sd->headset_connected = (data[2] != 0x01);
	sd->battery_capacity = data[3];
}

static void steelseries_arctis_7_parse_status(struct steelseries_device *sd,
					      u8 *data, int size)
{
	if (size < 4)
		return;

	if (data[0] == 0x06) {
		switch (data[1]) {
		case 0x14:
			sd->headset_connected = (data[2] == 0x03);
			break;
		case 0x18:
			sd->battery_capacity = data[2];
			break;
		case 0x24:
			sd->chatmix_game = steelseries_map_capacity(data[2], 0xbf, 0xff);
			sd->chatmix_chat = steelseries_map_capacity(data[3], 0xbf, 0xff);
			break;
		}
	}
}

static void steelseries_arctis_7_plus_parse_status(struct steelseries_device *sd,
						   u8 *data, int size)
{
	if (size < 6)
		return;

	if (data[0] == 0xb0) {
		sd->headset_connected = !(data[1] == 0x01);
		sd->battery_capacity = steelseries_map_capacity(data[2], 0x00, 0x04);
		sd->battery_charging = (data[3] == 0x01);
		sd->chatmix_game = steelseries_map_capacity(data[4], 0x00, 0x64);
		sd->chatmix_chat = steelseries_map_capacity(data[5], 0x00, 0x64);
	}
}

static void steelseries_arctis_9_parse_status(struct steelseries_device *sd,
					      u8 *data, int size)
{
	if (size < 11)
		return;

	if (data[0] == 0xaa) {
		sd->headset_connected = (data[1] == 0x01);
		sd->battery_charging = (data[4] == 0x01);
		sd->battery_capacity = steelseries_map_capacity(data[3], 0x64, 0x9A);
		sd->chatmix_game = steelseries_map_capacity(data[9], 0x00, 0x13);
		sd->chatmix_chat = steelseries_map_capacity(data[10], 0x00, 0x13);
	}
}

static void steelseries_arctis_nova_3p_parse_status(struct steelseries_device *sd,
						   u8 *data, int size)
{
	if (size < 4)
		return;

	if (data[0] == 0xb0) {
		sd->headset_connected = !(data[1] == 0x02);
		sd->battery_capacity = steelseries_map_capacity(data[3], 0x00, 0x64);
	}
}

static void steelseries_arctis_nova_5_parse_status(struct steelseries_device *sd,
						   u8 *data, int size)
{
	if (size < 5)
		return;

	if (data[0] == 0xb0) {
		sd->headset_connected = !(data[1] == 0x02);
		sd->battery_capacity = data[3];
		sd->battery_charging = (data[4] == 0x01);
	}
}

static void steelseries_arctis_nova_5x_parse_status(struct steelseries_device *sd,
						   u8 *data, int size)
{
	if (size < 7)
		return;

	if (data[0] == 0xb0) {
		sd->headset_connected = !(data[1] == 0x02);
		sd->battery_capacity = data[3];
		sd->battery_charging = (data[4] == 0x01);
		sd->chatmix_chat = data[5];
		sd->chatmix_game = data[6];
	}
}

static void steelseries_arctis_nova_7_parse_status(struct steelseries_device *sd,
						   u8 *data, int size)
{
	if (size < 6)
		return;

	if (data[0] == 0xb0) {
		sd->headset_connected = (data[1] == 0x03);
		sd->battery_capacity = steelseries_map_capacity(data[2], 0x00, 0x04);
		sd->battery_charging = (data[3] == 0x01);
		sd->chatmix_game = data[4];
		sd->chatmix_chat = data[5];
	}
}

static void steelseries_arctis_nova_7_gen2_parse_status(struct steelseries_device *sd,
							u8 *data, int size)
{
	if (size < 10)
		return;

	switch (data[0]) {
	case 0xb0:
		sd->headset_connected = (data[1] == 0x03);
		sd->battery_capacity = data[2];
		sd->battery_charging = (data[3] == 0x01);
		sd->chatmix_game = data[4];
		sd->chatmix_chat = data[5];
		switch (data[6]) {
		case 0x00:
			sd->bt_enabled = false;
			sd->bt_device_connected = false;
			break;
		case 0x03:
			sd->bt_enabled = true;
			sd->bt_device_connected = false;
			break;
		case 0x02:
			sd->bt_enabled = true;
			sd->bt_device_connected = true;
			break;
		}
		sd->mic_muted = (data[9] == 0x01);
		break;
	case 0xb7:
		sd->battery_capacity = data[1];
		break;
	case 0xb9:
		sd->headset_connected = (data[1] == 0x03);
		break;
	case 0xbb:
		sd->battery_charging = (data[1] == 0x01);
		break;
	case 0x45:
		sd->chatmix_game = data[1];
		sd->chatmix_chat = data[2];
		break;
	case 0x52:
		sd->mic_muted = (data[2] == 0x01);
		break;
	case 0xb5:
		if (data[1] == 0x01) {
			sd->bt_enabled = false;
			sd->bt_device_connected = false;
		} else if (data[1] == 0x04) {
			sd->bt_enabled = true;
			sd->bt_device_connected = (data[2] == 0x01);
		}
		break;
	}
}

static int steelseries_arctis_nova_7_gen2_request_settings(struct hid_device *hdev)
{
	const u8 data[] = { 0x00, 0x20 };

	return steelseries_send_output_report(hdev, data, sizeof(data));
}

static void steelseries_arctis_nova_7_gen2_parse_settings(
	struct steelseries_device *sd, u8 *data, int size)
{
	if (size < 4)
		return;

	switch (data[0]) {
	case 0x20:
		sd->mic_volume = data[1];
		sd->sidetone = data[2];
		sd->volume_limiter = data[3];
		break;
	case 0x37:
		sd->mic_volume = data[1];
		break;
	case 0x39:
		sd->sidetone = data[1];
		break;
	case 0x3a:
		sd->volume_limiter = data[1];
		break;
	}
}

static void steelseries_arctis_nova_pro_parse_settings(
	struct steelseries_device *sd, u8 *data, int size)
{
	if (size < 10)
		return;

	if (data[0] == 0x06 && data[1] == 0xb0)
		sd->mic_volume = data[9];
}

static void steelseries_arctis_nova_pro_parse_status(struct steelseries_device *sd,
						     u8 *data, int size)
{
	if (size < 16)
		return;

	if (data[0] == 0x06 && data[1] == 0xb0) {
		sd->headset_connected = (data[15] == 0x08 || data[15] == 0x02);
		sd->battery_capacity = steelseries_map_capacity(data[6], 0x00, 0x08);
		sd->battery_charging = (data[15] == 0x02);
		sd->mic_muted = (data[9] == 0x01);
		sd->bt_enabled = (data[4] == 0x00);
		sd->bt_device_connected = (data[5] == 0x01);
	} else if (data[0] == 0x07 && data[1] == 0x45) {
		sd->chatmix_game = data[2];
		sd->chatmix_chat = data[3];
	}
}

/*
 * Device info definitions
 */

static const struct steelseries_device_info srws1_info = { };

static const struct steelseries_device_info arctis_1_info = {
	.sync_interface = 3,
	.capabilities = SS_CAP_BATTERY | SS_CAP_SIDETONE,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 18,
	.request_status = steelseries_arctis_1_request_status,
	.parse_status = steelseries_arctis_1_parse_status,
	.write_setting = steelseries_arctis_1_write_setting,
};

static const struct steelseries_device_info arctis_7_info = {
	.sync_interface = 5,
	.capabilities = SS_CAP_BATTERY | SS_CAP_CHATMIX | SS_CAP_SIDETONE,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 18,
	.request_status = steelseries_arctis_7_request_status,
	.parse_status = steelseries_arctis_7_parse_status,
	.write_setting = steelseries_arctis_1_write_setting,
};

static const struct steelseries_device_info arctis_7_plus_info = {
	.sync_interface = 3,
	.capabilities = SS_CAP_BATTERY | SS_CAP_CHATMIX | SS_CAP_SIDETONE,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 3,
	.request_status = steelseries_arctis_nova_request_status,
	.parse_status = steelseries_arctis_7_plus_parse_status,
	.write_setting = steelseries_arctis_nova_5_write_setting,
};

static const struct steelseries_device_info arctis_9_info = {
	.sync_interface = 0,
	.capabilities = SS_CAP_BATTERY | SS_CAP_CHATMIX | SS_CAP_SIDETONE,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 61,
	.request_status = steelseries_arctis_9_request_status,
	.parse_status = steelseries_arctis_9_parse_status,
	.write_setting = steelseries_arctis_9_write_setting,
};

static const struct steelseries_device_info arctis_nova_3p_info = {
	.sync_interface = 4,
	.capabilities = SS_CAP_BATTERY | SS_CAP_SIDETONE | SS_CAP_MIC_VOLUME,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 10,
	.mic_volume_max = 14,
	.request_status = steelseries_arctis_nova_3p_request_status,
	.parse_status = steelseries_arctis_nova_3p_parse_status,
	.write_setting = steelseries_arctis_nova_3p_write_setting,
};

static const struct steelseries_device_info arctis_nova_5_info = {
	.sync_interface = 3,
	.capabilities = SS_CAP_BATTERY | SS_CAP_SIDETONE | SS_CAP_MIC_VOLUME |
			SS_CAP_VOLUME_LIMITER,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 10,
	.mic_volume_max = 15,
	.request_status = steelseries_arctis_nova_request_status,
	.parse_status = steelseries_arctis_nova_5_parse_status,
	.write_setting = steelseries_arctis_nova_5_write_setting,
};

static const struct steelseries_device_info arctis_nova_5x_info = {
	.sync_interface = 3,
	.capabilities = SS_CAP_BATTERY | SS_CAP_CHATMIX | SS_CAP_SIDETONE |
			SS_CAP_MIC_VOLUME | SS_CAP_VOLUME_LIMITER,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 10,
	.mic_volume_max = 15,
	.request_status = steelseries_arctis_nova_request_status,
	.parse_status = steelseries_arctis_nova_5x_parse_status,
	.write_setting = steelseries_arctis_nova_5_write_setting,
};

static const struct steelseries_device_info arctis_nova_7_info = {
	.sync_interface = 3,
	.capabilities = SS_CAP_BATTERY | SS_CAP_CHATMIX | SS_CAP_SIDETONE |
			SS_CAP_MIC_VOLUME | SS_CAP_VOLUME_LIMITER,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 3,
	.mic_volume_max = 7,
	.request_status = steelseries_arctis_nova_request_status,
	.parse_status = steelseries_arctis_nova_7_parse_status,
	.write_setting = steelseries_arctis_nova_7_write_setting,
};

static const struct steelseries_device_info arctis_nova_7p_info = {
	.sync_interface = 3,
	.capabilities = SS_CAP_BATTERY | SS_CAP_MIC_VOLUME | SS_CAP_VOLUME_LIMITER,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.mic_volume_max = 7,
	.request_status = steelseries_arctis_nova_request_status,
	.parse_status = steelseries_arctis_nova_7_parse_status,
	.write_setting = steelseries_arctis_nova_7_write_setting,
};

static const struct steelseries_device_info arctis_nova_7_gen2_info = {
	.sync_interface = 3,
	.async_interface = 5,
	.capabilities = SS_CAP_BATTERY | SS_CAP_CHATMIX | SS_CAP_MIC_MUTE |
			SS_CAP_BT_ENABLED | SS_CAP_BT_DEVICE_CONNECTED |
			SS_CAP_EXTERNAL_CONFIG | SS_CAP_SIDETONE |
			SS_CAP_MIC_VOLUME | SS_CAP_VOLUME_LIMITER,
	.sidetone_max = 3,
	.mic_volume_max = 7,
	.request_status = steelseries_arctis_nova_request_status,
	.parse_status = steelseries_arctis_nova_7_gen2_parse_status,
	.request_settings = steelseries_arctis_nova_7_gen2_request_settings,
	.parse_settings = steelseries_arctis_nova_7_gen2_parse_settings,
	.write_setting = steelseries_arctis_nova_7_write_setting,
};

static const struct steelseries_device_info arctis_nova_pro_info = {
	.sync_interface = 4,
	.capabilities = SS_CAP_BATTERY | SS_CAP_CHATMIX | SS_CAP_MIC_MUTE |
			SS_CAP_BT_ENABLED | SS_CAP_BT_DEVICE_CONNECTED |
			SS_CAP_SIDETONE | SS_CAP_MIC_VOLUME,
	.quirks = SS_QUIRK_STATUS_SYNC_POLL,
	.sidetone_max = 3,
	.mic_volume_min = 1,
	.mic_volume_max = 10,
	.request_status = steelseries_arctis_nova_pro_request_status,
	.parse_status = steelseries_arctis_nova_pro_parse_status,
	.parse_settings = steelseries_arctis_nova_pro_parse_settings,
	.write_setting = steelseries_arctis_nova_pro_write_setting,
};

/*
 * Headset wireless status and battery infrastructure
 */

#define STEELSERIES_HEADSET_STATUS_TIMEOUT_MS	3000

static void
steelseries_headset_set_wireless_status(struct hid_device *hdev,
					bool connected)
{
	struct usb_interface *intf;

	if (!hid_is_usb(hdev))
		return;

	intf = to_usb_interface(hdev->dev.parent);
	usb_set_wireless_status(intf, connected ?
				USB_WIRELESS_STATUS_CONNECTED :
				USB_WIRELESS_STATUS_DISCONNECTED);
}

#define STEELSERIES_PREFIX "SteelSeries "
#define STEELSERIES_PREFIX_LEN strlen(STEELSERIES_PREFIX)

static int steelseries_battery_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct steelseries_device *sd = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = sd->hdev->name;
		while (!strncmp(val->strval, STEELSERIES_PREFIX, STEELSERIES_PREFIX_LEN))
			val->strval += STEELSERIES_PREFIX_LEN;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "SteelSeries";
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (sd->headset_connected) {
			val->intval = sd->battery_charging ?
				POWER_SUPPLY_STATUS_CHARGING :
				POWER_SUPPLY_STATUS_DISCHARGING;
		} else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = sd->battery_capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = battery_capacity_to_level(sd->battery_capacity);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static enum power_supply_property steelseries_battery_props[] = {
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
};

/*
 * Delayed work handlers for status polling and settings requests
 */

static void steelseries_status_timer_work_handler(struct work_struct *work)
{
	struct steelseries_device *sd = container_of(
		work, struct steelseries_device, status_work.work);
	unsigned long flags;

	sd->info->request_status(sd->hdev);

	spin_lock_irqsave(&sd->lock, flags);
	if (!sd->removed && !sd->use_async_protocol)
		schedule_delayed_work(&sd->status_work,
				msecs_to_jiffies(STEELSERIES_HEADSET_STATUS_TIMEOUT_MS));
	spin_unlock_irqrestore(&sd->lock, flags);
}

static void steelseries_settings_work_handler(struct work_struct *work)
{
	struct steelseries_device *sd = container_of(
		work, struct steelseries_device, settings_work.work);

	if (sd->info->request_settings)
		sd->info->request_settings(sd->hdev);
}

static int steelseries_battery_register(struct steelseries_device *sd)
{
	static atomic_t battery_no = ATOMIC_INIT(0);
	struct power_supply_config battery_cfg = { .drv_data = sd, };
	unsigned long n;
	int ret;

	sd->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	sd->battery_desc.properties = steelseries_battery_props;
	sd->battery_desc.num_properties = ARRAY_SIZE(steelseries_battery_props);
	sd->battery_desc.get_property = steelseries_battery_get_property;
	sd->battery_desc.use_for_apm = 0;
	n = atomic_inc_return(&battery_no) - 1;
	sd->battery_desc.name = devm_kasprintf(&sd->hdev->dev, GFP_KERNEL,
						    "steelseries_headset_battery_%ld", n);
	if (!sd->battery_desc.name)
		return -ENOMEM;

	/* avoid the warning of 0% battery while waiting for the first info */
	sd->battery_capacity = 100;
	sd->battery_charging = false;
	sd->headset_connected = false;
	steelseries_headset_set_wireless_status(sd->hdev, false);

	sd->battery = devm_power_supply_register(&sd->hdev->dev,
			&sd->battery_desc, &battery_cfg);
	if (IS_ERR(sd->battery)) {
		ret = PTR_ERR(sd->battery);
		sd->battery = NULL;
		hid_err(sd->hdev,
				"%s:power_supply_register failed with error %d\n",
				__func__, ret);
		return ret;
	}
	power_supply_powers(sd->battery, &sd->hdev->dev);

	return 0;
}

/*
 * Sysfs attributes for device state
 */

static ssize_t bt_enabled_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct steelseries_device *sd = hid_get_drvdata(hdev);

	if (!sd->headset_connected)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", sd->bt_enabled);
}

static ssize_t bt_device_connected_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct steelseries_device *sd = hid_get_drvdata(hdev);

	if (!sd->headset_connected)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", sd->bt_device_connected);
}

static DEVICE_ATTR_RO(bt_enabled);
static DEVICE_ATTR_RO(bt_device_connected);

static struct attribute *steelseries_headset_attrs[] = {
	&dev_attr_bt_enabled.attr,
	&dev_attr_bt_device_connected.attr,
	NULL,
};

static umode_t steelseries_headset_attr_is_visible(struct kobject *kobj,
						   struct attribute *attr,
						   int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct hid_device *hdev = to_hid_device(dev);
	struct steelseries_device *sd = hid_get_drvdata(hdev);
	unsigned long caps;

	if (!sd)
		return 0;

	caps = sd->info->capabilities;

	if (attr == &dev_attr_bt_enabled.attr)
		return (caps & SS_CAP_BT_ENABLED) ? attr->mode : 0;
	if (attr == &dev_attr_bt_device_connected.attr)
		return (caps & SS_CAP_BT_DEVICE_CONNECTED) ? attr->mode : 0;

	return 0;
}

static const struct attribute_group steelseries_headset_attr_group = {
	.attrs = steelseries_headset_attrs,
	.is_visible = steelseries_headset_attr_is_visible,
};

#if IS_BUILTIN(CONFIG_SND) || \
	(IS_MODULE(CONFIG_SND) && IS_MODULE(CONFIG_HID_STEELSERIES))

static int steelseries_chatmix_info(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 100;
	uinfo->value.integer.step = 1;
	return 0;
}

static int steelseries_chatmix_chat_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&sd->lock, flags);
	ucontrol->value.integer.value[0] = sd->chatmix_chat;
	spin_unlock_irqrestore(&sd->lock, flags);
	return 0;
}

static int steelseries_chatmix_game_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&sd->lock, flags);
	ucontrol->value.integer.value[0] = sd->chatmix_game;
	spin_unlock_irqrestore(&sd->lock, flags);
	return 0;
}

static int steelseries_mic_muted_info(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	uinfo->value.integer.step = 1;
	return 0;
}

static int steelseries_mic_muted_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&sd->lock, flags);
	ucontrol->value.integer.value[0] = sd->mic_muted;
	spin_unlock_irqrestore(&sd->lock, flags);
	return 0;
}

static const struct snd_kcontrol_new steelseries_chatmix_chat_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "ChatMix Chat",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info = steelseries_chatmix_info,
	.get = steelseries_chatmix_chat_get,
};

static const struct snd_kcontrol_new steelseries_chatmix_game_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "ChatMix Game",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info = steelseries_chatmix_info,
	.get = steelseries_chatmix_game_get,
};

static const struct snd_kcontrol_new steelseries_mic_muted_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Mic Muted",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info = steelseries_mic_muted_info,
	.get = steelseries_mic_muted_get,
};

static int steelseries_sidetone_info(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_info *uinfo)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = sd->info->sidetone_max;
	uinfo->value.integer.step = 1;
	return 0;
}

static int steelseries_sidetone_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&sd->lock, flags);
	ucontrol->value.integer.value[0] = sd->sidetone;
	spin_unlock_irqrestore(&sd->lock, flags);
	return 0;
}

static int steelseries_sidetone_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	u8 new_sidetone;
	int ret;

	new_sidetone = ucontrol->value.integer.value[0];
	if (new_sidetone > sd->info->sidetone_max)
		return -EINVAL;

	spin_lock_irqsave(&sd->lock, flags);
	if (sd->sidetone == new_sidetone) {
		spin_unlock_irqrestore(&sd->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&sd->lock, flags);

	ret = sd->info->write_setting(sd->hdev, SS_SETTING_SIDETONE,
				      new_sidetone);
	if (ret)
		return ret;

	spin_lock_irqsave(&sd->lock, flags);
	sd->sidetone = new_sidetone;
	spin_unlock_irqrestore(&sd->lock, flags);

	return 1;
}

static const struct snd_kcontrol_new steelseries_sidetone_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Sidetone Volume",
	.info = steelseries_sidetone_info,
	.get = steelseries_sidetone_get,
	.put = steelseries_sidetone_put,
};

static int steelseries_mic_volume_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *uinfo)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = sd->info->mic_volume_min;
	uinfo->value.integer.max = sd->info->mic_volume_max;
	uinfo->value.integer.step = 1;
	return 0;
}

static int steelseries_mic_volume_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&sd->lock, flags);
	ucontrol->value.integer.value[0] = sd->mic_volume;
	spin_unlock_irqrestore(&sd->lock, flags);
	return 0;
}

static int steelseries_mic_volume_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	u8 new_mic_volume;
	int ret;

	new_mic_volume = ucontrol->value.integer.value[0];
	if (new_mic_volume < sd->info->mic_volume_min ||
	    new_mic_volume > sd->info->mic_volume_max)
		return -EINVAL;

	spin_lock_irqsave(&sd->lock, flags);
	if (sd->mic_volume == new_mic_volume) {
		spin_unlock_irqrestore(&sd->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&sd->lock, flags);

	ret = sd->info->write_setting(sd->hdev, SS_SETTING_MIC_VOLUME,
				      new_mic_volume);
	if (ret)
		return ret;

	spin_lock_irqsave(&sd->lock, flags);
	sd->mic_volume = new_mic_volume;
	spin_unlock_irqrestore(&sd->lock, flags);

	return 1;
}

static const struct snd_kcontrol_new steelseries_mic_volume_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Mic Volume",
	.info = steelseries_mic_volume_info,
	.get = steelseries_mic_volume_get,
	.put = steelseries_mic_volume_put,
};

static int steelseries_volume_limiter_info(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	uinfo->value.integer.step = 1;
	return 0;
}

static int steelseries_volume_limiter_get(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;

	spin_lock_irqsave(&sd->lock, flags);
	ucontrol->value.integer.value[0] = sd->volume_limiter;
	spin_unlock_irqrestore(&sd->lock, flags);
	return 0;
}

static int steelseries_volume_limiter_put(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	struct steelseries_device *sd = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	bool new_volume_limiter;
	int ret;

	new_volume_limiter = !!ucontrol->value.integer.value[0];

	spin_lock_irqsave(&sd->lock, flags);
	if (sd->volume_limiter == new_volume_limiter) {
		spin_unlock_irqrestore(&sd->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&sd->lock, flags);

	ret = sd->info->write_setting(sd->hdev, SS_SETTING_VOLUME_LIMITER,
				      new_volume_limiter ? 1 : 0);
	if (ret)
		return ret;

	spin_lock_irqsave(&sd->lock, flags);
	sd->volume_limiter = new_volume_limiter;
	spin_unlock_irqrestore(&sd->lock, flags);

	return 1;
}

static const struct snd_kcontrol_new steelseries_volume_limiter_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Volume Limiter",
	.info = steelseries_volume_limiter_info,
	.get = steelseries_volume_limiter_get,
	.put = steelseries_volume_limiter_put,
};

static int steelseries_snd_register(struct steelseries_device *sd)
{
	struct hid_device *hdev = sd->hdev;
	int ret;

	ret = snd_card_new(&hdev->dev, -1, "SteelSeries", THIS_MODULE,
			   0, &sd->card);
	if (ret < 0)
		return ret;

	sd->card->private_data = sd;
	strscpy(sd->card->driver, "SteelSeries");
	strscpy(sd->card->shortname, hdev->name);
	snprintf(sd->card->longname, sizeof(sd->card->longname),
		"%s at USB %s", hdev->name, dev_name(&hdev->dev));

	if (sd->info->capabilities & SS_CAP_CHATMIX) {
		struct snd_kcontrol *kctl;

		kctl = snd_ctl_new1(&steelseries_chatmix_chat_control, sd);
		ret = snd_ctl_add(sd->card, kctl);
		if (ret < 0)
			goto err_free_card;
		sd->chatmix_chat_id = kctl->id;

		kctl = snd_ctl_new1(&steelseries_chatmix_game_control, sd);
		ret = snd_ctl_add(sd->card, kctl);
		if (ret < 0)
			goto err_free_card;
		sd->chatmix_game_id = kctl->id;
	}

	if (sd->info->capabilities & SS_CAP_MIC_MUTE) {
		struct snd_kcontrol *kctl;

		kctl = snd_ctl_new1(&steelseries_mic_muted_control, sd);
		ret = snd_ctl_add(sd->card, kctl);
		if (ret < 0)
			goto err_free_card;
		sd->mic_muted_id = kctl->id;
	}

	if (sd->info->capabilities & SS_CAP_SIDETONE) {
		struct snd_kcontrol *kctl;
		struct snd_kcontrol_new sidetone_ctl = steelseries_sidetone_control;

		sidetone_ctl.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		if (sd->info->capabilities & SS_CAP_EXTERNAL_CONFIG)
			sidetone_ctl.access |= SNDRV_CTL_ELEM_ACCESS_VOLATILE;

		kctl = snd_ctl_new1(&sidetone_ctl, sd);
		ret = snd_ctl_add(sd->card, kctl);
		if (ret < 0)
			goto err_free_card;
		sd->sidetone_id = kctl->id;
	}

	if (sd->info->capabilities & SS_CAP_MIC_VOLUME) {
		struct snd_kcontrol *kctl;
		struct snd_kcontrol_new mic_vol_ctl = steelseries_mic_volume_control;

		mic_vol_ctl.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		if (sd->info->capabilities & SS_CAP_EXTERNAL_CONFIG)
			mic_vol_ctl.access |= SNDRV_CTL_ELEM_ACCESS_VOLATILE;

		kctl = snd_ctl_new1(&mic_vol_ctl, sd);
		ret = snd_ctl_add(sd->card, kctl);
		if (ret < 0)
			goto err_free_card;
		sd->mic_volume_id = kctl->id;
	}

	if (sd->info->capabilities & SS_CAP_VOLUME_LIMITER) {
		struct snd_kcontrol *kctl;
		struct snd_kcontrol_new vol_lim_ctl = steelseries_volume_limiter_control;

		vol_lim_ctl.access = SNDRV_CTL_ELEM_ACCESS_READWRITE;
		if (sd->info->capabilities & SS_CAP_EXTERNAL_CONFIG)
			vol_lim_ctl.access |= SNDRV_CTL_ELEM_ACCESS_VOLATILE;

		kctl = snd_ctl_new1(&vol_lim_ctl, sd);
		ret = snd_ctl_add(sd->card, kctl);
		if (ret < 0)
			goto err_free_card;
		sd->volume_limiter_id = kctl->id;
	}

	ret = snd_card_register(sd->card);
	if (ret < 0)
		goto err_free_card;

	return 0;

err_free_card:
	snd_card_free(sd->card);
	sd->card = NULL;
	return ret;
}

static void steelseries_snd_unregister(struct steelseries_device *sd)
{
	if (sd->card)
		snd_card_free(sd->card);
}

#endif

static int steelseries_raw_event(struct hid_device *hdev,
				 struct hid_report *report, u8 *data, int size)
{
	struct steelseries_device *sd = hid_get_drvdata(hdev);
	u8 old_capacity;
	bool old_connected;
	bool old_charging;
	u8 old_chatmix_chat;
	u8 old_chatmix_game;
	bool old_mic_muted;
	u8 old_sidetone;
	u8 old_mic_volume;
	bool old_volume_limiter;
	bool is_async_interface = false;

	if (hdev->product == USB_DEVICE_ID_STEELSERIES_SRWS1)
		return 0;

	if (!sd)
		return 0;

	old_capacity = sd->battery_capacity;
	old_connected = sd->headset_connected;
	old_charging = sd->battery_charging;
	old_chatmix_chat = sd->chatmix_chat;
	old_chatmix_game = sd->chatmix_game;
	old_mic_muted = sd->mic_muted;
	old_sidetone = sd->sidetone;
	old_mic_volume = sd->mic_volume;
	old_volume_limiter = sd->volume_limiter;

	if (hid_is_usb(hdev)) {
		struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

		is_async_interface = (intf->cur_altsetting->desc.bInterfaceNumber ==
				      sd->info->async_interface);
	}

	sd->info->parse_status(sd, data, size);

	if (sd->info->parse_settings)
		sd->info->parse_settings(sd, data, size);

	if (sd->headset_connected != old_connected) {
		hid_dbg(hdev,
			"Connected status changed from %sconnected to %sconnected\n",
			old_connected ? "" : "not ",
			sd->headset_connected ? "" : "not ");

		if (sd->headset_connected && !old_connected &&
		    sd->use_async_protocol && is_async_interface) {
			schedule_delayed_work(&sd->status_work, 0);
			if (sd->info->request_settings)
				schedule_delayed_work(&sd->settings_work,
						      msecs_to_jiffies(10));
		}

		if (sd->battery) {
			steelseries_headset_set_wireless_status(sd->hdev,
							       sd->headset_connected);
			power_supply_changed(sd->battery);
		}
	}

	if (sd->battery_capacity != old_capacity) {
		hid_dbg(hdev, "Battery capacity changed from %d%% to %d%%\n",
			old_capacity, sd->battery_capacity);
		if (sd->battery)
			power_supply_changed(sd->battery);
	}

	if (sd->battery_charging != old_charging) {
		hid_dbg(hdev,
			"Battery charging status changed from %scharging to %scharging\n",
			old_charging ? "" : "not ",
			sd->battery_charging ? "" : "not ");
		if (sd->battery)
			power_supply_changed(sd->battery);
	}

	if (sd->card) {
		if (sd->chatmix_chat != old_chatmix_chat)
			snd_ctl_notify(sd->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &sd->chatmix_chat_id);
		if (sd->chatmix_game != old_chatmix_game)
			snd_ctl_notify(sd->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &sd->chatmix_game_id);
		if (sd->mic_muted != old_mic_muted)
			snd_ctl_notify(sd->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &sd->mic_muted_id);
		if (sd->sidetone != old_sidetone)
			snd_ctl_notify(sd->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &sd->sidetone_id);
		if (sd->mic_volume != old_mic_volume)
			snd_ctl_notify(sd->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &sd->mic_volume_id);
		if (sd->volume_limiter != old_volume_limiter)
			snd_ctl_notify(sd->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &sd->volume_limiter_id);
	}

	return 0;
}

static struct hid_device *steelseries_get_sibling_hdev(struct hid_device *hdev,
						       int interface_num)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct usb_interface *sibling_intf;
	struct hid_device *sibling_hdev;

	sibling_intf = usb_ifnum_to_if(usb_dev, interface_num);
	if (!sibling_intf)
		return NULL;

	sibling_hdev = usb_get_intfdata(sibling_intf);

	return sibling_hdev;
}

static int steelseries_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	const struct steelseries_device_info *info =
		(const struct steelseries_device_info *)id->driver_data;
	struct steelseries_device *sd;
	struct usb_interface *intf;
	struct hid_device *master_hdev;
	u8 interface_num;
	int ret;

	if (hdev->product == USB_DEVICE_ID_STEELSERIES_SRWS1) {
#if IS_BUILTIN(CONFIG_LEDS_CLASS) || \
	(IS_MODULE(CONFIG_LEDS_CLASS) && IS_MODULE(CONFIG_HID_STEELSERIES))
		return steelseries_srws1_probe(hdev, id);
#else
		return -ENODEV;
#endif
	}

	if (hid_is_usb(hdev)) {
		intf = to_usb_interface(hdev->dev.parent);
		interface_num = intf->cur_altsetting->desc.bInterfaceNumber;
	} else {
		return -ENODEV;
	}

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	/* Let hid-generic handle non-vendor or unknown interfaces */
	if (interface_num != info->sync_interface &&
	    (!info->async_interface || interface_num != info->async_interface))
		return hid_hw_start(hdev, HID_CONNECT_DEFAULT);

	if (interface_num == info->sync_interface) {
		sd = devm_kzalloc(&hdev->dev, sizeof(*sd), GFP_KERNEL);
		if (!sd)
			return -ENOMEM;

		sd->hdev = hdev;
		sd->info = info;
		spin_lock_init(&sd->lock);

		hid_set_drvdata(hdev, sd);

		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		if (ret)
			return ret;

		ret = hid_hw_open(hdev);
		if (ret)
			goto err_stop;

		sd->use_async_protocol = !(info->quirks & SS_QUIRK_STATUS_SYNC_POLL);

		if (info->capabilities & SS_CAP_BATTERY) {
			ret = steelseries_battery_register(sd);
			if (ret < 0)
				hid_warn(hdev, "Failed to register battery: %d\n", ret);
		}

		if (info->capabilities & (SS_CAP_BT_ENABLED | SS_CAP_BT_DEVICE_CONNECTED)) {
			ret = sysfs_create_group(&hdev->dev.kobj,
						 &steelseries_headset_attr_group);
			if (ret)
				hid_warn(hdev, "Failed to create sysfs group: %d\n", ret);
		}

#if IS_BUILTIN(CONFIG_SND) || \
	(IS_MODULE(CONFIG_SND) && IS_MODULE(CONFIG_HID_STEELSERIES))
		ret = steelseries_snd_register(sd);
		if (ret < 0)
			hid_warn(hdev, "Failed to register sound card: %d\n", ret);
#endif

		INIT_DELAYED_WORK(&sd->status_work, steelseries_status_timer_work_handler);
		INIT_DELAYED_WORK(&sd->settings_work, steelseries_settings_work_handler);

		schedule_delayed_work(&sd->status_work, msecs_to_jiffies(100));
		if (info->request_settings)
			schedule_delayed_work(&sd->settings_work, msecs_to_jiffies(200));

		return 0;
	}

	if (info->async_interface && interface_num == info->async_interface) {
		master_hdev = steelseries_get_sibling_hdev(hdev, info->sync_interface);

		if (!master_hdev || !hid_get_drvdata(master_hdev))
			return -EPROBE_DEFER;

		sd = hid_get_drvdata(master_hdev);
		hid_set_drvdata(hdev, sd);

		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		if (ret)
			return ret;

		ret = hid_hw_open(hdev);
		if (ret) {
			hid_hw_stop(hdev);
			return ret;
		}
		return 0;
	}

	return -ENODEV;

err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void steelseries_remove(struct hid_device *hdev)
{
	struct steelseries_device *sd;
	unsigned long flags;
	struct usb_interface *intf;
	u8 interface_num;

	if (hdev->product == USB_DEVICE_ID_STEELSERIES_SRWS1) {
#if IS_BUILTIN(CONFIG_LEDS_CLASS) || \
	(IS_MODULE(CONFIG_LEDS_CLASS) && IS_MODULE(CONFIG_HID_STEELSERIES))
		hid_hw_stop(hdev);
#endif
		return;
	}

	if (hid_is_usb(hdev)) {
		intf = to_usb_interface(hdev->dev.parent);
		interface_num = intf->cur_altsetting->desc.bInterfaceNumber;
	} else {
		return;
	}

	sd = hid_get_drvdata(hdev);

	if (!sd) {
		hid_hw_stop(hdev);
		return;
	}

	if (interface_num == sd->info->sync_interface) {
		if (sd->info->capabilities & (SS_CAP_BT_ENABLED | SS_CAP_BT_DEVICE_CONNECTED))
			sysfs_remove_group(&hdev->dev.kobj,
					   &steelseries_headset_attr_group);

		if (sd->info->async_interface) {
			struct hid_device *sibling;

			sibling = steelseries_get_sibling_hdev(hdev,
							       sd->info->async_interface);
			if (sibling)
				hid_set_drvdata(sibling, NULL);
		}

#if IS_BUILTIN(CONFIG_SND) || \
	(IS_MODULE(CONFIG_SND) && IS_MODULE(CONFIG_HID_STEELSERIES))
		steelseries_snd_unregister(sd);
#endif

		spin_lock_irqsave(&sd->lock, flags);
		sd->removed = true;
		spin_unlock_irqrestore(&sd->lock, flags);

		cancel_delayed_work_sync(&sd->status_work);
		cancel_delayed_work_sync(&sd->settings_work);
	}

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id steelseries_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_SRWS1),
	  .driver_data = (unsigned long)&srws1_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_1),
	  .driver_data = (unsigned long)&arctis_1_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_1_X),
	  .driver_data = (unsigned long)&arctis_1_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_7),
	  .driver_data = (unsigned long)&arctis_7_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_7_P),
	  .driver_data = (unsigned long)&arctis_1_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_7_X),
	  .driver_data = (unsigned long)&arctis_1_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_7_GEN2),
	  .driver_data = (unsigned long)&arctis_7_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_7_PLUS),
	  .driver_data = (unsigned long)&arctis_7_plus_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_7_PLUS_P),
	  .driver_data = (unsigned long)&arctis_7_plus_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_7_PLUS_X),
	  .driver_data = (unsigned long)&arctis_7_plus_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_7_PLUS_DESTINY),
	  .driver_data = (unsigned long)&arctis_7_plus_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_9),
	  .driver_data = (unsigned long)&arctis_9_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_3_P),
	  .driver_data = (unsigned long)&arctis_nova_3p_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_3_X),
	  .driver_data = (unsigned long)&arctis_nova_3p_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_5),
	  .driver_data = (unsigned long)&arctis_nova_5_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_5_X),
	  .driver_data = (unsigned long)&arctis_nova_5x_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7),
	  .driver_data = (unsigned long)&arctis_nova_7_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_2),
	  .driver_data = (unsigned long)&arctis_nova_7_gen2_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_P),
	  .driver_data = (unsigned long)&arctis_nova_7p_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_X),
	  .driver_data = (unsigned long)&arctis_nova_7_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_X_2),
	  .driver_data = (unsigned long)&arctis_nova_7_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_X_3),
	  .driver_data = (unsigned long)&arctis_nova_7_gen2_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_DIABLO),
	  .driver_data = (unsigned long)&arctis_nova_7_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_DIABLO_2),
	  .driver_data = (unsigned long)&arctis_nova_7_gen2_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_WOW),
	  .driver_data = (unsigned long)&arctis_nova_7_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_GEN2),
	  .driver_data = (unsigned long)&arctis_nova_7_gen2_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_X_GEN2),
	  .driver_data = (unsigned long)&arctis_nova_7_gen2_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_7_X_GEN2_2),
	  .driver_data = (unsigned long)&arctis_nova_7_gen2_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_PRO),
	  .driver_data = (unsigned long)&arctis_nova_pro_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
			 USB_DEVICE_ID_STEELSERIES_ARCTIS_NOVA_PRO_X),
	  .driver_data = (unsigned long)&arctis_nova_pro_info },
	{}
};
MODULE_DEVICE_TABLE(hid, steelseries_devices);

static struct hid_driver steelseries_driver = {
	.name = "steelseries",
	.id_table = steelseries_devices,
	.probe = steelseries_probe,
	.remove = steelseries_remove,
	.report_fixup = steelseries_srws1_report_fixup,
	.raw_event = steelseries_raw_event,
};

module_hid_driver(steelseries_driver);
MODULE_DESCRIPTION("HID driver for Steelseries devices");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bastien Nocera <hadess@hadess.net>");
MODULE_AUTHOR("Simon Wood <simon@mungewell.org>");
MODULE_AUTHOR("Christian Mayer <git@mayer-bgk.de>");
MODULE_AUTHOR("Sriman Achanta <srimanachanta@gmail.com>");
