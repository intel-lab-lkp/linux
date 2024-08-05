// SPDX-License-Identifier: GPL-2.0+

/*
 * Part of fork context switch microbenchmark.
 *
 * Copyright 2018, Anton Blanchard, IBM Corp.
 */

#define _GNU_SOURCE
#include <sys/syscall.h>

void _start(void)
{
	asm volatile (
		"li %%r0, %[sys_exit];"
		"li %%r3, 0;"
		"sc;"
		:
		: [sys_exit] "i" (SYS_exit)
		: "r0", "r3"
	);
}
