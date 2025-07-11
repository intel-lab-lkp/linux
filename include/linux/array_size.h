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
 * ENDOF - get a pointer to one past the last element in array @a
 * @a: array
 */
#define ENDOF(a)  (a + ARRAY_SIZE(a))

#endif  /* _LINUX_ARRAY_SIZE_H */
