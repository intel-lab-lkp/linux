/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI__LINUX_BCF_H__
#define _UAPI__LINUX_BCF_H__

#include <linux/types.h>
#include <linux/bpf.h>

/* Expression Types */
#define BCF_TYPE(code)	((code) & 0x07)
#define BCF_BV		0x00	/* Bitvector */
#define BCF_BOOL	0x01	/* Boolean */
#define BCF_LIST	0x02	/* List of vals */
#define __MAX_BCF_TYPE	0x03

#define BCF_OP(code)	((code) & 0xf8)
/* Common Operations */
#define BCF_VAL		0x08	/* Value/Constant */
#define BCF_VAR		0x18	/* Variable */
#define BCF_ITE		0x28	/* If-Then-Else */

/* Bitvector Operations */
#define BCF_SDIV	0xb0
#define BCF_SMOD	0xd0
#define BCF_EXTRACT	0x38	/* Bitvector extraction */
#define BCF_SIGN_EXTEND	0x48	/* Sign extension */
#define BCF_ZERO_EXTEND	0x58	/* Zero extension */
#define BCF_BVSIZE	0x68	/* Bitvector size */
#define BCF_BVNOT	0x78	/* Bitvector not */
#define BCF_FROM_BOOL	0x88	/* Bool list to Bitvector */
#define BCF_CONCAT	0x98	/* Concatenation */
#define BCF_REPEAT	0xa8	/* Bitvector repeat */

/* Boolean Operations */
#define BCF_CONJ	0x00	/* Conjunction (AND) */
#define BCF_DISJ	0x40	/* Disjunction (OR) */
#define BCF_NOT		0x80	/* Negation */
#define BCF_IMPLIES	0x90	/* Implication */
#define BCF_XOR		0x38	/* Exclusive OR */
#define BCF_BITOF	0x48	/* Bitvector to Boolean */

/* Boolean Literals/Vals */
#define BCF_FALSE	0x00
#define BCF_TRUE	0x01

/**
 * struct bcf_expr - BCF expression structure
 * @code: Operation code (operation | type)
 * @vlen: Argument count
 * @params: Operation parameters
 * @args: Argument indices
 *
 * Parameter encoding by type:
 * - Bitvector: [7:0] bit width, except:
 *	- [15:8] and [7:0] extract `start` and `end` for EXTRACT;
 *	- [15:8] repeat count for REPEAT;
 *	- [15:8] extension size for SIGN/ZERO_EXTEND
 * - Boolean:
 *	- [0] literal value for constants;
 *	- [7:0] bit index for BITOF.
 * - List: element type encoding:
 *	- [7:0] for types;
 *	- [15:8] for type parameters, e.g., bit width.
 */
struct bcf_expr {
	__u8	code;
	__u8	vlen;
	__u16	params;
	__u32	args[];
};

#define BCF_PARAM_LOW(p)	((p) & 0xff)
#define BCF_PARAM_HIGH(p)	(((p) >> 8) & 0xff)

/* Operation-specific parameter meanings */
#define BCF_BV_WIDTH(p)		BCF_PARAM_LOW(p)
#define BCF_EXT_LEN(p)		BCF_PARAM_HIGH(p)
#define BCF_EXTRACT_START(p)	BCF_PARAM_HIGH(p)
#define BCF_EXTRACT_END(p)	BCF_PARAM_LOW(p)
#define BCF_REPEAT_N(p)		BCF_PARAM_HIGH(p)
#define BCF_BOOL_LITERAL(p)	((p) & 1)
#define BCF_BITOF_BIT(p)	BCF_PARAM_LOW(p)
#define BCF_LIST_TYPE(p)	BCF_PARAM_LOW(p)
#define BCF_LIST_TYPE_PARAM(p)	BCF_PARAM_HIGH(p)

/* BCF proof definitions */
#define BCF_MAGIC	0x0BCF

struct bcf_proof_header {
	__u32	magic;
	__u32	expr_cnt;
	__u32	step_cnt;
};

