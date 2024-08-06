// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/leds.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mutex_types.h>
#include <linux/platform_profile.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>

#define CASPER_WMI_GUID "644C5791-B7B0-4123-A90B-E93876E0DAAD"

#define CASPER_READ 0xfa00
#define CASPER_WRITE 0xfb00
#define CASPER_GET_HARDWAREINFO 0x0200
#define CASPER_SET_LED 0x0100
#define CASPER_POWERPLAN 0x0300

#define CASPER_KEYBOARD_LED_1 0x03
#define CASPER_KEYBOARD_LED_2 0x04
#define CASPER_KEYBOARD_LED_3 0x05
#define CASPER_ALL_KEYBOARD_LEDS 0x06
#define CASPER_CORNER_LEDS 0x07
#define CASPER_LED_COUNT 4

static const char * const zone_names[CASPER_LED_COUNT] = {
	"casper:rgb:kbd_zoned_backlight-right",
	"casper:rgb:kbd_zoned_backlight-middle",
	"casper:rgb:kbd_zoned_backlight-left",
	"casper:rgb:biaslight",
};

#define CASPER_LED_ALPHA GENMASK(31, 24)
#define CASPER_LED_RED	 GENMASK(23, 16)
#define CASPER_LED_GREEN GENMASK(15, 8)
#define CASPER_LED_BLUE  GENMASK(7, 0)
#define CASPER_DEFAULT_COLOR (CASPER_LED_RED | CASPER_LED_GREEN | \
			      CASPER_LED_BLUE)
#define CASPER_FAN_CPU 0
#define CASPER_FAN_GPU 1

enum casper_power_profile_old {
	CASPER_HIGH_PERFORMANCE = 1,
	CASPER_GAMING		= 2,
	CASPER_TEXT_MODE	= 3,
	CASPER_POWERSAVE	= 4
};

enum casper_power_profile_new {
	CASPER_NEW_HIGH_PERFORMANCE	= 0,
	CASPER_NEW_GAMING		= 1,
	CASPER_NEW_AUDIO		= 2
};

struct casper_quirk_entry {
	bool big_endian_fans;
	bool no_power_profiles;
	bool new_power_scheme;
};

struct casper_fourzone_led {
	struct led_classdev_mc mc_led;
	struct mc_subled subleds[3];
};

struct casper_drv {
	struct platform_profile_handler handler;
	struct mutex mutex;
	struct casper_fourzone_led *leds;
	struct wmi_device *wdev;
	struct casper_quirk_entry *quirk_applied;
};

struct casper_wmi_args {
	u16 a0, a1;
	u32 a2, a3, a4, a5, a6, a7, a8;
};

enum casper_led_mode {
	LED_NORMAL = 0x10,
	LED_BLINK = 0x20,
	LED_FADE = 0x30,
	LED_HEARTBEAT = 0x40,
	LED_REPEAT = 0x50,
	LED_RANDOM = 0x60
};

