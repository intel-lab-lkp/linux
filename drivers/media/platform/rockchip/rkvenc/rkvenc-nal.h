/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RKVENC_NAL_H
#define RKVENC_NAL_H

#include "rkvenc-vepu541.h"

int rkvenc_h264_headers_write(const struct rkvenc_h264_seq *seq, u8 *buf,
			      u32 size);
int rkvenc_hevc_headers_write(const struct rkvenc_hevc_seq *seq, u8 *buf,
			      u32 size);

#endif
