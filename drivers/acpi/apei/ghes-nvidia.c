// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVIDIA GHES vendor record handler
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/align.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>
#include <kunit/visibility.h>
#include <acpi/ghes.h>

#include "ghes-nvidia.h"

#define NVIDIA_GHES_VERA_VERSION	1
#define NVIDIA_GHES_VERA_CPU_INFO_MAJOR	0
/* Cap section-wide entry log lines so a large dump cannot flood the console. */
#define NVIDIA_GHES_MAX_PRINT_ENTRIES	32
#define NVIDIA_GHES_MAX_PRINT_CONTEXTS	16

static __printf(3, 4) void nvidia_ghes_decode_err(struct device *dev,
						 bool fatal, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (!dev)
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	if (fatal)
		dev_err(dev, "%pV", &vaf);
	else
		dev_err_ratelimited(dev, "%pV", &vaf);

	va_end(args);
}

static const guid_t nvidia_grace_sec_guid =
	GUID_INIT(0x6d5244f2, 0x2712, 0x11ec,
		  0xbe, 0xa7, 0xcb, 0x3f, 0xdb, 0x95, 0xc7, 0x86);

static const guid_t nvidia_vera_sec_guid =
	GUID_INIT(0x9068e568, 0x6ca0, 0x11f0,
		  0xae, 0xaf, 0x15, 0x93, 0x43, 0x59, 0x1e, 0xac);

/* Grace CPER section wire layout (header without flexible register array). */
struct cper_sec_nvidia {
	char	signature[16];
	__le16	error_type;
	__le16	error_instance;
	u8	severity;
	u8	socket;
	u8	number_regs;
	u8	reserved;
	__le64	instance_base;
	struct nvidia_ghes_grace_reg regs[] __counted_by(number_regs);
} __packed;

static_assert(sizeof(struct cper_sec_nvidia) == 32);

struct cper_sec_nvidia_vera_event {
	u8	version;
	u8	event_context_count;
	u8	source_device_type;
	u8	reserved;
	__le16	event_type;
	__le16	event_sub_type;
	__le64	event_link_id;
	char	source_module_signature[16];
} __packed;

static_assert(sizeof(struct cper_sec_nvidia_vera_event) == 32);

struct cper_sec_nvidia_vera_cpu_info {
	__le16	info_version;
	u8	info_size;
	u8	socket_number;
	__le32	architecture;
	u8	chip_serial_number[16];
	__le64	instance_base;
} __packed;

static_assert(sizeof(struct cper_sec_nvidia_vera_cpu_info) == 32);

struct cper_sec_nvidia_vera_context {
	__le32	context_size;
	__le16	context_version;
	__le16	reserved;
	__le16	data_format_type;
	__le16	data_format_version;
	__le32	data_size;
} __packed;

static_assert(sizeof(struct cper_sec_nvidia_vera_context) == 16);

struct nvidia_ghes_private {
	struct notifier_block	nb;
	struct device		*dev;
};

VISIBLE_IF_KUNIT
enum nvidia_ghes_format nvidia_ghes_format_from_guid(const guid_t *guid)
{
	if (guid_equal(guid, &nvidia_grace_sec_guid))
		return NVIDIA_GHES_FORMAT_GRACE;
	if (guid_equal(guid, &nvidia_vera_sec_guid))
		return NVIDIA_GHES_FORMAT_VERA;
	return NVIDIA_GHES_FORMAT_UNKNOWN;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_format_from_guid);

