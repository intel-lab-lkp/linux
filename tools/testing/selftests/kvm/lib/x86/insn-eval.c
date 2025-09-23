/*
 * Utility functions for x86 operand and address decoding
 *
 * Copyright (C) Intel Corporation 2017
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <asm/inat.h>
#include <asm/insn.h>

#include "insn-eval.h"
#include "kselftest.h"
#include "ucall_common.h"

#undef pr_fmt
#define pr_fmt(fmt) "insn: " fmt

#define savesegment(seg, value)				\
	asm("mov %%" #seg ",%0":"=r" (value) : : "memory")

static inline unsigned long regs_get_register(struct ex_regs *regs,
					      unsigned int offset)
{
	return *(unsigned long *)((unsigned long)regs + offset);
}

enum reg_type {
	REG_TYPE_RM = 0,
	REG_TYPE_REG,
	REG_TYPE_INDEX,
	REG_TYPE_BASE,
};

static __always_inline int user_mode(struct ex_regs *regs)
{
	return !!(regs->cs & 3);
}

/*
 * is_string_insn() - Determine if instruction is a string instruction
 * @insn:	Instruction containing the opcode to inspect
 *
 * Returns:
 *
 * true if the instruction, determined by the opcode, is any of the
 * string instructions as defined in the Intel Software Development manual.
 * False otherwise.
 */
static bool is_string_insn(struct insn *insn)
{
	/* All string instructions have a 1-byte opcode. */
	if (insn->opcode.nbytes != 1)
		return false;

	switch (insn->opcode.bytes[0]) {
	case 0x6c ... 0x6f:	/* INS, OUTS */
	case 0xa4 ... 0xa7:	/* MOVS, CMPS */
	case 0xaa ... 0xaf:	/* STOS, LODS, SCAS */
		return true;
	default:
		return false;
	}
}

/*
 * insn_has_rep_prefix() - Determine if instruction has a REP prefix
 * @insn:	Instruction containing the prefix to inspect
 *
 * Returns:
 *
 * true if the instruction has a REP prefix, false if not.
 */
bool insn_has_rep_prefix(struct insn *insn)
{
	insn_byte_t p;
	int i;

	insn_get_prefixes(insn);

	for_each_insn_prefix(insn, i, p) {
		if (p == 0xf2 || p == 0xf3)
			return true;
	}

	return false;
}

/*
 * get_seg_reg_override_idx() - obtain segment register override index
 * @insn:	Valid instruction with segment override prefixes
 *
 * Inspect the instruction prefixes in @insn and find segment overrides, if any.
 *
 * Returns:
 *
 * A constant identifying the segment register to use, among CS, SS, DS,
 * ES, FS, or GS. INAT_SEG_REG_DEFAULT is returned if no segment override
 * prefixes were found.
 *
 * -EINVAL in case of error.
 */
static int get_seg_reg_override_idx(struct insn *insn)
{
	int idx = INAT_SEG_REG_DEFAULT;
	int num_overrides = 0, i;
	insn_byte_t p;

	insn_get_prefixes(insn);

	/* Look for any segment override prefixes. */
	for_each_insn_prefix(insn, i, p) {
		insn_attr_t attr;

		attr = inat_get_opcode_attribute(p);
		switch (attr) {
		case INAT_MAKE_PREFIX(INAT_PFX_CS):
			idx = INAT_SEG_REG_CS;
			num_overrides++;
			break;
		case INAT_MAKE_PREFIX(INAT_PFX_SS):
			idx = INAT_SEG_REG_SS;
			num_overrides++;
			break;
		case INAT_MAKE_PREFIX(INAT_PFX_DS):
			idx = INAT_SEG_REG_DS;
			num_overrides++;
			break;
		case INAT_MAKE_PREFIX(INAT_PFX_ES):
			idx = INAT_SEG_REG_ES;
			num_overrides++;
			break;
		case INAT_MAKE_PREFIX(INAT_PFX_FS):
			idx = INAT_SEG_REG_FS;
			num_overrides++;
			break;
		case INAT_MAKE_PREFIX(INAT_PFX_GS):
			idx = INAT_SEG_REG_GS;
			num_overrides++;
			break;
		/* No default action needed. */
		}
	}

	/* More than one segment override prefix leads to undefined behavior. */
	if (num_overrides > 1)
		return -EINVAL;

	return idx;
}

/*
 * check_seg_overrides() - check if segment override prefixes are allowed
 * @insn:	Valid instruction with segment override prefixes
 * @regoff:	Operand offset, in ex_regs, for which the check is performed
 *
 * For a particular register used in register-indirect addressing, determine if
 * segment override prefixes can be used. Specifically, no overrides are allowed
 * for rDI if used with a string instruction.
 *
 * Returns:
 *
 * True if segment override prefixes can be used with the register indicated
 * in @regoff. False if otherwise.
 */
