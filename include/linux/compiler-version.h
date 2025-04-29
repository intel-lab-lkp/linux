/* SPDX-License-Identifier: GPL-2.0-only */

#ifdef  __LINUX_COMPILER_VERSION_H
#error "Please do not include <linux/compiler-version.h>. This is done by the build system."
#endif
#define __LINUX_COMPILER_VERSION_H

/*
 * This header exists to force full rebuild when the compiler is upgraded or
 * the randstruct is changed.
 *
 * When fixdep scans this, it will find this string "CONFIG_CC_VERSION_TEXT"
 * and add dependency on include/config/CC_VERSION_TEXT, which is touched
 * by Kconfig when the version string from the compiler changes.
 */
#ifdef CONFIG_RANDSTRUCT
/*
 * If CONFIG_RANDSTRUCT is enabled and scripts/basic/randstruct.seed changes,
 * randstruct_hash.h is updated.  Including it here, makes it a build
 * dependency for all build objects.
 */
#include <generated/randstruct_hash.h>
#undef RANDSTRUCT_HASHED_SEED
#endif
