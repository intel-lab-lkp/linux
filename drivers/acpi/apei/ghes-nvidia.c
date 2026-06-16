// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVIDIA GHES vendor record handler
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>
#include <kunit/visibility.h>
#include <acpi/ghes.h>

#include "ghes-nvidia.h"

#define NVIDIA_GHES_VERA_VERSION	1

static const guid_t nvidia_grace_sec_guid =
	GUID_INIT(0x6d5244f2, 0x2712, 0x11ec,
		  0xbe, 0xa7, 0xcb, 0x3f, 0xdb, 0x95, 0xc7, 0x86);

static const guid_t nvidia_vera_sec_guid =
	GUID_INIT(0x9068e568, 0x6ca0, 0x11f0,
		  0xae, 0xaf, 0x15, 0x93, 0x43, 0x59, 0x1e, 0xac);

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
};

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

struct cper_sec_nvidia_vera_cpu_info {
	__le16	info_version;
	u8	info_size;
	u8	socket_number;
	__le32	architecture;
	u8	chip_serial_number[16];
	__le64	instance_base;
} __packed;

struct cper_sec_nvidia_vera_context {
	__le32	context_size;
	__le16	context_version;
	__le16	reserved;
	__le16	data_format_type;
	__le16	data_format_version;
	__le32	data_size;
} __packed;

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

VISIBLE_IF_KUNIT
int nvidia_ghes_decode_grace(struct device *dev, const void *buf,
			     size_t len,
			     struct nvidia_ghes_decoded *decoded)
{
	const struct cper_sec_nvidia *nvidia_err = buf;
	size_t min_size;

	if (!buf || !decoded)
		return -EINVAL;
	if (len < sizeof(*nvidia_err)) {
		if (dev)
			dev_err(dev, "Section too small (%zu < %zu)\n",
				len, sizeof(*nvidia_err));
		return -ENODATA;
	}

	min_size = struct_size(nvidia_err, regs, nvidia_err->number_regs);
	if (len < min_size) {
		if (dev)
			dev_err(dev,
				"Invalid number_regs %u (section size %zu, need %zu)\n",
				nvidia_err->number_regs, len, min_size);
		return -ENODATA;
	}

	memset(decoded, 0, sizeof(*decoded));
	decoded->format = NVIDIA_GHES_FORMAT_GRACE;
	memcpy(decoded->signature, nvidia_err->signature, sizeof(nvidia_err->signature));
	decoded->signature[sizeof(nvidia_err->signature)] = '\0';
	decoded->error_type = le16_to_cpu(nvidia_err->error_type);
	decoded->error_instance = le16_to_cpu(nvidia_err->error_instance);
	decoded->severity = nvidia_err->severity;
	decoded->socket = nvidia_err->socket;
	decoded->number_regs = nvidia_err->number_regs;
	decoded->instance_base = le64_to_cpu(nvidia_err->instance_base);
	if (nvidia_err->number_regs)
		decoded->grace_regs = nvidia_err->regs;

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_decode_grace);