static bool check_seg_overrides(struct insn *insn, int regoff)
{
	if (regoff == offsetof(struct ex_regs, rdi) && is_string_insn(insn))
		return false;

	return true;
}

/*
 * resolve_default_seg() - resolve default segment register index for an operand
 * @insn:	Instruction with opcode and address size. Must be valid.
 * @regs:	Register values as seen when entering kernel mode
 * @off:	Operand offset, in ex_regs, for which resolution is needed
 *
 * Resolve the default segment register index associated with the instruction
 * operand register indicated by @off. Such index is resolved based on defaults
 * described in the Intel Software Development Manual.
 *
 * Returns:
 *
 * If in protected mode, a constant identifying the segment register to use,
 * among CS, SS, ES or DS. If in long mode, INAT_SEG_REG_IGNORE.
 *
 * -EINVAL in case of error.
 */
static int resolve_default_seg(struct insn *insn, struct ex_regs *regs, int off)
{
	return INAT_SEG_REG_IGNORE;
}

/*
 * resolve_seg_reg() - obtain segment register index
 * @insn:	Instruction with operands
 * @regs:	Register values as seen when entering kernel mode
 * @regoff:	Operand offset, in ex_regs, used to determine segment register
 *
 * Determine the segment register associated with the operands and, if
 * applicable, prefixes and the instruction pointed by @insn.
 *
 * The segment register associated to an operand used in register-indirect
 * addressing depends on:
 *
 * a) Whether running in long mode (in such a case segments are ignored, except
 * if FS or GS are used).
 *
 * b) Whether segment override prefixes can be used. Certain instructions and
 *    registers do not allow override prefixes.
 *
 * c) Whether segment overrides prefixes are found in the instruction prefixes.
 *
 * d) If there are not segment override prefixes or they cannot be used, the
 *    default segment register associated with the operand register is used.
 *
 * The function checks first if segment override prefixes can be used with the
 * operand indicated by @regoff. If allowed, obtain such overridden segment
 * register index. Lastly, if not prefixes were found or cannot be used, resolve
 * the segment register index to use based on the defaults described in the
 * Intel documentation. In long mode, all segment register indexes will be
 * ignored, except if overrides were found for FS or GS. All these operations
 * are done using helper functions.
 *
 * The operand register, @regoff, is represented as the offset from the base of
 * ex_regs.
 *
 * As stated, the main use of this function is to determine the segment register
 * index based on the instruction, its operands and prefixes. Hence, @insn
 * must be valid. However, if @regoff indicates rIP, we don't need to inspect
 * @insn at all as in this case CS is used in all cases. This case is checked
 * before proceeding further.
 *
 * Please note that this function does not return the value in the segment
 * register (i.e., the segment selector) but our defined index. The segment
 * selector needs to be obtained using get_segment_selector() and passing the
 * segment register index resolved by this function.
 *
 * Returns:
 *
 * An index identifying the segment register to use, among CS, SS, DS,
 * ES, FS, or GS. INAT_SEG_REG_IGNORE is returned if running in long mode.
 *
 * -EINVAL in case of error.
 */
static int resolve_seg_reg(struct insn *insn, struct ex_regs *regs, int regoff)
{
	int idx;

	/*
	 * In the unlikely event of having to resolve the segment register
	 * index for rIP, do it first. Segment override prefixes should not
	 * be used. Hence, it is not necessary to inspect the instruction,
	 * which may be invalid at this point.
	 */
	if (regoff == offsetof(struct ex_regs, rip))
		return INAT_SEG_REG_IGNORE;

	if (!insn)
		return -EINVAL;

	if (!check_seg_overrides(insn, regoff))
		return resolve_default_seg(insn, regs, regoff);

	idx = get_seg_reg_override_idx(insn);
	if (idx < 0)
		return idx;

	if (idx == INAT_SEG_REG_DEFAULT)
		return resolve_default_seg(insn, regs, regoff);

	/*
	 * In long mode, segment override prefixes are ignored, except for
	 * overrides for FS and GS.
	 */
	if (idx != INAT_SEG_REG_FS && idx != INAT_SEG_REG_GS)
		idx = INAT_SEG_REG_IGNORE;

	return idx;
}
/*
 * get_segment_selector() - obtain segment selector
 * @regs:		Register values as seen when entering kernel mode
 * @seg_reg_idx:	Segment register index to use
 *
 * Obtain the segment selector from any of the CS, SS, DS, ES, FS, GS segment
 * registers. CS and SS are obtained from ex_regs. DS, ES, FS and GS are
 * obtained by reading the actual CPU registers. This done for only for
 * completeness as in X86_64 segment registers are ignored.
 *
 * Returns:
 *
 * Value of the segment selector, including null when running in
 * long mode.
 *
 * -EINVAL on error.
 */
