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
