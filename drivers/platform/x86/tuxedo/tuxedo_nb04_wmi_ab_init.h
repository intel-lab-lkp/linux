/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This driver implements the WMI AB device found on TUXEDO Notebooks with board
 * vendor NB04.
 *
 * Copyright (C) 2024 Werner Sembach wse@tuxedocomputers.com
 */

#ifndef TUXEDO_NB04_WMI_AB_INIT_H
#define TUXEDO_NB04_WMI_AB_INIT_H

#include <linux/mutex.h>
#include <linux/hid.h>

struct tuxedo_nb04_wmi_driver_data_t {
	struct mutex wmi_access_mutex;
	struct hid_device *virtual_lamp_array_hdev;
};

#endif
