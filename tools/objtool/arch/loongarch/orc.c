// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/objtool_types.h>
#include <asm/orc_types.h>

#include <objtool/check.h>
#include <objtool/orc.h>
#include <objtool/warn.h>
#include <objtool/endianness.h>

int init_orc_entry(struct orc_entry *orc, struct cfi_state *cfi, struct instruction *insn)
{
	struct cfi_reg *bp = &cfi->regs[CFI_BP];
	struct cfi_reg *ra = &cfi->regs[CFI_RA];

	memset(orc, 0, sizeof(*orc));

	if (!cfi) {
		/*
		 * This is usually either unreachable nops/traps (which don't
		 * trigger unreachable instruction warnings), or
		 * STACK_FRAME_NON_STANDARD functions.
		 */
		orc->type = ORC_TYPE_UNDEFINED;
		return 0;
	}

	switch (cfi->type) {
	case UNWIND_HINT_TYPE_UNDEFINED:
		orc->type = ORC_TYPE_UNDEFINED;
		return 0;
	case UNWIND_HINT_TYPE_END_OF_STACK:
		orc->type = ORC_TYPE_END_OF_STACK;
		return 0;
	case UNWIND_HINT_TYPE_CALL:
		orc->type = ORC_TYPE_CALL;
		break;
	case UNWIND_HINT_TYPE_REGS:
		orc->type = ORC_TYPE_REGS;
		break;
	case UNWIND_HINT_TYPE_REGS_PARTIAL:
		orc->type = ORC_TYPE_REGS_PARTIAL;
		break;
	default:
		WARN_INSN(insn, "unknown unwind hint type %d", cfi->type);
		return -1;
	}

	orc->signal = cfi->signal;

	switch (cfi->cfa.base) {
	case CFI_SP:
		orc->sp_reg = ORC_REG_SP;
		break;
	case CFI_BP:
		orc->sp_reg = ORC_REG_BP;
		break;
	default:
		WARN_INSN(insn, "unknown CFA base reg %d", cfi->cfa.base);
		return -1;
	}

	switch (bp->base) {
	case CFI_UNDEFINED:
		orc->bp_reg = ORC_REG_UNDEFINED;
		orc->bp_offset = 0;
		break;
	case CFI_CFA:
		orc->bp_reg = ORC_REG_PREV_SP;
		orc->bp_offset = bp->offset;
		break;
	case CFI_BP:
		orc->bp_reg = ORC_REG_BP;
		break;
	default:
		WARN_INSN(insn, "unknown BP base reg %d", bp->base);
		return -1;
	}

	switch (ra->base) {
	case CFI_UNDEFINED:
		orc->ra_reg = ORC_REG_UNDEFINED;
		orc->ra_offset = 0;
		break;
	case CFI_CFA:
		orc->ra_reg = ORC_REG_PREV_SP;
		orc->ra_offset = ra->offset;
		break;
	case CFI_BP:
		orc->ra_reg = ORC_REG_BP;
		break;
	default:
		WARN_INSN(insn, "unknown RA base reg %d", ra->base);
		return -1;
	}

	orc->sp_offset = cfi->cfa.offset;

	return 0;
}

static const char *reg_name(unsigned int reg)
{
	switch (reg) {
	case ORC_REG_SP:
		return "sp";
	case ORC_REG_BP:
		return "fp";
	case ORC_REG_PREV_SP:
		return "prevsp";
	default:
		return "?";
	}
}

static const char *orc_type_name(unsigned int type)
{
	switch (type) {
	case UNWIND_HINT_TYPE_CALL:
		return "call";
	case UNWIND_HINT_TYPE_REGS:
		return "regs";
	case UNWIND_HINT_TYPE_REGS_PARTIAL:
		return "regs (partial)";
	default:
		return "?";
	}
}

static void print_reg(unsigned int reg, int offset)
{
	if (reg == ORC_REG_UNDEFINED)
		printf(" (und) ");
	else
		printf("%s + %3d", reg_name(reg), offset);

}

void orc_print_dump(struct elf *dummy_elf, struct orc_entry *orc, int i)
{
	printf("type:%s", orc_type_name(orc[i].type));

	printf(" sp:");

	print_reg(orc[i].sp_reg, bswap_if_needed(dummy_elf, orc[i].sp_offset));

	printf(" bp:");

	print_reg(orc[i].bp_reg, bswap_if_needed(dummy_elf, orc[i].bp_offset));

	printf(" ra:");

	print_reg(orc[i].ra_reg, bswap_if_needed(dummy_elf, orc[i].ra_offset));

	printf(" signal:%d\n", orc[i].signal);
}
