/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MODULE_H
#define _ASM_X86_MODULE_H

#include <asm-generic/module.h>

struct its_array {
#ifdef CONFIG_MITIGATION_ITS
	void **pages;
	int num;
#endif
};

struct mod_arch_specific {
	struct its_array its_pages;
};

#endif /* _ASM_X86_MODULE_H */
