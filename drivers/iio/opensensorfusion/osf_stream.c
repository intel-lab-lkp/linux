// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include "osf_core.h"
#include "osf_protocol.h"
#include "osf_stream.h"

#define OSF_STREAM_MAGIC_LEN	4
#define OSF_STREAM_MAX_PAYLOAD_LEN				\
	(OSF_STREAM_MAX_FRAME_LEN - OSF_FRAME_HEADER_LEN - OSF_FRAME_CRC_LEN)

static const u8 osf_stream_magic[OSF_STREAM_MAGIC_LEN] = {
	'O', 'S', 'F', '0',
};

static u16 osf_stream_get_le16(const u8 *buf)
{
	return buf[0] | buf[1] << 8;
}

static u32 osf_stream_get_le32(const u8 *buf)
{
	return (u32)buf[0] | (u32)buf[1] << 8 |
	       (u32)buf[2] << 16 | (u32)buf[3] << 24;
}

static void osf_stream_discard(struct osf_stream *stream, size_t count)
{
	if (count >= stream->len) {
		stream->len = 0;
		return;
	}

	memmove(stream->buf, stream->buf + count, stream->len - count);
	stream->len -= count;
}

static bool osf_stream_magic_match(const u8 *buf, size_t len)
{
	return !memcmp(buf, osf_stream_magic, len);
}

static size_t osf_stream_resync(struct osf_stream *stream)
{
	size_t old_len = stream->len;
	size_t match_len;
	size_t i;

	for (i = 0; i < stream->len; i++) {
		match_len = stream->len - i;
		if (match_len > OSF_STREAM_MAGIC_LEN)
			match_len = OSF_STREAM_MAGIC_LEN;

		if (osf_stream_magic_match(stream->buf + i, match_len)) {
			if (i)
				osf_stream_discard(stream, i);
			return i;
		}
	}

	stream->len = 0;
	return old_len;
}

static int osf_stream_process(struct osf_stream *stream)
{
	struct osf_frame frame;
	size_t decoded_len;
	size_t discarded;
	size_t frame_len;
	u32 payload_len;
	int first_err = 0;
	int ret;

	while (stream->len) {
		discarded = osf_stream_resync(stream);
		if (discarded) {
			stream->stats.bad_magic_resyncs++;
			stream->stats.dropped_bytes += discarded;
			if (!first_err)
				first_err = -EPROTO;
		}

		if (!stream->len)
			break;

		if (stream->len < OSF_FRAME_HEADER_LEN) {
			stream->stats.partial_frames++;
			break;
		}

		if (osf_stream_get_le16(stream->buf + 6) !=
		    OSF_FRAME_HEADER_LEN) {
			stream->stats.dropped_bytes++;
			osf_stream_discard(stream, 1);
			if (!first_err)
				first_err = -EPROTO;
			continue;
		}

		payload_len = osf_stream_get_le32(stream->buf + 10);
		if (payload_len > OSF_STREAM_MAX_PAYLOAD_LEN) {
			stream->stats.dropped_bytes++;
			osf_stream_discard(stream, 1);
			if (!first_err)
				first_err = -EMSGSIZE;
			continue;
		}

		frame_len = OSF_FRAME_HEADER_LEN + payload_len + OSF_FRAME_CRC_LEN;
		if (stream->len < frame_len) {
			stream->stats.partial_frames++;
			break;
		}

		ret = osf_protocol_decode_frame(stream->buf, frame_len, &frame,
						&decoded_len);
		if (ret) {
			if (ret == -EBADMSG)
				stream->stats.bad_crc_frames++;
			stream->stats.dropped_bytes++;
			osf_stream_discard(stream, 1);
			if (!first_err)
				first_err = ret;
			continue;
		}

		if (decoded_len != frame_len) {
			stream->stats.dropped_bytes++;
			osf_stream_discard(stream, 1);
			if (!first_err)
				first_err = -EMSGSIZE;
			continue;
		}

		ret = osf_core_receive_frame(stream->osf, stream->buf, frame_len);
		if (ret) {
			osf_stream_discard(stream, frame_len);
			if (!first_err)
				first_err = ret;
			continue;
		}

		stream->stats.valid_frames++;
		osf_stream_discard(stream, frame_len);
	}

	return first_err;
}

void osf_stream_init(struct osf_stream *stream, struct osf_device *osf)
{
	if (!stream)
		return;

	stream->osf = osf;
	stream->len = 0;
	memset(&stream->stats, 0, sizeof(stream->stats));
}

void osf_stream_reset(struct osf_stream *stream)
{
	if (stream) {
		stream->len = 0;
		memset(&stream->stats, 0, sizeof(stream->stats));
	}
}

int osf_stream_receive_bytes(struct osf_stream *stream, const u8 *buf,
			     size_t len)
{
	size_t copy_len;
	size_t space;
	int first_err = 0;
	int ret;

	if (!stream || !stream->osf || (!buf && len))
		return -EINVAL;

	if (!len) {
		ret = osf_stream_process(stream);
		if (ret && !first_err)
			first_err = ret;
		return first_err;
	}

	while (len) {
		space = OSF_STREAM_MAX_FRAME_LEN - stream->len;
		if (!space) {
			stream->stats.dropped_bytes++;
			osf_stream_discard(stream, 1);
			if (!first_err)
				first_err = -EMSGSIZE;
			continue;
		}

		copy_len = len < space ? len : space;
		memcpy(stream->buf + stream->len, buf, copy_len);
		stream->len += copy_len;
		buf += copy_len;
		len -= copy_len;

		ret = osf_stream_process(stream);
		if (ret && !first_err)
			first_err = ret;
	}

	return first_err;
}
