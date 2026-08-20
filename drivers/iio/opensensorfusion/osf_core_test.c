// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/device.h>
#include <kunit/test.h>

#include <linux/bitmap.h>
#include <linux/crc32.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/iio/buffer.h>
#include <linux/iio/buffer_impl.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "osf_core.h"
#include "osf_protocol.h"
#include "osf_stream.h"

#define OSF_TEST_SENSOR_INDEX		0
#define OSF_TEST_UNSUPPORTED_INDEX	7
#define OSF_TEST_SCALE_NANO		1000000
#define OSF_TEST_FLOOD_COUNT		(OSF_MAX_CAPABILITIES + 4)
#define OSF_TEST_MAX_PAYLOAD_LEN						\
	(OSF_SENSOR_SAMPLE_BASE_LEN + OSF_MAX_SAMPLE_CHANNELS * sizeof(__le32))
#define OSF_TEST_MAX_FRAME_LEN						\
	(OSF_FRAME_HEADER_LEN + OSF_CAP_REPORT_BASE_LEN +			\
	 2 * OSF_CAP_SENSOR_ENTRY_LEN + OSF_FRAME_CRC_LEN)

struct osf_test_context {
	struct osf_device osf;
	struct device *dev;
	struct iio_dev *indio_dev;
	struct iio_buffer *buffer;
	bool buffer_enabled;
};

struct osf_test_scan_3axis {
	s32 values[3];
};

static size_t osf_test_build_frame(u8 *buf, size_t buf_size,
				   u16 message_type, const u8 *payload,
				   u32 payload_len, u64 sequence)
{
	size_t frame_len = OSF_FRAME_HEADER_LEN + payload_len +
			   OSF_FRAME_CRC_LEN;
	u32 crc;

	if (frame_len > buf_size)
		return 0;

	memset(buf, 0, frame_len);
	put_unaligned_le32(OSF_FRAME_MAGIC, buf);
	buf[4] = OSF_PROTOCOL_MAJOR;
	buf[5] = OSF_PROTOCOL_MINOR;
	put_unaligned_le16(OSF_FRAME_HEADER_LEN, buf + 6);
	put_unaligned_le16(message_type, buf + 8);
	put_unaligned_le32(payload_len, buf + 10);
	put_unaligned_le64(sequence, buf + 14);
	put_unaligned_le64(sequence * 1000, buf + 22);
	memcpy(buf + OSF_FRAME_HEADER_LEN, payload, payload_len);

	crc = crc32_le(~0U, buf, OSF_FRAME_HEADER_LEN + payload_len) ^ ~0U;
	put_unaligned_le32(crc, buf + OSF_FRAME_HEADER_LEN + payload_len);

	return frame_len;
}

static size_t osf_test_build_capability_frame(u8 *buf, size_t buf_size,
					      u64 sequence)
{
	u8 payload[OSF_CAP_REPORT_BASE_LEN +
		   2 * OSF_CAP_SENSOR_ENTRY_LEN] = { };
	u8 *supported = payload + OSF_CAP_REPORT_BASE_LEN;
	u8 *unsupported = supported + OSF_CAP_SENSOR_ENTRY_LEN;

	put_unaligned_le16(2, payload);
	put_unaligned_le16(OSF_SENSOR_ACCELEROMETER, supported);
	put_unaligned_le16(OSF_TEST_SENSOR_INDEX, supported + 2);
	put_unaligned_le16(3, supported + 4);
	put_unaligned_le16(OSF_SAMPLE_FORMAT_S32, supported + 6);
	put_unaligned_le32(OSF_TEST_SCALE_NANO, supported + 8);

	put_unaligned_le16(OSF_SENSOR_BAROMETER, unsupported);
	put_unaligned_le16(OSF_TEST_UNSUPPORTED_INDEX, unsupported + 2);
	put_unaligned_le16(1, unsupported + 4);
	put_unaligned_le16(OSF_SAMPLE_FORMAT_S32, unsupported + 6);
	put_unaligned_le32(OSF_TEST_SCALE_NANO, unsupported + 8);

	return osf_test_build_frame(buf, buf_size, OSF_MSG_CAPABILITY_REPORT,
				    payload, sizeof(payload), sequence);
}

