// SPDX-License-Identifier: GPL-2.0-only
/*
 * derived from arch/arm/kernel/io.c
 *
 * Copyright (C) 2024 Kalray Inc.
 * Author(s): Julian Vetter
 */

#include <linux/export.h>
#include <linux/types.h>
#include <linux/io.h>

#define NATIVE_STORE_SIZE	(BITS_PER_LONG/8)

#if IS_ENABLED(CONFIG_64BIT)
#define __raw_write_native(val, dst) __raw_writeq(val, dst)
#define __raw_read_native(src) __raw_readq((src))
#else
#define __raw_write_native(val, dst) __raw_writel(val, dst)
#define __raw_read_native(src) __raw_readl(src)
#endif

void __memcpy_fromio(void *to, const volatile void __iomem *from, size_t count)
{
	while (count && !IS_ALIGNED((unsigned long)from, NATIVE_STORE_SIZE)) {
		*(u8 *)to = __raw_readb(from);
		from++;
		to++;
		count--;
	}

	while (count >= NATIVE_STORE_SIZE) {
		*(uintptr_t *)to = __raw_read_native(from);
		from += NATIVE_STORE_SIZE;
		to += NATIVE_STORE_SIZE;
		count -= NATIVE_STORE_SIZE;
	}

	while (count) {
		*(u8 *)to = __raw_readb(from);
		from++;
		to++;
		count--;
	}
}
EXPORT_SYMBOL(__memcpy_fromio);

void __memcpy_toio(volatile void __iomem *to, const void *from, size_t count)
{
	while (count && !IS_ALIGNED((unsigned long)to, NATIVE_STORE_SIZE)) {
		__raw_writeb(*(u8 *)from, to);
		from++;
		to++;
		count--;
	}

	while (count >= NATIVE_STORE_SIZE) {
		__raw_write_native(*(uintptr_t *)from, to);
		from += NATIVE_STORE_SIZE;
		to += NATIVE_STORE_SIZE;
		count -= NATIVE_STORE_SIZE;
	}

	while (count) {
		__raw_writeb(*(u8 *)from, to);
		from++;
		to++;
		count--;
	}
}
EXPORT_SYMBOL(__memcpy_toio);

void __memset_io(volatile void __iomem *dst, int c, size_t count)
{
	uintptr_t qc = (u8)c;

	qc |= qc << 8;
	qc |= qc << 16;
#if IS_ENABLED(CONFIG_64BIT)
	qc |= qc << 32;
#endif

	while (count && !IS_ALIGNED((unsigned long)dst, NATIVE_STORE_SIZE)) {
		__raw_writeb(c, dst);
		dst++;
		count--;
	}

	while (count >= NATIVE_STORE_SIZE) {
		__raw_write_native(qc, dst);
		dst += NATIVE_STORE_SIZE;
		count -= NATIVE_STORE_SIZE;
	}

	while (count) {
		__raw_writeb(c, dst);
		dst++;
		count--;
	}
}
EXPORT_SYMBOL(__memset_io);
