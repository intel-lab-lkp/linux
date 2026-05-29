/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OSF_CORE_H
#define _OSF_CORE_H

#include <linux/types.h>

struct device;

struct osf_device {
	struct device *dev;
	u64 last_sequence;
};

void osf_core_init(struct osf_device *osf, struct device *dev);
void osf_core_unregister_iio(struct osf_device *osf);
int osf_core_receive_frame(struct osf_device *osf, const u8 *buf, size_t len);

#endif