/**
 * struct bcf_proof_step - Proof step
 * @rule: Rule identifier (class | rule)
 * @premise_cnt: Number of premises
 * @param_cnt: Number of parameters
 * @args: Arguments (premises followed by parameters)
 */
struct bcf_proof_step {
	__u16	rule;
	__u8	premise_cnt;
	__u8	param_cnt;
	__u32	args[];
};

/* Rule Class */
#define BCF_RULE_CLASS(r)	((r) & 0xe000)
#define BCF_RULE_CORE		0x0000
#define BCF_RULE_BOOL		0x2000
#define BCF_RULE_BV		0x4000

#define BCF_STEP_RULE(r)	((r) & 0x1fff)

/* Core proof rules */
#define BCF_CORE_RULES(FN)  \
	FN(ASSUME)          \
	FN(EVALUATE)        \
	FN(DISTINCT_VALUES) \
	FN(ACI_NORM)        \
	FN(ABSORB)          \
	FN(REWRITE)         \
	FN(REFL)            \
	FN(SYMM)            \
	FN(TRANS)           \
	FN(CONG)            \
	FN(TRUE_INTRO)      \
	FN(TRUE_ELIM)       \
	FN(FALSE_INTRO)     \
	FN(FALSE_ELIM)

#define BCF_RULE_NAME(x) BCF_RULE_##x
#define BCF_RULE_ENUM_VARIANT(x) BCF_RULE_NAME(x),

enum bcf_core_rule {
	BCF_RULE_CORE_UNSPEC = 0,
	BCF_CORE_RULES(BCF_RULE_ENUM_VARIANT)
	__MAX_BCF_CORE_RULES,
};

/* Boolean proof rules */
#define BCF_BOOL_RULES(FN)   \
	FN(RESOLUTION)       \
	FN(FACTORING)        \
	FN(REORDERING)       \
	FN(SPLIT)            \
	FN(EQ_RESOLVE)       \
	FN(MODUS_PONENS)     \
	FN(NOT_NOT_ELIM)     \
	FN(CONTRA)           \
	FN(AND_ELIM)         \
	FN(AND_INTRO)        \
	FN(NOT_OR_ELIM)      \
	FN(IMPLIES_ELIM)     \
	FN(NOT_IMPLIES_ELIM) \
	FN(EQUIV_ELIM)       \
	FN(NOT_EQUIV_ELIM)   \
	FN(XOR_ELIM)         \
	FN(NOT_XOR_ELIM)     \
	FN(ITE_ELIM)         \
	FN(NOT_ITE_ELIM)     \
	FN(NOT_AND)          \
	FN(CNF_AND_POS)      \
	FN(CNF_AND_NEG)      \
	FN(CNF_OR_POS)       \
	FN(CNF_OR_NEG)       \
	FN(CNF_IMPLIES_POS)  \
	FN(CNF_IMPLIES_NEG)  \
	FN(CNF_EQUIV_POS)    \
	FN(CNF_EQUIV_NEG)    \
	FN(CNF_XOR_POS)      \
	FN(CNF_XOR_NEG)      \
	FN(CNF_ITE_POS)      \
	FN(CNF_ITE_NEG)      \
	FN(ITE_EQ)

enum bcf_bool_rule {
	BCF_RULE_BOOL_UNSPEC = 0,
	BCF_BOOL_RULES(BCF_RULE_ENUM_VARIANT)
	__MAX_BCF_BOOL_RULES,
};

/* Bitvector proof rules */
#define BCF_BV_RULES(FN) \
	FN(BITBLAST)     \
	FN(POLY_NORM)    \
	FN(POLY_NORM_EQ)

enum bcf_bv_rule {
	BCF_RULE_BV_UNSPEC = 0,
	BCF_BV_RULES(BCF_RULE_ENUM_VARIANT)
	__MAX_BCF_BV_RULES,
};
#undef BCF_RULE_ENUM_VARIANT

#endif /* _UAPI__LINUX_BCF_H__ */
