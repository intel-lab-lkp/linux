// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/align.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>

#include "ghes-nvidia.h"
#include "ghes-nvidia-test-fixtures.h"

static const u8 grace_valid_two_regs[] = {
	'N', 'V', 'D', 'A', '-', 'G', 'R', 'A',
	'C', 'E', 0, 0, 0, 0, 0, 0,
	0x34, 0x12, 0x78, 0x56, 0x02, 0x03, 0x02, 0x00,
	0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x22, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const u8 grace_valid_zero_regs[] = {
	'N', 'V', 'D', 'A', '-', 'G', 'R', 'A',
	'C', 'E', 0, 0, 0, 0, 0, 0,
	0x01, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00,
	0xef, 0xbe, 0xad, 0xde, 0x00, 0x00, 0x00, 0x00,
};

static const u8 grace_valid_full_signature[] = {
	'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
	'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
	0x01, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00,
	0xef, 0xbe, 0xad, 0xde, 0x00, 0x00, 0x00, 0x00,
};

static void nvidia_ghes_grace_decodes_registers(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u64 addr, val;

	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_grace(NULL, grace_valid_two_regs,
						 sizeof(grace_valid_two_regs),
						 &decoded));
	KUNIT_EXPECT_EQ(test, NVIDIA_GHES_FORMAT_GRACE, decoded.format);
	KUNIT_EXPECT_STREQ(test, "NVDA-GRACE", decoded.signature);
	KUNIT_EXPECT_EQ(test, 0x1234, decoded.error_type);
	KUNIT_EXPECT_EQ(test, 0x5678, decoded.error_instance);
	KUNIT_EXPECT_EQ(test, 2, decoded.severity);
	KUNIT_EXPECT_EQ(test, 3, decoded.socket);
	KUNIT_EXPECT_EQ(test, 2, decoded.number_regs);
	KUNIT_EXPECT_EQ(test, 0x0102030405060708ULL, decoded.instance_base);
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_grace_reg_pair(&decoded, 0, &addr, &val));
	KUNIT_EXPECT_EQ(test, 0x1000ULL, addr);
	KUNIT_EXPECT_EQ(test, 0x1111ULL, val);
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_grace_reg_pair(&decoded, 1, &addr, &val));
	KUNIT_EXPECT_EQ(test, 0x2000ULL, addr);
	KUNIT_EXPECT_EQ(test, 0x2222ULL, val);
}

static void nvidia_ghes_grace_decodes_unaligned_payload(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;
	u64 addr, val;

	buf = kunit_kzalloc(test, sizeof(grace_valid_two_regs) + 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	memcpy(buf + 1, grace_valid_two_regs, sizeof(grace_valid_two_regs));

	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_grace(NULL, buf + 1,
						 sizeof(grace_valid_two_regs),
						 &decoded));
	KUNIT_EXPECT_EQ(test, 0x1234, decoded.error_type);
	KUNIT_EXPECT_EQ(test, 0x0102030405060708ULL, decoded.instance_base);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_grace_reg_pair(&decoded, 0, &addr, &val));
	KUNIT_EXPECT_EQ(test, 0x1000ULL, addr);
	KUNIT_EXPECT_EQ(test, 0x1111ULL, val);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_grace_reg_pair(&decoded, 1, &addr, &val));
	KUNIT_EXPECT_EQ(test, 0x2000ULL, addr);
	KUNIT_EXPECT_EQ(test, 0x2222ULL, val);
}

static void nvidia_ghes_grace_accepts_zero_registers(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_grace(NULL, grace_valid_zero_regs,
						 sizeof(grace_valid_zero_regs),
						 &decoded));
	KUNIT_EXPECT_EQ(test, NVIDIA_GHES_FORMAT_GRACE, decoded.format);
	KUNIT_EXPECT_EQ(test, 0, decoded.number_regs);
	KUNIT_EXPECT_PTR_EQ(test, NULL, decoded.grace_regs);
}

static void nvidia_ghes_grace_copies_non_nul_signature(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_grace(NULL, grace_valid_full_signature,
						 sizeof(grace_valid_full_signature),
						 &decoded));
	KUNIT_EXPECT_STREQ(test, "ABCDEFGHIJKLMNOP", decoded.signature);
	KUNIT_EXPECT_EQ(test, '\0', decoded.signature[16]);
}

static void nvidia_ghes_grace_rejects_truncated_header(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_EXPECT_EQ(test, -ENODATA,
			nvidia_ghes_decode_grace(NULL, grace_valid_two_regs,
						 sizeof(grace_valid_zero_regs) - 1,
						 &decoded));
}

static void nvidia_ghes_grace_rejects_truncated_registers(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_EXPECT_EQ(test, -ENODATA,
			nvidia_ghes_decode_grace(NULL, grace_valid_two_regs,
						 sizeof(grace_valid_two_regs) - 1,
						 &decoded));
}

static const u8 vera_chip_serial[] = {
	0xc0, 0x01, 0xfe, 0x06, 0x00, 0x00, 0x00, 0x00,
	0x81, 0xc1, 0x18, 0x78, 0x01, 0x00, 0x00, 0x00,
};

#define VERA_EVENT_HDR_SIZE	32
#define VERA_CPU_INFO_SIZE	32
#define VERA_CONTEXT_HDR_SIZE	16
#define VERA_FIRST_CONTEXT_OFFSET	(VERA_EVENT_HDR_SIZE + VERA_CPU_INFO_SIZE)

struct vera_synth_context {
	u32 context_size;
	u16 context_version;
	u16 format;
	u16 data_format_version;
	const u8 *data;
	u32 data_size;
};