VISIBLE_IF_KUNIT
int nvidia_ghes_grace_reg_pair(const struct nvidia_ghes_decoded *decoded,
			       unsigned int index, u64 *addr, u64 *val)
{
	const struct nvidia_ghes_grace_reg *regs;

	if (!decoded || decoded->format != NVIDIA_GHES_FORMAT_GRACE || !addr || !val)
		return -EINVAL;
	if (index >= decoded->number_regs)
		return -ERANGE;

	regs = decoded->grace_regs;
	*addr = le64_to_cpu(regs[index].addr);
	*val = le64_to_cpu(regs[index].val);

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

VISIBLE_IF_KUNIT
int nvidia_ghes_decode_vera(struct device *dev, const void *buf,
			    size_t len,
			    struct nvidia_ghes_decoded *decoded)
{
	const struct cper_sec_nvidia_vera_event *event = buf;
	const struct cper_sec_nvidia_vera_cpu_info *cpu_info;
	const struct cper_sec_nvidia_vera_context *context;
	const u8 *bytes = buf;
	size_t data_end_advance;
	size_t advance;
	size_t offset;
	int ret;

	if (!buf || !decoded)
		return -EINVAL;
	if (len < sizeof(*event)) {
		if (dev)
			dev_err_ratelimited(dev, "Vera event header truncated (%zu < %zu)\n",
					    len, sizeof(*event));
		return -ENODATA;
	}
	if (event->version != NVIDIA_GHES_VERA_VERSION)
		return -EOPNOTSUPP;
	if (event->source_device_type != 0)
		return -EOPNOTSUPP;

	offset = sizeof(*event);
	if (len - offset < sizeof(*cpu_info)) {
		if (dev)
			dev_err_ratelimited(dev, "Vera CPU info truncated (%zu < %zu)\n",
					    len - offset, sizeof(*cpu_info));
		return -ENODATA;
	}

	cpu_info = (const void *)(bytes + offset);
	if (cpu_info->info_size < sizeof(*cpu_info)) {
		if (dev)
			dev_err_ratelimited(dev, "Vera CPU info size %u smaller than header %zu\n",
					    cpu_info->info_size, sizeof(*cpu_info));
		return -EINVAL;
	}
	if (len - offset < cpu_info->info_size) {
		if (dev)
			dev_err_ratelimited(dev, "Vera CPU info extends past section (%u > %zu)\n",
					    cpu_info->info_size, len - offset);
		return -ENODATA;
	}

	offset += cpu_info->info_size;
	if (event->event_context_count > NVIDIA_GHES_MAX_CONTEXTS) {
		if (dev)
			dev_err_ratelimited(dev, "Vera context count %u exceeds maximum %u\n",
					    event->event_context_count,
					    NVIDIA_GHES_MAX_CONTEXTS);
		return -E2BIG;
	}

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

	for (int i = 0; i < event->event_context_count; i++) {
		struct nvidia_ghes_vera_context *decoded_context = &decoded->contexts[i];
		u32 context_size;
		u32 data_size;
		u16 data_format_type;

		if (len - offset < sizeof(*context)) {
			if (dev)
				dev_err_ratelimited(dev, "Vera context[%d] header truncated (%zu < %zu)\n",
						    i, len - offset, sizeof(*context));
			return -ENODATA;
		}

		context = (const void *)(bytes + offset);
		context_size = get_unaligned_le32(&context->context_size);
		data_format_type = get_unaligned_le16(&context->data_format_type);
		data_size = get_unaligned_le32(&context->data_size);

		if (context_size < sizeof(*context)) {
			if (dev)
				dev_err_ratelimited(dev,
						    "Vera context[%d] size %u smaller than header %zu\n",
						    i, context_size, sizeof(*context));
			return -EINVAL;
		}
		if (data_format_type > 4) {
			if (dev)
				dev_dbg(dev,
					"Vera context[%d] unsupported data format %u\n",
					i, data_format_type);
			return -EOPNOTSUPP;
		}
		if (check_add_overflow((size_t)data_size, sizeof(*context),
				       &data_end_advance)) {
			if (dev)
				dev_err_ratelimited(dev,
						    "Vera context[%d] data_size %u overflows section accounting\n",
						    i, data_size);
			return -EOVERFLOW;
		}

		if (data_end_advance > len - offset) {
			if (dev)
				dev_err_ratelimited(dev,
						    "Vera context[%d] data extends past section (%zu > %zu)\n",
						    i, data_end_advance, len - offset);
			return -ENODATA;
		}

		/*
		 * Some Vera payloads use only the header size here and
		 * place the format-specific payload immediately after it.
		 */
		if (context_size == sizeof(*context))
			advance = data_end_advance;
		else if (data_size <= context_size - sizeof(*context))
			advance = context_size;
		else {
			if (dev)
				dev_err_ratelimited(dev,
						    "Vera context[%d] data_size %u exceeds context_size %u\n",
						    i, data_size, context_size);
			return -EINVAL;
		}

		if (advance > len - offset) {
			if (dev)
				dev_err_ratelimited(dev,
						    "Vera context[%d] advance %zu extends past section (%zu)\n",
						    i, advance, len - offset);
			return -ENODATA;
		}

		ret = nvidia_ghes_vera_validate_context_data(data_format_type, data_size);
		if (ret) {
			if (dev)
				dev_err_ratelimited(dev,
						    "Vera context[%d] format %u rejected data_size %u (ret=%d)\n",
						    i, data_format_type, data_size, ret);
			return ret;
		}

		decoded_context->context_size = context_size;
		decoded_context->context_version =
			get_unaligned_le16(&context->context_version);
		decoded_context->data_format_type = data_format_type;
		decoded_context->data_format_version =
			get_unaligned_le16(&context->data_format_version);
		decoded_context->data_size = data_size;
		decoded_context->data = bytes + offset + sizeof(*context);
		offset += advance;
	}

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(nvidia_ghes_decode_vera);

VISIBLE_IF_KUNIT
int nvidia_ghes_vera_context_entry_count(const struct nvidia_ghes_vera_context *ctx)
{
	if (!ctx)
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

static void nvidia_ghes_print_grace(struct device *dev,
				    const struct nvidia_ghes_decoded *decoded,
				    bool fatal)
{
	const char *level = fatal ? KERN_ERR : KERN_INFO;
	u64 addr, val;

	dev_printk(level, dev, "signature: %s\n", decoded->signature);
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
	const char *level = fatal ? KERN_ERR : KERN_INFO;

	dev_printk(level, dev, "signature: %s\n", decoded->signature);
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

	for (int i = 0; i < decoded->event_context_count; i++) {
		const struct nvidia_ghes_vera_context *ctx = &decoded->contexts[i];
		int entries = nvidia_ghes_vera_context_entry_count(ctx);

		dev_printk(level, dev,
			   "context[%d]: version=%u format=%u format_version=%u context_size=%u data_size=%u\n",
			   i, ctx->context_version, ctx->data_format_type,
			   ctx->data_format_version, ctx->context_size, ctx->data_size);
		if (ctx->data_format_type == 0 && ctx->data_size > 0) {
			int prefix_len = ctx->data_size > 16 ? 16 : ctx->data_size;

			dev_printk(level, dev, "context[%d]_opaque_prefix: %*phN\n",
				   i, prefix_len, ctx->data);
		} else if (entries >= 0) {
			dev_printk(level, dev, "context[%d]_entries: %d\n", i, entries);
		}
	}
}

static int nvidia_ghes_notify(struct notifier_block *nb,
			      unsigned long event, void *data)
{
	struct acpi_hest_generic_data *gdata = data;
	struct nvidia_ghes_decoded *decoded;
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
	decoded = kzalloc_obj(*decoded);
	if (!decoded) {
		dev_err_ratelimited(priv->dev,
				    "Failed to allocate NVIDIA CPER decode buffer\n");
		return NOTIFY_OK;
	}

	switch (format) {
	case NVIDIA_GHES_FORMAT_GRACE:
		ret = nvidia_ghes_decode_grace(priv->dev, payload, len, decoded);
		break;
	case NVIDIA_GHES_FORMAT_VERA:
		ret = nvidia_ghes_decode_vera(priv->dev, payload, len, decoded);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	if (ret) {
		if (ret == -EOPNOTSUPP && format == NVIDIA_GHES_FORMAT_VERA)
			dev_info(priv->dev,
				 "Unsupported NVIDIA Vera CPER section, error_data_length: %u, ret: %d\n",
				 len, ret);
		else if (format == NVIDIA_GHES_FORMAT_GRACE)
			dev_err(priv->dev,
				"Malformed NVIDIA Grace CPER section, error_data_length: %u, ret: %d\n",
				len, ret);
		else
			dev_err(priv->dev,
				"Malformed NVIDIA Vera CPER section, error_data_length: %u, ret: %d\n",
				len, ret);
		goto out;
	}

	if (format == NVIDIA_GHES_FORMAT_GRACE)
		dev_printk(fatal ? KERN_ERR : KERN_INFO, priv->dev,
			   "NVIDIA Grace CPER section, error_data_length: %u\n", len);
	else
		dev_printk(fatal ? KERN_ERR : KERN_INFO, priv->dev,
			   "NVIDIA Vera CPER section, error_data_length: %u\n", len);

	if (format == NVIDIA_GHES_FORMAT_VERA)
		nvidia_ghes_print_vera(priv->dev, decoded, fatal, event);
	else
		nvidia_ghes_print_grace(priv->dev, decoded, fatal);

out:
	kfree(decoded);
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
