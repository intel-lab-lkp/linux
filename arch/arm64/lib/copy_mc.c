// SPDX-License-Identifier: GPL-2.0-only

#include <linux/export.h>
#include <linux/instrumented.h>
#include <linux/uaccess.h>

unsigned long __memcpy_mc(void *to, const void *from, unsigned long n);

/**
 * copy_mc_to_kernel - copy memory, surviving uncorrectable source errors
 * @to: destination address
 * @from: source address
 * @size: number of bytes to copy
 *
 * Return: 0 on success, otherwise the number of bytes not copied because a
 * source load hit memory with an uncorrectable error.
 */
unsigned long __must_check copy_mc_to_kernel(void *to, const void *from, size_t size)
{
	unsigned long ret;

	instrument_memcpy_before(to, from, size);
	ret = __memcpy_mc(to, from, size);
	instrument_memcpy_after(to, from, size, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(copy_mc_to_kernel);