static u8 *nvidia_ghes_kunit_memdup(struct kunit *test, const u8 *src, size_t len)
{
	u8 *buf;

	buf = kunit_kmalloc(test, len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	memcpy(buf, src, len);

	return buf;
}

static u8 *vera_make_synthetic_contexts(struct kunit *test, u8 version, u8 info_size,
					const struct vera_synth_context *contexts,
					u8 count, size_t *len)
{
	u8 *buf;
	size_t offset;
	u8 i;

	KUNIT_ASSERT_TRUE(test, info_size >= VERA_CPU_INFO_SIZE);
	*len = VERA_EVENT_HDR_SIZE + info_size;
	for (i = 0; i < count; i++) {
		u32 context_size = contexts[i].context_size;
		u32 payload;

		KUNIT_ASSERT_TRUE(test, context_size >= VERA_CONTEXT_HDR_SIZE);
		if (context_size == VERA_CONTEXT_HDR_SIZE)
			payload = contexts[i].data_size;
		else
			payload = context_size - VERA_CONTEXT_HDR_SIZE;
		*len += VERA_CONTEXT_HDR_SIZE + payload;
	}

	buf = kunit_kzalloc(test, *len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	buf[0] = version;
	buf[1] = count;
	buf[2] = 0;
	put_unaligned_le16(0x22, &buf[4]);
	memcpy(&buf[16], "SYNTH", 5);
	put_unaligned_le16(0, &buf[32]);
	buf[34] = info_size;
	buf[35] = 0;
	put_unaligned_le32(0x14107, &buf[36]);
	memcpy(&buf[40], vera_chip_serial, sizeof(vera_chip_serial));
	put_unaligned_le64(0, &buf[56]);

	offset = VERA_EVENT_HDR_SIZE + info_size;
	for (i = 0; i < count; i++) {
		u32 context_size = contexts[i].context_size;
		u32 payload;

		put_unaligned_le32(context_size, &buf[offset]);
		put_unaligned_le16(contexts[i].context_version,
				   &buf[offset + 4]);
		put_unaligned_le16(contexts[i].format, &buf[offset + 8]);
		put_unaligned_le16(contexts[i].data_format_version,
				   &buf[offset + 10]);
		put_unaligned_le32(contexts[i].data_size, &buf[offset + 12]);
		if (contexts[i].data && contexts[i].data_size)
			memcpy(&buf[offset + VERA_CONTEXT_HDR_SIZE],
			       contexts[i].data, contexts[i].data_size);

		if (context_size == VERA_CONTEXT_HDR_SIZE)
			payload = contexts[i].data_size;
		else
			payload = context_size - VERA_CONTEXT_HDR_SIZE;
		offset += VERA_CONTEXT_HDR_SIZE + payload;
	}

	return buf;
}

static u8 *vera_make_synthetic(struct kunit *test, u8 version, u8 info_size,
			       u16 format, const u8 *data, u32 data_size,
			       size_t *len)
{
	struct vera_synth_context ctx = {
		.context_size = format == 0 ? VERA_CONTEXT_HDR_SIZE :
			ALIGN(VERA_CONTEXT_HDR_SIZE + data_size, 16),
		.format = format,
		.data = data,
		.data_size = data_size,
	};

	return vera_make_synthetic_contexts(test, version, info_size, &ctx, 1, len);
}

static u8 *vera_make_synthetic_default(struct kunit *test, u16 format,
				       const u8 *data, u32 data_size, size_t *len)
{
	return vera_make_synthetic(test, 1, VERA_CPU_INFO_SIZE, format, data,
				   data_size, len);
}

static int vera_context_at(const struct nvidia_ghes_decoded *decoded,
			   unsigned int index,
			   struct nvidia_ghes_vera_context *context)
{
	struct nvidia_ghes_vera_cursor cursor;
	int ret;

	nvidia_ghes_vera_cursor_init(decoded, &cursor);
	for (unsigned int i = 0; i <= index; i++) {
		ret = nvidia_ghes_vera_cursor_next(&cursor, context);
		if (ret != 1)
			return ret ?: -ERANGE;
	}

	return 0;
}

static void nvidia_ghes_vera_decodes_l1_reset(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context context;

	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, vera_l1_reset_id10,
						sizeof(vera_l1_reset_id10),
						&decoded));
	KUNIT_EXPECT_EQ(test, NVIDIA_GHES_FORMAT_VERA, decoded.format);
	KUNIT_EXPECT_STREQ(test, "L1 RESET", decoded.signature);
	KUNIT_EXPECT_EQ(test, 1, decoded.event_context_count);
	KUNIT_EXPECT_EQ(test, 1, decoded.event_type);
	KUNIT_EXPECT_EQ(test, 39, decoded.event_sub_type);
	KUNIT_EXPECT_EQ(test, 0, decoded.source_device_type);
	KUNIT_EXPECT_EQ(test, 0, decoded.socket);
	KUNIT_EXPECT_EQ(test, 0x14107U, decoded.architecture);
	KUNIT_EXPECT_MEMEQ(test, decoded.chip_serial_number, vera_chip_serial,
			   sizeof(vera_chip_serial));
	KUNIT_EXPECT_EQ(test, 0xfe1500200000ULL, decoded.instance_base);
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &context));
	KUNIT_EXPECT_EQ(test, 16U, context.context_size);
	KUNIT_EXPECT_EQ(test, 1, context.data_format_type);
	KUNIT_EXPECT_EQ(test, 0U, context.data_size);
}

