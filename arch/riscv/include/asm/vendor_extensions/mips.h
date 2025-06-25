/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 MIPS.
 */

#ifndef _ASM_RISCV_VENDOR_EXTENSIONS_MIPS_H
#define _ASM_RISCV_VENDOR_EXTENSIONS_MIPS_H

#include <asm/vendor_extensions.h>
#include <linux/types.h>

#define RISCV_ISA_VENDOR_EXT_XMIPSEXECTL	0

extern struct riscv_isa_vendor_ext_data_list riscv_isa_vendor_ext_list_mips;

/* MIPS.PAUSE is an alternative opcode which is implemented to have the */
/* same behavior as PAUSE on some MIPS RISCV cores. */
/* It is a ‘hint’ encoding of the SLLI instruction, */
/* with rd = 0, rs1 = 0 and imm = 5. */

#define MIPS_PAUSE	".4byte 0x00501013\n\t"

#endif // _ASM_RISCV_VENDOR_EXTENSIONS_MIPS_H
