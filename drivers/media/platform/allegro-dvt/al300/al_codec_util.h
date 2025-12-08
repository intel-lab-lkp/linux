/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 */

#ifndef __AL_CODEC_UTIL__
#define __AL_CODEC_UTIL__

#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/v4l2-common.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>

#define MB_IFT_MAGIC_H2M 0xabcd1230
#define MB_IFT_MAGIC_M2H 0xabcd1231
#define MB_IFT_VERSION 0x00010000

#define MAJOR_SHIFT 20
#define MAJOR_MASK 0xfff
#define MINOR_SHIFT 8
#define MINOR_MASK 0xfff
#define PATCH_SHIFT 0
#define PATCH_MASK 0xff

/*
 * AL_BOOT_VERSION() - Version format 32-bit, 12 bits for the major,
 * the same for minor, 8bits for the patch
 */
#define AL_BOOT_VERSION(major, minor, patch)       \
	((((major) & MAJOR_MASK) << MAJOR_SHIFT) | \
	 (((minor) & MINOR_MASK) << MINOR_SHIFT) | \
	 (((patch) & PATCH_MASK) << PATCH_SHIFT))

#define al_phys_to_virt(x) ((void *)(uintptr_t)x)
#define al_virt_to_phys(x) ((phys_addr_t)(uintptr_t)x)

#define DECLARE_FULL_REQ(s)                \
	struct s##_full {                  \
		struct msg_itf_header hdr; \
		struct s req;              \
	} __packed

#define DECLARE_FULL_REPLY(s)              \
	struct s##_full {                  \
		struct msg_itf_header hdr; \
		struct s reply;            \
	} __packed

#define DECLARE_FULL_EVENT(s)              \
	struct s##_full {                  \
		struct msg_itf_header hdr; \
		struct s event;            \
	} __packed

struct al_mb_itf {
	u32 magic;
	u32 version;
	u32 head;
	u32 tail;
} __packed;

struct al_codec_mb {
	struct al_mb_itf *hdr;
	struct mutex lock;
	char *data;
	int size;
};

struct al_codec_cmd {
	struct kref refcount;
	struct list_head list;
	struct completion done;
	int reply_size;
	void *reply;
};

#define al_codec_err(codec, fmt, args...)                                     \
	dev_err(&(codec)->pdev->dev, "[ALG_CODEC][ERROR] %s():%d: " fmt "\n", \
		__func__, __LINE__, ##args)

#define al_v4l2_err(codec, fmt, args...)                                     \
	dev_err(&(codec)->pdev->dev, "[ALG_V4L2][ERROR] %s():%d: " fmt "\n", \
		__func__, __LINE__, ##args)

#if defined(CONFIG_DEBUG_FS)
/* Log level */
extern int al_v4l2_dbg_level;
extern int al_codec_dbg;

/* V4L2 logs */
#define al_v4l2_dbg(codec, level, fmt, args...)                           \
	do {                                                              \
		if (al_v4l2_dbg_level >= level)                           \
			dev_dbg(&(codec)->pdev->dev,                      \
				"[ALG_V4L2] level=%d %s(),%d: " fmt "\n", \
				level, __func__, __LINE__, ##args);       \
	} while (0)

/* Codec logs */
#define al_codec_dbg(codec, fmt, args...)                                   \
	do {                                                                \
		if (al_codec_dbg)                                           \
			dev_dbg(&(codec)->pdev->dev,                        \
				"[ALG_CODEC] %s(),%d: " fmt "\n", __func__, \
				__LINE__, ##args);                          \
	} while (0)

#define al_mcu_dbg(codec, fmt, args...)                                   \
	do {                                                              \
		if (al_codec_dbg)                                         \
			dev_dbg(&(codec)->pdev->dev,                      \
				"[ALG_MCU] %s(),%d: " fmt "\n", __func__, \
				__LINE__, ##args);                        \
	} while (0)

#else

#define al_v4l2_dbg(codec, level, fmt, args...)                                \
	do {                                                                   \
		(void)level;                                                   \
		dev_dbg(&(codec)->pdev->dev, "[ALG_V4L2]: " fmt "\n", ##args); \
	} while (0)

#define al_codec_dbg(codec, fmt, args...) \
	dev_dbg(&(codec)->pdev->dev, "[ALG_CODEC]: " fmt "\n", ##args)

#define al_mcu_dbg(codec, fmt, args...) \
	dev_dbg(&(codec)->pdev->dev, "[ALG_MCU]: " fmt "\n", ##args)

#endif

#define MSG_ITF_TYPE_LIMIT BIT(10)

/* Message types host <-> mcu */
enum {
	MSG_ITF_TYPE_MCU_ALIVE = 0,
	MSG_ITF_TYPE_WRITE_REQ = 2,
	MSG_ITF_TYPE_FIRST_REQ = 1024,
	MSG_ITF_TYPE_NEXT_REQ,
	MSG_ITF_TYPE_FIRST_REPLY = 2048,
	MSG_ITF_TYPE_NEXT_REPLY,
	MSG_ITF_TYPE_ALLOC_MEM_REQ = 3072,
	MSG_ITF_TYPE_FREE_MEM_REQ,
	MSG_ITF_TYPE_ALLOC_MEM_REPLY = 4096,
	MSG_ITF_TYPE_FREE_MEM_REPLY,
	MSG_ITF_TYPE_FIRST_EVT = 5120,
	MSG_ITF_TYPE_NEXT_EVT = MSG_ITF_TYPE_FIRST_EVT
};

struct msg_itf_header {
	u64 drv_ctx_hdl;
	u64 drv_cmd_hdl;
	u16 type;
	u16 payload_len;
	u16 padding[2];
} __packed;

void al_codec_mb_init(struct al_codec_mb *mb, char *addr, int size, u32 magic);
int al_codec_msg_get_header(struct al_codec_mb *mb, struct msg_itf_header *hdr);
int al_codec_msg_get_data(struct al_codec_mb *mb, char *data, int len);
int al_codec_msg_send(struct al_codec_mb *mb, struct msg_itf_header *hdr,
		      void (*trigger)(void *), void *trigger_arg);

static inline bool is_type_reply(uint16_t type)
{
	return type >= MSG_ITF_TYPE_FIRST_REPLY &&
	       type < MSG_ITF_TYPE_FIRST_REPLY + MSG_ITF_TYPE_LIMIT;
}

static inline bool is_type_event(uint16_t type)
{
	return type >= MSG_ITF_TYPE_FIRST_EVT &&
	       type < MSG_ITF_TYPE_FIRST_EVT + MSG_ITF_TYPE_LIMIT;
}

void al_codec_cmd_put(struct al_codec_cmd *cmd);

struct al_codec_cmd *al_codec_cmd_create(int reply_size);

static inline struct al_codec_cmd *al_codec_cmd_get(struct list_head *cmd_list,
						    uint64_t hdl)
{
	struct al_codec_cmd *cmd = NULL;

	list_for_each_entry(cmd, cmd_list, list) {
		if (likely(cmd == al_phys_to_virt(hdl))) {
			kref_get(&cmd->refcount);
			break;
		}
	}
	return list_entry_is_head(cmd, cmd_list, list) ? NULL : cmd;
}

#endif /* __AL_CODEC_UTIL__ */
