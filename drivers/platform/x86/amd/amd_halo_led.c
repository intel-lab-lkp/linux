// SPDX-License-Identifier: GPL-2.0
/*
 * AMD Halo Box RGB LED Driver
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * This driver provides RGB LED control for AMD Halo Box devices through
 * the LED multicolor subsystem. The Halo Box light bar can be controlled
 * via sysfs to display any RGB color combination.
 */

#include <linux/acpi.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#define AMD_HALO_GUID "081E747B-E028-4232-AF24-EAAEAB2B1E86"

/* WMI method IDs from MOF */
enum {
	AMD_HALO_WMI_GET_LIGHTBAR = 0x01,
	AMD_HALO_WMI_SET_LIGHTBAR,
	AMD_HALO_WMI_TURN_ON,
	AMD_HALO_WMI_TURN_OFF
};

/* Channel selectors for Arg0 */
enum {
	AMD_HALO_CHANNEL_RED = 0x01,
	AMD_HALO_CHANNEL_GREEN,
	AMD_HALO_CHANNEL_BLUE
};

/* Status codes from spec */
#define AMD_HALO_STATUS_SUCCESS		0x0000
#define AMD_HALO_STATUS_INVALID_PARAM	0xFFFD

/* Brightness uses 0-100 range */
#define AMD_HALO_MAX_HW_BRIGHTNESS	100

/* Default RGB brightness */
#define AMD_HALO_DEFAULT_RED		50
#define AMD_HALO_DEFAULT_GREEN		30
#define AMD_HALO_DEFAULT_BLUE		30

/**
 * struct amd_halo_led_data - Driver private data
 * @wdev: WMI device pointer
 * @led_mc: LED multicolor class device
 * @subled_info: RGB channel information
 * @lock: Mutex to protect WMI calls
 */
struct amd_halo_led_data {
	struct wmi_device *wdev;
	struct led_classdev_mc led_mc;
	struct mc_subled subled_info[3];
	struct mutex lock;	/* Protects WMI method calls */
};

struct amd_halo_wmi_args {
	u32 arg0;
	u32 arg1;
};

/**
 * amd_halo_wmi_set_channel - Set a single RGB channel value
 * @wdev: WMI device pointer
 * @channel: Channel selector (1=Red, 2=Green, 3=Blue)
 * @brightness: brightness to set (0-100)
 *
 * Return: 0 on success, negative error code on failure
 */
static int amd_halo_wmi_set_channel(struct wmi_device *wdev, u32 channel,
				    u32 brightness)
{
	struct amd_halo_wmi_args args = {
		.arg0 = channel,
		.arg1 = brightness,
	};
	struct wmi_buffer input = {
		.length = sizeof(args),
		.data = &args,
	};
	struct wmi_buffer output = { 0 };
	u16 result_status;
	int ret;

	/* Validate input per spec */
	if (channel < AMD_HALO_CHANNEL_RED ||
	    channel > AMD_HALO_CHANNEL_BLUE ||
	    brightness > AMD_HALO_MAX_HW_BRIGHTNESS)
		return -EINVAL;

	ret = wmidev_invoke_method(wdev, 0, AMD_HALO_WMI_SET_LIGHTBAR,
					&input, &output, sizeof(result_status));
	if (ret)
		return ret;

	/* Return buffer per spec: Bytes[0:1] = Status (little-endian) */
	result_status = get_unaligned_le16(output.data);
	ret = (result_status == AMD_HALO_STATUS_SUCCESS) ? 0 : -EIO;

	kfree(output.data);
	return ret;
}

/**
 * amd_halo_brightness_set - Set LED brightness and color
 * @cdev: LED class device
 * @brightness: Brightness value
 *
 * Return: 0 on success, negative error code on failure
 */