static short get_segment_selector(struct ex_regs *regs, int seg_reg_idx)
{
	unsigned short sel;

	switch (seg_reg_idx) {
	case INAT_SEG_REG_IGNORE:
		return 0;
	case INAT_SEG_REG_CS:
		return (unsigned short)(regs->cs & 0xffff);
	case INAT_SEG_REG_SS:
		return (unsigned short)(regs->ss & 0xffff);
	case INAT_SEG_REG_DS:
		savesegment(ds, sel);
		return sel;
	case INAT_SEG_REG_ES:
		savesegment(es, sel);
		return sel;
	case INAT_SEG_REG_FS:
		savesegment(fs, sel);
		return sel;
	case INAT_SEG_REG_GS:
		savesegment(gs, sel);
		return sel;
	default:
		return -EINVAL;
	}
}

static const int ex_regoff[] = {
	offsetof(struct ex_regs, rax),
	offsetof(struct ex_regs, rcx),
	offsetof(struct ex_regs, rdx),
	offsetof(struct ex_regs, rbx),
	offsetof(struct ex_regs, rsp),
	offsetof(struct ex_regs, rbp),
	offsetof(struct ex_regs, rsi),
	offsetof(struct ex_regs, rdi),
	offsetof(struct ex_regs, r8),
	offsetof(struct ex_regs, r9),
	offsetof(struct ex_regs, r10),
	offsetof(struct ex_regs, r11),
	offsetof(struct ex_regs, r12),
	offsetof(struct ex_regs, r13),
	offsetof(struct ex_regs, r14),
	offsetof(struct ex_regs, r15),
};

int ex_regs_offset(struct ex_regs *regs, int regno)
{
	if ((unsigned)regno < ARRAY_SIZE(ex_regoff))
		return ex_regoff[regno];
	return -EDOM;
}

static int get_regno(struct insn *insn, enum reg_type type)
{
	int nr_registers = ARRAY_SIZE(ex_regoff);
	int regno = 0;

	/*
	 * Don't possibly decode a 32-bit instructions as
	 * reading a 64-bit-only register.
	 */
	switch (type) {
	case REG_TYPE_RM:
		regno = X86_MODRM_RM(insn->modrm.value);

		/*
		 * ModRM.mod == 0 and ModRM.rm == 5 means a 32-bit displacement
		 * follows the ModRM byte.
		 */
		if (!X86_MODRM_MOD(insn->modrm.value) && regno == 5)
			return -EDOM;

		if (X86_REX_B(insn->rex_prefix.value))
			regno += 8;
		break;

	case REG_TYPE_REG:
		regno = X86_MODRM_REG(insn->modrm.value);

		if (X86_REX_R(insn->rex_prefix.value))
			regno += 8;
		break;

	case REG_TYPE_INDEX:
		regno = X86_SIB_INDEX(insn->sib.value);
		if (X86_REX_X(insn->rex_prefix.value))
			regno += 8;

		/*
		 * If ModRM.mod != 3 and SIB.index = 4 the scale*index
		 * portion of the address computation is null. This is
		 * true only if REX.X is 0. In such a case, the SIB index
		 * is used in the address computation.
		 */
		if (X86_MODRM_MOD(insn->modrm.value) != 3 && regno == 4)
			return -EDOM;
		break;

	case REG_TYPE_BASE:
		regno = X86_SIB_BASE(insn->sib.value);
		/*
		 * If ModRM.mod is 0 and SIB.base == 5, the base of the
		 * register-indirect addressing is 0. In this case, a
		 * 32-bit displacement follows the SIB byte.
		 */
		if (!X86_MODRM_MOD(insn->modrm.value) && regno == 5)
			return -EDOM;

		if (X86_REX_B(insn->rex_prefix.value))
			regno += 8;
		break;

	default:
		__GUEST_ASSERT(false, "invalid register type: %d\n", type);
		return -EINVAL;
	}

	if (regno >= nr_registers) {
		__GUEST_ASSERT(false, "decoded an instruction with an invalid register");
		return -EINVAL;
	}
	return regno;
}

static int get_reg_offset(struct insn *insn, struct ex_regs *regs,
			  enum reg_type type)
{
	int regno = get_regno(insn, type);

	if (regno < 0)
		return regno;

	return ex_regs_offset(regs, regno);
}

/*
 * get_reg_offset_16() - Obtain offset of register indicated by instruction
 * @insn:	Instruction containing ModRM byte
 * @regs:	Register values as seen when entering kernel mode
 * @offs1:	Offset of the first operand register
 * @offs2:	Offset of the second operand register, if applicable
 *
 * Obtain the offset, in ex_regs, of the registers indicated by the ModRM byte
 * in @insn. This function is to be used with 16-bit address encodings. The
 * @offs1 and @offs2 will be written with the offset of the two registers
 * indicated by the instruction. In cases where any of the registers is not
 * referenced by the instruction, the value will be set to -EDOM.
 *
 * Returns:
 *
 * 0 on success, -EINVAL on error.
 */
