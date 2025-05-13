/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 */

#ifndef __AL_CODEC_COMMON__
#define __AL_CODEC_COMMON__

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <media/v4l2-device.h>

#include "al_codec_util.h"

#define fh_to_ctx(ptr, type) container_of(ptr, type, fh)

enum {
	MSG_ITF_TYPE_CREATE_INST_REQ = MSG_ITF_TYPE_NEXT_REQ,
	MSG_ITF_TYPE_DESTROY_INST_REQ,
	MSG_ITF_TYPE_PUSH_BITSTREAM_BUFFER_REQ,
	MSG_ITF_TYPE_PUT_DISPLAY_PICTURE_REQ,
	MSG_ITF_TYPE_FLUSH_REQ,
	MSG_ITF_TYPE_INFO_REQ,
	MSG_ITF_TYPE_CREATE_INST_REPLY = MSG_ITF_TYPE_NEXT_REPLY,
	MSG_ITF_TYPE_DESTROY_INST_REPLY,
	MSG_ITF_TYPE_PUSH_BITSTREAM_BUFFER_REPLY,
	MSG_ITF_TYPE_PUT_DISPLAY_PICTURE_REPLY,
	MSG_ITF_TYPE_FLUSH_REPLY,
	MSG_ITF_TYPE_INFO_REPLY,
	MSG_ITF_TYPE_EVT_ERROR = MSG_ITF_TYPE_NEXT_EVT,
};

struct msg_itf_write_req {
	u32 fd;
	u32 len;
	/* payload follow */
} __packed;
DECLARE_FULL_REQ(msg_itf_write_req);

struct msg_itf_free_mem_req {
	phys_addr_t phyAddr;
} __packed;
DECLARE_FULL_REQ(msg_itf_free_mem_req);

struct msg_itf_alloc_mem_req {
	u64 uSize;
} __packed;
DECLARE_FULL_REQ(msg_itf_alloc_mem_req);

struct msg_itf_alloc_mem_reply {
	phys_addr_t phyAddr;
} __packed;
DECLARE_FULL_REPLY(msg_itf_alloc_mem_reply);

struct msg_itf_free_mem_reply {
	s64 ret;
};
DECLARE_FULL_REPLY(msg_itf_free_mem_reply);

struct msg_itf_create_codec_reply {
	phys_addr_t hCodec;
	s32 ret;
} __packed;
DECLARE_FULL_REPLY(msg_itf_create_codec_reply);

struct msg_itf_destroy_codec_req {
	phys_addr_t hCodec;
} __packed;
DECLARE_FULL_REQ(msg_itf_destroy_codec_req);

/*
 * Note : no need to know the status of this request
 * The codec should be destroyed, in case of the mcu
 * hasn't received any request with the codec handler
 */
struct msg_itf_destroy_codec_reply {
	u32 unused;
} __packed;
DECLARE_FULL_REPLY(msg_itf_destroy_codec_reply);

struct al_buffer_meta {
	u64 timestamp;
	struct v4l2_timecode timecode;
	bool last;
};

struct msg_itf_push_src_buf_req {
	phys_addr_t hCodec;
	phys_addr_t bufferHandle;
	phys_addr_t phyAddr;
	u64 size;
	struct al_buffer_meta meta;
} __packed;
DECLARE_FULL_REQ(msg_itf_push_src_buf_req);

struct msg_itf_push_dst_buf_req {
	phys_addr_t hCodec;
	phys_addr_t bufferHandle;
	phys_addr_t phyAddr;
	u64 size;
} __packed;
DECLARE_FULL_REQ(msg_itf_push_dst_buf_req);

struct msg_itf_push_buffer_req {
	phys_addr_t hCodec;
	phys_addr_t bufferHandle;
	phys_addr_t phyAddr;
	u64 size;
} __packed;
DECLARE_FULL_REQ(msg_itf_push_buffer_req);

struct msg_itf_push_buffer_reply {
	s32 res;
} __packed;
DECLARE_FULL_REPLY(msg_itf_push_buffer_reply);

struct msg_itf_info_req {
	u64 unused;
} __packed;
DECLARE_FULL_REQ(msg_itf_info_req);

struct msg_itf_flush_req {
	phys_addr_t hCodec;
} __packed;
DECLARE_FULL_REQ(msg_itf_flush_req);

struct msg_itf_flush_reply {
	int32_t unused;
} __packed;
DECLARE_FULL_REPLY(msg_itf_flush_reply);

struct msg_itf_evt_error {
	uint32_t errno;
} __packed;
DECLARE_FULL_EVENT(msg_itf_evt_error);

struct al_match_data {
	const char *fw_name;
};

struct al_common_mcu_req {
	phys_addr_t pCtx;
	int req_type;
	size_t req_size;
	size_t reply_size;
	void *reply;
} __packed;

struct al_common_dev {
	struct platform_device *pdev;
	void *fw_cpu_mem;
	phys_addr_t fw_phys_addr;
	const char *fw_name;
	size_t fw_size;
	struct clk *mcu_clk;

	struct al_codec_mb mb_h2m;
	struct al_codec_mb mb_m2h;
	struct completion completion;

	struct resource regs_info;
	struct regmap *regmap;

	struct list_head alloc_buffers;
	struct mutex buf_lock;

	/* callbacks set by client before common_probe */
	void *cb_arg;
	void (*process_msg_cb)(void *cb_arg, struct msg_itf_header *hdr);
	void (*fw_ready_cb)(void *cb_arg);
};

struct al_codec_dev {
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct video_device video_dev;
	struct al_common_dev common;

	/* mutex protecting vb2_queue structure */
	struct mutex lock;

	/* list of ctx (aka decoder) */
	struct mutex ctx_mlock;
	struct list_head ctx_q_list;
	int is_video_init_done;

	struct completion res_done;
	/* list of cap/out supported formats */
	struct list_head codec_q_list;
	struct al_codec_cmd *codec_info_cmd;
};

static inline int al_common_get_header(struct al_common_dev *dev,
				       struct msg_itf_header *hdr)
{
	return al_codec_msg_get_header(&dev->mb_m2h, hdr);
}

static inline int al_common_get_data(struct al_common_dev *dev, char *data,
				     int len)
{
	return al_codec_msg_get_data(&dev->mb_m2h, data, len);
}

static inline int al_common_skip_data(struct al_common_dev *dev, int len)
{
	return al_common_get_data(dev, NULL, len);
}

int al_common_send(struct al_common_dev *dev, struct msg_itf_header *hdr);
int al_common_send_req_reply(struct al_codec_dev *dev,
			     struct list_head *cmd_list,
			     struct msg_itf_header *hdr,
			     struct al_common_mcu_req *req);
bool al_common_mcu_is_alive(struct al_codec_dev *dev);

int al_common_probe(struct platform_device *pdev, struct al_common_dev *dev);
void al_common_remove(struct al_common_dev *dev);

#endif /*__AL_CODEC_COMMON__*/