static int amd_halo_brightness_set(struct led_classdev *cdev,
				   enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct amd_halo_led_data *data = container_of(mc_cdev,
						       struct amd_halo_led_data,
						       led_mc);
	u32 red_hw, green_hw, blue_hw;
	int ret;

	guard(mutex)(&data->lock);

	led_mc_calc_color_components(mc_cdev, brightness);
	red_hw = mc_cdev->subled_info[0].brightness;
	green_hw = mc_cdev->subled_info[1].brightness;
	blue_hw = mc_cdev->subled_info[2].brightness;

	/* Set each channel individually - 3 WMI calls required */
	ret = amd_halo_wmi_set_channel(data->wdev, AMD_HALO_CHANNEL_RED,
				       red_hw);
	if (ret)
		return ret;

	ret = amd_halo_wmi_set_channel(data->wdev, AMD_HALO_CHANNEL_GREEN,
				       green_hw);
	if (ret)
		return ret;

	return amd_halo_wmi_set_channel(data->wdev, AMD_HALO_CHANNEL_BLUE,
					blue_hw);
}

/**
 * amd_halo_probe - Driver probe function
 * @wdev: WMI device
 * @context: Context data (unused)
 *
 * Return: 0 on success, negative error code on failure
 */
static int amd_halo_probe(struct wmi_device *wdev, const void *context)
{
	struct led_init_data led_init_data = {
		.devicename = "amd_halo",
		.default_label = "multicolor:" LED_FUNCTION_STATUS,
		.devname_mandatory = true
	};
	struct amd_halo_led_data *data;
	int ret;

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->wdev = wdev;
	mutex_init(&data->lock);
	dev_set_drvdata(&wdev->dev, data);

	data->subled_info[0].color_index = LED_COLOR_ID_RED;
	data->subled_info[1].color_index = LED_COLOR_ID_GREEN;
	data->subled_info[2].color_index = LED_COLOR_ID_BLUE;
	data->subled_info[0].intensity = AMD_HALO_DEFAULT_RED;
	data->subled_info[1].intensity = AMD_HALO_DEFAULT_GREEN;
	data->subled_info[2].intensity = AMD_HALO_DEFAULT_BLUE;

	data->led_mc.led_cdev.brightness = AMD_HALO_MAX_HW_BRIGHTNESS;
	data->led_mc.led_cdev.max_brightness = AMD_HALO_MAX_HW_BRIGHTNESS;
	data->led_mc.led_cdev.brightness_set_blocking = amd_halo_brightness_set;
	data->led_mc.led_cdev.flags = LED_CORE_SUSPENDRESUME | LED_RETAIN_AT_SHUTDOWN;
	data->led_mc.num_colors = ARRAY_SIZE(data->subled_info);
	data->led_mc.subled_info = data->subled_info;

	ret = amd_halo_brightness_set(&data->led_mc.led_cdev,
				      AMD_HALO_MAX_HW_BRIGHTNESS);
	if (ret)
		return dev_err_probe(&wdev->dev, ret,
				     "Failed to set default LED colors\n");

	ret = devm_led_classdev_multicolor_register_ext(&wdev->dev, &data->led_mc,
							&led_init_data);
	if (ret)
		return dev_err_probe(&wdev->dev, ret,
				     "Failed to register multicolor LED\n");
	return 0;
}

static const struct wmi_device_id amd_halo_id_table[] = {
	{ .guid_string = AMD_HALO_GUID },
	{ }
};
MODULE_DEVICE_TABLE(wmi, amd_halo_id_table);

static struct wmi_driver amd_halo_driver = {
	.driver = {
		.name = "amd_halo_led",
	},
	.id_table = amd_halo_id_table,
	.probe = amd_halo_probe,
	.no_singleton = true,
};

module_wmi_driver(amd_halo_driver);

MODULE_AUTHOR("Mario Limonciello (AMD) <superm1@kernel.org>");
MODULE_AUTHOR("Leo Lin <Leo.Lin@amd.com>");
MODULE_DESCRIPTION("AMD Halo Box RGB LED Control Driver");
MODULE_LICENSE("GPL");
