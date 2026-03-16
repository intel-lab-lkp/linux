// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * apple-brightness.c - EFI brightness saver on Macs
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/backlight.h>
#include <linux/efi.h>
#include <linux/platform_data/apple-brightness.h>

static u32 efi_attr;
static u16 last_saved_level;

static int (*get_brightness)(struct backlight_device *bl);
static struct backlight_device *bl_dev;

void apple_brightness_shutdown(void)
{
	u16 level;
	efi_status_t status;

	level = (u16)get_brightness(bl_dev);

	if (level == last_saved_level)
		return;

	status = efivar_set_variable(APPLE_BRIGHTNESS_NAME, &APPLE_BRIGHTNESS_GUID,
				efi_attr, sizeof(level), &level);
	if (status != EFI_SUCCESS)
		pr_debug("Unable to set brightness: 0x%lx\n", status);
}
EXPORT_SYMBOL(apple_brightness_shutdown);

int apple_brightness_probe(struct backlight_device *bl,
	int (*get_brightnessfn)(struct backlight_device *bl))
{
	efi_status_t status;
	unsigned long size = sizeof(last_saved_level);
	int ret;

	bl_dev = bl;
	get_brightness = get_brightnessfn;

	if (!efi_rt_services_supported(EFI_RT_SUPPORTED_SET_VARIABLE))
		return -ENODEV;

	ret = efivar_lock();
	if (ret)
		return ret;

	status = efivar_get_variable(APPLE_BRIGHTNESS_NAME, &APPLE_BRIGHTNESS_GUID,
				&efi_attr, &size, &last_saved_level);

	efivar_unlock();

	if (status != EFI_SUCCESS)
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL(apple_brightness_probe);

MODULE_AUTHOR("Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>");
MODULE_DESCRIPTION("EFI Brightness saver for Macs");
MODULE_LICENSE("Dual MIT/GPL");
