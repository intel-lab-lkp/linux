/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Chen Miao
 */

#ifndef _ASM_PATCHING_H_
#define _ASM_PATCHING_H_

#include <linux/types.h>

int patch_insn_write(void *addr, const void *insn);

#endif /* _ASM_PATCHING_H_ */
