/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ARRAY_SIZE_H
#define _LINUX_ARRAY_SIZE_H

#include <linux/compiler.h>

/**
 * ARRAY_SIZE - get the number of elements in array @arr
 * @arr: array to be sized
 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

/**
 * min_array_size - parameter decoration to hint to the compiler that the
 *                  passed array should have at least @n elements
 * @n: minimum number of elements, after which the compiler may warn
 */
#define min_array_size(n) static n

#endif  /* _LINUX_ARRAY_SIZE_H */
