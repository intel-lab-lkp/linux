/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Separated from arch/loongarch/kernel/signal.c:
 *
 * Author: Hanlu Li <lihanlu@loongson.cn>
 *         Huacai Chen <chenhuacai@loongson.cn>
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 *
 * Derived from MIPS:
 * Copyright (C) 1991, 1992  Linus Torvalds
 * Copyright (C) 1994 - 2000  Ralf Baechle
 * Copyright (C) 1999, 2000 Silicon Graphics, Inc.
 * Copyright (C) 2014, Imagination Technologies Ltd.
 */

#include <uapi/asm/ucontext.h>
#include <asm/siginfo.h>

struct rt_sigframe {
	struct siginfo rs_info;
	struct ucontext rs_uctx;
};
