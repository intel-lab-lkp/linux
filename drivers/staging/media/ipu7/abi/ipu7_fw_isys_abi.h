/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 - 2025 Intel Corporation
 */

#ifndef IPU7_FW_ISYS_ABI_H
#define IPU7_FW_ISYS_ABI_H

#include "ipu7_fw_common_abi.h"
#include "ipu7_fw_isys_abi.h"

#define IPU_INSYS_MAX_OUTPUT_QUEUES	(3U)
#define IPU_INSYS_STREAM_ID_MAX		(16U)

#define IPU_INSYS_MAX_INPUT_QUEUES	(IPU_INSYS_STREAM_ID_MAX + 1U)
#define IPU_INSYS_OUTPUT_FIRST_QUEUE	(0U)
#define IPU_INSYS_OUTPUT_LAST_QUEUE	(IPU_INSYS_MAX_OUTPUT_QUEUES - 1U)
#define IPU_INSYS_OUTPUT_MSG_QUEUE	(IPU_INSYS_OUTPUT_FIRST_QUEUE)
#define IPU_INSYS_OUTPUT_LOG_QUEUE	(IPU_INSYS_OUTPUT_FIRST_QUEUE + 1U)
#define IPU_INSYS_OUTPUT_RESERVED_QUEUE	(IPU_INSYS_OUTPUT_LAST_QUEUE)
#define IPU_INSYS_INPUT_FIRST_QUEUE	(IPU_INSYS_MAX_OUTPUT_QUEUES)
#define IPU_INSYS_INPUT_DEV_QUEUE	(IPU_INSYS_INPUT_FIRST_QUEUE)
#define IPU_INSYS_INPUT_MSG_QUEUE	(IPU_INSYS_INPUT_FIRST_QUEUE + 1U)

typedef u64 ipu7_insys_return_token;

enum ipu7_insys_resp_type {
	IPU_INSYS_RESP_TYPE_STREAM_OPEN_DONE = 0,
	IPU_INSYS_RESP_TYPE_STREAM_START_AND_CAPTURE_ACK = 1,
	IPU_INSYS_RESP_TYPE_STREAM_CAPTURE_ACK = 2,
	IPU_INSYS_RESP_TYPE_STREAM_ABORT_ACK = 3,
	IPU_INSYS_RESP_TYPE_STREAM_FLUSH_ACK = 4,
	IPU_INSYS_RESP_TYPE_STREAM_CLOSE_ACK = 5,
	IPU_INSYS_RESP_TYPE_PIN_DATA_READY = 6,
	IPU_INSYS_RESP_TYPE_FRAME_SOF = 7,
	IPU_INSYS_RESP_TYPE_FRAME_EOF = 8,
	IPU_INSYS_RESP_TYPE_STREAM_START_AND_CAPTURE_DONE = 9,
	IPU_INSYS_RESP_TYPE_STREAM_CAPTURE_DONE = 10,
	N_IPU_INSYS_RESP_TYPE
};

enum ipu7_insys_send_type {
	IPU_INSYS_SEND_TYPE_STREAM_OPEN = 0,
	IPU_INSYS_SEND_TYPE_STREAM_START_AND_CAPTURE = 1,
	IPU_INSYS_SEND_TYPE_STREAM_CAPTURE = 2,
	IPU_INSYS_SEND_TYPE_STREAM_FLUSH = 4,
	IPU_INSYS_SEND_TYPE_STREAM_CLOSE = 5,
	N_IPU_INSYS_SEND_TYPE
};

enum ipu7_insys_frame_format_type {
	IPU_INSYS_FRAME_FORMAT_UYVY = 16,
	IPU_INSYS_FRAME_FORMAT_YUYV = 17,
	IPU_INSYS_FRAME_FORMAT_RAW8 = 20,
	IPU_INSYS_FRAME_FORMAT_RAW10 = 21,
	IPU_INSYS_FRAME_FORMAT_RAW12 = 22,
	IPU_INSYS_FRAME_FORMAT_RAW16 = 24,
	IPU_INSYS_FRAME_FORMAT_RGB565 = 25,
	IPU_INSYS_FRAME_FORMAT_RGBA888 = 27,
};

#define N_IPU_INSYS_MIPI_DATA_TYPE 0x40

enum ipu7_insys_mipi_dt_rename_mode {
	IPU_INSYS_MIPI_DT_NO_RENAME = 0,
};

#define IPU_INSYS_STREAM_SYNC_MSG_SEND_RESP_SOF			BIT(0)
#define IPU_INSYS_STREAM_SYNC_MSG_SEND_IRQ_SOF			BIT(2)
#define IPU_INSYS_STREAM_SYNC_MSG_SEND_RESP_SOF_DISCARDED	BIT(4)
#define IPU_INSYS_STREAM_SYNC_MSG_SEND_IRQ_SOF_DISCARDED	BIT(6)

#define IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_OPEN_DONE		BIT(0)
#define IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_OPEN_DONE		BIT(1)
#define IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_START_ACK		BIT(2)
#define IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_START_ACK		BIT(3)
#define IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_CLOSE_ACK		BIT(4)
#define IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_CLOSE_ACK		BIT(5)
#define IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_FLUSH_ACK		BIT(6)
#define IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_FLUSH_ACK		BIT(7)
#define IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_ABORT_ACK		BIT(8)
#define IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_ABORT_ACK		BIT(9)
#define IPU_INSYS_STREAM_ENABLE_MSG_SEND_RESP ( \
	IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_OPEN_DONE | \
	IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_START_ACK | \
	IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_CLOSE_ACK | \
	IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_FLUSH_ACK | \
	IPU_INSYS_STREAM_MSG_SEND_RESP_STREAM_ABORT_ACK)
