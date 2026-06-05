/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#ifndef _LINUX_KHO_ABI_COMPAT_H
#define _LINUX_KHO_ABI_COMPAT_H

#include <linux/align.h>

/**
 * KHO_SUB_COMPAT - Helper to append a sub-component compatibility string.
 * @str: The compatibility string of the sub-component.
 *
 * Appends a KHO safe data structure compatibility string to a sub-system
 * compatibility string using a semicolon ';' as a separator.
 *
 * NOTE: Sub-components MUST be added in strict alphabetical order to maintain
 * a consistent and predictable compatibility string value.
 */
#define KHO_SUB_COMPAT(str) ";" str

/**
 * KHO_COMPAT_ALIGN - Align a compatibility string size to 8 bytes.
 * @str: The compatibility string.
 *
 * Aligns the size of a compatibility string to an 8-byte boundary for use
 * in ABI structures.
 */
#define KHO_COMPAT_ALIGN(str)	ALIGN(sizeof(str), 8)

#endif /* _LINUX_KHO_ABI_COMPAT_H */
