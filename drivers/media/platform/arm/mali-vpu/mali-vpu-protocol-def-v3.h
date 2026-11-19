/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_PROTOCOL_DEF_V3_H__
#define __MALI_VPU_PROTOCOL_DEF_V3_H__

/*****************************************************************************
 *
 * Communication protocol between the host/driver and the MVE firmware,
 * the 'host interface'.
 *
 * MVE == MALI Video Engine
 *
 * Protocol version 3.1
 *
 * Note: Message structs may be expanded in the future; the host should
 * use the 'size' of the message to determine how many bytes to
 * read from the message queue, rather than a sizeof(struct).
 *
 * All multibyte integer fields in the shared-memory interface are little-endian.
 * The driver supports only little-endian kernels and therefore accesses these
 * fields in native byte order.
 *
 ****************************************************************************/

#define MALI_VPU_HOST_PROTOCOL_MAJOR 3
#define MALI_VPU_HOST_PROTOCOL_MINOR 1

/*****************************************************************************
 *
 * Virtual memory regions
 *
 * ..._ADDR_BEGIN gives the starting virtual address of the region,
 * and ..._ADDR_END the (non-inclusive) ending address, such that
 * the size of the region is obtained with the subtraction
 * (..._ADDR_END - ..._ADDR_BEGIN).
 *
 ****************************************************************************/

/* Memory region for first firmware instance */
#define MVE_MEM_REGION_FW_INSTANCE0_ADDR_BEGIN	(0x00000000u)
#define MVE_MEM_REGION_FW_INSTANCE0_ADDR_END	(0x000FFFFFu + 1)

/* Memory regions for other firmware instances */
#define MVE_MEM_REGION_FW_INSTANCE1_ADDR_BEGIN	(0x01000000u)
#define MVE_MEM_REGION_FW_INSTANCE1_ADDR_END \
	(MVE_MEM_REGION_FW_INSTANCE1_ADDR_BEGIN + MVE_MEM_REGION_FW_INSTANCE0_ADDR_END)

#define MVE_MEM_REGION_FW_INSTANCE2_ADDR_BEGIN (0x02000000u)
#define MVE_MEM_REGION_FW_INSTANCE2_ADDR_END \
	(MVE_MEM_REGION_FW_INSTANCE2_ADDR_BEGIN + MVE_MEM_REGION_FW_INSTANCE0_ADDR_END)

#define MVE_MEM_REGION_FW_INSTANCE3_ADDR_BEGIN (0x03000000u)
#define MVE_MEM_REGION_FW_INSTANCE3_ADDR_END \
	(MVE_MEM_REGION_FW_INSTANCE3_ADDR_BEGIN + MVE_MEM_REGION_FW_INSTANCE0_ADDR_END)

#define MVE_MEM_REGION_FW_INSTANCE4_ADDR_BEGIN (0x04000000u)
#define MVE_MEM_REGION_FW_INSTANCE4_ADDR_END \
	(MVE_MEM_REGION_FW_INSTANCE4_ADDR_BEGIN + MVE_MEM_REGION_FW_INSTANCE0_ADDR_END)

#define MVE_MEM_REGION_FW_INSTANCE5_ADDR_BEGIN (0x05000000u)
#define MVE_MEM_REGION_FW_INSTANCE5_ADDR_END \
	(MVE_MEM_REGION_FW_INSTANCE5_ADDR_BEGIN + MVE_MEM_REGION_FW_INSTANCE0_ADDR_END)

#define MVE_MEM_REGION_FW_INSTANCE6_ADDR_BEGIN (0x06000000u)
#define MVE_MEM_REGION_FW_INSTANCE6_ADDR_END \
	(MVE_MEM_REGION_FW_INSTANCE6_ADDR_BEGIN + MVE_MEM_REGION_FW_INSTANCE0_ADDR_END)

#define MVE_MEM_REGION_FW_INSTANCE7_ADDR_BEGIN (0x07000000u)
#define MVE_MEM_REGION_FW_INSTANCE7_ADDR_END \
	(MVE_MEM_REGION_FW_INSTANCE7_ADDR_BEGIN + MVE_MEM_REGION_FW_INSTANCE0_ADDR_END)