static void nvidia_ghes_vera_decodes_crashdump_id(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context context;
	u64 key, val;

	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, vera_crashdump_id_id40,
						sizeof(vera_crashdump_id_id40),
						&decoded));
	KUNIT_EXPECT_STREQ(test, "CRASHDUMP-ID", decoded.signature);
	KUNIT_EXPECT_MEMEQ(test, decoded.chip_serial_number, vera_chip_serial,
			   sizeof(vera_chip_serial));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &context));
	KUNIT_EXPECT_EQ(test, 240U, context.context_size);
	KUNIT_EXPECT_EQ(test, 1, context.data_format_type);
	KUNIT_EXPECT_EQ(test, 224U, context.data_size);
	KUNIT_ASSERT_EQ(test, 14, nvidia_ghes_vera_context_entry_count(&context));
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u64_pair(&context, 0, &key, &val));
	KUNIT_EXPECT_EQ(test, 0x8300000000000000ULL, key);
	KUNIT_EXPECT_EQ(test, 0x372e33375f6c6572ULL, val);
}

static void nvidia_ghes_vera_decodes_crashdump_s1_opaque_context(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context context;
	u64 val;

	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, vera_crashdump_s1_id41,
						sizeof(vera_crashdump_s1_id41),
						&decoded));
	KUNIT_EXPECT_STREQ(test, "CRASHDUMP-S1", decoded.signature);
	KUNIT_EXPECT_EQ(test, 4096, decoded.event_type);
	KUNIT_EXPECT_EQ(test, 2304, decoded.event_sub_type);
	KUNIT_EXPECT_MEMEQ(test, decoded.chip_serial_number, vera_chip_serial,
			   sizeof(vera_chip_serial));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &context));
	KUNIT_EXPECT_EQ(test, 16U, context.context_size);
	KUNIT_EXPECT_EQ(test, 0, context.data_format_type);
	KUNIT_EXPECT_EQ(test, 1928U, context.data_size);
	KUNIT_ASSERT_NOT_NULL(test, context.data);
	KUNIT_EXPECT_EQ(test, 0x53, context.data[0]);
	KUNIT_EXPECT_EQ(test, 0x56, context.data[1]);
	KUNIT_EXPECT_EQ(test, 0x7f, context.data[2]);
	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_vera_context_u64_value(&context, 0, &val));
}

static void nvidia_ghes_vera_decodes_multi_context_aligned(struct kunit *test)
{
	static const u8 first_data[] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	};
	static const u8 second_data[] = {
		0xa0, 0xb0, 0xc0, 0xd0,
	};
	struct vera_synth_context contexts[] = {
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.format = 3,
			.data = first_data,
			.data_size = sizeof(first_data),
		},
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.format = 4,
			.data = second_data,
			.data_size = sizeof(second_data),
		},
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context parsed[2];
	size_t len;
	u8 *buf;
	u64 val64;
	u32 val32;

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, contexts,
					   ARRAY_SIZE(contexts), &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 2, decoded.event_context_count);
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &parsed[0]));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 1, &parsed[1]));
	KUNIT_EXPECT_EQ(test, VERA_CONTEXT_HDR_SIZE + 16,
			parsed[0].context_size);
	KUNIT_EXPECT_EQ(test, 3, parsed[0].data_format_type);
	KUNIT_EXPECT_EQ(test, sizeof(first_data), parsed[0].data_size);
	KUNIT_ASSERT_NOT_NULL(test, parsed[0].data);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u64_value(&parsed[0], 0, &val64));
	KUNIT_EXPECT_EQ(test, 0x8877665544332211ULL, val64);
	KUNIT_EXPECT_EQ(test, VERA_CONTEXT_HDR_SIZE + 16,
			parsed[1].context_size);
	KUNIT_EXPECT_EQ(test, 4, parsed[1].data_format_type);
	KUNIT_EXPECT_EQ(test, sizeof(second_data), parsed[1].data_size);
	KUNIT_ASSERT_NOT_NULL(test, parsed[1].data);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_value(&parsed[1], 0, &val32));
	KUNIT_EXPECT_EQ(test, 0xd0c0b0a0U, val32);
}

static void nvidia_ghes_vera_decodes_multi_context_covering_first(struct kunit *test)
{
	static const u8 first_data[] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
	};
	static const u8 second_data[] = {
		0xa0, 0xb0, 0xc0, 0xd0,
	};
	struct vera_synth_context contexts[] = {
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + sizeof(first_data),
			.format = 1,
			.data = first_data,
			.data_size = sizeof(first_data),
		},
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.format = 4,
			.data = second_data,
			.data_size = sizeof(second_data),
		},
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context parsed[2];
	size_t len;
	u8 *buf;
	u64 key, val;
	u32 val32;

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, contexts,
					   ARRAY_SIZE(contexts), &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 2, decoded.event_context_count);
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &parsed[0]));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 1, &parsed[1]));
	KUNIT_EXPECT_EQ(test, VERA_CONTEXT_HDR_SIZE + sizeof(first_data),
			parsed[0].context_size);
	KUNIT_EXPECT_EQ(test, 1, parsed[0].data_format_type);
	KUNIT_EXPECT_EQ(test, sizeof(first_data), parsed[0].data_size);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u64_pair(&parsed[0], 0, &key, &val));
	KUNIT_EXPECT_EQ(test, 0x8877665544332211ULL, key);
	KUNIT_EXPECT_EQ(test, 0x00ffeeddccbbaa99ULL, val);
	KUNIT_EXPECT_EQ(test, VERA_CONTEXT_HDR_SIZE + 16,
			parsed[1].context_size);
	KUNIT_EXPECT_EQ(test, 4, parsed[1].data_format_type);
	KUNIT_EXPECT_EQ(test, sizeof(second_data), parsed[1].data_size);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_value(&parsed[1], 0, &val32));
	KUNIT_EXPECT_EQ(test, 0xd0c0b0a0U, val32);
}

