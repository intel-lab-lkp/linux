/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OSF_STREAM_H
#define _OSF_STREAM_H

#include <linux/types.h>

#define OSF_STREAM_MAX_FRAME_LEN	4096

struct osf_device;

struct osf_stream_stats {
	u64 valid_frames;
	u64 bad_magic_resyncs;
	u64 bad_crc_frames;
	u64 partial_frames;
	u64 dropped_bytes;
};

struct osf_stream {
	struct osf_device *osf;
	u8 buf[OSF_STREAM_MAX_FRAME_LEN];
	size_t len;
	struct osf_stream_stats stats;
};

void osf_stream_init(struct osf_stream *stream, struct osf_device *osf);
void osf_stream_reset(struct osf_stream *stream);
int osf_stream_receive_bytes(struct osf_stream *stream, const u8 *buf,
			     size_t len);

#endif