/*
 * Areas for communication between host and MVE are placed in the interval
 * 0x10079000 - 0x1007FFFF, see special defines further down.
 */

/* PROTECTED virtual memory region */
#define MVE_MEM_REGION_PROTECTED_ADDR_BEGIN	(0x20000000u)
#define MVE_MEM_REGION_PROTECTED_ADDR_END	(0x6FFFFFFFu + 1)

/* FRAMEBUF virtual memory region */
#define MVE_MEM_REGION_FRAMEBUF_ADDR_BEGIN	(0x70000000u)
#define MVE_MEM_REGION_FRAMEBUF_ADDR_END	(0xEFFFFFFFu + 1)

/* 0xF0000000 - 0xFFFFFFFF is used internally in MVE */

/*****************************************************************************
 *
 * Communication queues between HOST/DRIVER and MVE
 *
 ****************************************************************************/

/*
 * Address for queue for messages in to MVE,
 * one struct mve_comm_area_host located here
 */
#define MVE_COMM_MSG_INQ_ADDR	(0x10079000u)

/*
 * Address for queue for messages out from MVE,
 * one struct mve_comm_area_mve located here
 */
#define MVE_COMM_MSG_OUTQ_ADDR	(0x1007A000u)

/*
 * Address for queue for input buffers in to MVE,
 * one struct mve_comm_area_host located here
 */
#define MVE_COMM_BUF_INQ_ADDR	(0x1007B000u)

/*
 * Address for queue for input buffers returned from MVE,
 * one struct mve_comm_area_mve located here
 */
#define MVE_COMM_BUF_INRQ_ADDR	(0x1007C000u)

/*
 * Address for queue for output buffers in to MVE,
 * one struct mve_comm_area_host located here
 */
#define MVE_COMM_BUF_OUTQ_ADDR	(0x1007D000u)

/*
 * Address for queue for output buffers returned from MVE,
 * one struct mve_comm_area_mve located here
 */
#define MVE_COMM_BUF_OUTRQ_ADDR	(0x1007E000u)

/* One struct mve_rpc_communication_area located here */
#define MVE_COMM_RPC_ADDR	(0x1007F000u)

/*
 * One page of memory (4 kB) is used for each queue,
 * so maximum 1024 words, but need room for some counters as well,
 * see structs mve_comm_area_mve and mve_comm_area_host below.
 */
#define MVE_COMM_QUEUE_SIZE_IN_WORDS 1020

/* This is the part of the message area that is written by host. */
struct mve_comm_area_host {
	u16 out_rpos;
	u16 in_wpos;
	u32 reserved[3];
	/*
	 * Queue of messages to MVE, each block of data prefixed with
	 * a mve_msg_header
	 */
	u32 in_data[MVE_COMM_QUEUE_SIZE_IN_WORDS];
};

/* This is the part of the message area that is written by MVE. */
struct mve_comm_area_mve {
	u16 out_wpos;
	u16 in_rpos;
	u32 reserved[3];
	/*
	 * Queue of messages to host, each block of data prefixed with
	 * a mve_msg_header
	 */
	u32 out_data[MVE_COMM_QUEUE_SIZE_IN_WORDS];
};

#define MVE_RPC_AREA_SIZE_IN_WORDS 256
#define MVE_RPC_DATA_SIZE_IN_WORDS (MVE_RPC_AREA_SIZE_IN_WORDS - 3)
union mve_rpc_params {
	u32 data[MVE_RPC_DATA_SIZE_IN_WORDS];
	struct {
		char string[MVE_RPC_DATA_SIZE_IN_WORDS * 4];
	} debug_print;
	struct {
		u32 size;
		u32 max_size;
		u8 region; /* Memory region selection */
			#define MVE_MEM_REGION_PROTECTED (0)
			#define MVE_MEM_REGION_OUTBUF    (1)
			#define MVE_MEM_REGION_FRAMEBUF  (MVE_MEM_REGION_OUTBUF)

		/* The newly allocated memory must be placed
		 * on (at least) a 2^(log2_alignment) boundary
		 */
		u8 log2_alignment;
	} mem_alloc;
	struct {
		u32 ve_pointer;
		u32 new_size;
	} mem_resize;
	struct {
		u32 ve_pointer;
	} mem_free;
};

