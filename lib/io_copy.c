// SPDX-License-Identifier: GPL-2.0-only
/*
 * derived from arch/arm/kernel/io.c
 *
 * Copyright (C) 2024 Kalray Inc.
 * Author(s): Julian Vetter
 */

#include <asm/unaligned.h>

#include <linux/export.h>
#include <linux/types.h>
#include <linux/io.h>

#define NATIVE_STORE_SIZE	(BITS_PER_LONG/8)

void __memcpy_fromio(void *to, const volatile void __iomem *from, size_t count)
{
	while (count && !IS_ALIGNED((unsigned long)from, NATIVE_STORE_SIZE)) {
		*(u8 *)to = __raw_readb(from);
		from++;
		to++;
		count--;
	}

	while (count >= NATIVE_STORE_SIZE) {
#ifdef CONFIG_64BIT
			put_unaligned(__raw_readq(from), (uintptr_t *)to);
#else
			put_unaligned(__raw_readl(from), (uintptr_t *)to);
#endif

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
#ifdef CONFIG_64BIT
			__raw_writeq(get_unaligned((uintptr_t *)from), to);
#else
			__raw_writel(get_unaligned((uintptr_t *)from), to);
#endif

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

	if (IS_ENABLED(CONFIG_64BIT))
		qc |= qc << 32;

	while (count && !IS_ALIGNED((unsigned long)dst, NATIVE_STORE_SIZE)) {
		__raw_writeb(c, dst);
		dst++;
		count--;
	}

	while (count >= NATIVE_STORE_SIZE) {
#ifdef CONFIG_64BIT
			__raw_writeq(qc, dst);
#else
			__raw_writel(qc, dst);
#endif

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
