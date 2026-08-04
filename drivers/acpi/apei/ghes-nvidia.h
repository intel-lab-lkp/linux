/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GHES_NVIDIA_H
#define GHES_NVIDIA_H

#include <linux/build_bug.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <kunit/visibility.h>

enum nvidia_ghes_format {
	NVIDIA_GHES_FORMAT_UNKNOWN,
	NVIDIA_GHES_FORMAT_GRACE,
	NVIDIA_GHES_FORMAT_VERA,
};

struct nvidia_ghes_grace_reg {
	__le64 addr;
	__le64 val;
} __packed;

struct nvidia_ghes_vera_context {
	u32 context_size;
	u16 context_version;
	u16 data_format_type;
	u16 data_format_version;
	u32 data_size;
	const u8 *data;
};

struct nvidia_ghes_vera_cursor {
	const u8 *pos;
	size_t remaining;
	u8 contexts_left;
};

struct nvidia_ghes_decoded {
	enum nvidia_ghes_format format;
	char signature[17];
	u16 error_type;
	u16 error_instance;
	u16 event_type;
	u16 event_sub_type;
	u8 severity;
	u8 socket;
	u8 number_regs;
	u8 source_device_type;
	u8 event_context_count;
	u8 valid_context_count;
	u32 architecture;
	u64 event_link_id;
	u64 instance_base;
	u8 chip_serial_number[16];
	const struct nvidia_ghes_grace_reg *grace_regs;
	const u8 *vera_contexts;
	size_t vera_contexts_len;
};

/* Bound the notifier stack frame. */
static_assert(sizeof(struct nvidia_ghes_decoded) <= 128);

struct device;

VISIBLE_IF_KUNIT enum nvidia_ghes_format nvidia_ghes_format_from_guid(const guid_t *guid);
VISIBLE_IF_KUNIT int nvidia_ghes_decode_grace(struct device *dev, const void *buf,
					      size_t len,
					      struct nvidia_ghes_decoded *decoded);
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
VISIBLE_IF_KUNIT int nvidia_ghes_grace_reg_pair(const struct nvidia_ghes_decoded *decoded,
						unsigned int index, u64 *addr, u64 *val);
VISIBLE_IF_KUNIT int nvidia_ghes_decode_vera(struct device *dev, const void *buf,
					     size_t len,
					     struct nvidia_ghes_decoded *decoded);
VISIBLE_IF_KUNIT
void nvidia_ghes_vera_cursor_init(const struct nvidia_ghes_decoded *decoded,
				  struct nvidia_ghes_vera_cursor *cursor);
VISIBLE_IF_KUNIT
int nvidia_ghes_vera_cursor_next(struct nvidia_ghes_vera_cursor *cursor,
				 struct nvidia_ghes_vera_context *ctx);
VISIBLE_IF_KUNIT
int nvidia_ghes_vera_print_context_count(const struct nvidia_ghes_decoded *decoded);
VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_entry_count(const struct nvidia_ghes_vera_context *ctx);
VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_u64_pair(const struct nvidia_ghes_vera_context *ctx,
				      unsigned int index, u64 *key, u64 *val);
VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_u32_pair(const struct nvidia_ghes_vera_context *ctx,
				      unsigned int index, u32 *key, u32 *val);
VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_u64_value(const struct nvidia_ghes_vera_context *ctx,
				       unsigned int index, u64 *val);
VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_u32_value(const struct nvidia_ghes_vera_context *ctx,
				       unsigned int index, u32 *val);
VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_print_count(const struct nvidia_ghes_vera_context *ctx,
					 unsigned int *remaining);

#endif