struct mve_rpc_communication_area {
	u32 state;
		#define MVE_RPC_STATE_FREE   (0)
		#define MVE_RPC_STATE_PARAM  (1)
		#define MVE_RPC_STATE_RETURN (2)
	u32 call_id;
		#define MVE_RPC_FUNCTION_DEBUG_PRINTF (1)
		#define MVE_RPC_FUNCTION_MEM_ALLOC    (2)
		#define MVE_RPC_FUNCTION_MEM_RESIZE   (3)
		#define MVE_RPC_FUNCTION_MEM_FREE     (4)
	u32 size;
	union mve_rpc_params params;
};

/*********************************************************************
 *
 *   Message codes
 *
 *********************************************************************/

/*
 * Messages consist of one struct mve_msg_header, possibly followed
 * by extra data.
 */
struct mve_msg_header {
	u16 code;
		/*
		 * REQUESTs are messages from the
		 * host/driver to the firmware:               Code:     Extra data in message:
		 */
		#define MVE_REQUEST_CODE_GO                   (1001) /* no extra data */
		#define MVE_REQUEST_CODE_STOP                 (1002) /* no extra data */
		#define MVE_REQUEST_CODE_INPUT_FLUSH          (1003) /* no extra data */
		#define MVE_REQUEST_CODE_OUTPUT_FLUSH         (1004) /* no extra data */
		#define MVE_REQUEST_CODE_PING                 (1006) /* no extra data */
		#define MVE_REQUEST_CODE_JOB                  (1009) /* struct mve_request_job */
		#define MVE_REQUEST_CODE_SET_OPTION           (1010) /* struct
								      * mve_request_set_option
								      * (variable size)
								      */
		/*
		 * RESPONSEs are messages from
		 * the firmware to the host:
		 */
		/* struct mve_response_switched_in */
		#define MVE_RESPONSE_CODE_SWITCHED_IN         (2001)
		/* struct mve_response_switched_out */
		#define MVE_RESPONSE_CODE_SWITCHED_OUT        (2002)
		#define MVE_RESPONSE_CODE_SET_OPTION_CONFIRM  (2003)
		/* struct mve_response_job_dequeued */
		#define MVE_RESPONSE_CODE_JOB_DEQUEUED        (2004)
		/* no extra data, but buffer placed in buffer queue */
		#define MVE_RESPONSE_CODE_INPUT               (2005)
		/* no extra data, but buffer placed in buffer queue */
		#define MVE_RESPONSE_CODE_OUTPUT              (2006)
		#define MVE_RESPONSE_CODE_INPUT_FLUSHED       (2007)
		#define MVE_RESPONSE_CODE_OUTPUT_FLUSHED      (2008)
		#define MVE_RESPONSE_CODE_PONG                (2009)
		/* struct mve_response_error */
		#define MVE_RESPONSE_CODE_ERROR               (2010)
		/* struct mve_response_state_change */
		#define MVE_RESPONSE_CODE_STATE_CHANGE        (2011)
		#define MVE_RESPONSE_CODE_IDLE                (2013)
		/* struct mve_response_frame_alloc_parameters */
		#define MVE_RESPONSE_CODE_FRAME_ALLOC_PARAM   (2014)
		/* struct mve_response_sequence_parameters */
		#define MVE_RESPONSE_CODE_SEQUENCE_PARAMETERS (2015)
		/* struct mve_response_event (variable size) */
		#define MVE_RESPONSE_CODE_EVENT               (2016)
		/* struct mve_response_set_option_fail */
		#define MVE_RESPONSE_CODE_SET_OPTION_FAIL     (2017)

		/* BUFFERs are sent from host to firmware,
		 * and then return at some time:
		 */
		/* struct mve_buffer_frame */
		#define MVE_BUFFER_CODE_FRAME                 (3001)
		/* struct mve_buffer_bitstream */
		#define MVE_BUFFER_CODE_BITSTREAM             (3002)
		/* struct mve_buffer_param */
		#define MVE_BUFFER_CODE_PARAM                 (3003)

	u16 size; /* size in bytes of trailing data, 0 if none */
};

