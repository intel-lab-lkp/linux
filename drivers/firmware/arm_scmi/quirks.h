/* SPDX-License-Identifier: GPL-2.0 */
/*
 * System Control and Management Interface (SCMI) Message Protocol Quirks
 *
 * Copyright (C) 2025 ARM Ltd.
 */
#ifndef _SCMI_QUIRKS_INTERNAL_H
#define _SCMI_QUIRKS_INTERNAL_H

#include <linux/device.h>
#include <linux/types.h>

#ifdef CONFIG_ARM_SCMI_QUIRKS

void scmi_quirks_initialize(void);
void scmi_quirks_enable(struct device *dev, const char *vend,
			const char *subv, const u32 impl);

#else

static inline void scmi_quirks_initialize(void) { }
static inline void scmi_quirks_enable(struct device *dev, const char *vend,
				      const char *sub_vend, const u32 impl) { }

#endif /* CONFIG_ARM_SCMI_QUIRKS */

#endif /* _SCMI_QUIRKS_INTERNAL_H */
