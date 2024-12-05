// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Alienware AlienFX control
 *
 * Copyright (C) 2014 Dell Inc <Dell.Client.Kernel@dell.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dmi.h>
#include <linux/wmi.h>
#include "alienware-wmi.h"

MODULE_AUTHOR("Mario Limonciello <mario.limonciello@outlook.com>");
MODULE_DESCRIPTION("Alienware special feature control");
MODULE_LICENSE("GPL");

static bool force_platform_profile;
module_param_unsafe(force_platform_profile, bool, 0);
MODULE_PARM_DESC(force_platform_profile, "Forces auto-detecting thermal profiles without checking if WMI thermal backend is available");

static bool force_gmode;
module_param_unsafe(force_gmode, bool, 0);
MODULE_PARM_DESC(force_gmode, "Forces G-Mode when performance profile is selected");

enum INTERFACE_FLAGS {
	LEGACY,
	WMAX,
};

struct quirk_entry {
	u8 num_zones;
	u8 hdmi_mux;
	u8 amplifier;
	u8 deepslp;
	bool thermal;
	bool gmode;
};

static struct quirk_entry *quirks;


static struct quirk_entry quirk_inspiron5675 = {
	.num_zones = 2,
	.hdmi_mux = 0,
	.amplifier = 0,
	.deepslp = 0,
	.thermal = false,
	.gmode = false,
};

static struct quirk_entry quirk_unknown = {
	.num_zones = 2,
	.hdmi_mux = 0,
	.amplifier = 0,
	.deepslp = 0,
	.thermal = false,
	.gmode = false,
};

static struct quirk_entry quirk_x51_r1_r2 = {
	.num_zones = 3,
	.hdmi_mux = 0,
	.amplifier = 0,
	.deepslp = 0,
	.thermal = false,
	.gmode = false,
};

static struct quirk_entry quirk_x51_r3 = {
	.num_zones = 4,
	.hdmi_mux = 0,
	.amplifier = 1,
	.deepslp = 0,
	.thermal = false,
	.gmode = false,
};

static struct quirk_entry quirk_asm100 = {
	.num_zones = 2,
	.hdmi_mux = 1,
	.amplifier = 0,
	.deepslp = 0,
	.thermal = false,
	.gmode = false,
};

static struct quirk_entry quirk_asm200 = {
	.num_zones = 2,
	.hdmi_mux = 1,
	.amplifier = 0,
	.deepslp = 1,
	.thermal = false,
	.gmode = false,
};

static struct quirk_entry quirk_asm201 = {
	.num_zones = 2,
	.hdmi_mux = 1,
	.amplifier = 1,
	.deepslp = 1,
	.thermal = false,
	.gmode = false,
};

static struct quirk_entry quirk_g_series = {
	.num_zones = 2,
	.hdmi_mux = 0,
	.amplifier = 0,
	.deepslp = 0,
	.thermal = true,
	.gmode = true,
};

static struct quirk_entry quirk_x_series = {
	.num_zones = 2,
	.hdmi_mux = 0,
	.amplifier = 0,
	.deepslp = 0,
	.thermal = true,
	.gmode = false,
};

static int __init dmi_matched(const struct dmi_system_id *dmi)
{
	quirks = dmi->driver_data;
	return 1;
}

