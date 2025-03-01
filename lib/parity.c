// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/parity.c
 *
 * Copyright (C) 2025 Kuan-Wei Chiu <visitorckw@gmail.com>
 * Copyright (C) 2025 Yu-Chun Lin <eleanor15x@gmail.com>
 *
 * __parity[sdt]i2 can be overridden by linking arch-specific versions.
 */

#include <linux/export.h>
#include <linux/kernel.h>

/*
 * One explanation of this algorithm:
 * https://funloop.org/codex/problem/parity/README.html
 */
int __weak __paritysi2(u32 val);
int __weak __paritysi2(u32 val)
{
	val ^= val >> 16;
	val ^= val >> 8;
	val ^= val >> 4;
	return (0x6996 >> (val & 0xf)) & 1;
}
EXPORT_SYMBOL(__paritysi2);

int __weak __paritydi2(u64 val);
int __weak __paritydi2(u64 val)
{
	val ^= val >> 32;
	val ^= val >> 16;
	val ^= val >> 8;
	val ^= val >> 4;
	return (0x6996 >> (val & 0xf)) & 1;
}
EXPORT_SYMBOL(__paritydi2);

int __weak __parityti2(u64 val);
int __weak __parityti2(u64 val)
{
	val ^= val >> 32;
	val ^= val >> 16;
	val ^= val >> 8;
	val ^= val >> 4;
	return (0x6996 >> (val & 0xf)) & 1;
}
EXPORT_SYMBOL(__parityti2);
