// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2006 PathScale, Inc.  All Rights Reserved.
 */

#include <linux/align.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/unaligned.h>

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

#ifndef memcpy_fromio
/**
 * memcpy_fromio	Copy a block of data from I/O memory
 * @to:			The (RAM) destination for the copy
 * @from:		The (I/O memory) source for the data
 * @count:		The number of bytes to copy
 *
 * Copy a block of data from I/O memory.
 */
void memcpy_fromio(void *to, const volatile void __iomem *from, size_t count)
{
	while (count && !IS_ALIGNED((long)from, sizeof(long))) {
		*(u8 *)to = __raw_readb(from);
		from++;
		to++;
		count--;
	}

	while (count >= sizeof(long)) {
#ifdef CONFIG_64BIT
		long val = __raw_readq(from);
#else
		long val = __raw_readl(from);
#endif
		put_unaligned(val, (long *)to);


		from += sizeof(long);
		to += sizeof(long);
		count -= sizeof(long);
	}

	while (count) {
		*(u8 *)to = __raw_readb(from);
		from++;
		to++;
		count--;
	}
}
EXPORT_SYMBOL(memcpy_fromio);
#endif

#ifndef memcpy_toio
/**
 * memcpy_toio		Copy a block of data into I/O memory
 * @to:			The (I/O memory) destination for the copy
 * @from:		The (RAM) source for the data
 * @count:		The number of bytes to copy
 *
 * Copy a block of data to I/O memory.
 */
void memcpy_toio(volatile void __iomem *to, const void *from, size_t count)
{
	while (count && !IS_ALIGNED((long)to, sizeof(long))) {
		__raw_writeb(*(u8 *)from, to);
		from++;
		to++;
		count--;
	}

	while (count >= sizeof(long)) {
		long val = get_unaligned((long *)from);
#ifdef CONFIG_64BIT
		__raw_writeq(val, to);
#else
		__raw_writel(val, to);
#endif

		from += sizeof(long);
		to += sizeof(long);
		count -= sizeof(long);
	}

	while (count) {
		__raw_writeb(*(u8 *)from, to);
		from++;
		to++;
		count--;
	}
}
EXPORT_SYMBOL(memcpy_toio);
#endif

#ifndef memset_io
/**
 * memset_io		Set a range of I/O memory to a constant value
 * @dst:		The beginning of the I/O-memory range to set
 * @c:			The value to set the memory to
 * @count:		The number of bytes to set
 *
 * Set a range of I/O memory to a given value.
 */
void memset_io(volatile void __iomem *dst, int c, size_t count)
{
	long qc = (u8)c;

	qc *= ~0UL / 0xff;

	while (count && !IS_ALIGNED((long)dst, sizeof(long))) {
		__raw_writeb(c, dst);
		dst++;
		count--;
	}

	while (count >= sizeof(long)) {
#ifdef CONFIG_64BIT
		__raw_writeq(qc, dst);
#else
		__raw_writel(qc, dst);
#endif

		dst += sizeof(long);
		count -= sizeof(long);
	}

	while (count) {
		__raw_writeb(c, dst);
		dst++;
		count--;
	}
}
EXPORT_SYMBOL(memset_io);
#endif