static int get_reg_offset_16(struct insn *insn, struct ex_regs *regs,
			     int *offs1, int *offs2)
{
	/*
	 * 16-bit addressing can use one or two registers. Specifics of
	 * encodings are given in Table 2-1. "16-Bit Addressing Forms with the
	 * ModR/M Byte" of the Intel Software Development Manual.
	 */
	static const int regoff1[] = {
		offsetof(struct ex_regs, rbx),
		offsetof(struct ex_regs, rbx),
		offsetof(struct ex_regs, rbp),
		offsetof(struct ex_regs, rbp),
		offsetof(struct ex_regs, rsi),
		offsetof(struct ex_regs, rdi),
		offsetof(struct ex_regs, rbp),
		offsetof(struct ex_regs, rbx),
	};

	static const int regoff2[] = {
		offsetof(struct ex_regs, rsi),
		offsetof(struct ex_regs, rdi),
		offsetof(struct ex_regs, rsi),
		offsetof(struct ex_regs, rdi),
		-EDOM,
		-EDOM,
		-EDOM,
		-EDOM,
	};

	if (!offs1 || !offs2)
		return -EINVAL;

	/* Operand is a register, use the generic function. */
	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		*offs1 = insn_get_modrm_rm_off(insn, regs);
		*offs2 = -EDOM;
		return 0;
	}

	*offs1 = regoff1[X86_MODRM_RM(insn->modrm.value)];
	*offs2 = regoff2[X86_MODRM_RM(insn->modrm.value)];

	/*
	 * If ModRM.mod is 0 and ModRM.rm is 110b, then we use displacement-
	 * only addressing. This means that no registers are involved in
	 * computing the effective address. Thus, ensure that the first
	 * register offset is invalid. The second register offset is already
	 * invalid under the aforementioned conditions.
	 */
	if ((X86_MODRM_MOD(insn->modrm.value) == 0) &&
	    (X86_MODRM_RM(insn->modrm.value) == 6))
		*offs1 = -EDOM;

	return 0;
}

/*
 * insn_get_seg_base() - Obtain base address of segment descriptor.
 * @regs:		Register values as seen when entering kernel mode
 * @seg_reg_idx:	Index of the segment register pointing to seg descriptor
 *
 * Obtain the base address of the segment as indicated by the segment descriptor
 * pointed by the segment selector. The segment selector is obtained from the
 * input segment register index @seg_reg_idx.
 *
 * Returns:
 *
 * In protected mode, base address of the segment. Zero in long mode,
 * except when FS or GS are used. In virtual-8086 mode, the segment
 * selector shifted 4 bits to the right.
 *
 * -1L in case of error.
 */
unsigned long insn_get_seg_base(struct ex_regs *regs, int seg_reg_idx)
{
	unsigned long base;
	short sel;

	sel = get_segment_selector(regs, seg_reg_idx);
	if (sel < 0)
		return -1L;

	/*
	 * Only FS or GS will have a base address, the rest of
	 * the segments' bases are forced to 0.
	 */

	if (seg_reg_idx == INAT_SEG_REG_FS) {
		base = rdmsr(MSR_FS_BASE);
	} else if (seg_reg_idx == INAT_SEG_REG_GS) {
		/*
		 * swapgs was called at the kernel entry point. Thus,
		 * MSR_KERNEL_GS_BASE will have the user-space GS base.
		 */
		base = rdmsr(MSR_GS_BASE);
	} else {
		base = 0;
	}
	return base;
}

/*
 * get_seg_limit() - Obtain the limit of a segment descriptor
 * @regs:		Register values as seen when entering kernel mode
 * @seg_reg_idx:	Index of the segment register pointing to seg descriptor
 *
 * Obtain the limit of the segment as indicated by the segment descriptor
 * pointed by the segment selector. The segment selector is obtained from the
 * input segment register index @seg_reg_idx.
 *
 * Returns:
 *
 * In protected mode, the limit of the segment descriptor in bytes.
 * In long mode and virtual-8086 mode, segment limits are not enforced. Thus,
 * limit is returned as -1L to imply a limit-less segment.
 *
 * Zero is returned on error.
 */
static unsigned long get_seg_limit(struct ex_regs *regs, int seg_reg_idx)
{
	short sel;

	sel = get_segment_selector(regs, seg_reg_idx);
	if (sel < 0)
		return 0;

	return -1L;
}

/*
 * insn_get_modrm_reg_off() - Obtain register in reg part of the ModRM byte
 * @insn:	Instruction containing the ModRM byte
 * @regs:	Register values as seen when entering kernel mode
 *
 * Returns:
 *
 * The register indicated by the reg part of the ModRM byte. The
 * register is obtained as an offset from the base of ex_regs.
 */
int insn_get_modrm_reg_off(struct insn *insn, struct ex_regs *regs)
{
	return get_reg_offset(insn, regs, REG_TYPE_REG);
}

/*
 * insn_get_modrm_reg_ptr() - Obtain register pointer based on ModRM byte
 * @insn:	Instruction containing the ModRM byte
 * @regs:	Register values as seen when entering kernel mode
 *
 * Returns:
 *
 * The register indicated by the reg part of the ModRM byte.
 * The register is obtained as a pointer within ex_regs.
 */