static void nvidia_ghes_vera_decodes_format2_u32_pairs(struct kunit *test)
{
	static const u8 data[] = {
		0x78, 0x56, 0x34, 0x12, 0xf0, 0xde, 0xbc, 0x9a,
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context context;
	size_t len;
	u8 *buf;
	u32 key, val;

	buf = vera_make_synthetic_default(test, 2, data, sizeof(data), &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &context));
	KUNIT_ASSERT_EQ(test, 1, nvidia_ghes_vera_context_entry_count(&context));
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_pair(&context, 0, &key, &val));
	KUNIT_EXPECT_EQ(test, 0x12345678U, key);
	KUNIT_EXPECT_EQ(test, 0x9abcdef0U, val);
}

static void nvidia_ghes_vera_decodes_format3_u64_values(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context context;
	size_t len;
	u8 *buf;
	u64 val;

	buf = vera_make_synthetic_default(test, 3, data, sizeof(data), &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &context));
	KUNIT_ASSERT_EQ(test, 1, nvidia_ghes_vera_context_entry_count(&context));
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u64_value(&context, 0, &val));
	KUNIT_EXPECT_EQ(test, 0x0102030405060708ULL, val);
}

static void nvidia_ghes_vera_decodes_format4_u32_values(struct kunit *test)
{
	static const u8 data[] = {
		0x78, 0x56, 0x34, 0x12,
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context context;
	size_t len;
	u8 *buf;
	u32 val;

	buf = vera_make_synthetic_default(test, 4, data, sizeof(data), &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &context));
	KUNIT_ASSERT_EQ(test, 1, nvidia_ghes_vera_context_entry_count(&context));
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_value(&context, 0, &val));
	KUNIT_EXPECT_EQ(test, 0x12345678U, val);
}

static void nvidia_ghes_vera_rejects_truncated_event_header(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_EXPECT_EQ(test, -ENODATA,
			nvidia_ghes_decode_vera(NULL, vera_l1_reset_id10,
						VERA_EVENT_HDR_SIZE - 1,
						&decoded));
}

static void nvidia_ghes_vera_rejects_truncated_cpu_info(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_EXPECT_EQ(test, -ENODATA,
			nvidia_ghes_decode_vera(NULL, vera_l1_reset_id10,
						VERA_FIRST_CONTEXT_OFFSET - 1,
						&decoded));
}

static void nvidia_ghes_vera_rejects_truncated_context_header(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_EXPECT_EQ(test, -ENODATA,
			nvidia_ghes_decode_vera(NULL, vera_l1_reset_id10,
						VERA_FIRST_CONTEXT_OFFSET +
						VERA_CONTEXT_HDR_SIZE - 1,
						&decoded));
}

static void nvidia_ghes_vera_rejects_bad_info_size(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10, sizeof(vera_l1_reset_id10));
	buf[34] = 31;

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_l1_reset_id10), &decoded));
}

static void nvidia_ghes_vera_rejects_unsupported_version(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10, sizeof(vera_l1_reset_id10));
	buf[0] = 2;

	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_l1_reset_id10), &decoded));
}

static void nvidia_ghes_vera_rejects_unsupported_info_major_version(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;
	int ret;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10,
				       sizeof(vera_l1_reset_id10));
	put_unaligned_le16(0x0100, &buf[VERA_EVENT_HDR_SIZE]);

	ret = nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_l1_reset_id10),
				      &decoded);
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP, ret);
}

static void nvidia_ghes_vera_accepts_newer_info_minor_version(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10,
				       sizeof(vera_l1_reset_id10));
	put_unaligned_le16(0x00ff, &buf[VERA_EVENT_HDR_SIZE]);

	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, buf,
						sizeof(vera_l1_reset_id10), &decoded));
}

static void nvidia_ghes_vera_accepts_extended_cpu_info(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context context;
	size_t len;
	u8 *buf;
	u64 val;

	buf = vera_make_synthetic(test, 1, VERA_CPU_INFO_SIZE + 4, 3,
				  data, sizeof(data), &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &context));
	KUNIT_EXPECT_EQ(test, VERA_CONTEXT_HDR_SIZE + 16,
			context.context_size);
	KUNIT_EXPECT_EQ(test, 3, context.data_format_type);
	KUNIT_EXPECT_EQ(test, 8U, context.data_size);
	KUNIT_EXPECT_EQ(test, 1, nvidia_ghes_vera_context_entry_count(&context));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_context_u64_value(&context, 0, &val));
	KUNIT_EXPECT_EQ(test, 0x0102030405060708ULL, val);
}

static void nvidia_ghes_vera_rejects_short_context_size(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10, sizeof(vera_l1_reset_id10));
	put_unaligned_le32(15, &buf[VERA_FIRST_CONTEXT_OFFSET]);

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_l1_reset_id10), &decoded));
}

