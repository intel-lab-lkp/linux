/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ANNOTATE_H
#define _LINUX_ANNOTATE_H

#include <linux/objtool_types.h>
#include <linux/stringify.h>
#include <asm/asm.h>

#define DEFINE___ANNOTATE						\
	.macro __ANNOTATE sec, type, loc;				\
	.pushsection __ASM_C(\sec, \\sec), "M", @progbits, 8;		\
		.long __ASM_C(\loc, \\loc) - .;				\
		.long __ASM_C(\type, \\type);				\
	.popsection;							\
	.endm

#define __DEFINE_ANNOTATE(sec, name, type)				\
	.macro name loc=910b;						\
	910: __ANNOTATE sec, type, __ASM_C(\loc, \\loc);		\
	.endm

#define DEFINE_ANNOTATE(type)						\
	__DEFINE_ANNOTATE(.discard.annotate_insn,			\
			  ANNOTATE_ ## type, ANNOTYPE_ ## type)

#define DEFINE_ANNOTATE_DATA(type)					\
	__DEFINE_ANNOTATE(.discard.annotate_data,			\
			  ANNOTATE_ ## type, ANNOTYPE_ ## type)

#ifdef CONFIG_OBJTOOL

DEFINE_MACRO(__ANNOTATE);
DEFINE_MACRO(ANNOTATE(NOENDBR));
DEFINE_MACRO(ANNOTATE(RETPOLINE_SAFE));
DEFINE_MACRO(ANNOTATE(INSTR_BEGIN));
DEFINE_MACRO(ANNOTATE(INSTR_END));
DEFINE_MACRO(ANNOTATE(IGNORE_ALTERNATIVE));
DEFINE_MACRO(ANNOTATE(INTRA_FUNCTION_CALL));
DEFINE_MACRO(ANNOTATE(UNRET_BEGIN));
DEFINE_MACRO(ANNOTATE(REACHABLE));
DEFINE_MACRO(ANNOTATE(NOCFI));
DEFINE_MACRO(ANNOTATE_DATA(DATA_SPECIAL));

#ifndef __ASSEMBLY__

/*
 * Annotate away the various 'relocation to !ENDBR` complaints; knowing that
 * these relocations will never be used for indirect calls.
 */
#define ANNOTATE_NOENDBR		"ANNOTATE_NOENDBR"
#define ANNOTATE_NOENDBR_SYM(sym)	asm("ANNOTATE_NOENDBR loc=" __stringify(sym))

/*
 * This should be used immediately before an indirect jump/call. It tells
 * objtool the subsequent indirect jump/call is vouched safe for retpoline
 * builds.
 */
#define ANNOTATE_RETPOLINE_SAFE		"ANNOTATE_RETPOLINE_SAFE"
/*
 * See linux/instrumentation.h
 */
#define ANNOTATE_INSTR_BEGIN(label)	"ANNOTATE_INSTR_BEGIN loc=" __stringify(label)
#define ANNOTATE_INSTR_END(label)	"ANNOTATE_INSTR_END loc=" __stringify(label)
/*
 * objtool annotation to ignore the alternatives and only consider the original
 * instruction(s).
 */
#define ANNOTATE_IGNORE_ALTERNATIVE	"ANNOTATE_IGNORE_ALTERNATIVE"
/*
 * This macro indicates that the following intra-function call is valid.
 * Any non-annotated intra-function call will cause objtool to issue a warning.
 */
#define ANNOTATE_INTRA_FUNCTION_CALL	"ANNOTATE_INTRA_FUNCTION_CALL"
/*
 * Use objtool to validate the entry requirement that all code paths do
 * VALIDATE_UNRET_END before RET.
 *
 * NOTE: The macro must be used at the beginning of a global symbol, otherwise
 * it will be ignored.
 */
#define ANNOTATE_UNRET_BEGIN		"ANNOTATE_UNRET_BEGIN"
/*
 * This should be used to refer to an instruction that is considered
 * terminating, like a noreturn CALL or UD2 when we know they are not -- eg
 * WARN using UD2.
 */
#define ANNOTATE_REACHABLE_LABEL(label)	"ANNOTATE_REACHABLE loc=" __stringify(label)
/*
 * This should not be used; it annotates away CFI violations. There are a few
 * valid use cases like kexec handover to the next kernel image, and there is
 * no security concern there.
 *
 * There are also a few real issues annotated away, like EFI because we can't
 * control the EFI code.
 */
#define ANNOTATE_NOCFI_SYM(sym)		asm("ANNOTATE_NOCFI loc=" __stringify(sym))

/*
 * Annotate a special section entry.  This emables livepatch module generation
 * to find and extract individual special section entries as needed.
 */
#define ANNOTATE_DATA_SPECIAL		"ANNOTATE_DATA_SPECIAL"

#endif /* !__ASSEMBLY__ */

#else /* !OBJTOOL */

#define ANNOTATE_NOENDBR
#define ANNOTATE_NOENDBR_SYM(sym)
#define ANNOTATE_RETPOLINE_SAFE
#define ANNOTATE_INSTR_BEGIN(label)
#define ANNOTATE_INSTR_END(label)
#define ANNOTATE_IGNORE_ALTERNATIVE
#define ANNOTATE_INTRA_FUNCTION_CALL
#define ANNOTATE_UNRET_BEGIN
#define ANNOTATE_REACHABLE
#define ANNOTATE_REACHABLE_LABEL(label)
#define ANNOTATE_NOCFI
#define ANNOTATE_NOCFI_SYM(sym)
#define ANNOTATE_DATA_SPECIAL
#define __ANNOTATE_DATA_SPECIAL(l)

#endif /* !OBJTOOL */

#endif /* _LINUX_ANNOTATE_H */
