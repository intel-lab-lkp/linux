#ifndef __ASM_LINKAGE_H
#define __ASM_LINKAGE_H

#ifdef __ASSEMBLY__
#include <asm/assembler.h>
#endif

#define __ALIGN .balign CONFIG_FUNCTION_ALIGNMENT
#define __ALIGN_STR ".balign " #CONFIG_FUNCTION_ALIGNMENT

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_CALL_OPS

#define PRE_FUNCTION_NOPS                                                   \
	ALIGN;                                                              \
	nops CONFIG_FUNCTION_ALIGNMENT / 4 - 2;                             \
	.pushsection __patchable_function_entries, "awo", @progbits, .text; \
	.p2align 3;                                                         \
	.8byte 1f;                                                          \
	.popsection;                                                        \
	1 :;                                                                \
	nops 2;

#define PRE_PROLOGUE_NOPS nops 2;

#elif defined(CONFIG_DYNAMIC_FTRACE_WITH_ARGS)

#define PRE_FUNCTION_NOPS

#define PRE_PROLOGUE_NOPS                                                   \
	.pushsection __patchable_function_entries, "awo", @progbits, .text; \
	.p2align 3;                                                         \
	.8byte 1f;                                                          \
	.popsection;                                                        \
	1 :;                                                                \
	nops 2;

#else

#define PRE_FUNCTION_NOPS
#define PRE_PROLOGUE_NOPS

#endif

#ifdef CONFIG_ARM64_BTI_KERNEL
#define BTI_C bti c;
#else
#define BTI_C
#endif

/*
 * When using in-kernel BTI we need to ensure that PCS-conformant
 * assembly functions have suitable annotations.  Override
 * SYM_FUNC_START to insert a BTI landing pad at the start of
 * everything, the override is done unconditionally so we're more
 * likely to notice any drift from the overridden definitions.
 */
#define SYM_FUNC_START_TRACE(name)                 \
	PRE_FUNCTION_NOPS                          \
	SYM_START(name, SYM_L_GLOBAL, SYM_A_ALIGN) \
	BTI_C                                      \
	PRE_PROLOGUE_NOPS

#define SYM_FUNC_START(name)                       \
	SYM_START(name, SYM_L_GLOBAL, SYM_A_ALIGN) \
	BTI_C

#define SYM_FUNC_START_NOALIGN(name)              \
	SYM_START(name, SYM_L_GLOBAL, SYM_A_NONE) \
	BTI_C                                     \

#define SYM_FUNC_START_LOCAL(name)                \
	SYM_START(name, SYM_L_LOCAL, SYM_A_ALIGN) \
	BTI_C                                     \

#define SYM_FUNC_START_LOCAL_NOALIGN(name)       \
	SYM_START(name, SYM_L_LOCAL, SYM_A_NONE) \
	BTI_C                                    \

#define SYM_FUNC_START_WEAK(name)                \
	SYM_START(name, SYM_L_WEAK, SYM_A_ALIGN) \
	BTI_C                                    \

#define SYM_FUNC_START_WEAK_NOALIGN(name)       \
	SYM_START(name, SYM_L_WEAK, SYM_A_NONE) \
	BTI_C                                   \

#define SYM_TYPED_FUNC_START(name)                       \
	SYM_TYPED_START(name, SYM_L_GLOBAL, SYM_A_ALIGN) \
	BTI_C                                            \

#endif