static const struct dmi_system_id alienware_quirks[] __initconst = {
	{
		.callback = dmi_matched,
		.ident = "Alienware ASM100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ASM100"),
		},
		.driver_data = &quirk_asm100,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware ASM200",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ASM200"),
		},
		.driver_data = &quirk_asm200,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware ASM201",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ASM201"),
		},
		.driver_data = &quirk_asm201,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware m17 R5",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Alienware m17 R5 AMD"),
		},
		.driver_data = &quirk_x_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware m18 R2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Alienware m18 R2"),
		},
		.driver_data = &quirk_x_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware x15 R1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Alienware x15 R1"),
		},
		.driver_data = &quirk_x_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware x17 R2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Alienware x17 R2"),
		},
		.driver_data = &quirk_x_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware X51 R1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Alienware X51"),
		},
		.driver_data = &quirk_x51_r1_r2,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware X51 R2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Alienware X51 R2"),
		},
		.driver_data = &quirk_x51_r1_r2,
	},
	{
		.callback = dmi_matched,
		.ident = "Alienware X51 R3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Alienware"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Alienware X51 R3"),
		},
		.driver_data = &quirk_x51_r3,
	},
	{
		.callback = dmi_matched,
		.ident = "Dell Inc. G15 5510",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Dell G15 5510"),
		},
		.driver_data = &quirk_g_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Dell Inc. G15 5511",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Dell G15 5511"),
		},
		.driver_data = &quirk_g_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Dell Inc. G15 5515",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Dell G15 5515"),
		},
		.driver_data = &quirk_g_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Dell Inc. G3 3500",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "G3 3500"),
		},
		.driver_data = &quirk_g_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Dell Inc. G3 3590",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "G3 3590"),
		},
		.driver_data = &quirk_g_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Dell Inc. G5 5500",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "G5 5500"),
		},
		.driver_data = &quirk_g_series,
	},
	{
		.callback = dmi_matched,
		.ident = "Dell Inc. Inspiron 5675",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Inspiron 5675"),
		},
		.driver_data = &quirk_inspiron5675,
	},
	{}
};

static u8 interface;
static struct wmi_driver *preferred_wmi_driver;

acpi_status alienware_wmi_command(struct wmi_device *wdev, u32 method_id,
				  void *in_args, size_t in_size, u32 *out_data)
{
	acpi_status ret;
	union acpi_object *obj;
	struct acpi_buffer in = { in_size,  in_args };
	struct acpi_buffer out = {  ACPI_ALLOCATE_BUFFER, NULL };

	if (out_data) {
		ret = wmidev_evaluate_method(wdev, 0, method_id, &in, &out);
		if (ACPI_FAILURE(ret))
			goto out_free_ptr;

		obj = (union acpi_object *) out.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER)
			*out_data = (u32) obj->integer.value;
	} else {
		ret = wmidev_evaluate_method(wdev, 0, method_id, &in, NULL);
	}

out_free_ptr:
	kfree(out.pointer);
	return ret;
}

/*
 * Legacy WMI device
 */
static int legacy_wmi_update_led(struct alienfx_priv *priv,
				 struct wmi_device *wdev, u8 location)
{
	acpi_status status;
	struct acpi_buffer input;
	struct legacy_led_args legacy_args;

	legacy_args.colors = priv->colors[location];
	legacy_args.brightness = priv->global_brightness;
	legacy_args.state = priv->lighting_control_state;

	input.length = sizeof(legacy_args);
	input.pointer = &legacy_args;

	if (legacy_args.state == LEGACY_RUNNING)
		status = alienware_wmi_command(wdev, location + 1, &legacy_args,
					       sizeof(legacy_args), NULL);
	else
		status = wmi_evaluate_method(LEGACY_POWER_CONTROL_GUID, 0,
					     location + 1, &input, NULL);

	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static int legacy_wmi_update_brightness(struct alienfx_priv *priv,
					struct wmi_device *wdev, u8 brightness)
{
	return legacy_wmi_update_led(priv, wdev, 0);
}

static int legacy_wmi_probe(struct wmi_device *wdev, const void *context)
{
	int ret = 0;
	struct alienfx_platdata pdata = {
		.wdev = wdev,
		.ops = {
			.upd_led = legacy_wmi_update_led,
			.upd_brightness = legacy_wmi_update_brightness,
		},
		.num_zones = quirks->num_zones,
		.hdmi_mux = quirks->hdmi_mux,
		.amplifier = quirks->amplifier,
		.deepslp = quirks->deepslp,
		.running_code = LEGACY_RUNNING,
	};

	if (quirks->num_zones > 0)
		ret = alienfx_wmi_init(&pdata);

	if (ret < 0)
		return ret;

	return 0;
}

static void legacy_wmi_remove(struct wmi_device *wdev)
{
	if (quirks->num_zones > 0)
		alienfx_wmi_exit(wdev);
}

static struct wmi_device_id alienware_legacy_device_id_table[] = {
	{ LEGACY_CONTROL_GUID, NULL },
	{ },
};
MODULE_DEVICE_TABLE(wmi, alienware_legacy_device_id_table);

static struct wmi_driver alienware_legacy_wmi_driver = {
	.driver = {
		.name = "alienware-wmi-alienfx",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = alienware_legacy_device_id_table,
	.probe = legacy_wmi_probe,
	.remove = legacy_wmi_remove,
};

/*
 * WMAX WMI device
 */
static int wmax_wmi_update_led(struct alienfx_priv *priv,
			       struct wmi_device *wdev, u8 location)
{
	acpi_status status;
	struct wmax_led_args in_args = {
		.led_mask = 1 << location,
		.colors = priv->colors[location],
		.state = priv->lighting_control_state,
	};

