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
#ifndef _clb06f_h_
#define _clb06f_h_

/* fields and values */
// NOTE - MEM_OP_A and MEM_OP_B have been removed for gm20x to make room for
// possible future MEM_OP features.  MEM_OP_C/D have identical functionality
// to the previous MEM_OP_A/B methods.
#define NVB06F_MEM_OP_C                                            (0x00000030)
#define NVB06F_MEM_OP_C_OPERAND_LOW                                       31:2
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_PDB                                 0:0
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_PDB_ONE                      0x00000000
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_PDB_ALL                      0x00000001
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_GPC                                 1:1
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_GPC_ENABLE                   0x00000000
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_GPC_DISABLE                  0x00000001
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_TARGET                            11:10
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_TARGET_VID_MEM               0x00000000
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_TARGET_SYS_MEM_COHERENT      0x00000002
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_TARGET_SYS_MEM_NONCOHERENT   0x00000003
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_ADDR_LO                           31:12
#define NVB06F_MEM_OP_D                                            (0x00000034)
#define NVB06F_MEM_OP_D_OPERAND_HIGH                                       7:0
#define NVB06F_MEM_OP_D_OPERATION                                        31:27
#define NVB06F_MEM_OP_D_OPERATION_MEMBAR                            0x00000005
#define NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE                0x00000009
#define NVB06F_MEM_OP_D_OPERATION_L2_PEERMEM_INVALIDATE             0x0000000d
#define NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE              0x0000000e
#define NVB06F_MEM_OP_D_OPERATION_L2_CLEAN_COMPTAGS                 0x0000000f
#define NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY                    0x00000010
#define NVB06F_MEM_OP_D_TLB_INVALIDATE_ADDR_HI                             7:0

#endif /* _clb06f_h_ */