unsigned long *insn_get_modrm_reg_ptr(struct insn *insn, struct ex_regs *regs)
{
	int offset;

	offset = insn_get_modrm_reg_off(insn, regs);
	if (offset < 0)
		return NULL;
	return (void *)regs + offset;
}

/*
 * get_seg_base_limit() - obtain base address and limit of a segment
 * @insn:	Instruction. Must be valid.
 * @regs:	Register values as seen when entering kernel mode
 * @regoff:	Operand offset, in ex_regs, used to resolve segment descriptor
 * @base:	Obtained segment base
 * @limit:	Obtained segment limit
 *
 * Obtain the base address and limit of the segment associated with the operand
 * @regoff and, if any or allowed, override prefixes in @insn. This function is
 * different from insn_get_seg_base() as the latter does not resolve the segment
 * associated with the instruction operand. If a limit is not needed (e.g.,
 * when running in long mode), @limit can be NULL.
 *
 * Returns:
 *
 * 0 on success. @base and @limit will contain the base address and of the
 * resolved segment, respectively.
 *
 * -EINVAL on error.
 */
static int get_seg_base_limit(struct insn *insn, struct ex_regs *regs,
			      int regoff, unsigned long *base,
			      unsigned long *limit)
{
	int seg_reg_idx;

	if (!base)
		return -EINVAL;

	seg_reg_idx = resolve_seg_reg(insn, regs, regoff);
	if (seg_reg_idx < 0)
		return seg_reg_idx;

	*base = insn_get_seg_base(regs, seg_reg_idx);
	if (*base == -1L)
		return -EINVAL;

	if (!limit)
		return 0;

	*limit = get_seg_limit(regs, seg_reg_idx);
	if (!(*limit))
		return -EINVAL;

	return 0;
}

/*
 * get_eff_addr_reg() - Obtain effective address from register operand
 * @insn:	Instruction. Must be valid.
 * @regs:	Register values as seen when entering kernel mode
 * @regoff:	Obtained operand offset, in ex_regs, with the effective address
 * @eff_addr:	Obtained effective address
 *
 * Obtain the effective address stored in the register operand as indicated by
 * the ModRM byte. This function is to be used only with register addressing
 * (i.e.,  ModRM.mod is 3). The effective address is saved in @eff_addr. The
 * register operand, as an offset from the base of ex_regs, is saved in @regoff;
 * such offset can then be used to resolve the segment associated with the
 * operand. This function can be used with any of the supported address sizes
 * in x86.
 *
 * Returns:
 *
 * 0 on success. @eff_addr will have the effective address stored in the
 * operand indicated by ModRM. @regoff will have such operand as an offset from
 * the base of ex_regs.
 *
 * -EINVAL on error.
 */
static int get_eff_addr_reg(struct insn *insn, struct ex_regs *regs,
			    int *regoff, long *eff_addr)
{
	int ret;

	ret = insn_get_modrm(insn);
	if (ret)
		return ret;

	if (X86_MODRM_MOD(insn->modrm.value) != 3)
		return -EINVAL;

	*regoff = get_reg_offset(insn, regs, REG_TYPE_RM);
	if (*regoff < 0)
		return -EINVAL;

	/* Ignore bytes that are outside the address size. */
	if (insn->addr_bytes == 2)
		*eff_addr = regs_get_register(regs, *regoff) & 0xffff;
	else if (insn->addr_bytes == 4)
		*eff_addr = regs_get_register(regs, *regoff) & 0xffffffff;
	else /* 64-bit address */
		*eff_addr = regs_get_register(regs, *regoff);

	return 0;
}

/*
 * get_eff_addr_modrm() - Obtain referenced effective address via ModRM
 * @insn:	Instruction. Must be valid.
 * @regs:	Register values as seen when entering kernel mode
 * @regoff:	Obtained operand offset, in ex_regs, associated with segment
 * @eff_addr:	Obtained effective address
 *
 * Obtain the effective address referenced by the ModRM byte of @insn. After
 * identifying the registers involved in the register-indirect memory reference,
 * its value is obtained from the operands in @regs. The computed address is
 * stored @eff_addr. Also, the register operand that indicates the associated
 * segment is stored in @regoff, this parameter can later be used to determine
 * such segment.
 *
 * Returns:
 *
 * 0 on success. @eff_addr will have the referenced effective address. @regoff
 * will have a register, as an offset from the base of ex_regs, that can be used
 * to resolve the associated segment.
 *
 * -EINVAL on error.
 */