static void nvidia_ghes_vera_rejects_data_size_exceeding_context_size(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_default(test, 3, data, sizeof(data), &len);
	put_unaligned_le32(VERA_CONTEXT_HDR_SIZE,
			   &buf[VERA_FIRST_CONTEXT_OFFSET]);

	KUNIT_EXPECT_EQ(test, -EINVAL, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
}

static void nvidia_ghes_vera_rejects_unaligned_context_size(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_default(test, 3, data, sizeof(data), &len);
	put_unaligned_le32(VERA_CONTEXT_HDR_SIZE + sizeof(data),
			   &buf[VERA_FIRST_CONTEXT_OFFSET]);

	KUNIT_EXPECT_EQ(test, -EINVAL, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
}

static void nvidia_ghes_vera_accepts_padded_context_size(struct kunit *test)
{
	static const u8 first_data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	static const u8 second_data[] = {
		0x78, 0x56, 0x34, 0x12,
	};
	struct vera_synth_context contexts[] = {
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.format = 3,
			.data = first_data,
			.data_size = sizeof(first_data),
		},
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.format = 4,
			.data = second_data,
			.data_size = sizeof(second_data),
		},
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context parsed[2];
	size_t len;
	u8 *buf;
	u32 val;

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, contexts,
					   ARRAY_SIZE(contexts), &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 2, decoded.event_context_count);
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &parsed[0]));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 1, &parsed[1]));
	KUNIT_EXPECT_EQ(test, VERA_CONTEXT_HDR_SIZE + 16,
			parsed[0].context_size);
	KUNIT_EXPECT_EQ(test, sizeof(first_data), parsed[0].data_size);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_value(&parsed[1], 0, &val));
	KUNIT_EXPECT_EQ(test, 0x12345678U, val);
}

static void nvidia_ghes_vera_rejects_context_beyond_section(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_default(test, 3, data, sizeof(data), &len);
	KUNIT_EXPECT_EQ(test, -ENODATA,
			nvidia_ghes_decode_vera(NULL, buf, len - 8, &decoded));
}

static void nvidia_ghes_vera_preserves_legacy_opaque_advance(struct kunit *test)
{
	static const u8 opaque_data[] = { 0xaa, 0xbb, 0xcc };
	static const u8 value_data[] = { 0x78, 0x56, 0x34, 0x12 };
	struct vera_synth_context contexts[] = {
		{
			.context_size = VERA_CONTEXT_HDR_SIZE,
			.format = 0,
			.data = opaque_data,
			.data_size = sizeof(opaque_data),
		},
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.format = 4,
			.data = value_data,
			.data_size = sizeof(value_data),
		},
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context parsed[2];
	size_t len;
	u8 *buf;
	u32 val;

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, contexts,
					   ARRAY_SIZE(contexts), &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 2, decoded.event_context_count);
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &parsed[0]));
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 1, &parsed[1]));
	KUNIT_EXPECT_EQ(test, sizeof(opaque_data), parsed[0].data_size);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_value(&parsed[1], 0, &val));
	KUNIT_EXPECT_EQ(test, 0x12345678U, val);
}

static void nvidia_ghes_vera_retains_contexts_before_failure(struct kunit *test)
{
	static const u8 first_data[] = { 0x78, 0x56, 0x34, 0x12 };
	static const u8 second_data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct vera_synth_context contexts[] = {
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.format = 4,
			.data = first_data,
			.data_size = sizeof(first_data),
		},
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.format = 3,
			.data = second_data,
			.data_size = sizeof(second_data),
		},
	};
	struct nvidia_ghes_decoded decoded = {};
	struct nvidia_ghes_vera_context context;
	size_t second_offset = VERA_FIRST_CONTEXT_OFFSET + contexts[0].context_size;
	size_t len;
	u8 *buf;
	u32 val;

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, contexts,
					   ARRAY_SIZE(contexts), &len);
	put_unaligned_le32(VERA_CONTEXT_HDR_SIZE + sizeof(second_data),
			   &buf[second_offset]);

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 2, decoded.event_context_count);
	KUNIT_EXPECT_EQ(test, 1, decoded.valid_context_count);
	KUNIT_ASSERT_EQ(test, 0, vera_context_at(&decoded, 0, &context));
	KUNIT_EXPECT_EQ(test, sizeof(first_data), context.data_size);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_value(&context, 0, &val));
	KUNIT_EXPECT_EQ(test, 0x12345678U, val);
}

static void nvidia_ghes_vera_accepts_all_wire_contexts(struct kunit *test)
{
	struct nvidia_ghes_vera_cursor cursor;
	struct nvidia_ghes_vera_context context;
	struct vera_synth_context *contexts;
	struct nvidia_ghes_decoded decoded = {};
	static const u8 empty;
	size_t len;
	u8 *buf;
	int i;

	contexts = kunit_kcalloc(test, U8_MAX, sizeof(*contexts), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, contexts);
	for (i = 0; i < U8_MAX; i++) {
		contexts[i].context_size = VERA_CONTEXT_HDR_SIZE;
		contexts[i].format = 0;
		contexts[i].data = &empty;
		contexts[i].data_size = 0;
	}

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, contexts,
					   U8_MAX, &len);
	KUNIT_ASSERT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, U8_MAX, decoded.event_context_count);
	KUNIT_EXPECT_EQ(test, U8_MAX, decoded.valid_context_count);

	nvidia_ghes_vera_cursor_init(&decoded, &cursor);
	for (i = 0; i < U8_MAX; i++)
		KUNIT_ASSERT_EQ(test, 1,
				nvidia_ghes_vera_cursor_next(&cursor, &context));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_cursor_next(&cursor, &context));
	KUNIT_EXPECT_EQ(test, 16,
			nvidia_ghes_vera_print_context_count(&decoded));
}

