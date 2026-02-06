/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * apple-brightness.h - EFI brightness saver for Macs
 * Copyright (C) 2026 Atharva Tiwari <atharvatiwarilinuxdev@gmail.com>
 */

#ifndef _APPLE_BL_H_
#define _APPLE_BL_H_

#include <linux/backlight.h>
#include <linux/efi.h>

#define APPLE_BRIGHTNESS_NAME           L"backlight-level"
#define APPLE_BRIGHTNESS_GUID           EFI_GUID(0x7c436110, 0xab2a, 0x4bbb, 0xa8, 0x80, 0xfe, 0x41, 0x99, 0x5c, 0x9f, 0x82)

#define APPLE_BRIGHTNESS_POLL           300

int apple_brightness_probe(struct backlight_device *bl,
	int (*get_brightnessfn)(struct backlight_device *bl));

#endif /* _APPLE_BL_H */