static int get_eff_addr_modrm(struct insn *insn, struct ex_regs *regs,
			      int *regoff, long *eff_addr)
{
	long tmp;
	int ret;

	if (insn->addr_bytes != 8 && insn->addr_bytes != 4)
		return -EINVAL;

	ret = insn_get_modrm(insn);
	if (ret)
		return ret;

	if (X86_MODRM_MOD(insn->modrm.value) > 2)
		return -EINVAL;

	*regoff = get_reg_offset(insn, regs, REG_TYPE_RM);

	/*
	 * -EDOM means that we must ignore the address_offset. In such a case,
	 * in 64-bit mode the effective address relative to the rIP of the
	 * following instruction.
	 */
	if (*regoff == -EDOM) {
		tmp = regs->rip + insn->length;
	} else if (*regoff < 0) {
		return -EINVAL;
	} else {
		tmp = regs_get_register(regs, *regoff);
	}

	if (insn->addr_bytes == 4) {
		int addr32 = (int)(tmp & 0xffffffff) + insn->displacement.value;

		*eff_addr = addr32 & 0xffffffff;
	} else {
		*eff_addr = tmp + insn->displacement.value;
	}

	return 0;
}

/*
 * get_eff_addr_modrm_16() - Obtain referenced effective address via ModRM
 * @insn:	Instruction. Must be valid.
 * @regs:	Register values as seen when entering kernel mode
 * @regoff:	Obtained operand offset, in ex_regs, associated with segment
 * @eff_addr:	Obtained effective address
 *
 * Obtain the 16-bit effective address referenced by the ModRM byte of @insn.
 * After identifying the registers involved in the register-indirect memory
 * reference, its value is obtained from the operands in @regs. The computed
 * address is stored @eff_addr. Also, the register operand that indicates
 * the associated segment is stored in @regoff, this parameter can later be used
 * to determine such segment.
 *
 * Returns:
 *
 * 0 on success. @eff_addr will have the referenced effective address. @regoff
 * will have a register, as an offset from the base of ex_regs, that can be used
 * to resolve the associated segment.
 *
 * -EINVAL on error.
 */
static int get_eff_addr_modrm_16(struct insn *insn, struct ex_regs *regs,
				 int *regoff, short *eff_addr)
{
	int addr_offset1, addr_offset2, ret;
	short addr1 = 0, addr2 = 0, displacement;

	if (insn->addr_bytes != 2)
		return -EINVAL;

	insn_get_modrm(insn);

	if (!insn->modrm.nbytes)
		return -EINVAL;

	if (X86_MODRM_MOD(insn->modrm.value) > 2)
		return -EINVAL;

	ret = get_reg_offset_16(insn, regs, &addr_offset1, &addr_offset2);
	if (ret < 0)
		return -EINVAL;

	/*
	 * Don't fail on invalid offset values. They might be invalid because
	 * they cannot be used for this particular value of ModRM. Instead, use
	 * them in the computation only if they contain a valid value.
	 */
	if (addr_offset1 != -EDOM)
		addr1 = regs_get_register(regs, addr_offset1) & 0xffff;

	if (addr_offset2 != -EDOM)
		addr2 = regs_get_register(regs, addr_offset2) & 0xffff;

	displacement = insn->displacement.value & 0xffff;
	*eff_addr = addr1 + addr2 + displacement;

	/*
	 * The first operand register could indicate to use of either SS or DS
	 * registers to obtain the segment selector.  The second operand
	 * register can only indicate the use of DS. Thus, the first operand
	 * will be used to obtain the segment selector.
	 */
	*regoff = addr_offset1;

	return 0;
}

/*
 * get_eff_addr_sib() - Obtain referenced effective address via SIB
 * @insn:	Instruction. Must be valid.
 * @regs:	Register values as seen when entering kernel mode
 * @base_offset: Obtained operand offset, in ex_regs, associated with segment
 * @eff_addr:	Obtained effective address
 *
 * Obtain the effective address referenced by the SIB byte of @insn. After
 * identifying the registers involved in the indexed, register-indirect memory
 * reference, its value is obtained from the operands in @regs. The computed
 * address is stored @eff_addr. Also, the register operand that indicates the
 * associated segment is stored in @base_offset; this parameter can later be
 * used to determine such segment.
 *
 * Returns:
 *
 * 0 on success. @eff_addr will have the referenced effective address.
 * @base_offset will have a register, as an offset from the base of ex_regs,
 * that can be used to resolve the associated segment.
 *
 * Negative value on error.
 */