static void nvidia_ghes_vera_accepts_zero_contexts(struct kunit *test)
{
	struct nvidia_ghes_vera_cursor cursor;
	struct nvidia_ghes_vera_context context;
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, NULL, 0,
					   &len);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 0, decoded.event_context_count);
	KUNIT_EXPECT_EQ(test, 0, decoded.valid_context_count);
	nvidia_ghes_vera_cursor_init(&decoded, &cursor);
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_cursor_next(&cursor, &context));
}

static void nvidia_ghes_vera_accepts_seventeen_contexts(struct kunit *test)
{
	struct nvidia_ghes_vera_cursor cursor;
	struct nvidia_ghes_vera_context context;
	struct vera_synth_context *contexts;
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;
	int i;

	contexts = kunit_kcalloc(test, 17, sizeof(*contexts), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, contexts);
	for (i = 0; i < 17; i++)
		contexts[i].context_size = VERA_CONTEXT_HDR_SIZE;

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, contexts,
					   17, &len);
	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 17, decoded.event_context_count);
	KUNIT_EXPECT_EQ(test, 17, decoded.valid_context_count);
	nvidia_ghes_vera_cursor_init(&decoded, &cursor);
	for (i = 0; i < 17; i++)
		KUNIT_ASSERT_EQ(test, 1,
				nvidia_ghes_vera_cursor_next(&cursor, &context));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_cursor_next(&cursor, &context));
}

static void nvidia_ghes_vera_accepts_schema_extensions_together(struct kunit *test)
{
	static const u8 first_data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct vera_synth_context contexts[] = {
		{
			.context_size = VERA_CONTEXT_HDR_SIZE + 16,
			.context_version = 2,
			.format = 3,
			.data_format_version = 3,
			.data = first_data,
			.data_size = sizeof(first_data),
		},
		{
			.context_size = VERA_CONTEXT_HDR_SIZE,
			.context_version = 4,
			.format = 0,
			.data_format_version = 5,
		},
	};
	struct nvidia_ghes_vera_cursor cursor;
	struct nvidia_ghes_vera_context context;
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *section;
	u8 *buf;

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE + 4,
					   contexts, ARRAY_SIZE(contexts),
					   &len);
	put_unaligned_le16(0x00ff, &buf[VERA_EVENT_HDR_SIZE]);
	section = kunit_kzalloc(test, len + 7, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, section);
	memcpy(section, buf, len);

	KUNIT_ASSERT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, section, len + 7, &decoded));
	KUNIT_EXPECT_EQ(test, 2, decoded.event_context_count);
	KUNIT_EXPECT_EQ(test, 2, decoded.valid_context_count);
	nvidia_ghes_vera_cursor_init(&decoded, &cursor);
	KUNIT_ASSERT_EQ(test, 1,
			nvidia_ghes_vera_cursor_next(&cursor, &context));
	KUNIT_EXPECT_EQ(test, 2, context.context_version);
	KUNIT_EXPECT_EQ(test, 3, context.data_format_version);
	KUNIT_ASSERT_EQ(test, 1,
			nvidia_ghes_vera_cursor_next(&cursor, &context));
	KUNIT_EXPECT_EQ(test, 4, context.context_version);
	KUNIT_EXPECT_EQ(test, 5, context.data_format_version);
	KUNIT_EXPECT_EQ(test, 7, cursor.remaining);
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_cursor_next(&cursor, &context));
}

static void nvidia_ghes_vera_retains_large_valid_prefix(struct kunit *test)
{
	struct nvidia_ghes_vera_cursor cursor;
	struct nvidia_ghes_vera_context context;
	struct vera_synth_context *contexts;
	struct nvidia_ghes_decoded decoded = {};
	static const u8 empty;
	size_t bad_offset;
	size_t len;
	u8 *buf;
	int i;

	contexts = kunit_kcalloc(test, 18, sizeof(*contexts), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, contexts);
	for (i = 0; i < 18; i++) {
		contexts[i].context_size = VERA_CONTEXT_HDR_SIZE;
		contexts[i].format = 0;
		contexts[i].data = &empty;
	}

	buf = vera_make_synthetic_contexts(test, 1, VERA_CPU_INFO_SIZE, contexts,
					   18, &len);
	bad_offset = VERA_FIRST_CONTEXT_OFFSET + 17 * VERA_CONTEXT_HDR_SIZE;
	put_unaligned_le32(VERA_CONTEXT_HDR_SIZE - 1, &buf[bad_offset]);

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 18, decoded.event_context_count);
	KUNIT_EXPECT_EQ(test, 17, decoded.valid_context_count);
	KUNIT_EXPECT_EQ(test, 16,
			nvidia_ghes_vera_print_context_count(&decoded));

	nvidia_ghes_vera_cursor_init(&decoded, &cursor);
	for (i = 0; i < 17; i++)
		KUNIT_ASSERT_EQ(test, 1,
				nvidia_ghes_vera_cursor_next(&cursor, &context));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_cursor_next(&cursor, &context));
}

static void nvidia_ghes_vera_rejects_data_beyond_section(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_default(test, 3, data, sizeof(data), &len);
	put_unaligned_le32(24, &buf[VERA_FIRST_CONTEXT_OFFSET + 12]);

	KUNIT_EXPECT_EQ(test, -ENODATA, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
}

static void nvidia_ghes_vera_rejects_mis_sized_format1(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_crashdump_id_id40,
				       sizeof(vera_crashdump_id_id40));
	put_unaligned_le32(15, &buf[VERA_FIRST_CONTEXT_OFFSET + 12]);

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_decode_vera(NULL, buf,
						sizeof(vera_crashdump_id_id40),
						&decoded));
}