static size_t osf_test_build_sample_frame(u8 *buf, size_t buf_size,
					  u16 sensor_type, u16 sensor_index,
					  u16 channel_count, u16 sample_format,
					  const s32 *values, u64 sequence)
{
	u8 payload[OSF_TEST_MAX_PAYLOAD_LEN] = { };
	u32 payload_len;

	if (!channel_count || channel_count > OSF_MAX_SAMPLE_CHANNELS)
		return 0;

	put_unaligned_le16(sensor_type, payload);
	put_unaligned_le16(sensor_index, payload + 2);
	put_unaligned_le16(channel_count, payload + 4);
	put_unaligned_le16(sample_format, payload + 6);
	put_unaligned_le32(OSF_TEST_SCALE_NANO, payload + 8);

	for (u16 i = 0; i < channel_count; i++)
		put_unaligned_le32(values[i],
				   payload + OSF_SENSOR_SAMPLE_BASE_LEN +
				   i * sizeof(__le32));

	payload_len = OSF_SENSOR_SAMPLE_BASE_LEN +
		      channel_count * sizeof(__le32);

	return osf_test_build_frame(buf, buf_size, OSF_MSG_SENSOR_SAMPLE,
				    payload, payload_len, sequence);
}

static size_t osf_test_build_truncated_sample_frame(u8 *buf, size_t buf_size,
						    u64 sequence)
{
	u8 payload[OSF_SENSOR_SAMPLE_BASE_LEN + sizeof(__le32)] = { };

	put_unaligned_le16(OSF_SENSOR_ACCELEROMETER, payload);
	put_unaligned_le16(OSF_TEST_SENSOR_INDEX, payload + 2);
	put_unaligned_le16(3, payload + 4);
	put_unaligned_le16(OSF_SAMPLE_FORMAT_S32, payload + 6);
	put_unaligned_le32(OSF_TEST_SCALE_NANO, payload + 8);
	put_unaligned_le32(42, payload + OSF_SENSOR_SAMPLE_BASE_LEN);

	return osf_test_build_frame(buf, buf_size, OSF_MSG_SENSOR_SAMPLE,
				    payload, sizeof(payload), sequence);
}

static int osf_test_submit_sample(struct osf_test_context *ctx,
				  u16 sensor_type, u16 sensor_index,
				  u16 channel_count, u16 sample_format,
				  const s32 *values, u64 sequence)
{
	u8 frame[OSF_TEST_MAX_FRAME_LEN];
	size_t frame_len;

	frame_len = osf_test_build_sample_frame(frame, sizeof(frame),
						sensor_type, sensor_index,
						channel_count, sample_format,
						values, sequence);
	if (!frame_len)
		return -EINVAL;

	return osf_core_receive_frame(&ctx->osf, frame, frame_len);
}

static int osf_test_read_raw(struct osf_test_context *ctx,
			     unsigned int channel, int *value)
{
	int value2 = 0;

	return ctx->indio_dev->info->read_raw(ctx->indio_dev,
					     &ctx->indio_dev->channels[channel],
					     value, &value2,
					     IIO_CHAN_INFO_RAW);
}

static int osf_test_remove_scan(struct osf_test_context *ctx,
				struct osf_test_scan_3axis *scan)
{
	return iio_pop_from_buffer(ctx->buffer, scan);
}

static int osf_test_enable_buffer(struct osf_test_context *ctx)
{
	unsigned long *scan_mask;
	int ret;

	scan_mask = (unsigned long *)ctx->buffer->scan_mask;
	for (unsigned int i = 0; i < 3; i++)
		bitmap_set(scan_mask, ctx->indio_dev->channels[i].scan_index, 1);
	ret = iio_update_buffers(ctx->indio_dev, ctx->buffer, NULL);
	if (!ret)
		ctx->buffer_enabled = true;

	return ret;
}

static void osf_test_cleanup(void *data)
{
	struct osf_test_context *ctx = data;

	if (ctx->buffer_enabled) {
		iio_update_buffers(ctx->indio_dev, NULL, ctx->buffer);
		ctx->buffer_enabled = false;
	}

	osf_core_unregister_iio(&ctx->osf);
}

