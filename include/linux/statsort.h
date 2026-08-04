/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STATSORT_H
#define _LINUX_STATSORT_H

#include <linux/types.h>

/**
 * typedef statsort_key_func_t - sort-key extraction callback
 * @elem: pointer to the element to extract a key from
 * @priv: opaque pointer, passed through from statsort()'s @priv
 *
 * Return: a signed 64-bit key such that, for any two elements a and
 * b, (key(a) < key(b)) matches the order statsort() should produce.
 */
typedef s64 (*statsort_key_func_t)(const void *elem, const void *priv);

void statsort(void *base, size_t num, size_t size,
	      statsort_key_func_t key_func, const void *priv);

void statsort_longs(long *base, size_t num);

#endif /* _LINUX_STATSORT_H */