static void nvidia_ghes_vera_print_budget_is_section_wide(struct kunit *test)
{
	static const u8 data;
	struct nvidia_ghes_vera_context ctx = {
		.data_format_type = 1,
		.data_size = 24 * 16,
		.data = &data,
	};
	unsigned int remaining = 32;

	KUNIT_EXPECT_EQ(test, 24,
			nvidia_ghes_vera_context_print_count(&ctx, &remaining));
	KUNIT_EXPECT_EQ(test, 8U, remaining);
	KUNIT_EXPECT_EQ(test, 8,
			nvidia_ghes_vera_context_print_count(&ctx, &remaining));
	KUNIT_EXPECT_EQ(test, 0U, remaining);
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_context_print_count(&ctx, &remaining));
}

static void nvidia_ghes_vera_print_budget_rejects_null_remaining(struct kunit *test)
{
	struct nvidia_ghes_vera_context ctx = {};

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_vera_context_print_count(&ctx, NULL));
}

static void nvidia_ghes_vera_accessors_reject_null_data(struct kunit *test)
{
	struct nvidia_ghes_vera_context ctx = {
		.data_size = 16,
	};
	u64 key64, val64;
	u32 key32, val32;

	ctx.data_format_type = 1;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_vera_context_u64_pair(&ctx, 0, &key64,
							  &val64));
	ctx.data_format_type = 2;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_vera_context_u32_pair(&ctx, 0, &key32,
							  &val32));
	ctx.data_format_type = 3;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_vera_context_u64_value(&ctx, 0, &val64));
	ctx.data_format_type = 4;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_vera_context_u32_value(&ctx, 0, &val32));
}

static void nvidia_ghes_vera_rejects_mis_sized_format2(struct kunit *test)
{
	static const u8 data[] = {
		0x78, 0x56, 0x34, 0x12, 0xf0, 0xde, 0xbc,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_default(test, 2, data, sizeof(data), &len);
	KUNIT_EXPECT_EQ(test, -EINVAL, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
}

static void nvidia_ghes_vera_rejects_mis_sized_format3(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_default(test, 3, data, sizeof(data), &len);
	KUNIT_EXPECT_EQ(test, -EINVAL, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
}

static void nvidia_ghes_vera_rejects_mis_sized_format4(struct kunit *test)
{
	static const u8 data[] = {
		0x78, 0x56, 0x34,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_default(test, 4, data, sizeof(data), &len);
	KUNIT_EXPECT_EQ(test, -EINVAL, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
}

static void nvidia_ghes_vera_rejects_unsupported_source_device(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10, sizeof(vera_l1_reset_id10));
	buf[2] = 1;

	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_l1_reset_id10), &decoded));
}

static void nvidia_ghes_vera_rejects_unsupported_data_format(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10, sizeof(vera_l1_reset_id10));
	put_unaligned_le16(5, &buf[VERA_FIRST_CONTEXT_OFFSET + 8]);

	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_l1_reset_id10), &decoded));
}

static void nvidia_ghes_vera_copies_non_nul_signature(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10, sizeof(vera_l1_reset_id10));
	memcpy(&buf[16], "ABCDEFGHIJKLMNOP", 16);

	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_l1_reset_id10), &decoded));
	KUNIT_EXPECT_STREQ(test, "ABCDEFGHIJKLMNOP", decoded.signature);
	KUNIT_EXPECT_EQ(test, '\0', decoded.signature[16]);
}

static void nvidia_ghes_guid_routes_grace(struct kunit *test)
{
	const guid_t guid = GUID_INIT(0x6d5244f2, 0x2712, 0x11ec,
				      0xbe, 0xa7, 0xcb, 0x3f, 0xdb, 0x95, 0xc7, 0x86);

	KUNIT_EXPECT_EQ(test, NVIDIA_GHES_FORMAT_GRACE,
			nvidia_ghes_format_from_guid(&guid));
}

static void nvidia_ghes_guid_routes_vera(struct kunit *test)
{
	const guid_t guid = GUID_INIT(0x9068e568, 0x6ca0, 0x11f0,
				      0xae, 0xaf, 0x15, 0x93, 0x43, 0x59, 0x1e, 0xac);

	KUNIT_EXPECT_EQ(test, NVIDIA_GHES_FORMAT_VERA,
			nvidia_ghes_format_from_guid(&guid));
}

static void nvidia_ghes_guid_rejects_unknown(struct kunit *test)
{
	const guid_t guid = GUID_INIT(0x00000000, 0x0000, 0x0000,
				      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

	KUNIT_EXPECT_EQ(test, NVIDIA_GHES_FORMAT_UNKNOWN,
			nvidia_ghes_format_from_guid(&guid));
}

static void nvidia_ghes_grace_reg_pair_rejects_null_decoded(struct kunit *test)
{
	u64 addr, val;

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_grace_reg_pair(NULL, 0, &addr, &val));
}

static void nvidia_ghes_grace_reg_pair_rejects_wrong_format(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {
		.format = NVIDIA_GHES_FORMAT_VERA,
	};
	u64 addr, val;

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_grace_reg_pair(&decoded, 0, &addr, &val));
}

static void nvidia_ghes_grace_reg_pair_rejects_null_regs(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {
		.format = NVIDIA_GHES_FORMAT_GRACE,
		.number_regs = 1,
		.grace_regs = NULL,
	};
	u64 addr, val;

	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_grace_reg_pair(&decoded, 0, &addr, &val));
}

