// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <dwarf-regs.h>
#include "../annotate.h"
#include "../../../arch/arm64/include/uapi/asm/perf_regs.h"

/*
 * ARM64 instruction field extraction.
 * Mirrors definitions in annotate-arm64.c.
 */
#define A64_RT(insn)	((insn) & 0x1f)
#define A64_RN(insn)	(((insn) >> 5) & 0x1f)
#define A64_RT2(insn)	(((insn) >> 10) & 0x1f)
#define A64_RM(insn)	(((insn) >> 16) & 0x1f)

/*
 * Load/Store encoding sub-class detection.
 * Derived from ARM Architecture Reference Manual, C4.1.
 *
 * Load/Store Pair (offset/pre/post): bits[29:27]=101, bit[26]=0
 * Load/Store Register:              bits[29:27]=111, bit[26]=0
 *   - Unsigned offset:              bits[25:24]=01
 *   - Pre/Post-indexed:             bits[25:24]=00, bit[21]=0
 *   - Register offset:              bits[25:24]=00, bit[21]=1, bits[11:10]=10
 */
#define A64_INSN_LS_PAIR_MASK		0x3c000000
#define A64_INSN_LS_PAIR_VAL		0x28000000

#define A64_INSN_LS_REG_MASK		0x3c000000
#define A64_INSN_LS_REG_VAL		0x38000000

#define A64_INSN_LS_UNSIGNED_MASK	0x3b000000
#define A64_INSN_LS_UNSIGNED_VAL	0x39000000

#define A64_INSN_LS_PREPOST_MASK	0x3b200000
#define A64_INSN_LS_PREPOST_VAL		0x38000000

#define A64_INSN_LS_REG_OFF_MASK	0x3b200c00
#define A64_INSN_LS_REG_OFF_VAL	0x38200800

static int arm64_get_immoff_unsigned(u32 insn)
{
	int size = (insn >> 30) & 0x3;
	int imm12 = (insn >> 10) & 0xfff;

	return imm12 << size;
}

static int arm64_get_immoff_prepost(u32 insn)
{
	int imm9 = (insn >> 12) & 0x1ff;

	/* sign-extend 9-bit immediate */
	if (imm9 & 0x100)
		imm9 |= ~0x1ff;

	return imm9;
}

static int arm64_get_immoff_pair(u32 insn)
{
	int imm7 = (insn >> 15) & 0x7f;
	int scale = 2 + ((insn >> 31) & 1);

	/* sign-extend 7-bit immediate */
	if (imm7 & 0x40)
		imm7 |= ~0x7f;

	return imm7 << scale;
}

/*
 * Fills op_loc fields depending on whether it is a source or target operand.
 *
 * ARM64 load/store encoding forms:
 *   Register (unsigned offset):  [Rn, #imm12 << scale]
 *   Register (pre/post-indexed): [Rn, #imm9]  or  [Rn], #imm9
 *   Register (register offset):  [Rn, Rm{, extend/shift}]
 *   Pair:                        [Rn, #imm7 << scale]
 *
 * For source (memory) operand: reg1=Rn (base), offset=immediate
 * For target (register) operand: reg1=Rt
 */
void get_arm64_regs(u32 raw_insn, int is_source,
		    struct annotated_op_loc *op_loc)
{
	if (is_source)
		op_loc->reg1 = A64_RN(raw_insn);
	else
		op_loc->reg1 = A64_RT(raw_insn);

	if (op_loc->multi_regs) {
		/* LDP/STP pair: second register is Rt2 (bits[14:10]) */
		if ((raw_insn & A64_INSN_LS_PAIR_MASK) == A64_INSN_LS_PAIR_VAL)
			op_loc->reg2 = A64_RT2(raw_insn);
		else
			op_loc->reg2 = A64_RM(raw_insn);
	}

	if (!op_loc->mem_ref || !is_source)
		return;

	/* Load/Store Pair */
	if ((raw_insn & A64_INSN_LS_PAIR_MASK) == A64_INSN_LS_PAIR_VAL) {
		op_loc->offset = arm64_get_immoff_pair(raw_insn);
		return;
	}

	/* Load/Store Register */
	if ((raw_insn & A64_INSN_LS_REG_MASK) == A64_INSN_LS_REG_VAL) {
		/* Unsigned offset */
		if ((raw_insn & A64_INSN_LS_UNSIGNED_MASK) == A64_INSN_LS_UNSIGNED_VAL) {
			op_loc->offset = arm64_get_immoff_unsigned(raw_insn);
			return;
		}

		/* Register offset */
		if ((raw_insn & A64_INSN_LS_REG_OFF_MASK) == A64_INSN_LS_REG_OFF_VAL) {
			op_loc->offset = 0;
			return;
		}

		/* Pre/Post-indexed */
		if ((raw_insn & A64_INSN_LS_PREPOST_MASK) == A64_INSN_LS_PREPOST_VAL) {
			op_loc->offset = arm64_get_immoff_prepost(raw_insn);
			return;
		}
	}
}

int __get_dwarf_regnum_for_perf_regnum_arm64(int perf_regnum)
{
	if (perf_regnum < 0 || perf_regnum >= PERF_REG_ARM64_MAX)
		return -ENOENT;

	return perf_regnum;
}
