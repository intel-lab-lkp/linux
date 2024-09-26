/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This code gives the built in RGB lighting of the TUXEDO NB04 devices a
 * standardised interface, namely HID LampArray.
 *
 * Copyright (C) 2024 Werner Sembach wse@tuxedocomputers.com
 */

#ifndef TUXEDO_NB04_WMI_AB_VIRTUAL_LAMP_ARRAY_H
#define TUXEDO_NB04_WMI_AB_VIRTUAL_LAMP_ARRAY_H

#include <linux/wmi.h>
#include <linux/hid.h>

int tuxedo_nb04_virtual_lamp_array_add_device(struct wmi_device *wdev,
					      struct hid_device **hdev_out);

#endif