/*********************************************************************
 *
 *   REQUESTs are messages from the host to the firmware
 *
 *   Some of the MVE_REQUEST_CODE_ codes are followed by one of the
 *   structs below.
 *
 *********************************************************************/

struct mve_request_job {
	u16 cores; /* >= 1, number of cores to use, must match request to HW scheduler */
	u16 frames; /* number of frames to process, zero means infinite */
	u32 flags; /* can be zero */
		#define MVE_JOB_FLAG_DISABLE_BNDMGR (0x01)
};

struct mve_request_set_option {
	u32 index;
		#define MVE_SET_OPT_INDEX_NALU_FORMAT              (1)  /* see arg,
									 * MVE_OPT_NALU_FORMAT_
									 */
		#define MVE_SET_OPT_INDEX_STREAM_ESCAPING          (2)  /* arg=1 to enable (def),
									 * arg=0 to disable
									 */
		#define MVE_SET_OPT_INDEX_HOST_PROTOCOL_PRINTS     (4)  /* arg=1 to enable,
									 * arg=0 to disable (def)
									 */
		#define MVE_SET_OPT_INDEX_IGNORE_STREAM_HEADERS    (7)  /* decode, arg=1 to enable,
									 * arg=0 to disable (def)
									 */
		#define MVE_SET_OPT_INDEX_FRAME_REORDERING         (8)  /* decode arg=1 enable(def),
									 * arg=0 to disable
									 */
		#define MVE_SET_OPT_INDEX_INTBUF_SIZE              (9)  /* decode, arg = suggested
									 * limit of intermediate
									 * buffer allocation
									 */
	union {
		u32 arg; /* Most options only need a u32 as argument */
		/*
		 * For option MVE_SET_OPT_INDEX_NALU_FORMAT, arg should
		 * be one of these:
		 */
		#define MVE_OPT_NALU_FORMAT_START_CODES             (1)
		#define MVE_OPT_NALU_FORMAT_ONE_NALU_PER_BUFFER     (2)
		#define MVE_OPT_NALU_FORMAT_ONE_BYTE_LENGTH_FIELD   (4)
		#define MVE_OPT_NALU_FORMAT_TWO_BYTE_LENGTH_FIELD   (8)
		#define MVE_OPT_NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD  (16)
		#define MVE_OPT_NALU_FORMAT_MBINFO                  (32) /* debugging only */
	} data;
};

/*********************************************************************
 *
 *   RESPONSEs are messages from the firmware to the host
 *
 *   Some of the MVE_RESPONSE_CODE_ codes are followed by one of the
 *   structs below.
 *
 *********************************************************************/

/* Sent when firmware has booted. */
struct mve_response_switched_in {
	u32 core;
};

/* Sent when last core in a session has switched out. */
struct mve_response_switched_out {
	u32 core;
	u32 reason;
	u32 sub_reason;
};

/*
 * Response confirming state transition after either GO or STOP
 * command from host.
 */
struct mve_response_state_change {
	u32 new_state;
		#define MVE_STATE_STOPPED (0)
		#define MVE_STATE_RUNNING (2)
};

/*
 * Message sent when all cores in the session have dequeued a
 * job from the firmware job queue.
 */
struct mve_response_job_dequeued {
	u32 valid_job;
};

/* Decode only: This message is sent from MVE to the host so that it can
 * allocate large enough output buffers. Output buffers that are to small
 * will be returned to the host marked as 'rejected'.
 */
struct mve_response_frame_alloc_parameters {
	/* Please note that the below information is a hint
	 * for what buffers to allocate, it does not say
	 * what actual resolution an output picture has.
	 */

	/* To use if allocating PLANAR YUV output buffers: */
	u16 planar_alloc_frame_width;
	u16 planar_alloc_frame_height;
	u32 afbc_alloc_bytes;
	u32 afbc_alloc_bytes_downscaled;
	u16 afbc_width_in_superblocks;
	u16 afbc_width_in_superblocks_downscaled;

	/* For PLANAR YUV output, every plane's address need to be adjusted to get
	 * optimal AXI bursts when the pixel data is written, the values below may
	 * be used to calculate address offsets.
	 */
	u16 cropx;
	u16 cropy;

