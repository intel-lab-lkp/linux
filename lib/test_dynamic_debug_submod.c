// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel module to test/demonstrate dynamic_debug features,
 * particularly classmaps and their support for subsystems, like DRM,
 * which defines its drm_debug classmap in drm module, and uses it in
 * helpers & drivers.
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

MODULE_DESCRIPTION("test/demonstrate dynamic-debug subsystem support");
MODULE_AUTHOR("Jim Cromie <jim.cromie@gmail.com>");
MODULE_LICENSE("GPL");
