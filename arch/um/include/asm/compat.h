/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_COMPAT_H
#define __UM_COMPAT_H

#include <asm-generic/compat.h>

#if defined(CONFIG_UML_X86) && defined(CONFIG_64BIT)
/* From arch/x86/include/asm/compat.h */
#define COMPAT_UTS_MACHINE     "i686\0\0"
#endif

#endif
