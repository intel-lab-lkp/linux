// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2006 PathScale, Inc.  All Rights Reserved.
 */

#include <asm/unaligned.h>

#include <linux/align.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/io.h>

#define NATIVE_STORE_SIZE	(BITS_PER_LONG/8)

/**
 * __iowrite32_copy - copy data to MMIO space, in 32-bit units
 * @to: destination, in MMIO space (must be 32-bit aligned)
 * @from: source (must be 32-bit aligned)
 * @count: number of 32-bit quantities to copy
 *
 * Copy data from kernel space to MMIO space, in units of 32 bits at a
 * time.  Order of access is not guaranteed, nor is a memory barrier
 * performed afterwards.
 */
#ifndef __iowrite32_copy
void __iowrite32_copy(void __iomem *to, const void *from, size_t count)
{
	u32 __iomem *dst = to;
	const u32 *src = from;
	const u32 *end = src + count;

	while (src < end)
		__raw_writel(*src++, dst++);
}
EXPORT_SYMBOL_GPL(__iowrite32_copy);
#endif

/**
 * __ioread32_copy - copy data from MMIO space, in 32-bit units
 * @to: destination (must be 32-bit aligned)
 * @from: source, in MMIO space (must be 32-bit aligned)
 * @count: number of 32-bit quantities to copy
 *
 * Copy data from MMIO space to kernel space, in units of 32 bits at a
 * time.  Order of access is not guaranteed, nor is a memory barrier
 * performed afterwards.
 */
void __ioread32_copy(void *to, const void __iomem *from, size_t count)
{
	u32 *dst = to;
	const u32 __iomem *src = from;
	const u32 __iomem *end = src + count;

	while (src < end)
		*dst++ = __raw_readl(src++);
}
EXPORT_SYMBOL_GPL(__ioread32_copy);

/**
 * __iowrite64_copy - copy data to MMIO space, in 64-bit or 32-bit units
 * @to: destination, in MMIO space (must be 64-bit aligned)
 * @from: source (must be 64-bit aligned)
 * @count: number of 64-bit quantities to copy
 *
 * Copy data from kernel space to MMIO space, in units of 32 or 64 bits at a
 * time.  Order of access is not guaranteed, nor is a memory barrier
 * performed afterwards.
 */
#ifndef __iowrite64_copy
void __iowrite64_copy(void __iomem *to, const void *from, size_t count)
{
#ifdef CONFIG_64BIT
	u64 __iomem *dst = to;
	const u64 *src = from;
	const u64 *end = src + count;

	while (src < end)
		__raw_writeq(*src++, dst++);
#else
	__iowrite32_copy(to, from, count * 2);
#endif
}
EXPORT_SYMBOL_GPL(__iowrite64_copy);
#endif


#ifndef __memcpy_fromio
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
#endif

#ifndef __memcpy_toio
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
#endif

#ifndef __memset_io
void __memset_io(volatile void __iomem *dst, int c, size_t count)
{
	uintptr_t qc = (u8)c;

	qc |= qc << 8;
	qc |= qc << 16;

#ifdef CONFIG_64BIT
	qc |= qc << 32;
#endif

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
#endif
