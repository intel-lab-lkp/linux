/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 SiFive
 */

#ifndef _ASM_RISCV_INSN_H
#define _ASM_RISCV_INSN_H

#include <linux/bits.h>

/*
 * Generate a function to check if a sequence of bits matches an instruction
 */
#define __RISCV_INSN_FUNCS(name)							\
static __always_inline bool riscv_insn_is_##name(u32 _insn)				\
{											\
	BUILD_BUG_ON(~(riscv_insn_##name##_MASK) & (riscv_insn_##name##_MATCH));	\
	return (_insn & (riscv_insn_##name##_MASK)) == (riscv_insn_##name##_MATCH);	\
}

/*
 * Generate a function to check if a sequence of bits matches an instruction
 * with constraints. Some instructions require inputs to be specific values.
 */
#define __RISCV_INSN_FUNCS_CONSTRAINED(name, constraints)				\
static __always_inline bool riscv_insn_is_##name(u32 _insn)				\
{											\
	BUILD_BUG_ON(~(riscv_insn_##name##_MASK) & (riscv_insn_##name##_MATCH));	\
	return ((_insn & (riscv_insn_##name##_MASK)) == (riscv_insn_##name##_MATCH)) && \
	       (constraints);								\
}

#define __RISCV_INSN_FUNCS_UNSUPPORTED(name)				\
static __always_inline bool riscv_insn_is_##name(u32 _insn)		\
{									\
	return 0;							\
}

#include <asm/insn_gen.h>

#define RV_INSN_OPCODE_MASK	GENMASK(6, 0)

/* parts of opcode for RVG*/
#define RVG_OPCODE_BRANCH	0x63
#define RVG_OPCODE_SYSTEM	0x73
#define RVG_SYSTEM_CSR_OFF	20
#define RVG_SYSTEM_CSR_MASK	GENMASK(12, 0)

// THESE ARE ALL ACTUALLY USED
/* parts of opcode for RVF, RVD and RVQ */
#define RVFDQ_FL_FS_WIDTH_OFF	12
#define RVFDQ_FL_FS_WIDTH_MASK	GENMASK(2, 0)
#define RVFDQ_OPCODE_FL		0x07
#define RVFDQ_OPCODE_FS		0x27

// THESE ARE ALL ACTUALLY USED
/* parts of opcode for RVV */
#define RVV_OPCODE_VECTOR	0x57
#define RVV_VL_VS_WIDTH_8	0
#define RVV_VL_VS_WIDTH_16	5
#define RVV_VL_VS_WIDTH_32	6
#define RVV_VL_VS_WIDTH_64	7
#define RVV_OPCODE_VL		RVFDQ_OPCODE_FL
#define RVV_OPCODE_VS		RVFDQ_OPCODE_FS

#define __INSN_LENGTH_MASK	_UL(0x3)
#define __INSN_LENGTH_GE_32	_UL(0x3)
#define __INSN_OPCODE_MASK	_UL(0x7F)
#define __INSN_BRANCH_OPCODE	_UL(RVG_OPCODE_BRANCH)

/* special case to catch _any_ system instruction */
static __always_inline bool riscv_insn_is_system(u32 code)
{
	return (code & RV_INSN_OPCODE_MASK) == RVG_OPCODE_SYSTEM;
}

/* special case to catch _any_ branch instruction */
static __always_inline bool riscv_insn_is_branch(u32 code)
{
	return (code & RV_INSN_OPCODE_MASK) == RVG_OPCODE_BRANCH;
}

#define INSN_OPCODE_MASK	0x007c
#define INSN_OPCODE_SHIFT	2
#define INSN_OPCODE_SYSTEM	28

#define INSN_16BIT_MASK		0x3
#define INSN_IS_16BIT(insn)	(((insn) & INSN_16BIT_MASK) != INSN_16BIT_MASK)
#define INSN_LEN(insn)		(INSN_IS_16BIT(insn) ? 2 : 4)

#define REG_MASK			\
	((1 << (5 + LOG_REGBYTES)) - (1 << LOG_REGBYTES))

#if defined(CONFIG_64BIT)
#define LOG_REGBYTES		3
#else
#define LOG_REGBYTES		2
#endif

#define RV_X_MASK(X, s, mask)  (((X) >> (s)) & (mask))

// These three are used by vector stuff
#define RVG_EXTRACT_SYSTEM_CSR(x) \
	({typeof(x) x_ = (x); RV_X_MASK(x_, RVG_SYSTEM_CSR_OFF, RVG_SYSTEM_CSR_MASK); })

#define RVFDQ_EXTRACT_FL_FS_WIDTH(x) \
	({typeof(x) x_ = (x); RV_X_MASK(x_, RVFDQ_FL_FS_WIDTH_OFF, \
				   RVFDQ_FL_FS_WIDTH_MASK); })

#define RVV_EXTRACT_VL_VS_WIDTH(x) RVFDQ_EXTRACT_FL_FS_WIDTH(x)

static inline unsigned long riscv_insn_reg_get_val(unsigned long *regs, u32 index)
{
	/* register 0 is always 0 and not stored in the register struct */
	return index ? *(regs + index) : 0;
}

static inline void riscv_insn_reg_set_val(unsigned long *regs, u32 index, unsigned long val)
{
	/* register 0 is always 0 and not stored in the register struct */
	if (index != 0)
		*(regs + index) = val;
}

#define riscv_insn_branch(_insn, regs_ptr, _opcode, _pc, _comparison, type)     \
	({                                                                      \
		unsigned long _ret;                                             \
		if ((type)riscv_insn_reg_get_val(                               \
			    regs_ptr,                                           \
			    riscv_insn_##_insn##_extract_xs1(_opcode))          \
			    _comparison(type) riscv_insn_reg_get_val(           \
				    regs_ptr,                                   \
				    riscv_insn_##_insn##_extract_xs2(_opcode))) \
			_ret = _pc + riscv_insn_##_insn##_extract_imm(_opcode); \
		else                                                            \
			_ret = _pc + 4;                                         \
		_ret;                                                           \
	})

#endif /* _ASM_RISCV_INSN_H */