static int __nvidia_ghes_decode_grace(struct device *dev, const void *buf,
				      size_t len,
				      struct nvidia_ghes_decoded *decoded,
				      bool fatal)
{
	const struct cper_sec_nvidia *nvidia_err = buf;
	size_t min_size;
	u8 number_regs;

	if (!buf || !decoded)
		return -EINVAL;
	if (len < sizeof(*nvidia_err)) {
		nvidia_ghes_decode_err(dev, fatal,
				       "Section too small (%zu < %zu)\n",
				       len, sizeof(*nvidia_err));
		return -ENODATA;
	}

	number_regs = nvidia_err->number_regs;
	min_size = struct_size(nvidia_err, regs, number_regs);
	if (len < min_size) {
		nvidia_ghes_decode_err(dev, fatal,
				       "Invalid number_regs %u (section size %zu, need %zu)\n",
				       number_regs, len, min_size);
		return -ENODATA;
	}

	memset(decoded, 0, sizeof(*decoded));
	decoded->format = NVIDIA_GHES_FORMAT_GRACE;
	memcpy(decoded->signature, nvidia_err->signature, sizeof(nvidia_err->signature));
	decoded->signature[sizeof(nvidia_err->signature)] = '\0';
	decoded->error_type = get_unaligned_le16(&nvidia_err->error_type);
	decoded->error_instance = get_unaligned_le16(&nvidia_err->error_instance);
	decoded->severity = nvidia_err->severity;
	decoded->socket = nvidia_err->socket;
	decoded->number_regs = number_regs;
	decoded->instance_base = get_unaligned_le64(&nvidia_err->instance_base);
	if (number_regs)
		decoded->grace_regs = nvidia_err->regs;

	return 0;
}

VISIBLE_IF_KUNIT
__maybe_unused int nvidia_ghes_decode_grace(struct device *dev, const void *buf,
					    size_t len,
					    struct nvidia_ghes_decoded *decoded)
{
	return __nvidia_ghes_decode_grace(dev, buf, len, decoded, false);
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_decode_grace);

VISIBLE_IF_KUNIT
int nvidia_ghes_grace_reg_pair(const struct nvidia_ghes_decoded *decoded,
			       unsigned int index, u64 *addr, u64 *val)
{
	const struct nvidia_ghes_grace_reg *regs;

	if (!decoded || decoded->format != NVIDIA_GHES_FORMAT_GRACE || !addr || !val)
		return -EINVAL;
	if (decoded->number_regs && !decoded->grace_regs)
		return -EINVAL;
	if (index >= decoded->number_regs)
		return -ERANGE;

	regs = decoded->grace_regs;
	*addr = get_unaligned_le64(&regs[index].addr);
	*val = get_unaligned_le64(&regs[index].val);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_grace_reg_pair);

static int nvidia_ghes_vera_validate_context_data(u16 data_format_type,
						  u32 data_size)
{
	switch (data_format_type) {
	case 0:
		return 0;
	case 1:
		return data_size % 16 ? -EINVAL : 0;
	case 2:
	case 3:
		return data_size % 8 ? -EINVAL : 0;
	case 4:
		return data_size % 4 ? -EINVAL : 0;
	default:
		return -EOPNOTSUPP;
	}
}

static void
__nvidia_ghes_vera_cursor_init(const struct nvidia_ghes_decoded *decoded,
			       struct nvidia_ghes_vera_cursor *cursor, u8 count)
{
	*cursor = (struct nvidia_ghes_vera_cursor) {
		.pos = decoded->vera_contexts,
		.remaining = decoded->vera_contexts_len,
		.contexts_left = count,
	};
}

VISIBLE_IF_KUNIT
void nvidia_ghes_vera_cursor_init(const struct nvidia_ghes_decoded *decoded,
				  struct nvidia_ghes_vera_cursor *cursor)
{
	if (!cursor)
		return;

	memset(cursor, 0, sizeof(*cursor));
	if (!decoded || decoded->format != NVIDIA_GHES_FORMAT_VERA)
		return;

	__nvidia_ghes_vera_cursor_init(decoded, cursor,
				       decoded->valid_context_count);
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_cursor_init);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_cursor_next(struct nvidia_ghes_vera_cursor *cursor,
				 struct nvidia_ghes_vera_context *ctx)
{
	const struct cper_sec_nvidia_vera_context *wire;
	size_t data_end_advance;
	size_t advance;
	u32 context_size;
	u32 data_size;
	u16 data_format_type;
	int ret;

	if (!cursor || !ctx)
		return -EINVAL;
	if (!cursor->contexts_left)
		return 0;
	if (!cursor->pos || cursor->remaining < sizeof(*wire))
		return -ENODATA;

