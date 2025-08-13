/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 */

#ifndef __AL_VDEC_DRV__
#define __AL_VDEC_DRV__

#include <media/v4l2-ctrls.h>
#include <media/videobuf2-core.h>
#include <media/v4l2-mem2mem.h>

#include "al_codec_common.h"

enum {
	MSG_ITF_TYPE_EVT_RESOLUTION_FOUND = MSG_ITF_TYPE_NEXT_EVT + 1,
	MSG_ITF_TYPE_EVT_BITSTREAM_BUFFER_RELEASE,
	MSG_ITF_TYPE_EVT_FRAME_BUFFER_DECODE,
	MSG_ITF_TYPE_EVT_EOS,
	/* Mark the end of the events list.*/
	MSG_ITF_TYPE_END_EVT,
};

struct msg_itf_create_decoder_req {
	unsigned int codec;
} __packed;
DECLARE_FULL_REQ(msg_itf_create_decoder_req);

struct msg_itf_evt_resolution_found {
	u16 buffer_nb;
	u16 width;
	u16 height;
	u32 pixelformat;
	u32 sizeimage;
	u32 bytesperline;
} __packed;
DECLARE_FULL_EVENT(msg_itf_evt_resolution_found);

struct msg_itf_evt_bitstream_buffer_release {
	u64 bufferHandle;
} __packed;
DECLARE_FULL_EVENT(msg_itf_evt_bitstream_buffer_release);

struct msg_itf_evt_frame_buffer_decode {
	u64 bufferHandle;
	u64 size;
	struct al_buffer_meta meta;
} __packed;
DECLARE_FULL_EVENT(msg_itf_evt_frame_buffer_decode);

struct msg_itf_evt_eos {
	u32 unused;
} __packed;
DECLARE_FULL_EVENT(msg_itf_evt_eos);

struct al_fmt {
	u32 pixelformat;
	u8 bpp;
};

struct al_frame {
	u32 width;
	u32 height;
	u32 bytesperline;
	u32 sizeimage;
	u32 nbuffers;
	const struct al_fmt *fmt;
	enum v4l2_field field;
	enum v4l2_colorspace colorspace;
	enum v4l2_ycbcr_encoding ycbcr_enc;
	enum v4l2_quantization quantization;
	enum v4l2_xfer_func xfer_func;
};

struct al_dec_ctx {
	struct al_codec_dev *codec;
	struct v4l2_fh fh;
	struct v4l2_ctrl_handler ctrl_handler;
	struct kref refcount;
	struct list_head list;
	/* CAP and OUT frames */
	struct al_frame src;
	struct al_frame dst;
	struct completion res_done; /* Resolution found event */
	struct list_head cmd_q_list; /* Store active commands */
	struct mutex buf_q_mlock;
	struct list_head frame_q_list;
	struct list_head stream_q_list;
	u64 hDec;
	u32 csequence;
	u32 osequence;
	u64 id;
	bool stopped;
	bool aborting;
};

#endif /*__AL_VDEC_DRV__*/