static int osf_test_init(struct kunit *test)
{
	struct osf_test_context *ctx;
	u8 frame[OSF_TEST_MAX_FRAME_LEN];
	size_t frame_len;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	test->priv = ctx;
	ctx->dev = kunit_device_register(test, "osf-core");
	if (IS_ERR(ctx->dev))
		return PTR_ERR(ctx->dev);
	if (!ctx->dev)
		return -ENOMEM;

	osf_core_init(&ctx->osf, ctx->dev);
	ret = kunit_add_action_or_reset(test, osf_test_cleanup, ctx);
	if (ret)
		return ret;

	frame_len = osf_test_build_capability_frame(frame, sizeof(frame), 1);
	if (!frame_len)
		return -EINVAL;

	ret = osf_core_receive_frame(&ctx->osf, frame, frame_len);
	if (ret != OSF_STREAM_FRAME_HANDLED)
		return ret < 0 ? ret : -EINVAL;
	if (ctx->osf.iio_dev_count != 1 ||
	    ctx->osf.capability_cache.capability_count != 1)
		return -EINVAL;

	ctx->indio_dev = ctx->osf.iio_devs[0].indio_dev;
	ctx->buffer = ctx->indio_dev->buffer;
	if (!ctx->buffer)
		return -EINVAL;

	return 0;
}

static void osf_test_expect_direct_sample(struct kunit *test,
					  struct osf_test_context *ctx,
					  const s32 *expected)
{
	int value;
	int ret;

	for (unsigned int i = 0; i < 3; i++) {
		value = 0;
		ret = osf_test_read_raw(ctx, i, &value);
		KUNIT_EXPECT_EQ(test, ret, IIO_VAL_INT);
		KUNIT_EXPECT_EQ(test, value, expected[i]);
	}
}

static void
osf_rejected_channel_count_preserves_latest_test(struct kunit *test)
{
	struct osf_test_context *ctx = test->priv;
	const s32 baseline_values[] = { 11, 22, 33 };
	const s32 rejected_values[] = { -999 };
	struct osf_sensor_sample decoded_sample;
	struct osf_latest_sample baseline;
	struct osf_test_scan_3axis scan;
	struct osf_frame decoded_frame;
	u8 rejected_frame[OSF_TEST_MAX_FRAME_LEN];
	size_t decoded_len;
	size_t frame_len;
	u64 baseline_sequence;
	int ret;

	KUNIT_ASSERT_EQ(test, osf_test_enable_buffer(ctx), 0);
	KUNIT_ASSERT_TRUE(test, iio_buffer_enabled(ctx->indio_dev));

	ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
				     OSF_TEST_SENSOR_INDEX, 3,
				     OSF_SAMPLE_FORMAT_S32,
				     baseline_values, 2);
	KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_HANDLED);
	KUNIT_ASSERT_EQ(test, ctx->osf.latest_sample_count, 1U);
	osf_test_expect_direct_sample(test, ctx, baseline_values);

	KUNIT_ASSERT_EQ(test, osf_test_remove_scan(ctx, &scan), 0);

	baseline = ctx->osf.latest_samples[0];
	baseline_sequence = ctx->osf.last_sequence;
	frame_len = osf_test_build_sample_frame(rejected_frame,
						sizeof(rejected_frame),
						OSF_SENSOR_ACCELEROMETER,
						OSF_TEST_SENSOR_INDEX, 1,
						OSF_SAMPLE_FORMAT_S32,
						rejected_values, 3);
	KUNIT_ASSERT_NE(test, frame_len, (size_t)0);
	KUNIT_ASSERT_EQ(test,
			osf_protocol_decode_frame(rejected_frame, frame_len,
						  &decoded_frame, &decoded_len), 0);
	KUNIT_ASSERT_EQ(test, decoded_len, frame_len);
	KUNIT_ASSERT_EQ(test,
			osf_protocol_decode_sensor_sample(&decoded_frame,
							  &decoded_sample), 0);
	KUNIT_ASSERT_EQ(test, decoded_sample.channel_count, (u16)1);
	ret = osf_core_receive_frame(&ctx->osf, rejected_frame, frame_len);
	KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_REJECTED);
	KUNIT_EXPECT_EQ(test, ctx->osf.latest_sample_count, 1U);
	KUNIT_EXPECT_MEMEQ(test, &ctx->osf.latest_samples[0], &baseline,
			   sizeof(baseline));
	KUNIT_EXPECT_EQ(test, ctx->osf.last_sequence, baseline_sequence);
	osf_test_expect_direct_sample(test, ctx, baseline_values);
	KUNIT_EXPECT_EQ(test, osf_test_remove_scan(ctx, &scan), -EBUSY);
}