static int casper_set(struct casper_drv *drv, u16 a1, u8 led_id, u32 data)
{
	struct casper_wmi_args wmi_args;
	struct acpi_buffer input;
	acpi_status status;

	wmi_args = (struct casper_wmi_args) {
		.a0 = CASPER_WRITE,
		.a1 = a1,
		.a2 = led_id,
		.a3 = data
	};

	input = (struct acpi_buffer) {
		(acpi_size) sizeof(struct casper_wmi_args),
		&wmi_args
	};

	guard(mutex)(&drv->mutex);

	status = wmidev_block_set(drv->wdev, 0, &input);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static int casper_query(struct casper_drv *drv, u16 a1,
			struct casper_wmi_args *out)
{
	struct casper_wmi_args wmi_args;
	struct acpi_buffer input;
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;

	wmi_args = (struct casper_wmi_args) {
		.a0 = CASPER_READ,
		.a1 = a1
	};
	input = (struct acpi_buffer) {
		(acpi_size) sizeof(struct casper_wmi_args),
		&wmi_args
	};

	guard(mutex)(&drv->mutex);

	status = wmidev_block_set(drv->wdev, 0, &input);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = wmidev_block_query(drv->wdev, 0);
	if (!obj)
		return -EIO;

	if (obj->type != ACPI_TYPE_BUFFER) { // obj will be 0x10 on failure
		ret = -EINVAL;
		goto freeobj;
	}
	if (obj->buffer.length != sizeof(struct casper_wmi_args)) {
		ret = -EIO;
		goto freeobj;
	}

	memcpy(out, obj->buffer.pointer, sizeof(struct casper_wmi_args));

freeobj:
	kfree(obj);
	return ret;
}

static u32 get_zone_color(struct casper_fourzone_led z)
{
	return  FIELD_PREP(CASPER_LED_RED, z.subleds[0].intensity) |
		FIELD_PREP(CASPER_LED_GREEN, z.subleds[1].intensity) |
		FIELD_PREP(CASPER_LED_BLUE, z.subleds[2].intensity);
}

static enum led_brightness get_casper_brightness(struct led_classdev *led_cdev)
{
	struct casper_drv *drv = dev_get_drvdata(led_cdev->dev->parent);
	struct casper_wmi_args hardware_alpha = {0};

	if (strcmp(led_cdev->name, zone_names[3]) == 0)
		return drv->leds[3].mc_led.led_cdev.brightness;

	casper_query(drv, CASPER_GET_HARDWAREINFO, &hardware_alpha);

	return hardware_alpha.a6;
}

static void set_casper_brightness(struct led_classdev *led_cdev,
				  enum led_brightness brightness)
{
	u32 bright_with_mode, bright_prep, led_data, led_data_no_alpha;
	struct casper_drv *drv;
	u8 zone_to_change;
	size_t zone;

	drv = dev_get_drvdata(led_cdev->dev->parent);

	for (size_t i = 0; i < CASPER_LED_COUNT; i++)
		if (strcmp(led_cdev->name, zone_names[i]) == 0)
			zone = i;
	if (zone == 3)
		zone_to_change = CASPER_CORNER_LEDS;
	else
		zone_to_change = zone + CASPER_KEYBOARD_LED_1;

	led_data_no_alpha = get_zone_color(drv->leds[zone]) & ~CASPER_LED_ALPHA;

	if (brightness == drv->leds[zone].mc_led.led_cdev.brightness)
		brightness = get_casper_brightness(&drv->leds[zone].mc_led.led_cdev);
	bright_with_mode = brightness | LED_NORMAL;

	bright_prep = FIELD_PREP(CASPER_LED_ALPHA, bright_with_mode);
	led_data = bright_prep | led_data_no_alpha;
	casper_set(drv, CASPER_SET_LED, zone_to_change, led_data);
}

static int casper_platform_profile_get(struct platform_profile_handler *pprof,
				       enum platform_profile_option *profile)
{
	struct casper_drv *drv = container_of(pprof, struct casper_drv,
					      handler);
	struct casper_wmi_args ret_buff = {0};
	int ret;

	ret = casper_query(drv, CASPER_POWERPLAN, &ret_buff);
	if (ret)
		return ret;

	if (drv->quirk_applied->new_power_scheme) {
		switch (ret_buff.a2) {
		case CASPER_NEW_HIGH_PERFORMANCE:
			*profile = PLATFORM_PROFILE_PERFORMANCE;
			break;
		case CASPER_NEW_GAMING:
			*profile = PLATFORM_PROFILE_BALANCED;
			break;
		case CASPER_NEW_AUDIO:
			*profile = PLATFORM_PROFILE_LOW_POWER;
			break;
		default:
			return -EINVAL;
		}
		return 0;
	}

	switch (ret_buff.a2) {
	case CASPER_HIGH_PERFORMANCE:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case CASPER_GAMING:
		*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
		break;
	case CASPER_TEXT_MODE:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case CASPER_POWERSAVE:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int casper_platform_profile_set(struct platform_profile_handler *pprof,
				       enum platform_profile_option profile)
{
	struct casper_drv *drv = container_of(pprof, struct casper_drv,
					      handler);
	enum casper_power_profile_old prf_old;
	enum casper_power_profile_new prf_new;

	if (drv->quirk_applied->new_power_scheme) {

		switch (profile) {
		case PLATFORM_PROFILE_PERFORMANCE:
			prf_new = CASPER_NEW_HIGH_PERFORMANCE;
			break;
		case PLATFORM_PROFILE_BALANCED:
			prf_new = CASPER_NEW_GAMING;
			break;
		case PLATFORM_PROFILE_LOW_POWER:
			prf_new = CASPER_NEW_AUDIO;
			break;
		default:
			return -EINVAL;
		}

		return casper_set(drv, CASPER_POWERPLAN, prf_new, 0);
	}

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		prf_old = CASPER_HIGH_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		prf_old = CASPER_GAMING;
		break;
	case PLATFORM_PROFILE_BALANCED:
		prf_old = CASPER_TEXT_MODE;
		break;
	case PLATFORM_PROFILE_LOW_POWER:
		prf_old = CASPER_POWERSAVE;
		break;
	default:
		return -EINVAL;
	}

	return casper_set(drv, CASPER_POWERPLAN, prf_old, 0);
}

static umode_t casper_wmi_hwmon_is_visible(const void *drvdata,
					   enum hwmon_sensor_types type,
					   u32 attr, int channel)
{
	return 0444;
}

static int casper_wmi_hwmon_read(struct device *dev,
				 enum hwmon_sensor_types type, u32 attr,
				 int channel, long *val)
{
	struct casper_drv *drv = dev_get_drvdata(dev->parent);
	struct casper_wmi_args out = { 0 };
	int ret;

	ret = casper_query(drv, CASPER_GET_HARDWAREINFO, &out);
	if (ret)
		return ret;

	switch (channel) {
	case CASPER_FAN_CPU:
		if (drv->quirk_applied->big_endian_fans)
			*val = be16_to_cpu(*(__be16 *)&out.a4);
		else
			*val = out.a5;
		break;
	case CASPER_FAN_GPU:
		if (drv->quirk_applied->big_endian_fans)
			*val = be16_to_cpu(*(__be16 *)&out.a5);
		else
			*val = out.a5;
		break;
	}

	return 0;
}

static int casper_wmi_hwmon_read_string(struct device *dev,
					enum hwmon_sensor_types type, u32 attr,
					int channel, const char **str)
{
	if (channel == CASPER_FAN_CPU)
		*str = "cpu_fan_speed";
	else if (channel == CASPER_FAN_GPU)
		*str = "gpu_fan_speed";
	return 0;
}

static const struct hwmon_ops casper_wmi_hwmon_ops = {
	.is_visible = &casper_wmi_hwmon_is_visible,
	.read = &casper_wmi_hwmon_read,
	.read_string = &casper_wmi_hwmon_read_string,
};

static const struct hwmon_channel_info *const casper_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_chip_info casper_wmi_hwmon_chip_info = {
	.ops = &casper_wmi_hwmon_ops,
	.info = casper_wmi_hwmon_info,
};

static struct casper_quirk_entry gen_older_than_11 = {
	.big_endian_fans = true,
	.new_power_scheme = false
};

static struct casper_quirk_entry gen_newer_than_11 = {
	.big_endian_fans = false,
	.new_power_scheme = true
};

static const struct x86_cpu_id casper_gen[] = {
	X86_MATCH_INTEL_FAM6_MODEL(KABYLAKE, &gen_older_than_11),
	X86_MATCH_INTEL_FAM6_MODEL(COMETLAKE, &gen_older_than_11),
	X86_MATCH_INTEL_FAM6_MODEL(TIGERLAKE, &gen_newer_than_11),
	X86_MATCH_INTEL_FAM6_MODEL(ALDERLAKE, &gen_newer_than_11),
	X86_MATCH_INTEL_FAM6_MODEL(RAPTORLAKE, &gen_newer_than_11),
	X86_MATCH_INTEL_FAM6_MODEL(METEORLAKE, &gen_newer_than_11),
	{}
};

static struct casper_quirk_entry quirk_no_power_profile = {
	.no_power_profiles = true
};

static struct casper_quirk_entry quirk_has_power_profile = {
	.no_power_profiles = false
};

static const struct dmi_system_id casper_quirks[] = {
	{
		.ident = "CASPER EXCALIBUR G650",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "CASPER BILGISAYAR SISTEMLERI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EXCALIBUR G650")
		},
		.driver_data = &quirk_no_power_profile
	},
	{
		.ident = "CASPER EXCALIBUR G670",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "CASPER BILGISAYAR SISTEMLERI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EXCALIBUR G670")
		},
		.driver_data = &quirk_no_power_profile
	},
	{
		.ident = "CASPER EXCALIBUR G750",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "CASPER BILGISAYAR SISTEMLERI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EXCALIBUR G750")
		},
		.driver_data = &quirk_no_power_profile
	},
	{
		.ident = "CASPER EXCALIBUR G770",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "CASPER BILGISAYAR SISTEMLERI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EXCALIBUR G770")
		},
		.driver_data = &quirk_has_power_profile
	},
	{
		.ident = "CASPER EXCALIBUR G780",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "CASPER BILGISAYAR SISTEMLERI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EXCALIBUR G780")
		},
		.driver_data = &quirk_has_power_profile
	},
	{
		.ident = "CASPER EXCALIBUR G870",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "CASPER BILGISAYAR SISTEMLERI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EXCALIBUR G870")
		},
		.driver_data = &quirk_has_power_profile
	},
	{
		.ident = "CASPER EXCALIBUR G900",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "CASPER BILGISAYAR SISTEMLERI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EXCALIBUR G900")
		},
		.driver_data = &quirk_has_power_profile
	},
	{
		.ident = "CASPER EXCALIBUR G911",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "CASPER BILGISAYAR SISTEMLERI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EXCALIBUR G911")
		},
		.driver_data = &quirk_has_power_profile
	},
	{ }
};