	u32 mbinfo_alloc_bytes; /* Only for debugging */
};

/* Decode only: This message is sent from MVE to the host so that it can
 * allocate suitable output buffers. The needed size of the buffer is sent
 * in a separate message (above).
 * When MVE sends the message below, it enters a waiting-state and will not
 * make any progress until the host sends an output-flush command, upon
 * which MVE will return all output buffers, followed by a message saying
 * that the output has been flushed. Only then should the host start
 * enqueueing new output buffers.
 */
struct mve_response_sequence_parameters {
	/* Other stream parameters affecting buffer allocation,
	 * any change in these values will trigger a flush.
	 */
	u8 interlace;              /* 0 or 1 */
	u8 chroma_format;
		#define MVE_CHROMA_FORMAT_MONO (0x0)
		#define MVE_CHROMA_FORMAT_420  (0x1)
		#define MVE_CHROMA_FORMAT_422  (0x2)
		#define MVE_CHROMA_FORMAT_440  (0x3)
		#define MVE_CHROMA_FORMAT_ARGB (0x4)
	u8 bitdepth_luma;          /* 8, 9 or 10 */
	u8 bitdepth_chroma;        /* 8, 9 or 10 */
	u8 num_buffers_planar;     /* number of planar buffers needed */
	u8 num_buffers_afbc;
	u8 range_mapping_enabled;
	u8 reserved0;
};

/*
 * Fatal error message from firmware, if sent then no further
 * operation is possible.
 */
struct mve_response_error {
	u32 error_code;
		#define MVE_ERROR_ABORT          (1)
		#define MVE_ERROR_OUT_OF_MEMORY  (2)
		#define MVE_ERROR_ASSERT         (3)
		#define MVE_ERROR_UNSUPPORTED    (4)
		#define MVE_ERROR_INVALID_BUFFER (6)
		#define MVE_ERROR_INVALID_STATE  (8)
		#define MVE_ERROR_WATCHDOG       (9)

		#define MVE_MAX_ERROR_MESSAGE_SIZE  (128)
	char message[MVE_MAX_ERROR_MESSAGE_SIZE];
};

/*
 * Per-frame processing statistics emitted when firmware profiling is
 * enabled.
 */
struct mve_event_processed {
	u8 pic_format;
	u8 qp;
	u8 pad0;
	u8 pad1;
	u32 parse_start_time;	/* Timestamp, absolute time */
	u32 parse_end_time;	/* Timestamp, absolute time */
	u32 parse_idle_time;	/* Waiting for in/out buffers or available RAM */
	u32 pipe_start_time;	/* Timestamp */
	u32 pipe_end_time;	/* Timestamp, end-start = process time. Idle time while in a frame
				 * is not measured.
				 */
	u32 pipe_idle_time;	/* Always 0 in decode, */
	u32 parser_coreid;	/* Core used to parse this frame */
	u32 pipe_coreid;	/* Core used to pipe this frame */
	u32 bitstream_bits;	/* Number of bitstream bits used for this frame. */
	u32 intermediate_buffer_size;	/* Size of intermediate (mbinfo/residuals) buffer after this
					 * frame was parsed.
					 */
	u32 total_memory_allocated;	/* after the frame was parsed. Including reference frames */
	u32 bus_read_bytes;		/* bus read bytes */
	u32 bus_write_bytes;		/* bus written bytes */
	u32 afbc_bytes;
	u32 slice0_end_time;		/* Timestamp, absolute time */
	u32 stream_start_time;		/* Timestamp, absolute stream start time */
	u32 stream_open_time;		/* Timestamp, absolute stream open time */
};

/*
 * Encoder reference-frame metadata emitted when reference-frame exposure
 * is enabled.
 */
struct mve_event_ref_frame {
	u32 ref_width;		/* Width of display area in luma pixels */
	u32 ref_height;		/* Height of display area in luma pixels */
	u32 ref_mb_width;	/* Width in macroblocks */
	u32 ref_mb_height;	/* Height in macroblocks */
	u32 ref_left_crop;	/* Left crop in luma pixels */
	u32 ref_top_crop;	/* Top crop in luma pixels */
	u32 ref_frame_size;
	u32 ref_display_order;
	u32 ref_addr;
	u16 bit_width;		/* bit width of the YUV either 8 or 10 */
	u16 tiled_headers;
};

