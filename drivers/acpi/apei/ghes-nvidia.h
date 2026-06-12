/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GHES_NVIDIA_H
#define GHES_NVIDIA_H

#include <linux/types.h>
#include <kunit/visibility.h>

struct device;

enum nvidia_ghes_format {
	NVIDIA_GHES_FORMAT_UNKNOWN,
	NVIDIA_GHES_FORMAT_GRACE,
};

struct nvidia_ghes_grace_reg {
	__le64 addr;
	__le64 val;
};

struct nvidia_ghes_decoded {
	enum nvidia_ghes_format format;
	char signature[17];
	u16 error_type;
	u16 error_instance;
	u8 severity;
	u8 socket;
	u8 number_regs;
	u64 instance_base;
	const struct nvidia_ghes_grace_reg *grace_regs;
};

VISIBLE_IF_KUNIT int nvidia_ghes_decode_grace(struct device *dev, const void *buf,
					      size_t len,
					      struct nvidia_ghes_decoded *decoded);
VISIBLE_IF_KUNIT int nvidia_ghes_grace_reg_pair(const struct nvidia_ghes_decoded *decoded,
						unsigned int index, u64 *addr, u64 *val);

#endif
