/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ASM_POWERPC_VDSO_PAGE_H
#define _ASM_POWERPC_VDSO_PAGE_H

#include <vdso/const.h>

#define PAGE_SHIFT	CONFIG_PPC_PAGE_SHIFT
#define PAGE_SIZE	(UL(1) << PAGE_SHIFT)

#endif // _ASM_POWERPC_VDSO_PAGE_H
