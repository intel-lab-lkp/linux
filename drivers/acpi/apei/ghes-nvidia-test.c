// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
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

	KUNIT_EXPECT_EQ(test, 0,
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

static void nvidia_ghes_grace_accepts_zero_registers(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_EXPECT_EQ(test, 0,
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

	KUNIT_EXPECT_EQ(test, 0,
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

static u8 *nvidia_ghes_kunit_memdup(struct kunit *test, const u8 *src, size_t len)
{
	u8 *buf;

	buf = kunit_kmalloc(test, len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	memcpy(buf, src, len);

	return buf;
}

static u8 *vera_make_synthetic(struct kunit *test, u8 version, u8 info_size,
			       u16 format, const u8 *data, u32 data_size,
			       size_t *len)
{
	u8 *buf;
	size_t context_offset;

	KUNIT_ASSERT_TRUE(test, info_size >= VERA_CPU_INFO_SIZE);

	context_offset = VERA_EVENT_HDR_SIZE + info_size;
	*len = context_offset + VERA_CONTEXT_HDR_SIZE + data_size;
	buf = kunit_kzalloc(test, *len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	buf[0] = version;
	buf[1] = 1;
	buf[2] = 0;
	put_unaligned_le16(0x22, &buf[4]);
	memcpy(&buf[16], "SYNTH", 5);
	put_unaligned_le16(0, &buf[32]);
	buf[34] = info_size;
	buf[35] = 0;
	put_unaligned_le32(0x14107, &buf[36]);
	memcpy(&buf[40], vera_chip_serial, sizeof(vera_chip_serial));
	put_unaligned_le64(0, &buf[56]);
	put_unaligned_le32(VERA_CONTEXT_HDR_SIZE, &buf[context_offset]);
	put_unaligned_le16(0, &buf[context_offset + 4]);
	put_unaligned_le16(format, &buf[context_offset + 8]);
	put_unaligned_le16(0, &buf[context_offset + 10]);
	put_unaligned_le32(data_size, &buf[context_offset + 12]);
	memcpy(&buf[context_offset + VERA_CONTEXT_HDR_SIZE], data, data_size);

	return buf;
}

static u8 *vera_make_synthetic_default(struct kunit *test, u16 format,
				       const u8 *data, u32 data_size, size_t *len)
{
	return vera_make_synthetic(test, 1, VERA_CPU_INFO_SIZE, format, data,
				   data_size, len);
}

static int nvidia_ghes_vera_context_u64_pair(const struct nvidia_ghes_vera_context *ctx,
					     unsigned int index, u64 *addr, u64 *val)
{
	int count;

	if (!ctx || !addr || !val || ctx->data_format_type != 1)
		return -EINVAL;

	count = nvidia_ghes_vera_context_entry_count(ctx);
	if (count < 0)
		return count;
	if (index >= count)
		return -ERANGE;

	*addr = get_unaligned_le64(ctx->data + index * 16);
	*val = get_unaligned_le64(ctx->data + index * 16 + 8);

	return 0;
}

static int nvidia_ghes_vera_context_u32_pair(const struct nvidia_ghes_vera_context *ctx,
					     unsigned int index, u32 *addr, u32 *val)
{
	int count;

	if (!ctx || !addr || !val || ctx->data_format_type != 2)
		return -EINVAL;

	count = nvidia_ghes_vera_context_entry_count(ctx);
	if (count < 0)
		return count;
	if (index >= count)
		return -ERANGE;

	*addr = get_unaligned_le32(ctx->data + index * 8);
	*val = get_unaligned_le32(ctx->data + index * 8 + 4);

	return 0;
}

static int nvidia_ghes_vera_context_u64_value(const struct nvidia_ghes_vera_context *ctx,
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

static int nvidia_ghes_vera_context_u32_value(const struct nvidia_ghes_vera_context *ctx,
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

static void nvidia_ghes_vera_decodes_l1_reset(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};

	KUNIT_EXPECT_EQ(test, 0,
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
	KUNIT_EXPECT_EQ(test, 16U, decoded.contexts[0].context_size);
	KUNIT_EXPECT_EQ(test, 1, decoded.contexts[0].data_format_type);
	KUNIT_EXPECT_EQ(test, 0U, decoded.contexts[0].data_size);
}

static void nvidia_ghes_vera_decodes_crashdump_id(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u64 addr, val;

	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, vera_crashdump_id_id40,
						sizeof(vera_crashdump_id_id40),
						&decoded));
	KUNIT_EXPECT_STREQ(test, "CRASHDUMP-ID", decoded.signature);
	KUNIT_EXPECT_MEMEQ(test, decoded.chip_serial_number, vera_chip_serial,
			   sizeof(vera_chip_serial));
	KUNIT_EXPECT_EQ(test, 240U, decoded.contexts[0].context_size);
	KUNIT_EXPECT_EQ(test, 1, decoded.contexts[0].data_format_type);
	KUNIT_EXPECT_EQ(test, 224U, decoded.contexts[0].data_size);
	KUNIT_EXPECT_EQ(test, 14, nvidia_ghes_vera_context_entry_count(&decoded.contexts[0]));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_context_u64_pair(&decoded.contexts[0], 0,
							  &addr, &val));
	KUNIT_EXPECT_EQ(test, 0x8300000000000000ULL, addr);
	KUNIT_EXPECT_EQ(test, 0x372e33375f6c6572ULL, val);
}

static void nvidia_ghes_vera_decodes_crashdump_s1_opaque_context(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u64 val;

	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_decode_vera(NULL, vera_crashdump_s1_id41,
						sizeof(vera_crashdump_s1_id41),
						&decoded));
	KUNIT_EXPECT_STREQ(test, "CRASHDUMP-S1", decoded.signature);
	KUNIT_EXPECT_EQ(test, 4096, decoded.event_type);
	KUNIT_EXPECT_EQ(test, 2304, decoded.event_sub_type);
	KUNIT_EXPECT_MEMEQ(test, decoded.chip_serial_number, vera_chip_serial,
			   sizeof(vera_chip_serial));
	KUNIT_EXPECT_EQ(test, 16U, decoded.contexts[0].context_size);
	KUNIT_EXPECT_EQ(test, 0, decoded.contexts[0].data_format_type);
	KUNIT_EXPECT_EQ(test, 1928U, decoded.contexts[0].data_size);
	KUNIT_EXPECT_NOT_NULL(test, decoded.contexts[0].data);
	KUNIT_EXPECT_EQ(test, 0x53, decoded.contexts[0].data[0]);
	KUNIT_EXPECT_EQ(test, 0x56, decoded.contexts[0].data[1]);
	KUNIT_EXPECT_EQ(test, 0x7f, decoded.contexts[0].data[2]);
	KUNIT_EXPECT_EQ(test, -EINVAL,
			nvidia_ghes_vera_context_u64_value(&decoded.contexts[0], 0, &val));
}

static void nvidia_ghes_vera_decodes_format2_u32_pairs(struct kunit *test)
{
	static const u8 data[] = {
		0x78, 0x56, 0x34, 0x12, 0xf0, 0xde, 0xbc, 0x9a,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;
	u32 addr, val;

	buf = vera_make_synthetic_default(test, 2, data, sizeof(data), &len);
	KUNIT_EXPECT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 1, nvidia_ghes_vera_context_entry_count(&decoded.contexts[0]));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_pair(&decoded.contexts[0], 0,
							  &addr, &val));
	KUNIT_EXPECT_EQ(test, 0x12345678U, addr);
	KUNIT_EXPECT_EQ(test, 0x9abcdef0U, val);
}

static void nvidia_ghes_vera_decodes_format3_u64_values(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;
	u64 val;

	buf = vera_make_synthetic_default(test, 3, data, sizeof(data), &len);
	KUNIT_EXPECT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 1, nvidia_ghes_vera_context_entry_count(&decoded.contexts[0]));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_context_u64_value(&decoded.contexts[0], 0, &val));
	KUNIT_EXPECT_EQ(test, 0x0102030405060708ULL, val);
}

static void nvidia_ghes_vera_decodes_format4_u32_values(struct kunit *test)
{
	static const u8 data[] = {
		0x78, 0x56, 0x34, 0x12,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;
	u32 val;

	buf = vera_make_synthetic_default(test, 4, data, sizeof(data), &len);
	KUNIT_EXPECT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, 1, nvidia_ghes_vera_context_entry_count(&decoded.contexts[0]));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_context_u32_value(&decoded.contexts[0], 0, &val));
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

static void nvidia_ghes_vera_rejects_too_many_contexts(struct kunit *test)
{
	struct nvidia_ghes_decoded decoded = {};
	u8 *buf;

	buf = nvidia_ghes_kunit_memdup(test, vera_l1_reset_id10, sizeof(vera_l1_reset_id10));
	buf[1] = NVIDIA_GHES_MAX_CONTEXTS + 1;

	KUNIT_EXPECT_EQ(test, -E2BIG,
			nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_l1_reset_id10), &decoded));
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

