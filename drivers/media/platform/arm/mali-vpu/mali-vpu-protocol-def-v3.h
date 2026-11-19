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

#endif /* __MALI_VPU_PROTOCOL_DEF_V3_H__ */