	wire = (const void *)cursor->pos;
	context_size = get_unaligned_le32(&wire->context_size);
	data_format_type = get_unaligned_le16(&wire->data_format_type);
	data_size = get_unaligned_le32(&wire->data_size);

	if (context_size < sizeof(*wire) || !IS_ALIGNED(context_size, 16))
		return -EINVAL;

	ret = nvidia_ghes_vera_validate_context_data(data_format_type, data_size);
	if (ret)
		return ret;
	if (check_add_overflow((size_t)data_size, sizeof(*wire),
			       &data_end_advance))
		return -EOVERFLOW;
	if (data_end_advance > cursor->remaining)
		return -ENODATA;

	/*
	 * CtxSize covers the header, data, and alignment padding. Older opaque
	 * records used a header-only CtxSize, so advance over their data directly.
	 */
	if (data_format_type == 0 && context_size == sizeof(*wire)) {
		advance = data_end_advance;
	} else {
		if (data_end_advance > context_size)
			return -EINVAL;
		if (context_size > cursor->remaining)
			return -ENODATA;
		advance = context_size;
	}

	*ctx = (struct nvidia_ghes_vera_context) {
		.context_size = context_size,
		.context_version = get_unaligned_le16(&wire->context_version),
		.data_format_type = data_format_type,
		.data_format_version = get_unaligned_le16(&wire->data_format_version),
		.data_size = data_size,
		.data = cursor->pos + sizeof(*wire),
	};
	cursor->pos += advance;
	cursor->remaining -= advance;
	cursor->contexts_left--;

	return 1;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_cursor_next);

static int __nvidia_ghes_decode_vera(struct device *dev, const void *buf,
				     size_t len,
				     struct nvidia_ghes_decoded *decoded,
				     bool fatal)
{
	const struct cper_sec_nvidia_vera_event *event = buf;
	const struct cper_sec_nvidia_vera_cpu_info *cpu_info;
	struct nvidia_ghes_vera_cursor cursor;
	struct nvidia_ghes_vera_context context;
	const u8 *bytes = buf;
	size_t offset;
	u16 info_version;
	int ret;

	if (!buf || !decoded)
		return -EINVAL;
	if (len < sizeof(*event)) {
		nvidia_ghes_decode_err(dev, fatal,
				       "Vera event header truncated (%zu < %zu)\n",
				       len, sizeof(*event));
		return -ENODATA;
	}
	if (event->version != NVIDIA_GHES_VERA_VERSION)
		return -EOPNOTSUPP;
	if (event->source_device_type != 0)
		return -EOPNOTSUPP;

	offset = sizeof(*event);
	if (len - offset < sizeof(*cpu_info)) {
		nvidia_ghes_decode_err(dev, fatal,
				       "Vera CPU info truncated (%zu < %zu)\n",
				       len - offset, sizeof(*cpu_info));
		return -ENODATA;
	}

	cpu_info = (const void *)(bytes + offset);
	info_version = get_unaligned_le16(&cpu_info->info_version);
	if ((info_version >> 8) != NVIDIA_GHES_VERA_CPU_INFO_MAJOR)
		return -EOPNOTSUPP;
	if (cpu_info->info_size < sizeof(*cpu_info)) {
		nvidia_ghes_decode_err(dev, fatal,
				       "Vera CPU info size %u smaller than header %zu\n",
				       cpu_info->info_size, sizeof(*cpu_info));
		return -EINVAL;
	}
	if (len - offset < cpu_info->info_size) {
		nvidia_ghes_decode_err(dev, fatal,
				       "Vera CPU info extends past section (%u > %zu)\n",
				       cpu_info->info_size, len - offset);
		return -ENODATA;
	}

	offset += cpu_info->info_size;

	memset(decoded, 0, sizeof(*decoded));
	decoded->format = NVIDIA_GHES_FORMAT_VERA;
	memcpy(decoded->signature, event->source_module_signature,
	       sizeof(event->source_module_signature));
	decoded->signature[sizeof(event->source_module_signature)] = '\0';
	decoded->event_context_count = event->event_context_count;
	decoded->source_device_type = event->source_device_type;
	decoded->event_type = get_unaligned_le16(&event->event_type);
	decoded->event_sub_type = get_unaligned_le16(&event->event_sub_type);
	decoded->event_link_id = get_unaligned_le64(&event->event_link_id);
	decoded->socket = cpu_info->socket_number;
	decoded->architecture = get_unaligned_le32(&cpu_info->architecture);
	memcpy(decoded->chip_serial_number, cpu_info->chip_serial_number,
	       sizeof(cpu_info->chip_serial_number));
	decoded->instance_base = get_unaligned_le64(&cpu_info->instance_base);
	decoded->vera_contexts = bytes + offset;
	decoded->vera_contexts_len = len - offset;
	__nvidia_ghes_vera_cursor_init(decoded, &cursor,
				       event->event_context_count);
	while ((ret = nvidia_ghes_vera_cursor_next(&cursor, &context)) > 0)
		decoded->valid_context_count++;
	if (ret < 0) {
		nvidia_ghes_decode_err(dev, fatal,
				       "Vera context[%u] is malformed (ret=%d)\n",
				       decoded->valid_context_count, ret);
		return ret;
	}

	if (cursor.remaining && dev)
		dev_dbg(dev,
			"Vera section has %zu trailing byte(s) after %u context(s)\n",
			cursor.remaining, decoded->valid_context_count);

	return 0;
}