static void casper_pp_remove(void *data)
{
	platform_profile_remove();
}

static int casper_platform_profile_register(struct casper_drv *drv)
{
	int ret = 0;

	drv->handler.profile_get = casper_platform_profile_get;
	drv->handler.profile_set = casper_platform_profile_set;

	set_bit(PLATFORM_PROFILE_LOW_POWER, drv->handler.choices);
	set_bit(PLATFORM_PROFILE_BALANCED, drv->handler.choices);
	if (!drv->quirk_applied->new_power_scheme)
		set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE,
			drv->handler.choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, drv->handler.choices);

	ret = platform_profile_register(&drv->handler);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&drv->wdev->dev, casper_pp_remove,
				       NULL);
	if (ret)
		platform_profile_remove();

	return ret;
}

static int casper_multicolor_register(struct casper_drv *drv)
{
	int ret = 0;

	drv->leds = devm_kcalloc(&drv->wdev->dev,
		CASPER_LED_COUNT, sizeof(*drv->leds), GFP_KERNEL);
	if (!drv->leds)
		return -ENOMEM;

	for (size_t i = 0; i < CASPER_LED_COUNT; i++) {
		for (size_t j = 0; j < 3; j++) {
			drv->leds[i].subleds[j] = (struct mc_subled) {
				.color_index = LED_COLOR_ID_RED + j,
				.brightness = 255,
				.intensity = 255
			};
		}
		drv->leds[i].mc_led = (struct led_classdev_mc){
			.led_cdev = {
				.name = zone_names[i],
				.brightness = 0,
				.max_brightness = 2,
				.brightness_set = &set_casper_brightness,
				.brightness_get = &get_casper_brightness,
				.color = LED_COLOR_ID_MULTI,
			},
			.num_colors = 3,
			.subled_info = drv->leds[i].subleds
		};

		ret = devm_led_classdev_multicolor_register(&drv->wdev->dev,
							&drv->leds[i].mc_led);
		if (ret)
			return -ENODEV;
	}

	// Setting leds to the default color
	ret = casper_set(drv, CASPER_SET_LED, CASPER_ALL_KEYBOARD_LEDS,
			 CASPER_DEFAULT_COLOR);
	if (ret)
		return ret;

	ret = casper_set(drv, CASPER_SET_LED, CASPER_CORNER_LEDS,
			 CASPER_DEFAULT_COLOR);
	return ret;
}

