/*
 * Copyright (C) 2003 Matjaz Breskvar <phoenix@bsemi.com>
 * Copyright (C) 2010-2011 Jonas Bonn <jonas@southpole.se>
 * Copyright (C) 2012 Regents of the University of California
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _UAPI_ASM_RISCV_ELF_H
#define _UAPI_ASM_RISCV_ELF_H

#include <asm/ptrace.h>

/* ELF register definitions */
typedef unsigned long elf_greg_t;
typedef struct user_regs_struct elf_gregset_t;
#define ELF_NGREG (sizeof(elf_gregset_t) / sizeof(elf_greg_t))

/* We don't support f without d, or q.  */
typedef __u64 elf_fpreg_t;
typedef union __riscv_fp_state elf_fpregset_t;
#define ELF_NFPREG (sizeof(struct __riscv_d_ext_state) / sizeof(elf_fpreg_t))

#if __riscv_xlen == 64
#define ELF_RISCV_R_SYM(r_info)		ELF64_R_SYM(r_info)
#define ELF_RISCV_R_TYPE(r_info)	ELF64_R_TYPE(r_info)
#else
#define ELF_RISCV_R_SYM(r_info)		ELF32_R_SYM(r_info)
#define ELF_RISCV_R_TYPE(r_info)	ELF32_R_TYPE(r_info)
#endif

#endif /* _UAPI_ASM_RISCV_ELF_H */
