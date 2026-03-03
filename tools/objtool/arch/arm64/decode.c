// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <objtool/check.h>
#include <objtool/disas.h>
#include <objtool/elf.h>
#include <objtool/arch.h>
#include <objtool/warn.h>
#include <objtool/builtin.h>

const char *arch_reg_name[CFI_NUM_REGS] = {};

int arch_ftrace_match(const char *name)
{
	return 0;
}

s64 arch_insn_adjusted_addend(struct instruction *insn, struct reloc *reloc)
{
	return reloc_addend(reloc);
}

bool arch_callee_saved_reg(unsigned char reg)
{
	return false;
}

int arch_decode_hint_reg(u8 sp_reg, int *base)
{
	exit(-1);
}

const char *arch_nop_insn(int len)
{
	exit(-1);
}

const char *arch_ret_insn(int len)
{
	exit(-1);
}

int arch_decode_instruction(struct objtool_file *file, const struct section *sec,
			    unsigned long offset, unsigned int maxlen,
			    struct instruction *insn)
{
	u32 ins;

	if (maxlen < 4) {
		ERROR_INSN(insn, "can't decode instruction");
		return -1;
	}

	/* arm64 instructions are always LE, thus no bswap_if_needed() */
	ins = le32toh(*(u32 *)(sec->data->d_buf + offset));

	/*
	 * These are the bare minimum needed for static branch detection and
	 * checksum calculations.
	 */
	if (ins == 0xd503201f) {
		/* NOP: static branch */
		insn->type = INSN_NOP;
	} else if ((ins & 0xfc000000) == 0x14000000) {
		/* B: static branch, intra-TU sibling call */
		insn->type = INSN_JUMP_UNCONDITIONAL;
		insn->immediate = sign_extend64(ins & 0x03ffffff, 25);
	} else if ((ins & 0xfc000000) == 0x94000000) {
		/* BL: intra-TU call */
		insn->type = INSN_CALL;
		insn->immediate = sign_extend64(ins & 0x03ffffff, 25);
	} else if ((ins & 0xff000000) == 0x54000000) {
		/* B.cond: intra-TU sibling call */
		insn->type = INSN_JUMP_CONDITIONAL;
		insn->immediate = sign_extend64((ins >> 5) & 0x7ffff, 18);
	} else if ((ins & 0x7e000000) == 0x34000000) {
		/* CBZ/CBNZ: intra-TU sibling call */
		insn->type = INSN_JUMP_CONDITIONAL;
		insn->immediate = sign_extend64((ins >> 5) & 0x7ffff, 18);
	} else if ((ins & 0x7e000000) == 0x36000000) {
		/* TBZ/TBNZ: intra-TU sibling call */
		insn->type = INSN_JUMP_CONDITIONAL;
		insn->immediate = sign_extend64((ins >> 5) & 0x3fff, 13);
	} else {
		insn->type = INSN_OTHER;
	}

	insn->len = 4;
	return 0;
}

size_t arch_jump_opcode_bytes(struct objtool_file *file, struct instruction *insn,
			      unsigned char *buf)
{
	u32 ins;

	ins = le32toh(*(u32 *)(insn->sec->data->d_buf + insn->offset));

	switch (insn->type) {
	case INSN_JUMP_UNCONDITIONAL:
	case INSN_CALL:
		ins &= ~0x03ffffff;
		break;
	case INSN_JUMP_CONDITIONAL:
		if ((ins & 0xff000000) == 0x54000000)
			ins &= ~0x00ffffe0;		   /* B.cond */
		else if ((ins & 0x7e000000) == 0x34000000)
			ins &= ~0x00ffffe0;		   /* CBZ/CBNZ */
		else
			ins &= ~0x0007ffe0;		   /* TBZ/TBNZ */
		break;
	default:
		break;
	}

	ins = htole32(ins);
	memcpy(buf, &ins, 4);
	return 4;
}

u64 arch_adjusted_addend(struct reloc *reloc)
{
	return reloc_addend(reloc);
}

unsigned long arch_jump_destination(struct instruction *insn)
{
	return insn->offset + (insn->immediate << 2);
}

bool arch_pc_relative_reloc(struct reloc *reloc)
{
	switch (reloc_type(reloc)) {
	case R_AARCH64_PREL64:
	case R_AARCH64_PREL32:
	case R_AARCH64_PREL16:
	case R_AARCH64_LD_PREL_LO19:
	case R_AARCH64_ADR_PREL_LO21:
	case R_AARCH64_ADR_PREL_PG_HI21:
	case R_AARCH64_ADR_PREL_PG_HI21_NC:
	case R_AARCH64_JUMP26:
	case R_AARCH64_CALL26:
	case R_AARCH64_CONDBR19:
	case R_AARCH64_TSTBR14:
		return true;
	default:
		return false;
	}
}

void arch_initial_func_cfi_state(struct cfi_init_state *state)
{
	state->cfa.base = CFI_UNDEFINED;
}

unsigned int arch_reloc_size(struct reloc *reloc)
{
	switch (reloc_type(reloc)) {
	case R_AARCH64_ABS64:
	case R_AARCH64_PREL64:
		return 8;
	case R_AARCH64_PREL16:
		return 2;
	default:
		return 4;
	}
}

#ifdef DISAS
int arch_disas_info_init(struct disassemble_info *dinfo)
{
	return disas_info_init(dinfo, bfd_arch_aarch64,
			       bfd_mach_arm_unknown, bfd_mach_aarch64,
			       NULL);
}
#endif /* DISAS */