static void
osf_valid_sample_updates_direct_and_buffer_test(struct kunit *test)
{
	struct osf_test_context *ctx = test->priv;
	const s32 baseline_values[] = { 1, 2, 3 };
	const s32 rejected_values[] = { -1 };
	const s32 valid_values[] = { 101, -202, 303 };
	struct osf_test_scan_3axis scan = { };
	int ret;

	KUNIT_ASSERT_EQ(test, osf_test_enable_buffer(ctx), 0);

	ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
				     OSF_TEST_SENSOR_INDEX, 3,
				     OSF_SAMPLE_FORMAT_S32,
				     baseline_values, 2);
	KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_HANDLED);
	KUNIT_ASSERT_EQ(test, osf_test_remove_scan(ctx, &scan), 0);

	ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
				     OSF_TEST_SENSOR_INDEX, 1,
				     OSF_SAMPLE_FORMAT_S32,
				     rejected_values, 3);
	KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_REJECTED);
	KUNIT_ASSERT_EQ(test, osf_test_remove_scan(ctx, &scan), -EBUSY);

	ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
				     OSF_TEST_SENSOR_INDEX, 3,
				     OSF_SAMPLE_FORMAT_S32,
				     valid_values, 4);
	KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_HANDLED);
	osf_test_expect_direct_sample(test, ctx, valid_values);
	KUNIT_ASSERT_EQ(test, ctx->indio_dev->scan_bytes, (int)sizeof(scan));
	KUNIT_ASSERT_EQ(test, osf_test_remove_scan(ctx, &scan), 0);
	KUNIT_EXPECT_EQ(test, scan.values[0], valid_values[0]);
	KUNIT_EXPECT_EQ(test, scan.values[1], valid_values[1]);
	KUNIT_EXPECT_EQ(test, scan.values[2], valid_values[2]);
	KUNIT_EXPECT_EQ(test, osf_test_remove_scan(ctx, &scan), -EBUSY);
}

static void osf_unregistered_sample_is_ignored_test(struct kunit *test)
{
	struct osf_test_context *ctx = test->priv;
	const s32 baseline_values[] = { 10, 20, 30 };
	const s32 ignored_values[] = { -10, -20, -30 };
	struct osf_latest_sample baseline;
	struct osf_test_scan_3axis scan;
	s32 value;
	int ret;

	KUNIT_ASSERT_EQ(test, osf_test_enable_buffer(ctx), 0);
	ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
				     OSF_TEST_SENSOR_INDEX, 3,
				     OSF_SAMPLE_FORMAT_S32,
				     baseline_values, 2);
	KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_HANDLED);
	KUNIT_ASSERT_EQ(test, osf_test_remove_scan(ctx, &scan), 0);
	baseline = ctx->osf.latest_samples[0];

	ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
				     OSF_TEST_SENSOR_INDEX + 1, 3,
				     OSF_SAMPLE_FORMAT_S32,
				     ignored_values, 3);
	KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_IGNORED);
	KUNIT_EXPECT_EQ(test, ctx->osf.iio_dev_count, 1U);
	KUNIT_EXPECT_EQ(test, ctx->osf.latest_sample_count, 1U);
	KUNIT_EXPECT_MEMEQ(test, &ctx->osf.latest_samples[0], &baseline,
			   sizeof(baseline));
	osf_test_expect_direct_sample(test, ctx, baseline_values);
	ret = osf_core_read_latest_sample(&ctx->osf,
					  OSF_SENSOR_ACCELEROMETER,
					  OSF_TEST_SENSOR_INDEX + 1,
					  0, &value);
	KUNIT_EXPECT_EQ(test, ret, -ENODATA);
	KUNIT_EXPECT_EQ(test, osf_test_remove_scan(ctx, &scan), -EBUSY);
}