/*
 * Trace-buffer allocations emitted by firmware build with event tracing
 * enabled.
 */
struct mve_event_trace_buffers {
	u16 reserved;
	u8 num_cores;
	u8 rasc_mask;
		#define MVE_MAX_TRACE_BUFFERS 40
	/* this array will contain one buffer per rasc in rasc_mask per num_core */
	struct {
		u32 rasc_addr;	/* rasc address of the buffer */
		u32 size;	/* size of the buffer in bytes */
	} buffers[MVE_MAX_TRACE_BUFFERS];
};

/*
 * Firmware event response. Profiling and tracing events are informational,
 * while stream-error events may require host action.
 */
struct mve_response_event {
	u32 event_code;
		#define MVE_EVENT_ERROR_STREAM_CORRUPT		(1) /* message, text string */
		#define MVE_EVENT_ERROR_STREAM_NOT_SUPPORTED	(2) /* message, text string */
		#define MVE_EVENT_PROCESSED			(3) /* struct mve_event_processed */
		#define MVE_EVENT_REF_FRAME			(4) /* struct mve_event_ref_frame */
		#define MVE_EVENT_TRACE_BUFFERS			(5)
	union {
		struct mve_event_processed event_processed;
		struct mve_event_ref_frame event_ref_frame;
		struct mve_event_trace_buffers event_trace_buffers;
		char message[MVE_MAX_ERROR_MESSAGE_SIZE];
	} event_data;
};

/*
 * When a set-option succeeds, a confirmation message is
 * sent, including the index-code for that particular option.
 */
struct mve_response_set_option_confirm {
	u32 index; /* Same as 'index' in struct mve_request_set_option */
};

/*
 * If a set-option request fails, this message is returned.
 * This is not a fatal error. The set-option had no effect,
 * and the session is still alive.
 * For example, trying to set an option with a too large
 * or small parameter would result in this message.
 * The included text string is meant for development and
 * debugging purposes only.
 * (When a set-option succeeds the set-option-confirm
 * message code is sent instead.)
 */
struct mve_response_set_option_fail {
	u32 index; /* Same as 'index' in struct mve_request_set_option */
	char message[MVE_MAX_ERROR_MESSAGE_SIZE];
};

 #define MVE_CHROMA_FORMAT_420 (0x1)

/* format bitfield: */
#define MVE_FORMAT_BF_C  (0) /* 3 bits, chroma subsampling   */
#define MVE_FORMAT_BF_B  (4) /* 4 bits, max bitdepth minus 8 */
#define MVE_FORMAT_BF_N  (8) /* 2 bits, number of planes     */
#define MVE_FORMAT_BF_V (12) /* 2 bits, format variant       */
/* formats: */
#define MVE_FORMAT_YUV420_I420	((MVE_CHROMA_FORMAT_420  << MVE_FORMAT_BF_C) | \
				((8 - 8) << MVE_FORMAT_BF_B) | \
				(3 << MVE_FORMAT_BF_N) | \
				(0 << MVE_FORMAT_BF_V))

#define MVE_FORMAT_YUV420_NV12	((MVE_CHROMA_FORMAT_420  << MVE_FORMAT_BF_C) | \
				((8 - 8) << MVE_FORMAT_BF_B) | \
				(2 << MVE_FORMAT_BF_N) | \
				(0 << MVE_FORMAT_BF_V))

#define MVE_FORMAT_YUV420_NV21	((MVE_CHROMA_FORMAT_420  << MVE_FORMAT_BF_C) | \
				((8 - 8) << MVE_FORMAT_BF_B) | \
				(2 << MVE_FORMAT_BF_N) | \
				(1 << MVE_FORMAT_BF_V))

/*********************************************************************
 *
 *   BUFFERs are sent both ways, from host to firmware and back again
 *
 *   Each MVE_BUFFER_CODE_ code is followed by one of the structs
 *   below.
 *
 *********************************************************************/

