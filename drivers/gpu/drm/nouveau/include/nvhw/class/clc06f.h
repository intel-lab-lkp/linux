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
#ifndef _clc06f_h_
#define _clc06f_h_

/* fields and values */
// NOTE - MEM_OP_A and MEM_OP_B have been replaced in gp100 with methods for
// specifying the page address for a targeted TLB invalidate and the uTLB for
// a targeted REPLAY_CANCEL for UVM.
// The previous MEM_OP_A/B functionality is in MEM_OP_C/D, with slightly
// rearranged fields.
#define NVC06F_MEM_OP_A                                            (0x00000028)
#define NVC06F_MEM_OP_A_TLB_INVALIDATE_CANCEL_TARGET_CLIENT_UNIT_ID        5:0  // only relevant for REPLAY_CANCEL_TARGETED
#define NVC06F_MEM_OP_A_TLB_INVALIDATE_CANCEL_TARGET_GPC_ID               10:6  // only relevant for REPLAY_CANCEL_TARGETED
#define NVC06F_MEM_OP_A_TLB_INVALIDATE_SYSMEMBAR                         11:11
#define NVC06F_MEM_OP_A_TLB_INVALIDATE_SYSMEMBAR_EN                 0x00000001
#define NVC06F_MEM_OP_A_TLB_INVALIDATE_SYSMEMBAR_DIS                0x00000000
#define NVC06F_MEM_OP_A_TLB_INVALIDATE_TARGET_ADDR_LO                    31:12
#define NVC06F_MEM_OP_B                                            (0x0000002c)
#define NVC06F_MEM_OP_B_TLB_INVALIDATE_TARGET_ADDR_HI                     31:0
#define NVC06F_MEM_OP_C                                            (0x00000030)
#define NVC06F_MEM_OP_C_MEMBAR_TYPE                                        2:0
#define NVC06F_MEM_OP_C_MEMBAR_TYPE_SYS_MEMBAR                      0x00000000
#define NVC06F_MEM_OP_C_MEMBAR_TYPE_MEMBAR                          0x00000001
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PDB                                 0:0
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PDB_ONE                      0x00000000
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PDB_ALL                      0x00000001  // Probably nonsensical for MMU_TLB_INVALIDATE_TARGETED
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_GPC                                 1:1
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_GPC_ENABLE                   0x00000000
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_GPC_DISABLE                  0x00000001
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_REPLAY                              4:2  // only relevant if GPC ENABLE
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_REPLAY_NONE                  0x00000000
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_REPLAY_START                 0x00000001
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_REPLAY_START_ACK_ALL         0x00000002
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_REPLAY_CANCEL_TARGETED       0x00000003
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_REPLAY_CANCEL_GLOBAL         0x00000004
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_ACK_TYPE                            6:5  // only relevant if GPC ENABLE
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_ACK_TYPE_NONE                0x00000000
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_ACK_TYPE_GLOBALLY            0x00000001
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_ACK_TYPE_INTRANODE           0x00000002
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL                    9:7  // Invalidate affects this level and all below
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL_ALL         0x00000000  // Invalidate tlb caches at all levels of the page table
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL_PTE_ONLY    0x00000001
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL_UP_TO_PDE0  0x00000002
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL_UP_TO_PDE1  0x00000003
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL_UP_TO_PDE2  0x00000004
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL_UP_TO_PDE3  0x00000005
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL_UP_TO_PDE4  0x00000006
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PAGE_TABLE_LEVEL_UP_TO_PDE5  0x00000007
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PDB_APERTURE                          11:10  // only relevant if PDB_ONE
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PDB_APERTURE_VID_MEM             0x00000000
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PDB_APERTURE_SYS_MEM_COHERENT    0x00000002
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PDB_APERTURE_SYS_MEM_NONCOHERENT 0x00000003
#define NVC06F_MEM_OP_C_TLB_INVALIDATE_PDB_ADDR_LO                       31:12  // only relevant if PDB_ONE
// MEM_OP_D MUST be preceded by MEM_OPs A-C.
#define NVC06F_MEM_OP_D                                            (0x00000034)
#define NVC06F_MEM_OP_D_TLB_INVALIDATE_PDB_ADDR_HI                        26:0  // only relevant if PDB_ONE
#define NVC06F_MEM_OP_D_OPERATION                                        31:27
#define NVC06F_MEM_OP_D_OPERATION_MEMBAR                            0x00000005
#define NVC06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE                0x00000009
#define NVC06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE_TARGETED       0x0000000a
#define NVC06F_MEM_OP_D_OPERATION_L2_PEERMEM_INVALIDATE             0x0000000d
#define NVC06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE              0x0000000e
// CLEAN_LINES is an alias for Tegra/GPU IP usage
#define NVC06F_MEM_OP_D_OPERATION_L2_INVALIDATE_CLEAN_LINES         0x0000000e
// This B alias is confusing but it was missed as part of the update. Left here
// for compatibility.
#define NVC06F_MEM_OP_B_OPERATION_L2_INVALIDATE_CLEAN_LINES         0x0000000e
#define NVC06F_MEM_OP_D_OPERATION_L2_CLEAN_COMPTAGS                 0x0000000f
#define NVC06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY                    0x00000010
#define NVC06F_MEM_OP_D_OPERATION_L2_WAIT_FOR_SYS_PENDING_READS     0x00000015

#endif /* _clc06f_h_ */
