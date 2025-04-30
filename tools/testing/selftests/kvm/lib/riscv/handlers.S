/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 Intel Corporation
 */

#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif

#include <asm/csr.h>

#ifdef __ASSEMBLY__
#define __ASM_STR(x)	x
#else
#define __ASM_STR(x)	#x
#endif

#if __riscv_xlen == 64
#define __REG_SEL(a, b)	__ASM_STR(a)
#elif __riscv_xlen == 32
#define __REG_SEL(a, b)	__ASM_STR(b)
#else
#error "Unexpected __riscv_xlen"
#endif

#define REG_L		__REG_SEL(ld, lw)
#define REG_S		__REG_SEL(sd, sw)

.macro save_context
	addi  sp, sp, (-8*36)
	REG_S    x1, 8(sp)
	REG_S    x2, 16(sp)
	REG_S    x3, 24(sp)
	REG_S    x4, 32(sp)
	REG_S    x5, 40(sp)
	REG_S    x6, 48(sp)
	REG_S    x7, 56(sp)
	REG_S    x8, 64(sp)
	REG_S    x9, 72(sp)
	REG_S    x10, 80(sp)
	REG_S    x11, 88(sp)
	REG_S    x12, 96(sp)
	REG_S    x13, 104(sp)
	REG_S    x14, 112(sp)
	REG_S    x15, 120(sp)
	REG_S    x16, 128(sp)
	REG_S    x17, 136(sp)
	REG_S    x18, 144(sp)
	REG_S    x19, 152(sp)
	REG_S    x20, 160(sp)
	REG_S    x21, 168(sp)
	REG_S    x22, 176(sp)
	REG_S    x23, 184(sp)
	REG_S    x24, 192(sp)
	REG_S    x25, 200(sp)
	REG_S    x26, 208(sp)
	REG_S    x27, 216(sp)
	REG_S    x28, 224(sp)
	REG_S    x29, 232(sp)
	REG_S    x30, 240(sp)
	REG_S    x31, 248(sp)
	csrr  s0, CSR_SEPC
	csrr  s1, CSR_SSTATUS
	csrr  s2, CSR_STVAL
	csrr  s3, CSR_SCAUSE
	REG_S    s0, 0(sp)
	REG_S    s1, 256(sp)
	REG_S    s2, 264(sp)
	REG_S    s3, 272(sp)
.endm

.macro restore_context
	REG_L    s3, 272(sp)
	REG_L    s2, 264(sp)
	REG_L    s1, 256(sp)
	REG_L    s0, 0(sp)
	csrw  CSR_SCAUSE, s3
	csrw  CSR_SSTATUS, s1
	csrw  CSR_SEPC, s0
	REG_L    x31, 248(sp)
	REG_L    x30, 240(sp)
	REG_L    x29, 232(sp)
	REG_L    x28, 224(sp)
	REG_L    x27, 216(sp)
	REG_L    x26, 208(sp)
	REG_L    x25, 200(sp)
	REG_L    x24, 192(sp)
	REG_L    x23, 184(sp)
	REG_L    x22, 176(sp)
	REG_L    x21, 168(sp)
	REG_L    x20, 160(sp)
	REG_L    x19, 152(sp)
	REG_L    x18, 144(sp)
	REG_L    x17, 136(sp)
	REG_L    x16, 128(sp)
	REG_L    x15, 120(sp)
	REG_L    x14, 112(sp)
	REG_L    x13, 104(sp)
	REG_L    x12, 96(sp)
	REG_L    x11, 88(sp)
	REG_L    x10, 80(sp)
	REG_L    x9, 72(sp)
	REG_L    x8, 64(sp)
	REG_L    x7, 56(sp)
	REG_L    x6, 48(sp)
	REG_L    x5, 40(sp)
	REG_L    x4, 32(sp)
	REG_L    x3, 24(sp)
	REG_L    x2, 16(sp)
	REG_L    x1, 8(sp)
	addi  sp, sp, (8*36)
.endm

.balign 4
.global exception_vectors
exception_vectors:
	save_context
	move  a0, sp
	call  route_exception
	restore_context
	sret