static int get_eff_addr_sib(struct insn *insn, struct ex_regs *regs,
			    int *base_offset, long *eff_addr)
{
	long base, indx;
	int indx_offset;
	int ret;

	if (insn->addr_bytes != 8 && insn->addr_bytes != 4)
		return -EINVAL;

	ret = insn_get_modrm(insn);
	if (ret)
		return ret;

	if (!insn->modrm.nbytes)
		return -EINVAL;

	if (X86_MODRM_MOD(insn->modrm.value) > 2)
		return -EINVAL;

	ret = insn_get_sib(insn);
	if (ret)
		return ret;

	if (!insn->sib.nbytes)
		return -EINVAL;

	*base_offset = get_reg_offset(insn, regs, REG_TYPE_BASE);
	indx_offset = get_reg_offset(insn, regs, REG_TYPE_INDEX);

	/*
	 * Negative values in the base and index offset means an error when
	 * decoding the SIB byte. Except -EDOM, which means that the registers
	 * should not be used in the address computation.
	 */
	if (*base_offset == -EDOM)
		base = 0;
	else if (*base_offset < 0)
		return -EINVAL;
	else
		base = regs_get_register(regs, *base_offset);

	if (indx_offset == -EDOM)
		indx = 0;
	else if (indx_offset < 0)
		return -EINVAL;
	else
		indx = regs_get_register(regs, indx_offset);

	if (insn->addr_bytes == 4) {
		int addr32, base32, idx32;

		base32 = base & 0xffffffff;
		idx32 = indx & 0xffffffff;

		addr32 = base32 + idx32 * (1 << X86_SIB_SCALE(insn->sib.value));
		addr32 += insn->displacement.value;

		*eff_addr = addr32 & 0xffffffff;
	} else {
		*eff_addr = base + indx * (1 << X86_SIB_SCALE(insn->sib.value));
		*eff_addr += insn->displacement.value;
	}

	return 0;
}

/*
 * get_addr_ref_16() - Obtain the 16-bit address referred by instruction
 * @insn:	Instruction containing ModRM byte and displacement
 * @regs:	Register values as seen when entering kernel mode
 *
 * This function is to be used with 16-bit address encodings. Obtain the memory
 * address referred by the instruction's ModRM and displacement bytes. Also, the
 * segment used as base is determined by either any segment override prefixes in
 * @insn or the default segment of the registers involved in the address
 * computation. In protected mode, segment limits are enforced.
 *
 * Returns:
 *
 * Linear address referenced by the instruction operands on success.
 *
 * -1L on error.
 */
static void __user *get_addr_ref_16(struct insn *insn, struct ex_regs *regs)
{
	unsigned long linear_addr = -1L, seg_base, seg_limit;
	int ret, regoff;
	short eff_addr;
	long tmp;

	if (insn_get_displacement(insn))
		goto out;

	if (insn->addr_bytes != 2)
		goto out;

	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		ret = get_eff_addr_reg(insn, regs, &regoff, &tmp);
		if (ret)
			goto out;

		eff_addr = tmp;
	} else {
		ret = get_eff_addr_modrm_16(insn, regs, &regoff, &eff_addr);
		if (ret)
			goto out;
	}

	ret = get_seg_base_limit(insn, regs, regoff, &seg_base, &seg_limit);
	if (ret)
		goto out;

	/*
	 * Before computing the linear address, make sure the effective address
	 * is within the limits of the segment. In virtual-8086 mode, segment
	 * limits are not enforced. In such a case, the segment limit is -1L to
	 * reflect this fact.
	 */
	if ((unsigned long)(eff_addr & 0xffff) > seg_limit)
		goto out;

	linear_addr = (unsigned long)(eff_addr & 0xffff) + seg_base;


out:
	return (void __user *)linear_addr;
}

/*
 * get_addr_ref_32() - Obtain a 32-bit linear address
 * @insn:	Instruction with ModRM, SIB bytes and displacement
 * @regs:	Register values as seen when entering kernel mode
 *
 * This function is to be used with 32-bit address encodings to obtain the
 * linear memory address referred by the instruction's ModRM, SIB,
 * displacement bytes and segment base address, as applicable. If in protected
 * mode, segment limits are enforced.
 *
 * Returns:
 *
 * Linear address referenced by instruction and registers on success.
 *
 * -1L on error.
 */
static void __user *get_addr_ref_32(struct insn *insn, struct ex_regs *regs)
{
	unsigned long linear_addr = -1L, seg_base, seg_limit;
	int eff_addr, regoff;
	long tmp;
	int ret;

	if (insn->addr_bytes != 4)
		goto out;

	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		ret = get_eff_addr_reg(insn, regs, &regoff, &tmp);
		if (ret)
			goto out;

		eff_addr = tmp;

	} else {
		if (insn->sib.nbytes) {
			ret = get_eff_addr_sib(insn, regs, &regoff, &tmp);
			if (ret)
				goto out;

			eff_addr = tmp;
		} else {
			ret = get_eff_addr_modrm(insn, regs, &regoff, &tmp);
			if (ret)
				goto out;

			eff_addr = tmp;
		}
	}

	ret = get_seg_base_limit(insn, regs, regoff, &seg_base, &seg_limit);
	if (ret)
		goto out;

	/*
	 * Data type long could be 64 bits in size. Ensure that our 32-bit
	 * effective address is not sign-extended when computing the linear
	 * address.
	 */
	linear_addr = (unsigned long)(eff_addr & 0xffffffff) + seg_base;


out:
	return (void __user *)linear_addr;
}

