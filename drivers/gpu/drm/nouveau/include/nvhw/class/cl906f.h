/*******************************************************************************
    Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/
#ifndef _cl906f_h_
#define _cl906f_h_

/* fields and values */
#define NV906F_SEMAPHOREA                                          (0x00000010)
#define NV906F_SEMAPHOREA_OFFSET_UPPER                                     7:0
#define NV906F_SEMAPHOREB                                          (0x00000014)
#define NV906F_SEMAPHOREB_OFFSET_LOWER                                    31:2
#define NV906F_SEMAPHOREC                                          (0x00000018)
#define NV906F_SEMAPHOREC_PAYLOAD                                         31:0
#define NV906F_SEMAPHORED                                          (0x0000001C)
#define NV906F_SEMAPHORED_OPERATION                                        3:0
#define NV906F_SEMAPHORED_OPERATION_ACQUIRE                         0x00000001
#define NV906F_SEMAPHORED_OPERATION_RELEASE                         0x00000002
#define NV906F_SEMAPHORED_OPERATION_ACQ_GEQ                         0x00000004
#define NV906F_SEMAPHORED_OPERATION_ACQ_AND                         0x00000008
#define NV906F_SEMAPHORED_ACQUIRE_SWITCH                                 12:12
#define NV906F_SEMAPHORED_ACQUIRE_SWITCH_DISABLED                   0x00000000
#define NV906F_SEMAPHORED_ACQUIRE_SWITCH_ENABLED                    0x00000001
#define NV906F_SEMAPHORED_RELEASE_WFI                                    20:20
#define NV906F_SEMAPHORED_RELEASE_WFI_EN                            0x00000000
#define NV906F_SEMAPHORED_RELEASE_WFI_DIS                           0x00000001
#define NV906F_SEMAPHORED_RELEASE_SIZE                                   24:24
#define NV906F_SEMAPHORED_RELEASE_SIZE_16BYTE                       0x00000000
#define NV906F_SEMAPHORED_RELEASE_SIZE_4BYTE                        0x00000001
#define NV906F_NON_STALL_INTERRUPT                                 (0x00000020)
#define NV906F_NON_STALL_INTERRUPT_HANDLE                                 31:0
#define NV906F_MEM_OP_A                                            (0x00000028)
#define NV906F_MEM_OP_A_OPERAND_LOW                                       31:2
#define NV906F_MEM_OP_A_TLB_INVALIDATE_ADDR                               29:2
#define NV906F_MEM_OP_A_TLB_INVALIDATE_TARGET                            31:30
#define NV906F_MEM_OP_A_TLB_INVALIDATE_TARGET_VID_MEM               0x00000000
#define NV906F_MEM_OP_A_TLB_INVALIDATE_TARGET_SYS_MEM_COHERENT      0x00000002
#define NV906F_MEM_OP_A_TLB_INVALIDATE_TARGET_SYS_MEM_NONCOHERENT   0x00000003
#define NV906F_MEM_OP_B                                            (0x0000002c)
#define NV906F_MEM_OP_B_OPERAND_HIGH                                       7:0
#define NV906F_MEM_OP_B_OPERATION                                        31:27
#define NV906F_MEM_OP_B_OPERATION_SYSMEMBAR_FLUSH                   0x00000005
#define NV906F_MEM_OP_B_OPERATION_SOFT_FLUSH                        0x00000006
#define NV906F_MEM_OP_B_OPERATION_MMU_TLB_INVALIDATE                0x00000009
#define NV906F_MEM_OP_B_OPERATION_L2_PEERMEM_INVALIDATE             0x0000000d
#define NV906F_MEM_OP_B_OPERATION_L2_SYSMEM_INVALIDATE              0x0000000e
#define NV906F_MEM_OP_B_OPERATION_L2_CLEAN_COMPTAGS                 0x0000000f
#define NV906F_MEM_OP_B_OPERATION_L2_FLUSH_DIRTY                    0x00000010
#define NV906F_MEM_OP_B_MMU_TLB_INVALIDATE_PDB                             0:0
#define NV906F_MEM_OP_B_MMU_TLB_INVALIDATE_PDB_ONE                  0x00000000
#define NV906F_MEM_OP_B_MMU_TLB_INVALIDATE_PDB_ALL                  0x00000001
#define NV906F_MEM_OP_B_MMU_TLB_INVALIDATE_GPC                             1:1
#define NV906F_MEM_OP_B_MMU_TLB_INVALIDATE_GPC_ENABLE               0x00000000
#define NV906F_MEM_OP_B_MMU_TLB_INVALIDATE_GPC_DISABLE              0x00000001
#define NV906F_SET_REFERENCE                                       (0x00000050)
#define NV906F_SET_REFERENCE_COUNT                                        31:0

/* dma method formats */
#define NV906F_DMA_METHOD_ADDRESS                                  11:0
#define NV906F_DMA_SUBDEVICE_MASK                                  15:4
#define NV906F_DMA_METHOD_SUBCHANNEL                               15:13
#define NV906F_DMA_TERT_OP                                         17:16
#define NV906F_DMA_TERT_OP_GRP0_INC_METHOD                         (0x00000000)
#define NV906F_DMA_TERT_OP_GRP0_SET_SUB_DEV_MASK                   (0x00000001)
#define NV906F_DMA_TERT_OP_GRP0_STORE_SUB_DEV_MASK                 (0x00000002)
#define NV906F_DMA_TERT_OP_GRP0_USE_SUB_DEV_MASK                   (0x00000003)
#define NV906F_DMA_TERT_OP_GRP2_NON_INC_METHOD                     (0x00000000)
#define NV906F_DMA_METHOD_COUNT                                    28:16
#define NV906F_DMA_IMMD_DATA                                       28:16
#define NV906F_DMA_SEC_OP                                          31:29
#define NV906F_DMA_SEC_OP_GRP0_USE_TERT                            (0x00000000)
#define NV906F_DMA_SEC_OP_INC_METHOD                               (0x00000001)
#define NV906F_DMA_SEC_OP_GRP2_USE_TERT                            (0x00000002)
#define NV906F_DMA_SEC_OP_NON_INC_METHOD                           (0x00000003)
#define NV906F_DMA_SEC_OP_IMMD_DATA_METHOD                         (0x00000004)
#define NV906F_DMA_SEC_OP_ONE_INC                                  (0x00000005)
#define NV906F_DMA_SEC_OP_RESERVED6                                (0x00000006)
#define NV906F_DMA_SEC_OP_END_PB_SEGMENT                           (0x00000007)
#endif /* _cl906f_h_ */
