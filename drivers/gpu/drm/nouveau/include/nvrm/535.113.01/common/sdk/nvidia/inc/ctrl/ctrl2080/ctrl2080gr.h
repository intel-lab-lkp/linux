#ifndef __src_common_sdk_nvidia_inc_ctrl_ctrl2080_ctrl2080gr_h__
#define __src_common_sdk_nvidia_inc_ctrl_ctrl2080_ctrl2080gr_h__

/* Excerpt of RM headers from https://github.com/NVIDIA/open-gpu-kernel-modules/tree/535.113.01 */

/*
 * SPDX-FileCopyrightText: Copyright (c) 2006-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* valid zcullMode values */
#define NV2080_CTRL_CTXSW_ZCULL_MODE_GLOBAL          (0x00000000U)
#define NV2080_CTRL_CTXSW_ZCULL_MODE_NO_CTXSW        (0x00000001U)
#define NV2080_CTRL_CTXSW_ZCULL_MODE_SEPARATE_BUFFER (0x00000002U)

/**
 * NV2080_CTRL_CMD_GR_GET_ZCULL_INFO
 *
 * This command is used to query the RM for zcull information that the
 * driver will need to allocate and manage the zcull regions.
 *
 *   widthAlignPixels
 *     This parameter returns the width alignment restrictions in pixels
 *     used to adjust a surface for proper aliquot coverage (typically
 *     #TPC's * 16).
 *
 *   heightAlignPixels
 *     This parameter returns the height alignment restrictions in pixels
 *     used to adjust a surface for proper aliquot coverage (typically 32).
 *
 *   pixelSquaresByAliquots
 *     This parameter returns the pixel area covered by an aliquot
 *     (typically #Zcull_banks * 16 * 16).
 *
 *   aliquotTotal
 *     This parameter returns the total aliquot pool available in HW.
 *
 *   zcullRegionByteMultiplier
 *     This parameter returns multiplier used to convert aliquots in a region
 *     to the number of bytes required to save/restore them.
 *
 *   zcullRegionHeaderSize
 *     This parameter returns the region header size which is required to be
 *     allocated and accounted for in any save/restore operation on a region.
 *
 *   zcullSubregionHeaderSize
 *     This parameter returns the subregion header size which is required to be
 *     allocated and accounted for in any save/restore operation on a region.
 *
 *   subregionCount
 *     This parameter returns the subregion count.
 *
 *   subregionWidthAlignPixels
 *     This parameter returns the subregion width alignment restrictions in
 *     pixels used to adjust a surface for proper aliquot coverage
 *     (typically #TPC's * 16).
 *
 *   subregionHeightAlignPixels
 *     This parameter returns the subregion height alignment restrictions in
 *     pixels used to adjust a surface for proper aliquot coverage
 *     (typically 62).
 *
 *   The callee should compute the size of a zcull region as follows.
 *     (numBytes = aliquots * zcullRegionByteMultiplier +
 *                 zcullRegionHeaderSize + zcullSubregionHeaderSize)
 */
#define NV2080_CTRL_CMD_GR_GET_ZCULL_INFO            (0x20801206U) /* finn: Evaluated from "(FINN_NV20_SUBDEVICE_0_GR_INTERFACE_ID << 8) | NV2080_CTRL_GR_GET_ZCULL_INFO_PARAMS_MESSAGE_ID" */

#define NV2080_CTRL_GR_GET_ZCULL_INFO_PARAMS_SUBREGION_SUPPORTED
#define NV2080_CTRL_GR_GET_ZCULL_INFO_PARAMS_MESSAGE_ID (0x6U)

typedef struct NV2080_CTRL_GR_GET_ZCULL_INFO_PARAMS {
    NvU32 widthAlignPixels;
    NvU32 heightAlignPixels;
    NvU32 pixelSquaresByAliquots;
    NvU32 aliquotTotal;
    NvU32 zcullRegionByteMultiplier;
    NvU32 zcullRegionHeaderSize;
    NvU32 zcullSubregionHeaderSize;
    NvU32 subregionCount;
    NvU32 subregionWidthAlignPixels;
    NvU32 subregionHeightAlignPixels;
} NV2080_CTRL_GR_GET_ZCULL_INFO_PARAMS;


/*
 * NV2080_CTRL_CMD_GR_CTXSW_ZCULL_BIND
 *
 * This command is used to set the zcull context switch mode and virtual address
 * for the specified channel. A value of NV_ERR_NOT_SUPPORTED is
 * returned if the target channel does not support zcull context switch mode
 * changes.
 *
 *   hClient
 *     This parameter specifies the client handle of
 *     that owns the zcull context buffer. This field must match
 *     the hClient used in the control call for non-kernel clients.
 *   hChannel
 *     This parameter specifies the channel handle of
 *     the channel that is to have its zcull context switch mode changed.
 *   vMemPtr
 *     This parameter specifies the 64 bit virtual address
 *     for the allocated zcull context buffer.
 *   zcullMode
 *     This parameter specifies the new zcull context switch mode.
 *     Legal values for this parameter include:
 *       NV2080_CTRL_GR_SET_CTXSW_ZCULL_MODE_GLOBAL
 *         This mode is the normal zcull operation where it is not
 *         context switched and there is one set of globally shared
 *         zcull memory and tables.  This mode is only supported as
 *         long as all channels use this mode.
 *       NV2080_CTRL_GR_SET_CTXSW_ZCULL_MODE_NO_CTXSW
 *         This mode causes the zcull tables to be reset on a context
 *         switch, but the zcull buffer will not be saved/restored.
 *       NV2080_CTRL_GR_SET_CTXSW_ZCULL_MODE_SEPARATE_BUFFER
 *         This mode will cause the zcull buffers and tables to be
 *         saved/restored on context switches.  If a share channel
 *         ID is given (shareChID), then the 2 channels will share
 *         the zcull context buffers.
 */
#define NV2080_CTRL_CMD_GR_CTXSW_ZCULL_BIND        (0x20801208U) /* finn: Evaluated from "(FINN_NV20_SUBDEVICE_0_GR_INTERFACE_ID << 8) | NV2080_CTRL_GR_CTXSW_ZCULL_BIND_PARAMS_MESSAGE_ID" */

#define NV2080_CTRL_GR_CTXSW_ZCULL_BIND_PARAMS_MESSAGE_ID (0x8U)

typedef struct NV2080_CTRL_GR_CTXSW_ZCULL_BIND_PARAMS {
    NvHandle hClient;
    NvHandle hChannel;
    NV_DECLARE_ALIGNED(NvU64 vMemPtr, 8);
    NvU32    zcullMode;
} NV2080_CTRL_GR_CTXSW_ZCULL_BIND_PARAMS;
/* valid zcullMode values same as above NV2080_CTRL_CTXSW_ZCULL_MODE */

typedef enum NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS {
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_MAIN = 0,
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_SPILL = 1,
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_PAGEPOOL = 2,
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_BETACB = 3,
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_RTV = 4,
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_CONTEXT_POOL = 5,
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_CONTEXT_POOL_CONTROL = 6,
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_CONTEXT_POOL_CONTROL_CPU = 7,
    NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS_END = 8,
} NV2080_CTRL_CMD_GR_CTXSW_PREEMPTION_BIND_BUFFERS;

#endif
