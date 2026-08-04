/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GHES_NVIDIA_H
#define GHES_NVIDIA_H

#include <linux/build_bug.h>
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
} __packed;

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

/**
 * nvidia_ghes_grace_reg_pair() - Read one Grace register address/value pair
 * @decoded: Decoded Grace section; format must be NVIDIA_GHES_FORMAT_GRACE
 * @index: Register index in [0, number_regs)
 * @addr: Output register address
 * @val: Output register value
 *
 * When number_regs is non-zero, decoded->grace_regs must be non-NULL.
 * Returns -EINVAL for bad arguments / missing grace_regs, -ERANGE for
 * index >= number_regs, and 0 on success.
 */
VISIBLE_IF_KUNIT int nvidia_ghes_decode_grace(struct device *dev, const void *buf,
					      size_t len,
					      struct nvidia_ghes_decoded *decoded);
VISIBLE_IF_KUNIT int nvidia_ghes_grace_reg_pair(const struct nvidia_ghes_decoded *decoded,
						unsigned int index, u64 *addr, u64 *val);

#endif