static int casper_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct casper_quirk_entry *pp_quirk;
	const struct dmi_system_id *dmi_id;
	const struct x86_cpu_id *gen_id;
	struct device *hwmon_dev;
	struct casper_drv *drv;
	int ret;

	drv = devm_kzalloc(&wdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->wdev = wdev;
	dev_set_drvdata(&wdev->dev, drv);

	gen_id = x86_match_cpu(casper_gen);
	if (!gen_id)
		return -ENODEV;

	drv->quirk_applied = (struct casper_quirk_entry *)gen_id->driver_data;

	dmi_id = dmi_first_match(casper_quirks);
	if (!dmi_id)
		return -ENODEV;

	pp_quirk = (struct casper_quirk_entry *)dmi_id->driver_data;
	drv->quirk_applied->no_power_profiles = pp_quirk->no_power_profiles;

	mutex_init(&drv->mutex);
	ret = devm_mutex_init(&wdev->dev, &drv->mutex);
	if (ret)
		return ret;

	ret = casper_multicolor_register(drv);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_info(&wdev->dev,
						"casper_wmi", wdev,
						&casper_wmi_hwmon_chip_info,
						NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	if (!drv->quirk_applied->no_power_profiles) {
		ret = casper_platform_profile_register(drv);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct wmi_device_id casper_wmi_id_table[] = {
	{ CASPER_WMI_GUID, NULL },
	{ }
};
MODULE_DEVICE_TABLE(wmi, casper_wmi_id_table);

static struct wmi_driver casper_drv = {
	.driver = {
		.name = "casper-wmi",
	},
	.id_table = casper_wmi_id_table,
	.probe = casper_wmi_probe,
	.no_singleton = true,
};

module_wmi_driver(casper_drv);

MODULE_AUTHOR("Mustafa Ekşi <mustafa.eskieksi@gmail.com>");
MODULE_DESCRIPTION("Casper Excalibur Laptop WMI driver");
MODULE_LICENSE("GPL");