VISIBLE_IF_KUNIT
__maybe_unused int nvidia_ghes_decode_vera(struct device *dev, const void *buf,
					   size_t len,
					   struct nvidia_ghes_decoded *decoded)
{
	return __nvidia_ghes_decode_vera(dev, buf, len, decoded, false);
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_decode_vera);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_entry_count(const struct nvidia_ghes_vera_context *ctx)
{
	if (!ctx)
		return -EINVAL;
	if (ctx->data_size && !ctx->data)
		return -EINVAL;
	if (ctx->data_size > INT_MAX)
		return -EOVERFLOW;

	switch (ctx->data_format_type) {
	case 0:
		return 0;
	case 1:
		return ctx->data_size / 16;
	case 2:
		return ctx->data_size / 8;
	case 3:
		return ctx->data_size / 8;
	case 4:
		return ctx->data_size / 4;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_context_entry_count);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_u64_pair(const struct nvidia_ghes_vera_context *ctx,
				      unsigned int index, u64 *key, u64 *val)
{
	int count;

	if (!ctx || !key || !val || ctx->data_format_type != 1)
		return -EINVAL;

	count = nvidia_ghes_vera_context_entry_count(ctx);
	if (count < 0)
		return count;
	if (index >= count)
		return -ERANGE;

	*key = get_unaligned_le64(ctx->data + index * 16);
	*val = get_unaligned_le64(ctx->data + index * 16 + 8);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_context_u64_pair);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_u32_pair(const struct nvidia_ghes_vera_context *ctx,
				      unsigned int index, u32 *key, u32 *val)
{
	int count;

	if (!ctx || !key || !val || ctx->data_format_type != 2)
		return -EINVAL;

	count = nvidia_ghes_vera_context_entry_count(ctx);
	if (count < 0)
		return count;
	if (index >= count)
		return -ERANGE;

	*key = get_unaligned_le32(ctx->data + index * 8);
	*val = get_unaligned_le32(ctx->data + index * 8 + 4);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_context_u32_pair);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_u64_value(const struct nvidia_ghes_vera_context *ctx,
				       unsigned int index, u64 *val)
{
	int count;

	if (!ctx || !val || ctx->data_format_type != 3)
		return -EINVAL;

	count = nvidia_ghes_vera_context_entry_count(ctx);
	if (count < 0)
		return count;
	if (index >= count)
		return -ERANGE;

	*val = get_unaligned_le64(ctx->data + index * 8);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_context_u64_value);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_u32_value(const struct nvidia_ghes_vera_context *ctx,
				       unsigned int index, u32 *val)
{
	int count;

	if (!ctx || !val || ctx->data_format_type != 4)
		return -EINVAL;

	count = nvidia_ghes_vera_context_entry_count(ctx);
	if (count < 0)
		return count;
	if (index >= count)
		return -ERANGE;

	*val = get_unaligned_le32(ctx->data + index * 4);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_context_u32_value);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_print_count(const struct nvidia_ghes_vera_context *ctx,
					 unsigned int *remaining)
{
	int entries;
	int count;

	if (!remaining)
		return -EINVAL;

	entries = nvidia_ghes_vera_context_entry_count(ctx);
	if (entries < 0)
		return entries;

	count = min_t(unsigned int, entries, *remaining);
	*remaining -= count;

	return count;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_context_print_count);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_print_context_count(const struct nvidia_ghes_decoded *decoded)
{
	if (!decoded || decoded->format != NVIDIA_GHES_FORMAT_VERA)
		return -EINVAL;

	return min_t(unsigned int, decoded->valid_context_count,
		     NVIDIA_GHES_MAX_PRINT_CONTEXTS);
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_vera_print_context_count);

static void nvidia_ghes_print_grace(struct device *dev,
				    const struct nvidia_ghes_decoded *decoded,
				    bool fatal)
{
	const char *level = fatal ? KERN_ERR : KERN_INFO;
	u64 addr, val;

	dev_printk(level, dev, "signature: %*pE\n",
		   (int)strnlen(decoded->signature, sizeof(decoded->signature)),
		   decoded->signature);
	dev_printk(level, dev, "error_type: %u\n", decoded->error_type);
	dev_printk(level, dev, "error_instance: %u\n", decoded->error_instance);
	dev_printk(level, dev, "severity: %u\n", decoded->severity);
	dev_printk(level, dev, "socket: %u\n", decoded->socket);
	dev_printk(level, dev, "number_regs: %u\n", decoded->number_regs);
	dev_printk(level, dev, "instance_base: 0x%016llx\n",
		   decoded->instance_base);

	for (int i = 0; i < decoded->number_regs; i++) {
		if (nvidia_ghes_grace_reg_pair(decoded, i, &addr, &val))
			break;
		dev_printk(level, dev, "register[%d]: address=0x%016llx value=0x%016llx\n",
			   i, addr, val);
	}
}

static void nvidia_ghes_print_vera(struct device *dev,
				   const struct nvidia_ghes_decoded *decoded,
				   bool fatal, unsigned long ghes_severity)
{
	struct nvidia_ghes_vera_cursor cursor;
	struct nvidia_ghes_vera_context context;
	const char *level = fatal ? KERN_ERR : KERN_INFO;
	unsigned int remaining = NVIDIA_GHES_MAX_PRINT_ENTRIES;
	unsigned int print_contexts;

	dev_printk(level, dev, "signature: %*pE\n",
		   (int)strnlen(decoded->signature, sizeof(decoded->signature)),
		   decoded->signature);
	dev_printk(level, dev, "event_type: %u\n", decoded->event_type);
	dev_printk(level, dev, "event_sub_type: %u\n", decoded->event_sub_type);
	dev_printk(level, dev, "ghes_severity: %lu\n", ghes_severity);
	dev_printk(level, dev, "event_link_id: 0x%016llx\n",
		   decoded->event_link_id);
	dev_printk(level, dev, "socket: %u\n", decoded->socket);
	dev_printk(level, dev, "architecture: 0x%x\n", decoded->architecture);
	dev_printk(level, dev, "chip_serial_number: %*phN\n",
		   (int)sizeof(decoded->chip_serial_number),
		   decoded->chip_serial_number);
	dev_printk(level, dev, "instance_base: 0x%016llx\n", decoded->instance_base);
	dev_printk(level, dev, "event_context_count: %u\n", decoded->event_context_count);
	if (decoded->valid_context_count != decoded->event_context_count)
		dev_printk(level, dev, "valid_context_count: %u\n",
			   decoded->valid_context_count);

	print_contexts = nvidia_ghes_vera_print_context_count(decoded);
	nvidia_ghes_vera_cursor_init(decoded, &cursor);
	for (int i = 0; i < print_contexts; i++) {
		const struct nvidia_ghes_vera_context *ctx = &context;
		int entries;
		int print_n;

		if (nvidia_ghes_vera_cursor_next(&cursor, &context) != 1)
			break;
		entries = nvidia_ghes_vera_context_entry_count(ctx);
		dev_printk(level, dev,
			   "context[%d]: version=%u format=%u format_version=%u context_size=%u data_size=%u\n",
			   i, ctx->context_version, ctx->data_format_type,
			   ctx->data_format_version, ctx->context_size, ctx->data_size);
		if (ctx->data_format_type == 0 && ctx->data_size > 0) {
			int prefix_len = ctx->data_size > 16 ? 16 : ctx->data_size;

			dev_printk(level, dev, "context[%d]_opaque_prefix: %*phN\n",
				   i, prefix_len, ctx->data);
			continue;
		}
		if (entries < 0)
			continue;

		dev_printk(level, dev, "context[%d]_entries: %d\n", i, entries);
		print_n = nvidia_ghes_vera_context_print_count(ctx, &remaining);
		if (print_n < 0)
			continue;
		for (int j = 0; j < print_n; j++) {
			u64 key64, val64;
			u32 key32, val32;

			switch (ctx->data_format_type) {
			case 1:
				if (nvidia_ghes_vera_context_u64_pair(ctx, j, &key64, &val64))
					break;
				dev_printk(level, dev,
					   "context[%d]_entry[%d]: key=0x%016llx value=0x%016llx\n",
					   i, j, key64, val64);
				break;
			case 2:
				if (nvidia_ghes_vera_context_u32_pair(ctx, j, &key32, &val32))
					break;
				dev_printk(level, dev,
					   "context[%d]_entry[%d]: key=0x%08x value=0x%08x\n",
					   i, j, key32, val32);
				break;
			case 3:
				if (nvidia_ghes_vera_context_u64_value(ctx, j, &val64))
					break;
				dev_printk(level, dev,
					   "context[%d]_entry[%d]: value=0x%016llx\n",
					   i, j, val64);
				break;
			case 4:
				if (nvidia_ghes_vera_context_u32_value(ctx, j, &val32))
					break;
				dev_printk(level, dev,
					   "context[%d]_entry[%d]: value=0x%08x\n",
					   i, j, val32);
				break;
			default:
				break;
			}
		}
		if (entries > print_n)
			dev_printk(level, dev,
				   "context[%d]_entries_omitted: %d\n",
				   i, entries - print_n);
	}
	if (decoded->valid_context_count > print_contexts)
		dev_printk(level, dev, "contexts_omitted: %u\n",
			   decoded->valid_context_count - print_contexts);
}

static void nvidia_ghes_log_decode_failure(struct device *dev,
					   enum nvidia_ghes_format format,
					   bool fatal, u32 len, int ret)
{
	const char *level = fatal ? KERN_ERR : KERN_INFO;

	if (ret == -EOPNOTSUPP && format == NVIDIA_GHES_FORMAT_VERA) {
		if (fatal)
			dev_printk(level, dev,
				   "Unsupported NVIDIA Vera CPER section, error_data_length: %u, ret: %d\n",
				   len, ret);
		else
			dev_info_ratelimited(dev,
					     "Unsupported NVIDIA Vera CPER section, error_data_length: %u, ret: %d\n",
					     len, ret);
		return;
	}

	if (format == NVIDIA_GHES_FORMAT_GRACE) {
		if (fatal)
			dev_printk(level, dev,
				   "Malformed NVIDIA Grace CPER section, error_data_length: %u, ret: %d\n",
				   len, ret);
		else
			dev_err_ratelimited(dev,
					    "Malformed NVIDIA Grace CPER section, error_data_length: %u, ret: %d\n",
					    len, ret);
		return;
	}

	if (fatal)
		dev_printk(level, dev,
			   "Malformed NVIDIA Vera CPER section, error_data_length: %u, ret: %d\n",
			   len, ret);
	else
		dev_err_ratelimited(dev,
				    "Malformed NVIDIA Vera CPER section, error_data_length: %u, ret: %d\n",
				    len, ret);
}

static int nvidia_ghes_notify(struct notifier_block *nb,
			      unsigned long event, void *data)
{
	struct acpi_hest_generic_data *gdata = data;
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_private *priv;
	enum nvidia_ghes_format format;
	const void *payload;
	guid_t sec_guid;
	u32 len;
	int ret;
	bool fatal;

	import_guid(&sec_guid, gdata->section_type);
	format = nvidia_ghes_format_from_guid(&sec_guid);
	if (format == NVIDIA_GHES_FORMAT_UNKNOWN)
		return NOTIFY_DONE;

	priv = container_of(nb, struct nvidia_ghes_private, nb);
	len = acpi_hest_get_error_length(gdata);

	payload = acpi_hest_get_payload(gdata);
	fatal = event >= GHES_SEV_RECOVERABLE;

	switch (format) {
	case NVIDIA_GHES_FORMAT_GRACE:
		ret = __nvidia_ghes_decode_grace(priv->dev, payload, len,
						 &decoded, fatal);
		break;
	case NVIDIA_GHES_FORMAT_VERA:
		ret = __nvidia_ghes_decode_vera(priv->dev, payload, len,
						&decoded, fatal);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	if (ret) {
		nvidia_ghes_log_decode_failure(priv->dev, format, fatal, len, ret);
		/*
		 * Print any contexts successfully decoded before the failure.
		 * Raw section bytes remain available via log_non_standard_event().
		 */
		if (format == NVIDIA_GHES_FORMAT_VERA &&
		    decoded.format == NVIDIA_GHES_FORMAT_VERA &&
		    decoded.valid_context_count > 0) {
			dev_printk(fatal ? KERN_ERR : KERN_INFO, priv->dev,
				   "NVIDIA Vera CPER section (partial), error_data_length: %u\n",
				   len);
			nvidia_ghes_print_vera(priv->dev, &decoded, fatal, event);
		}
		return NOTIFY_OK;
	}

	if (format == NVIDIA_GHES_FORMAT_GRACE)
		dev_printk(fatal ? KERN_ERR : KERN_INFO, priv->dev,
			   "NVIDIA Grace CPER section, error_data_length: %u\n", len);
	else
		dev_printk(fatal ? KERN_ERR : KERN_INFO, priv->dev,
			   "NVIDIA Vera CPER section, error_data_length: %u\n", len);

	if (format == NVIDIA_GHES_FORMAT_VERA)
		nvidia_ghes_print_vera(priv->dev, &decoded, fatal, event);
	else
		nvidia_ghes_print_grace(priv->dev, &decoded, fatal);

	return NOTIFY_OK;
}

static int nvidia_ghes_probe(struct platform_device *pdev)
{
	struct nvidia_ghes_private *priv;
	int ret;

	priv = devm_kmalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	*priv = (struct nvidia_ghes_private) {
		.nb.notifier_call = nvidia_ghes_notify,
		.dev = &pdev->dev,
	};

	ret = devm_ghes_register_vendor_record_notifier(&pdev->dev, &priv->nb);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register NVIDIA GHES vendor record notifier\n");

	return 0;
}

static const struct acpi_device_id nvidia_ghes_acpi_match[] = {
	{ "NVDA2012" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, nvidia_ghes_acpi_match);

static struct platform_driver nvidia_ghes_driver = {
	.driver = {
		.name = "nvidia-ghes",
		.acpi_match_table = nvidia_ghes_acpi_match,
	},
	.probe = nvidia_ghes_probe,
};
module_platform_driver(nvidia_ghes_driver);

MODULE_AUTHOR("Kai-Heng Feng <kaihengf@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA GHES vendor CPER record handler");
MODULE_LICENSE("GPL");
