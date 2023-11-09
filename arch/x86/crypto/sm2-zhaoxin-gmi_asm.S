/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Zhaoxin GMI implementation of a SM2 function
 *
 * Copyright(c) 2023 Zhaoxin Semiconductor LTD.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Contact Information:
 *  YunShen <YunShen@zhaoxin.com>
 */
#include <linux/linkage.h>

#define KEY_PTR     %rdi /* 1st arg */
#define HASH_PTR    %rsi /* 2nd arg */
#define SIG_PTR     %rdx /* 3rd arg */
#define SCRATCH_PTR %rcx /* 4rd arg */
#define VER_RESULT  %rax /* ret     */

.text
.align 32
###############################################################################
# int zhaoxin_gmi_sm2_verify (
#         unsigned char *key, /*key*/
#         unsigned char *hash, /*hash*/
#         unsigned char *sig, /*signature*/
#         unsigned char *scratch /*8 kbytes scratch space*/
#     );
###############################################################################
SYM_FUNC_START(zhaoxin_gmi_sm2_verify)
	push %r12
	push %rbx

	mov HASH_PTR, %rax
	mov KEY_PTR, %rbx
	mov SIG_PTR, %r12
	mov $8, %rdx
	mov SCRATCH_PTR, %rsi
	mov %r12, %rdi

	.byte 0XF2
	.byte 0X0F
	.byte 0XA6
	.byte 0XC0

	mov %rcx, %rax

	pop %rbx
	pop %r12

	RET
SYM_FUNC_END(zhaoxin_gmi_sm2_verify)