static void nvidia_ghes_grace_reg_pair_rejects_out_of_range(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {
		.format = NVIDIA_GHES_FORMAT_GRACE,
		.number_regs = 0,
	};
	u64 addr, val;

	KUNIT_EXPECT_EQ(test, -ERANGE,
			nvidia_ghes_grace_reg_pair(&decoded, 0, &addr, &val));
}

static struct kunit_case nvidia_ghes_test_cases[] = {
	KUNIT_CASE(nvidia_ghes_grace_decodes_registers),
	KUNIT_CASE(nvidia_ghes_grace_decodes_unaligned_payload),
	KUNIT_CASE(nvidia_ghes_grace_accepts_zero_registers),
	KUNIT_CASE(nvidia_ghes_grace_copies_non_nul_signature),
	KUNIT_CASE(nvidia_ghes_grace_rejects_truncated_header),
	KUNIT_CASE(nvidia_ghes_grace_rejects_truncated_registers),
	KUNIT_CASE(nvidia_ghes_grace_reg_pair_rejects_null_decoded),
	KUNIT_CASE(nvidia_ghes_grace_reg_pair_rejects_wrong_format),
	KUNIT_CASE(nvidia_ghes_grace_reg_pair_rejects_null_regs),
	KUNIT_CASE(nvidia_ghes_grace_reg_pair_rejects_out_of_range),
	KUNIT_CASE(nvidia_ghes_vera_decodes_l1_reset),
	KUNIT_CASE(nvidia_ghes_vera_decodes_crashdump_id),
	KUNIT_CASE(nvidia_ghes_vera_decodes_crashdump_s1_opaque_context),
	KUNIT_CASE(nvidia_ghes_vera_decodes_multi_context_aligned),
	KUNIT_CASE(nvidia_ghes_vera_decodes_multi_context_covering_first),
	KUNIT_CASE(nvidia_ghes_vera_decodes_format2_u32_pairs),
	KUNIT_CASE(nvidia_ghes_vera_decodes_format3_u64_values),
	KUNIT_CASE(nvidia_ghes_vera_decodes_format4_u32_values),
	KUNIT_CASE(nvidia_ghes_vera_rejects_truncated_event_header),
	KUNIT_CASE(nvidia_ghes_vera_rejects_truncated_cpu_info),
	KUNIT_CASE(nvidia_ghes_vera_rejects_truncated_context_header),
	KUNIT_CASE(nvidia_ghes_vera_rejects_bad_info_size),
	KUNIT_CASE(nvidia_ghes_vera_rejects_unsupported_version),
	KUNIT_CASE(nvidia_ghes_vera_rejects_unsupported_info_major_version),
	KUNIT_CASE(nvidia_ghes_vera_accepts_newer_info_minor_version),
	KUNIT_CASE(nvidia_ghes_vera_accepts_extended_cpu_info),
	KUNIT_CASE(nvidia_ghes_vera_rejects_short_context_size),
	KUNIT_CASE(nvidia_ghes_vera_rejects_data_size_exceeding_context_size),
	KUNIT_CASE(nvidia_ghes_vera_rejects_unaligned_context_size),
	KUNIT_CASE(nvidia_ghes_vera_accepts_padded_context_size),
	KUNIT_CASE(nvidia_ghes_vera_rejects_context_beyond_section),
	KUNIT_CASE(nvidia_ghes_vera_preserves_legacy_opaque_advance),
	KUNIT_CASE(nvidia_ghes_vera_retains_contexts_before_failure),
	KUNIT_CASE(nvidia_ghes_vera_accepts_all_wire_contexts),
	KUNIT_CASE(nvidia_ghes_vera_accepts_zero_contexts),
	KUNIT_CASE(nvidia_ghes_vera_accepts_seventeen_contexts),
	KUNIT_CASE(nvidia_ghes_vera_accepts_schema_extensions_together),
	KUNIT_CASE(nvidia_ghes_vera_retains_large_valid_prefix),
	KUNIT_CASE(nvidia_ghes_vera_rejects_data_beyond_section),
	KUNIT_CASE(nvidia_ghes_vera_rejects_mis_sized_format1),
	KUNIT_CASE(nvidia_ghes_vera_print_budget_is_section_wide),
	KUNIT_CASE(nvidia_ghes_vera_print_budget_rejects_null_remaining),
	KUNIT_CASE(nvidia_ghes_vera_accessors_reject_null_data),
	KUNIT_CASE(nvidia_ghes_vera_rejects_mis_sized_format2),
	KUNIT_CASE(nvidia_ghes_vera_rejects_mis_sized_format3),
	KUNIT_CASE(nvidia_ghes_vera_rejects_mis_sized_format4),
	KUNIT_CASE(nvidia_ghes_vera_rejects_unsupported_source_device),
	KUNIT_CASE(nvidia_ghes_vera_rejects_unsupported_data_format),
	KUNIT_CASE(nvidia_ghes_vera_copies_non_nul_signature),
	KUNIT_CASE(nvidia_ghes_guid_routes_grace),
	KUNIT_CASE(nvidia_ghes_guid_routes_vera),
	KUNIT_CASE(nvidia_ghes_guid_rejects_unknown),
	{}
};

static struct kunit_suite nvidia_ghes_test_suite = {
	.name = "acpi_apei_ghes_nvidia",
	.test_cases = nvidia_ghes_test_cases,
};

kunit_test_suite(nvidia_ghes_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for NVIDIA GHES CPER parser helpers");
MODULE_LICENSE("GPL");
