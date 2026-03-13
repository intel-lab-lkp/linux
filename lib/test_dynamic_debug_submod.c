// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel module for testing dynamic_debug
 *
 * Authors:
 *      Jim Cromie	<jim.cromie@gmail.com>
 */

/*
 * clone the parent, inherit all the properties, for consistency and
 * simpler accounting in test expectations.
 */
#define TEST_DYNAMIC_DEBUG_SUBMOD
#include "test_dynamic_debug.c"