/* Flags in mve_buffer_frame::frame_flags:
 *                                     Set by whom?    Meaning:
 *                                     DECODE:
 * MVE_BUFFER_FRAME_FLAG_REJECTED       fw             Buffer was too small, host should re-allocate
 *
 * MVE_BUFFER_FRAME_FLAG_CORRUPT        fw             Frame contains visual corruption
 *
 * MVE_BUFFER_FRAME_FLAG_EOS            fw             This is the last frame in the stream.
 */

/* mve_buffer_frame_planar stores uncompressed YUV pictures.
 *       ________________________________________
 *      |  ^                          |          |  ^
 *      |<-:--visible_frame_width---->|          |  :
 *      |  :                          |          |  :
 *      |  :                          |          |  :
 *      | visible_frame_height        |          | max_frame_height
 *      |  :                          |          |  :
 *      |  :                          |          |  :
 *      |__v__________________________|          |  :
 *      |                                        |  :
 *      |<-------------max_frame_width---------->|  :
 *      |________________________________________|  v
 *
 */
struct mve_buffer_frame_planar {
	/* Y,Cb,Cr top field */
	u32 plane_top[3];

	/* Y,Cb,Cr bottom field (interlace only) */
	u32 plane_bot[3];

	/* Stride between rows, in bytes */
	s32 stride[3];

	/* Size of largest frame allowed to put in this buffer */
	u16 max_frame_width;
	u16 max_frame_height;
};

/* mve_buffer_frame stores PLANAR buffer information. */
struct mve_buffer_frame {
	/* For identification of the buffer, this is not changed by
	 * the firmware.
	 */
	u64 host_handle;

	/* For matching input buffer with output buffers, the firmware
	 * copies these values between frame buffers and bitstream buffers.
	 */
	u64 user_data_tag;

	/* Frame buffer flags, see commentary above */
	u32 frame_flags;
		#define MVE_BUFFER_FRAME_FLAG_REJECTED		(0x00001000)
		#define MVE_BUFFER_FRAME_FLAG_CORRUPT		(0x00002000)
		#define MVE_BUFFER_FRAME_FLAG_EOS		(0x00010000)
	/* Height (in luma samples) of visible part of frame,
	 * may be smaller than allocated frame size.
	 */
	u16 visible_frame_height;

	/* Width (in luma samples) of visible part of frame,
	 * may be smaller than allocated frame size.
	 */
	u16 visible_frame_width;

	/* Color format of buffer */
	u16 format;

	u16 reserved0; /* force 'data' to be 4-byte aligned */

	union {
		struct mve_buffer_frame_planar planar;
	} data;

	u32 reserved1; /* force size to be multiple of 8 bytes */
};

/* The bitstream buffer stores a number of bitstream bytes */
struct mve_buffer_bitstream {
	/* For identification of the buffer, this is not changed by
	 * the firmware.
	 */
	u64 host_handle;

	/* For matching input buffer with output buffers, the firmware
	 * copies these values between frame buffers and bitstream buffers.
	 */
	u64 user_data_tag;

	/* BufferFlags */
	u32 bitstream_flags;
		#define MVE_BUFFER_BITSTREAM_FLAG_EOS              (0x00000001)

	/* Length of allocated buffer */
	u32 bitstream_alloc_bytes;
	/* Byte offset from start to first byte */
	u32 bitstream_offset;
	/* Number of bytes in the buffer */
	u32 bitstream_filled_len;
	/* Pointer to buffer start */
	u32 bitstream_buf_addr;
	/* Pad to force 8-byte alignment */
	u32 reserved;
};

/* Parameters that are sent in the same communication channels
 * as the buffers. A parameter applies to all subsequent buffers.
 * Some types are only valid for decode, and some only for encode.
 */
struct mve_buffer_param {
	u32 type;                                                   /* Extra data: */
		#define MVE_BUFFER_PARAM_TYPE_COLOUR_DESCRIPTION        (15)
		/* arg, number of output buffers that are complete and held by firmware in the
		 * DPB for reordering purposes. Valid after the next frame is output.
		 */
		#define MVE_BUFFER_PARAM_TYPE_DPB_HELD_FRAMES           (19)

	union {
		u32                                        arg;
	} data;
};

#endif /* __MALI_VPU_PROTOCOL_DEF_V3_H__ */
