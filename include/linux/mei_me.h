/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Intel Corporation. All rights reserved.
 */

#ifndef _LINUX_MEI_ME_H
#define _LINUX_MEI_ME_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_INTEL_MEI_ME)
bool mei_me_device_present(void);
#else
static inline bool mei_me_device_present(void)
{
	return false;
}
#endif

#endif /* _LINUX_MEI_ME_H */