	status = alienware_wmi_command(wdev, WMAX_METHOD_ZONE_CONTROL,
				       &in_args, sizeof(in_args), NULL);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static int wmax_wmi_update_brightness(struct alienfx_priv *priv,
				      struct wmi_device *wdev, u8 brightness)
{
	acpi_status status;
	struct wmax_brightness_args in_args = {
		.led_mask = 0xFF,
		.percentage = brightness,
	};

	status = alienware_wmi_command(wdev, WMAX_METHOD_BRIGHTNESS, &in_args,
				       sizeof(in_args), NULL);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static int wmax_wmi_probe(struct wmi_device *wdev, const void *context)
{
	int ret = 0;
	struct alienfx_platdata pdata = {
		.wdev = wdev,
		.ops = {
			.upd_led = wmax_wmi_update_led,
			.upd_brightness = wmax_wmi_update_brightness,
		},
		.num_zones = quirks->num_zones,
		.hdmi_mux = quirks->hdmi_mux,
		.amplifier = quirks->amplifier,
		.deepslp = quirks->deepslp,
		.running_code = WMAX_RUNNING,
	};

	if (quirks->thermal)
		ret = create_thermal_profile(wdev, quirks->gmode);
	else if (quirks->num_zones > 0)
		ret = alienfx_wmi_init(&pdata);

	if (ret < 0)
		return ret;

	return 0;
}

static void wmax_wmi_remove(struct wmi_device *wdev)
{
	if (quirks->thermal)
		remove_thermal_profile();
	else if (quirks->num_zones > 0)
		alienfx_wmi_exit(wdev);
}

static struct wmi_device_id alienware_wmax_device_id_table[] = {
	{ WMAX_CONTROL_GUID, NULL },
	{ },
};
MODULE_DEVICE_TABLE(wmi, alienware_wmax_device_id_table);

static struct wmi_driver alienware_wmax_wmi_driver = {
	.driver = {
		.name = "alienware-wmi-wmax",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = alienware_wmax_device_id_table,
	.probe = wmax_wmi_probe,
	.remove = wmax_wmi_remove,
};

static int __init alienware_wmi_init(void)
{
	if (wmi_has_guid(WMAX_CONTROL_GUID)) {
		interface = WMAX;
		preferred_wmi_driver = &alienware_wmax_wmi_driver;
	} else {
		interface = LEGACY;
		preferred_wmi_driver = &alienware_legacy_wmi_driver;
	}

	dmi_check_system(alienware_quirks);
	if (quirks == NULL)
		quirks = &quirk_unknown;

	if (force_platform_profile)
		quirks->thermal = true;

	if (force_gmode) {
		if (quirks->thermal)
			quirks->gmode = true;
		else
			pr_warn("force_gmode requires platform profile support\n");
	}

	return wmi_driver_register(preferred_wmi_driver);
}

module_init(alienware_wmi_init);

static void __exit alienware_wmi_exit(void)
{
	wmi_driver_unregister(preferred_wmi_driver);
}

module_exit(alienware_wmi_exit);
