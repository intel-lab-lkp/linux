/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright 2023 Rivos, Inc
 */

#ifndef _UAPI_ASM_HWPROBE_H
#define _UAPI_ASM_HWPROBE_H

#include <linux/types.h>

/*
 * Interface for probing hardware capabilities from userspace, see
 * Documentation/riscv/hwprobe.rst for more information.
 */
struct riscv_hwprobe {
	__s64 key;
	__u64 value;
};

#define RISCV_HWPROBE_KEY_MVENDORID	0
#define RISCV_HWPROBE_KEY_MARCHID	1
#define RISCV_HWPROBE_KEY_MIMPID	2
#define RISCV_HWPROBE_KEY_BASE_BEHAVIOR	3
#define		RISCV_HWPROBE_BASE_BEHAVIOR_IMA	(1 << 0)
#define RISCV_HWPROBE_KEY_IMA_EXT_0	4
#define		RISCV_HWPROBE_IMA_FD		(1 << 0)
#define		RISCV_HWPROBE_IMA_C		(1 << 1)
#define		RISCV_HWPROBE_IMA_V		(1 << 2)
#define		RISCV_HWPROBE_EXT_ZBA		(1 << 3)
#define		RISCV_HWPROBE_EXT_ZBB		(1 << 4)
#define		RISCV_HWPROBE_EXT_ZBS		(1 << 5)
#define		RISCV_HWPROBE_EXT_ZVBB		(1 << 6)
#define		RISCV_HWPROBE_EXT_ZVBC		(1 << 7)
#define		RISCV_HWPROBE_EXT_ZVKB		(1 << 8)
#define		RISCV_HWPROBE_EXT_ZVKG		(1 << 9)
#define		RISCV_HWPROBE_EXT_ZVKN		(1 << 10)
#define		RISCV_HWPROBE_EXT_ZVKNC		(1 << 11)
#define		RISCV_HWPROBE_EXT_ZVKNED	(1 << 12)
#define		RISCV_HWPROBE_EXT_ZVKNG		(1 << 13)
#define		RISCV_HWPROBE_EXT_ZVKNHA	(1 << 14)
#define		RISCV_HWPROBE_EXT_ZVKNHB	(1 << 15)
#define		RISCV_HWPROBE_EXT_ZVKS		(1 << 16)
#define		RISCV_HWPROBE_EXT_ZVKSC		(1 << 17)
#define		RISCV_HWPROBE_EXT_ZVKSED	(1 << 18)
#define		RISCV_HWPROBE_EXT_ZVKSH		(1 << 19)
#define		RISCV_HWPROBE_EXT_ZVKSG		(1 << 20)
#define		RISCV_HWPROBE_EXT_ZVKT		(1 << 21)
#define		RISCV_HWPROBE_EXT_ZFH		(1 << 22)
#define		RISCV_HWPROBE_EXT_ZFHMIN	(1 << 23)
#define RISCV_HWPROBE_KEY_CPUPERF_0	5
#define		RISCV_HWPROBE_MISALIGNED_UNKNOWN	(0 << 0)
#define		RISCV_HWPROBE_MISALIGNED_EMULATED	(1 << 0)
#define		RISCV_HWPROBE_MISALIGNED_SLOW		(2 << 0)
#define		RISCV_HWPROBE_MISALIGNED_FAST		(3 << 0)
#define		RISCV_HWPROBE_MISALIGNED_UNSUPPORTED	(4 << 0)
#define		RISCV_HWPROBE_MISALIGNED_MASK		(7 << 0)
/* Increase RISCV_HWPROBE_MAX_KEY when adding items. */

#endif