static void
osf_unaccepted_samples_do_not_exhaust_cache_test(struct kunit *test)
{
	struct osf_test_context *ctx = test->priv;
	const s32 three_values[] = { 7, 8, 9 };
	const s32 one_value[] = { 42 };
	struct osf_latest_sample empty_cache[OSF_MAX_CAPABILITIES];
	struct osf_sensor_sample decoded_sample;
	struct osf_frame decoded_frame;
	u8 malformed_frame[OSF_TEST_MAX_FRAME_LEN];
	size_t decoded_len;
	size_t frame_len;
	int value;
	int ret;

	KUNIT_ASSERT_EQ(test, ctx->osf.latest_sample_count, 0U);
	memcpy(empty_cache, ctx->osf.latest_samples, sizeof(empty_cache));

	for (unsigned int i = 0; i < OSF_TEST_FLOOD_COUNT; i++) {
		ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
					     OSF_TEST_SENSOR_INDEX + i + 1,
					     3, OSF_SAMPLE_FORMAT_S32,
					     three_values, 100 + i);
		KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_IGNORED);
	}
	KUNIT_ASSERT_EQ(test, ctx->osf.latest_sample_count, 0U);

	for (unsigned int i = 0; i < OSF_TEST_FLOOD_COUNT; i++) {
		ret = osf_test_submit_sample(ctx, OSF_SENSOR_BAROMETER,
					     OSF_TEST_UNSUPPORTED_INDEX, 1,
					     OSF_SAMPLE_FORMAT_S32,
					     one_value, 200 + i);
		KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_IGNORED);
	}
	KUNIT_ASSERT_EQ(test, ctx->osf.latest_sample_count, 0U);

	for (unsigned int i = 0; i < OSF_TEST_FLOOD_COUNT; i++) {
		ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
					     OSF_TEST_SENSOR_INDEX, 1,
					     OSF_SAMPLE_FORMAT_S32,
					     one_value, 300 + i);
		KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_REJECTED);
	}
	KUNIT_ASSERT_EQ(test, ctx->osf.latest_sample_count, 0U);

	frame_len = osf_test_build_truncated_sample_frame(malformed_frame,
							  sizeof(malformed_frame), 400);
	KUNIT_ASSERT_NE(test, frame_len, (size_t)0);
	KUNIT_ASSERT_EQ(test,
			osf_protocol_decode_frame(malformed_frame, frame_len,
						  &decoded_frame, &decoded_len), 0);
	KUNIT_ASSERT_EQ(test, decoded_len, frame_len);
	KUNIT_ASSERT_EQ(test,
			osf_protocol_decode_sensor_sample(&decoded_frame,
							  &decoded_sample),
			-EMSGSIZE);

	for (unsigned int i = 0; i < OSF_TEST_FLOOD_COUNT; i++) {
		ret = osf_core_receive_frame(&ctx->osf, malformed_frame,
					     frame_len);
		KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_REJECTED);
	}
	KUNIT_ASSERT_EQ(test, ctx->osf.latest_sample_count, 0U);

	KUNIT_EXPECT_MEMEQ(test, ctx->osf.latest_samples, empty_cache,
			   sizeof(empty_cache));
	KUNIT_EXPECT_EQ(test, ctx->osf.last_sequence, (u64)1);

	ret = osf_test_submit_sample(ctx, OSF_SENSOR_ACCELEROMETER,
				     OSF_TEST_SENSOR_INDEX, 3,
				     OSF_SAMPLE_FORMAT_S32,
				     three_values, 500);
	KUNIT_ASSERT_EQ(test, ret, OSF_STREAM_FRAME_HANDLED);
	KUNIT_ASSERT_EQ(test, ctx->osf.latest_sample_count, 1U);

	for (unsigned int i = 0; i < 3; i++) {
		value = 0;
		ret = osf_test_read_raw(ctx, i, &value);
		KUNIT_ASSERT_EQ(test, ret, IIO_VAL_INT);
		KUNIT_EXPECT_EQ(test, value, three_values[i]);
	}
}

static struct kunit_case osf_core_test_cases[] = {
	KUNIT_CASE(osf_rejected_channel_count_preserves_latest_test),
	KUNIT_CASE(osf_valid_sample_updates_direct_and_buffer_test),
	KUNIT_CASE(osf_unregistered_sample_is_ignored_test),
	KUNIT_CASE(osf_unaccepted_samples_do_not_exhaust_cache_test),
	{ }
};

static struct kunit_suite osf_core_test_suite = {
	.name = "osf-core",
	.init = osf_test_init,
	.test_cases = osf_core_test_cases,
};

kunit_test_suite(osf_core_test_suite);

MODULE_LICENSE("GPL");