static void nvidia_ghes_vera_accepts_extended_cpu_info(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;
	u64 val;

	buf = vera_make_synthetic(test, 1, VERA_CPU_INFO_SIZE + 4, 3,
				  data, sizeof(data), &len);
	KUNIT_EXPECT_EQ(test, 0, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
	KUNIT_EXPECT_EQ(test, VERA_CONTEXT_HDR_SIZE, decoded.contexts[0].context_size);
	KUNIT_EXPECT_EQ(test, 3, decoded.contexts[0].data_format_type);
	KUNIT_EXPECT_EQ(test, 8U, decoded.contexts[0].data_size);
	KUNIT_EXPECT_EQ(test, 1, nvidia_ghes_vera_context_entry_count(&decoded.contexts[0]));
	KUNIT_EXPECT_EQ(test, 0,
			nvidia_ghes_vera_context_u64_value(&decoded.contexts[0], 0, &val));
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

static void nvidia_ghes_vera_rejects_ambiguous_context_size(struct kunit *test)
{
	static const u8 data[] = {
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	};
	struct nvidia_ghes_decoded decoded = {};
	size_t len;
	u8 *buf;

	buf = vera_make_synthetic_default(test, 3, data, sizeof(data), &len);
	put_unaligned_le32(20, &buf[VERA_FIRST_CONTEXT_OFFSET]);

	KUNIT_EXPECT_EQ(test, -EINVAL, nvidia_ghes_decode_vera(NULL, buf, len, &decoded));
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
	put_unaligned_le32(16, &buf[VERA_FIRST_CONTEXT_OFFSET + 12]);

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
			nvidia_ghes_decode_vera(NULL, buf, sizeof(vera_crashdump_id_id40), &decoded));
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
	KUNIT_CASE(nvidia_ghes_grace_accepts_zero_registers),
	KUNIT_CASE(nvidia_ghes_grace_copies_non_nul_signature),
	KUNIT_CASE(nvidia_ghes_grace_rejects_truncated_header),
	KUNIT_CASE(nvidia_ghes_grace_rejects_truncated_registers),
	KUNIT_CASE(nvidia_ghes_grace_reg_pair_rejects_null_decoded),
	KUNIT_CASE(nvidia_ghes_grace_reg_pair_rejects_wrong_format),
	KUNIT_CASE(nvidia_ghes_grace_reg_pair_rejects_out_of_range),
	KUNIT_CASE(nvidia_ghes_vera_decodes_l1_reset),
	KUNIT_CASE(nvidia_ghes_vera_decodes_crashdump_id),
	KUNIT_CASE(nvidia_ghes_vera_decodes_crashdump_s1_opaque_context),
	KUNIT_CASE(nvidia_ghes_vera_decodes_format2_u32_pairs),
	KUNIT_CASE(nvidia_ghes_vera_decodes_format3_u64_values),
	KUNIT_CASE(nvidia_ghes_vera_decodes_format4_u32_values),
	KUNIT_CASE(nvidia_ghes_vera_rejects_truncated_event_header),
	KUNIT_CASE(nvidia_ghes_vera_rejects_truncated_cpu_info),
	KUNIT_CASE(nvidia_ghes_vera_rejects_truncated_context_header),
	KUNIT_CASE(nvidia_ghes_vera_rejects_too_many_contexts),
	KUNIT_CASE(nvidia_ghes_vera_rejects_bad_info_size),
	KUNIT_CASE(nvidia_ghes_vera_rejects_unsupported_version),
	KUNIT_CASE(nvidia_ghes_vera_accepts_extended_cpu_info),
	KUNIT_CASE(nvidia_ghes_vera_rejects_short_context_size),
	KUNIT_CASE(nvidia_ghes_vera_rejects_ambiguous_context_size),
	KUNIT_CASE(nvidia_ghes_vera_rejects_data_beyond_section),
	KUNIT_CASE(nvidia_ghes_vera_rejects_mis_sized_format1),
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
	.name = "ghes-nvidia",
	.test_cases = nvidia_ghes_test_cases,
};

kunit_test_suite(nvidia_ghes_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for NVIDIA GHES CPER parser helpers");
MODULE_LICENSE("GPL");
