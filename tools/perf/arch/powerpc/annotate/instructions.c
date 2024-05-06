// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>

static struct ins_ops *powerpc__associate_instruction_ops(struct arch *arch, const char *name)
{
	int i;
	struct ins_ops *ops;

	/*
	 * - Interested only if instruction starts with 'b'.
	 * - Few start with 'b', but aren't branch instructions.
	 */
	if (name[0] != 'b'             ||
	    !strncmp(name, "bcd", 3)   ||
	    !strncmp(name, "brinc", 5) ||
	    !strncmp(name, "bper", 4))
		return NULL;

	ops = &jump_ops;

	i = strlen(name) - 1;
	if (i < 0)
		return NULL;

	/* ignore optional hints at the end of the instructions */
	if (name[i] == '+' || name[i] == '-')
		i--;

	if (name[i] == 'l' || (name[i] == 'a' && name[i-1] == 'l')) {
		/*
		 * if the instruction ends up with 'l' or 'la', then
		 * those are considered 'calls' since they update LR.
		 * ... except for 'bnl' which is branch if not less than
		 * and the absolute form of the same.
		 */
		if (strcmp(name, "bnl") && strcmp(name, "bnl+") &&
		    strcmp(name, "bnl-") && strcmp(name, "bnla") &&
		    strcmp(name, "bnla+") && strcmp(name, "bnla-"))
			ops = &call_ops;
	}
	if (name[i] == 'r' && name[i-1] == 'l')
		/*
		 * instructions ending with 'lr' are considered to be
		 * return instructions
		 */
		ops = &ret_ops;

	arch__associate_ins_ops(arch, name, ops);
	return ops;
}

/*
 * Instruction tracking function to track register state moves.
 * Example sequence:
 *    ld      r10,264(r3)
 *    mr      r31,r3
 *    <<after some sequence>
 *    ld      r9,312(r31)
 *
 * Previous instruction sequence shows that register state of r3
 * is moved to r31. update_insn_state_powerpc tracks these state
 * changes
 */
#ifdef HAVE_DWARF_SUPPORT
static void update_insn_state_powerpc(struct type_state *state,
		struct data_loc_info *dloc, Dwarf_Die *cu_die __maybe_unused,
		struct disasm_line *dl)
{
	struct annotated_insn_loc loc;
	struct annotated_op_loc *src = &loc.ops[INSN_OP_SOURCE];
	struct annotated_op_loc *dst = &loc.ops[INSN_OP_TARGET];
	struct type_state_reg *tsr;
	u32 insn_offset = dl->al.offset;

	if (annotate_get_insn_location(dloc->arch, dl, &loc) < 0)
		return;

	if (strncmp(dl->ins.name, "mr", 2))
		return;

	if (!strncmp(dl->ins.name, "mr", 2)) {
		int src_reg = src->reg1;

		src->reg1 = dst->reg1;
		dst->reg1 = src_reg;
	}

	if (!has_reg_type(state, dst->reg1))
		return;

	tsr = &state->regs[dst->reg1];

	if (!has_reg_type(state, src->reg1) ||
			!state->regs[src->reg1].ok) {
		tsr->ok = false;
		return;
	}

	tsr->type = state->regs[src->reg1].type;
	tsr->kind = state->regs[src->reg1].kind;
	tsr->ok = true;

	pr_debug("mov [%x] reg%d -> reg%d",
			insn_offset, src->reg1, dst->reg1);
	pr_debug_type_name(&tsr->type, tsr->kind);
}
#else /* HAVE_DWARF_SUPPORT */
static void update_insn_state_powerpc(struct type_state *state __maybe_unused, struct data_loc_info *dloc __maybe_unused,
		Dwarf_Die *cu_die __maybe_unused, struct disasm_line *dl __maybe_unused)
{
	return;
}
#endif /* HAVE_DWARF_SUPPORT */

static int powerpc__annotate_init(struct arch *arch, char *cpuid __maybe_unused)
{
	if (!arch->initialized) {
		arch->initialized = true;
		arch->associate_instruction_ops = powerpc__associate_instruction_ops;
		arch->objdump.comment_char      = '#';
	}

	return 0;
}