/*
 * get_addr_ref_64() - Obtain a 64-bit linear address
 * @insn:	Instruction struct with ModRM and SIB bytes and displacement
 * @regs:	Structure with register values as seen when entering kernel mode
 *
 * This function is to be used with 64-bit address encodings to obtain the
 * linear memory address referred by the instruction's ModRM, SIB,
 * displacement bytes and segment base address, as applicable.
 *
 * Returns:
 *
 * Linear address referenced by instruction and registers on success.
 *
 * -1L on error.
 */
static void __user *get_addr_ref_64(struct insn *insn, struct ex_regs *regs)
{
	unsigned long linear_addr = -1L, seg_base;
	int regoff, ret;
	long eff_addr;

	if (insn->addr_bytes != 8)
		goto out;

	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		ret = get_eff_addr_reg(insn, regs, &regoff, &eff_addr);
		if (ret)
			goto out;

	} else {
		if (insn->sib.nbytes) {
			ret = get_eff_addr_sib(insn, regs, &regoff, &eff_addr);
			if (ret)
				goto out;
		} else {
			ret = get_eff_addr_modrm(insn, regs, &regoff, &eff_addr);
			if (ret)
				goto out;
		}

	}

	ret = get_seg_base_limit(insn, regs, regoff, &seg_base, NULL);
	if (ret)
		goto out;

	linear_addr = (unsigned long)eff_addr + seg_base;

out:
	return (void __user *)linear_addr;
}

/*
 * insn_get_addr_ref() - Obtain the linear address referred by instruction
 * @insn:	Instruction structure containing ModRM byte and displacement
 * @regs:	Structure with register values as seen when entering kernel mode
 *
 * Obtain the linear address referred by the instruction's ModRM, SIB and
 * displacement bytes, and segment base, as applicable. In protected mode,
 * segment limits are enforced.
 *
 * Returns:
 *
 * Linear address referenced by instruction and registers on success.
 *
 * -1L on error.
 */
void __user *insn_get_addr_ref(struct insn *insn, struct ex_regs *regs)
{
	if (!insn || !regs)
		return (void __user *)-1L;

	if (insn_get_opcode(insn))
		return (void __user *)-1L;

	switch (insn->addr_bytes) {
	case 2:
		return get_addr_ref_16(insn, regs);
	case 4:
		return get_addr_ref_32(insn, regs);
	case 8:
		return get_addr_ref_64(insn, regs);
	default:
		return (void __user *)-1L;
	}
}

/*
 * insn_decode_mmio() - Decode a MMIO instruction
 * @insn:	Structure to store decoded instruction
 * @bytes:	Returns size of memory operand
 *
 * Decodes instruction that used for Memory-mapped I/O.
 *
 * Returns:
 *
 * Type of the instruction. Size of the memory operand is stored in
 * @bytes. If decode failed, INSN_MMIO_DECODE_FAILED returned.
 */
enum insn_mmio_type insn_decode_mmio(struct insn *insn, int *bytes)
{
	enum insn_mmio_type type = INSN_MMIO_DECODE_FAILED;

	*bytes = 0;

	if (insn_get_opcode(insn))
		return INSN_MMIO_DECODE_FAILED;

	switch (insn->opcode.bytes[0]) {
	case 0x88: /* MOV m8,r8 */
		*bytes = 1;
	case 0x89: /* MOV m16/m32/m64, r16/m32/m64 */
		if (!*bytes)
			*bytes = insn->opnd_bytes;
		type = INSN_MMIO_WRITE;
		break;

	case 0xc6: /* MOV m8, imm8 */
		*bytes = 1;
	case 0xc7: /* MOV m16/m32/m64, imm16/imm32/imm64 */
		if (!*bytes)
			*bytes = insn->opnd_bytes;
		type = INSN_MMIO_WRITE_IMM;
		break;

	case 0x8a: /* MOV r8, m8 */
		*bytes = 1;
	case 0x8b: /* MOV r16/r32/r64, m16/m32/m64 */
		if (!*bytes)
			*bytes = insn->opnd_bytes;
		type = INSN_MMIO_READ;
		break;

	case 0xa4: /* MOVS m8, m8 */
		*bytes = 1;
	case 0xa5: /* MOVS m16/m32/m64, m16/m32/m64 */
		if (!*bytes)
			*bytes = insn->opnd_bytes;
		type = INSN_MMIO_MOVS;
		break;

	case 0x0f: /* Two-byte instruction */
		switch (insn->opcode.bytes[1]) {
		case 0xb6: /* MOVZX r16/r32/r64, m8 */
			*bytes = 1;
		case 0xb7: /* MOVZX r32/r64, m16 */
			if (!*bytes)
				*bytes = 2;
			type = INSN_MMIO_READ_ZERO_EXTEND;
			break;

		case 0xbe: /* MOVSX r16/r32/r64, m8 */
			*bytes = 1;
		case 0xbf: /* MOVSX r32/r64, m16 */
			if (!*bytes)
				*bytes = 2;
			type = INSN_MMIO_READ_SIGN_EXTEND;
			break;
		}
		break;
	}

	return type;
}
