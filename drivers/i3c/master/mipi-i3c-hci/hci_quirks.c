// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD SOC I3C HCI quirks
 *
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * Authors: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 *          Guruvendra Punugupati <Guruvendra.Punugupati@amd.com>
 */

#include <linux/i3c/master.h>
#include "hci.h"

/* Timing registers */
#define HCI_SCL_I3C_OD_TIMING		0x214
#define HCI_SCL_I3C_PP_TIMING		0x218
#define HCI_SDA_HOLD_SWITCH_DLY_TIMING	0x230

/* Timing values to configure 9MHz frequency */
#define AMD_SCL_I3C_OD_TIMING		0x00cf00cf
#define AMD_SCL_I3C_PP_TIMING		0x00160016

void amd_i3c_hci_quirks_init(struct i3c_hci *hci)
{
	if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD) {
		hci->quirks |= HCI_QUIRK_AMD_PIO_MODE;
		hci->quirks |= HCI_QUIRK_AMD_OD_PP_TIMING;
	}
}

void amd_set_od_pp_timing(struct i3c_hci *hci)
{
	u32 data;

	reg_write(HCI_SCL_I3C_OD_TIMING, AMD_SCL_I3C_OD_TIMING);
	reg_write(HCI_SCL_I3C_PP_TIMING, AMD_SCL_I3C_PP_TIMING);
	data = reg_read(HCI_SDA_HOLD_SWITCH_DLY_TIMING);
	/* Configure maximum TX hold time */
	data |= W0_MASK(18, 16);
	reg_write(HCI_SDA_HOLD_SWITCH_DLY_TIMING, data);
}
