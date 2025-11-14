/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Intel Corporation. All rights reserved.
 */

#ifndef _LINUX_MEI_ME_H
#define _LINUX_MEI_ME_H

#include <linux/pci.h>

#if IS_ENABLED(CONFIG_INTEL_MEI_ME)
extern const struct pci_device_id mei_me_pci_tbl[];
#endif

#endif /* _LINUX_MEI_ME_H */
