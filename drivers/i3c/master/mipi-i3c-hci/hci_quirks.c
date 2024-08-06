// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * I3C HCI Quirks
 *
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * Authors: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 *			Guruvendra Punugupati <Guruvendra.Punugupati@amd.com>
 */

#include <linux/i3c/master.h>
#include "hci.h"

void amd_i3c_hci_quirks_init(struct i3c_hci *hci)
{
#if defined(CONFIG_X86)
	if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD)
		hci->quirks |= HCI_QUIRK_PIO_MODE;
#endif
}