#define IPU_INSYS_STREAM_ENABLE_MSG_SEND_IRQ ( \
	IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_OPEN_DONE | \
	IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_START_ACK | \
	IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_CLOSE_ACK | \
	IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_FLUSH_ACK | \
	IPU_INSYS_STREAM_MSG_SEND_IRQ_STREAM_ABORT_ACK)

#define IPU_INSYS_FRAME_MSG_SEND_RESP_CAPTURE_ACK		BIT(0)
#define IPU_INSYS_FRAME_MSG_SEND_IRQ_CAPTURE_ACK		BIT(1)
#define IPU_INSYS_FRAME_MSG_SEND_RESP_CAPTURE_DONE		BIT(2)
#define IPU_INSYS_FRAME_MSG_SEND_IRQ_CAPTURE_DONE		BIT(3)
#define IPU_INSYS_FRAME_MSG_SEND_RESP_PIN_DATA_READY		BIT(4)
#define IPU_INSYS_FRAME_MSG_SEND_IRQ_PIN_DATA_READY		BIT(5)
#define IPU_INSYS_FRAME_ENABLE_MSG_SEND_RESP ( \
	IPU_INSYS_FRAME_MSG_SEND_RESP_CAPTURE_ACK | \
	IPU_INSYS_FRAME_MSG_SEND_RESP_CAPTURE_DONE | \
	IPU_INSYS_FRAME_MSG_SEND_RESP_PIN_DATA_READY)
#define IPU_INSYS_FRAME_ENABLE_MSG_SEND_IRQ ( \
	IPU_INSYS_FRAME_MSG_SEND_IRQ_CAPTURE_ACK | \
	IPU_INSYS_FRAME_MSG_SEND_IRQ_CAPTURE_DONE | \
	IPU_INSYS_FRAME_MSG_SEND_IRQ_PIN_DATA_READY)

enum ipu7_insys_output_link_dest {
	IPU_INSYS_OUTPUT_LINK_DEST_MEM = 0,
};

enum ipu7_insys_send_queue_token_flag {
	IPU_INSYS_SEND_QUEUE_TOKEN_FLAG_NONE = 0,
};

#pragma pack(push, 1)
struct ipu7_insys_resolution {
	u32 width;
	u32 height;
};

struct ipu7_insys_capture_output_pin_payload {
	u64 user_token;
	ia_gofo_addr_t addr;
	u8 pad[4];
};

struct ipu7_insys_output_link {
	u32 buffer_lines;
	u16 foreign_key;
	u16 granularity_pointer_update;
	u8 msg_link_streaming_mode;
	u8 pbk_id;
	u8 pbk_slot_id;
	u8 dest;
	u8 use_sw_managed;
	u8 is_snoop;
	u8 pad[2];
};

struct ipu7_insys_output_cropping {
	u16 line_top;
	u16 line_bottom;
};

struct ipu7_insys_output_dpcm {
	u8 enable;
	u8 type;
	u8 predictor;
	u8 pad;
};

struct ipu7_insys_output_pin {
	struct ipu7_insys_output_link link;
	struct ipu7_insys_output_cropping crop;
	struct ipu7_insys_output_dpcm dpcm;
	u32 stride;
	u16 ft;
	u8 send_irq;
	u8 input_pin_id;
	u8 early_ack_en;
	u8 pad[3];
};

struct ipu7_insys_input_pin {
	struct ipu7_insys_resolution input_res;
	u16 sync_msg_map;
	u8 dt;
	u8 disable_mipi_unpacking;
	u8 dt_rename_mode;
	u8 mapped_dt;
	u8 pad[2];
};

struct ipu7_insys_stream_cfg {
	struct ipu7_insys_input_pin input_pins[4];
	struct ipu7_insys_output_pin output_pins[4];
	u16 stream_msg_map;
	u8 port_id;
	u8 vc;
	u8 nof_input_pins;
	u8 nof_output_pins;
	u8 pad[2];
};

struct ipu7_insys_buffset {
	struct ipu7_insys_capture_output_pin_payload output_pins[4];
	u8 capture_msg_map;
	u8 frame_id;
	u8 skip_frame;
	u8 pad[5];
};

struct ipu7_insys_resp {
	u64 buf_id;
	struct ipu7_insys_capture_output_pin_payload pin;
	struct ia_gofo_msg_err error_info;
	u32 timestamp[2];
	u8 type;
	u8 msg_link_streaming_mode;
	u8 stream_id;
	u8 pin_id;
	u8 frame_id;
	u8 skip_frame;
	u8 pad[2];
};

struct ipu7_insys_send_queue_token {
	u64 buf_handle;
	ia_gofo_addr_t addr;
	u16 stream_id;
	u8 send_type;
	u8 flag;
};

#pragma pack(pop)

enum insys_msg_err_capture {
	INSYS_MSG_ERR_CAPTURE_SYNC_FRAME_DROP = 10,
};

enum insys_msg_err_groups {
	INSYS_MSG_ERR_GROUP_CAPTURE = 3,
};

#endif
