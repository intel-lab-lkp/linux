/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_STATIC_CALL_H
#define _ASM_ARM64_STATIC_CALL_H

#include <linux/compiler.h>
#include <asm/linkage.h>

/* Generates a CFI-compliant "return 0" stub matching @reffunc signature */
#define __ARCH_DEFINE_TYPED_STUB_RET0(name, reffunc)	\
	typeof(reffunc) name;				\
	__ADDRESSABLE(name);				\
	asm(						\
	"	" __ALIGN_STR "                \n"	\
	"	.4byte	__kcfi_typeid_" #name "\n"	\
	#name ":                               \n"	\
	"	bti c                          \n"	\
	"	mov x0, xzr                    \n"	\
	"	ret"					\
	);
#define ARCH_DEFINE_TYPED_STUB_RET0(name, reffunc)	\
	__ARCH_DEFINE_TYPED_STUB_RET0(name, reffunc)

#endif /* _ASM_ARM64_STATIC_CALL_H */
