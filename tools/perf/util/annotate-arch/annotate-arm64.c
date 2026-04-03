// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <linux/zalloc.h>
#include <linux/string.h>
#include <regex.h>
#include <inttypes.h>
#include "../annotate.h"
#include "../disasm.h"
#include "../annotate-data.h"
#include "../debug.h"
#include "../map.h"
#include "../symbol.h"
#include "../dso.h"

struct arch_arm64 {
	struct arch arch;
	regex_t call_insn;
	regex_t jump_insn;
	regex_t ldst_insn; /* load and store instruction */
};

static bool arm64__check_multi_regs(const char *op)
{
	char *comma = strchr(op, ',');

	while (comma) {
		char *next = comma + 1;

		next = skip_spaces(next);

		/*
		 * Check the first valid character after the comma:
		 * - If it is '#', it indicates an immediate offset (e.g., [x1, #16]).
		 * - If it is an alphabetic character, it is highly likely a
		 *   register name (e.g., x, w, s, d, q, v, p, z).
		 * - Special cases: Alias and control registers like sp, xzr,
		 *   and wzr all start with an alphabetic character.
		 */
		if (*next && *next != '#' && isalpha(*next))
			return true;

		comma = strchr(next, ',');
	}

	return false;
}

static int arm64_mov__parse(const struct arch *arch __maybe_unused,
			    struct ins_operands *ops,
			    struct map_symbol *ms __maybe_unused,
			    struct disasm_line *dl __maybe_unused)
{
	char *s = strchr(ops->raw, ','), *target, *endptr, *comment, prev;

	if (s == NULL)
		return -1;

	*s = '\0';
	ops->source.raw = strdup(ops->raw);
	*s = ',';

	if (ops->source.raw == NULL)
		return -1;

	target = skip_spaces(++s);
	comment = strchr(s, arch->objdump.comment_char);

	if (comment != NULL)
		s = comment - 1;
	else
		s = strchr(s, '\0') - 1;

	while (s > target && isspace(s[0]))
		--s;
	s++;
	prev = *s;
	*s = '\0';
	ops->target.raw = strdup(target);
	*s = prev;

	if (ops->target.raw == NULL)
		goto out_free_source;

	ops->target.multi_regs = arm64__check_multi_regs(ops->target.raw);

	/* Parse address followed by symbol name, e.g. "addr <symbol>" */
	if (strchr(target, '<') != NULL) {
		ops->target.addr = strtoull(target, &endptr, 16);
		if (endptr == target)
			goto out_free_target;

		s = strchr(endptr, '<');
		if (s == NULL)
			goto out_free_target;
		endptr = strchr(s + 1, '>');
		if (endptr == NULL)
			goto out_free_target;

		*endptr = '\0';
		*s = ' ';
		ops->target.name = strdup(s);
		*s = '<';
		*endptr = '>';
		if (ops->target.name == NULL)
			goto out_free_target;
	}

	return 0;

out_free_target:
	zfree(&ops->target.raw);
out_free_source:
	zfree(&ops->source.raw);
	return -1;
}

static const struct ins_ops arm64_mov_ops = {
	.parse	   = arm64_mov__parse,
	.scnprintf = mov__scnprintf,
};

static int arm64_ldst__parse(const struct arch *arch __maybe_unused,
			     struct ins_operands *ops,
			     struct map_symbol *ms __maybe_unused,
			     struct disasm_line *dl __maybe_unused)
{
	char *s, *target;

	/*
	 * The part starting from the memory access annotation '[' is parsed
	 * as 'target', while the part before it is parsed as 'source'.
	 */
	target = s = strchr(ops->raw, arch->objdump.memory_ref_char);
	if (!s)
		return -1;

	while (s > ops->raw && *s != ',')
		--s;

	if (s == ops->raw)
		return -1;

	*s = '\0';
	ops->source.raw = strdup(ops->raw);

	*s = ',';
	if (!ops->source.raw)
		return -1;

	ops->source.multi_regs = arm64__check_multi_regs(ops->source.raw);

	ops->target.raw = strdup(target);
	if (!ops->target.raw) {
		zfree(&ops->source.raw);
		return -1;
	}
	ops->target.mem_ref = true;
	ops->target.multi_regs = arm64__check_multi_regs(ops->target.raw);

	return 0;
}

static int arm64_ldst__scnprintf(const struct ins *ins, char *bf, size_t size,
				 struct ins_operands *ops, int max_ins_name)
{
	return scnprintf(bf, size, "%-*s %s,%s", max_ins_name, ins->name,
			 ops->source.raw, ops->target.raw);
}

static struct ins_ops arm64_ldst_ops = {
	.parse	   = arm64_ldst__parse,
	.scnprintf = arm64_ldst__scnprintf,
};

static const struct ins_ops *arm64__associate_instruction_ops(struct arch *arch, const char *name)
{
	struct arch_arm64 *arm = container_of(arch, struct arch_arm64, arch);
	const struct ins_ops *ops;
	regmatch_t match[2];

	if (!regexec(&arm->jump_insn, name, 2, match, 0))
		ops = &jump_ops;
	else if (!regexec(&arm->call_insn, name, 2, match, 0))
		ops = &call_ops;
	else if (!regexec(&arm->ldst_insn, name, 2, match, 0))
		ops = &arm64_ldst_ops;
	else if (!strcmp(name, "ret"))
		ops = &ret_ops;
	else
		ops = &arm64_mov_ops;

	arch__associate_ins_ops(arch, name, ops);
	return ops;
}

static int extract_op_location_arm64(const struct arch *arch,
				     struct disasm_line *dl __maybe_unused,
				     const char *op_str, int op_idx __maybe_unused,
				     struct annotated_op_loc *op_loc)
{
	const char *s = op_str;
	char *p = NULL;

	if (op_str == NULL)
		return 0;

	/* Handle standalone immediate operands (e.g., #0x10) */
	if (*s == arch->objdump.imm_char) {
		op_loc->offset = strtol(s + 1, &p, 0);
		if (p && p != s + 1)
			op_loc->imm = true;
		return 0;
	}

	/*
	 * Handle memory references (e.g., [x0, #8]), identify
	 * arm64 specific addressing modes
	 */
	if (*s == arch->objdump.memory_ref_char) {
		op_loc->mem_ref = true;

		p = strchr(s, ']');
		if (p == NULL)
			return -1;

		/* Pre-index: [base, #imm]! */
		if (p[1] == '!')
			op_loc->addr_mode = INSN_ADDR_PRE_INDEX;
		/* Post-index: [base], #imm */
		else if (p[1] == ',' && strchr(p + 1, arch->objdump.imm_char))
			op_loc->addr_mode = INSN_ADDR_POST_INDEX;
		/* Signed offset: [base{, #imm}] */
		else
			op_loc->addr_mode = INSN_ADDR_SIGNED_OFFSET;

		s++;
	}

	/* Extract the primary register */
	op_loc->reg1 = arch__dwarf_regnum(arch, s);
	if (op_loc->reg1 == -1)
		return -1;

	/* Move to the next symbol of the operand, if any */
	s = strchr(s, ',');
	if (s == NULL)
		return 0;
	s = skip_spaces(s + 1);

	/* Parse secondary register or immediate offset */
	if (op_loc->multi_regs)
		op_loc->reg2 = arch__dwarf_regnum(arch, s);
	else if (*s == arch->objdump.imm_char)
		op_loc->offset = strtol(s + 1, &p, 0);

	return 0;
}

#ifdef HAVE_LIBDW_SUPPORT
static int get_mem_offset(struct annotated_op_loc *op_loc, int type_offset)
{
	if (op_loc->addr_mode == INSN_ADDR_POST_INDEX)
		return type_offset;

	return op_loc->offset + type_offset;
}

static void adjust_reg_index_state(struct type_state *state, int reg,
				   struct annotated_op_loc *op_loc,
				   const char *insn_name, u32 insn_offset)
{
	struct type_state_reg *tsr;

	if (!has_reg_type(state, reg) ||
	    (op_loc->addr_mode != INSN_ADDR_PRE_INDEX &&
	    op_loc->addr_mode != INSN_ADDR_POST_INDEX))
		return;

	tsr = &state->regs[reg];
	tsr->offset = op_loc->offset + tsr->offset;
	tsr->ok = true;

	pr_debug_dtp("%s [%x] %s-index %#x(reg%d) -> reg%d", insn_name,
		     insn_offset, op_loc->addr_mode == INSN_ADDR_PRE_INDEX ?
		     "pre" : "post", op_loc->offset, reg, reg);
	pr_debug_type_name(&tsr->type, tsr->kind);
}

static Dwarf_Off task_struct_off;

static void update_insn_state_arm64(struct type_state *state,
				    struct data_loc_info *dloc, Dwarf_Die *cu_die,
				    struct disasm_line *dl)
{
	struct annotated_insn_loc loc;
	struct annotated_op_loc *src = &loc.ops[INSN_OP_SOURCE];
	struct annotated_op_loc *dst = &loc.ops[INSN_OP_TARGET];
	struct type_state_reg *tsr;
	Dwarf_Die type_die;
	u32 insn_offset = dl->al.offset;
	int sreg, dreg;
	int fbreg = dloc->fbreg;
	int fboff = 0;

	if (annotate_get_insn_location(dloc->arch, dl, &loc) < 0)
		return;

	sreg = src->reg1;
	dreg = dst->reg1;

	if (!strcmp(dl->ins.name, "mrs")) {
		Dwarf_Die func_die;
		Dwarf_Attribute attr;
		u64 ip, pc;

		if (!has_reg_type(state, sreg))
			return;

		/* Handle case difference: LLVM (SP_EL0) vs objdump (sp_el0) */
		if (!dso__kernel(map__dso(dloc->ms->map)) ||
		    strcasecmp(dl->ops.target.raw, "sp_el0"))
			return;

		ip = dloc->ms->sym->start + dl->al.offset;
		pc = map__rip_2objdump(dloc->ms->map, ip);

		if (!task_struct_off ||
		    !dwarf_offdie(dloc->di->dbg, task_struct_off, &type_die)) {
			/*
			 * Find the inline function 'get_current()' Dwarf_Die
			 * and obtain its return value data type, which should
			 * be 'struct task_struct *'.
			 */
			if (!die_find_inlinefunc(cu_die, pc, &func_die) ||
			    !dwarf_attr_integrate(&func_die, DW_AT_type, &attr) ||
			    !dwarf_formref_die(&attr, &type_die))
				return;

			/*
			 * Cache the 'struct task_struct *' die offset globally.
			 * This allows us to resolve stack canary accesses even
			 * in CUs that lack a full task_struct definition (e.g.,
			 * compiler-generated entry/exit code).
			 */
			task_struct_off = dwarf_dieoffset(&type_die);
		}

		tsr = &state->regs[sreg];
		tsr->copied_from = -1;
		tsr->type = type_die;
		tsr->kind = TSR_KIND_TYPE;
		tsr->offset = 0;
		tsr->addr = 0;
		tsr->ok = true;

		pr_debug_dtp("mrs [%x] sp_el0 -> reg%d", insn_offset, sreg);
		pr_debug_type_name(&type_die, tsr->kind);
		return;
	}

	if (!strcmp(dl->ins.name, "adrp")) {
		if (!has_reg_type(state, sreg) || !dl->ops.target.addr)
			return;

		tsr = &state->regs[sreg];
		tsr->copied_from = -1;
		tsr->kind = TSR_KIND_GLOBAL_ADDR;
		/* Partial page-relative address, finalized in next 'add/ldr' */
		tsr->addr = dl->ops.target.addr;
		tsr->offset = 0;
		tsr->ok = true;

		pr_debug_dtp("adrp [%x] global addr=%"PRIx64" -> reg%d\n",
			     insn_offset, tsr->addr, sreg);
		return;
	}

	if (!strcmp(dl->ins.name, "add")) {
		struct type_state_reg dst_tsr;

		if (!has_reg_type(state, sreg) ||
		    !has_reg_type(state, dreg) ||
		    !state->regs[dreg].ok)
			return;

		tsr = &state->regs[sreg];
		tsr->copied_from = -1;
		dst_tsr = state->regs[dreg];

		/* Handle calculation of a register holding a typed pointer */
		if (dst_tsr.kind == TSR_KIND_POINTER ||
		    (dst_tsr.kind == TSR_KIND_TYPE &&
		    dwarf_tag(&dst_tsr.type) == DW_TAG_pointer_type)) {
			s32 offset;

			if (dst_tsr.kind == TSR_KIND_TYPE &&
			    __die_get_real_type(&dst_tsr.type, &type_die) == NULL)
				return;

			if (dst_tsr.kind == TSR_KIND_POINTER)
				type_die = dst_tsr.type;

			/* Check if the target type has a member at the new offset */
			offset = dst->offset + dst_tsr.offset;
			if (die_get_member_type(&type_die, offset, &type_die) == NULL)
				return;

			tsr->type = dst_tsr.type;
			tsr->kind = dst_tsr.kind;
			tsr->offset = offset;
			tsr->addr = 0;
			tsr->ok = true;

			pr_debug_dtp("add [%x] address of %s%#x(reg%d) -> reg%d",
				     insn_offset, dst->offset < 0 ? "-" : "",
				     abs(dst->offset), dreg, sreg);

			pr_debug_type_name(&tsr->type, tsr->kind);
		}

		/* Handle PC-relative global address calculation (adrp/add pair) */
		if (dst_tsr.kind == TSR_KIND_GLOBAL_ADDR) {
			tsr->kind = dst_tsr.kind;
			tsr->addr = dst_tsr.addr + dst->offset;
			tsr->offset = 0;
			tsr->ok = true;

			pr_debug_dtp("add [%x] global addr=%"PRIx64" -> reg%d\n",
				     insn_offset, tsr->addr, sreg);
			return;
		}

		/* Handle per-cpu base addresses */
		if (dst_tsr.kind == TSR_KIND_PERCPU_BASE) {
			if (!dst->multi_regs || !has_reg_type(state, dst->reg2) ||
			    state->regs[dst->reg2].kind != TSR_KIND_GLOBAL_ADDR ||
			    !state->regs[dst->reg2].ok)
				return;

			/* Inherit type from the global variable */
			tsr->type = state->regs[dst->reg2].type;
			tsr->kind = state->regs[dst->reg2].kind;
			tsr->offset = state->regs[dst->reg2].offset;
			tsr->addr = state->regs[dst->reg2].addr;
			tsr->ok = true;

			pr_debug_dtp("add [%x] percpu %#"PRIx64" -> reg%d",
				     insn_offset, tsr->addr, sreg);
			pr_debug_type_name(&tsr->type, tsr->kind);
		}

		return;
	}

	/* Register to register transfers */
	if (!strcmp(dl->ins.name, "mov")) {
		if (!has_reg_type(state, sreg))
			return;

		tsr = &state->regs[sreg];
		tsr->copied_from = -1;

		if (!has_reg_type(state, dreg) ||
		    !state->regs[dreg].ok) {
			tsr->ok = false;
			return;
		}

		tsr->type = state->regs[dreg].type;
		tsr->kind = state->regs[dreg].kind;
		tsr->offset = state->regs[dreg].offset;
		tsr->addr = state->regs[dreg].addr;
		tsr->ok = true;

		if (tsr->kind == TSR_KIND_TYPE || tsr->kind == TSR_KIND_POINTER)
			tsr->copied_from = dreg;

		pr_debug_dtp("mov [%x] reg%d -> reg%d",
			     insn_offset, dreg, sreg);
		pr_debug_type_name(&tsr->type, tsr->kind);
		return;
	}

	if (dloc->fb_cfa) {
		u64 ip = dloc->ms->sym->start + dl->al.offset;
		u64 pc = map__rip_2objdump(dloc->ms->map, ip);

		if (die_get_cfa(dloc->di->dbg, pc, &fbreg, &fboff) < 0)
			fbreg = -1;
	}

	/* Memory to register transfers */
	if (!strncmp(dl->ins.name, "ld", 2)) {
		struct type_state_reg dst_tsr;

		if (!has_reg_type(state, sreg))
			return;

		tsr = &state->regs[sreg];
		tsr->copied_from = -1;

		/* Check stack variables with offset */
		if (sreg == fbreg || sreg == state->stack_reg) {
			struct type_state_stack *stack;
			int offset = src->offset - fboff;

			stack = find_stack_state(state, offset);
			if (stack == NULL) {
				tsr->ok = false;
				return;
			} else if (!stack->compound) {
				tsr->type = stack->type;
				tsr->kind = stack->kind;
				tsr->offset = stack->ptr_offset;
				tsr->ok = true;
			} else if (die_get_member_type(&stack->type,
						       offset - stack->offset,
						       &type_die)) {
				tsr->type = type_die;
				tsr->kind = TSR_KIND_TYPE;
				tsr->offset = 0;
				tsr->ok = true;
			} else {
				tsr->ok = false;
				return;
			}

			pr_debug_dtp("ldr [%x] -%#x(stack) -> reg%d",
				     insn_offset, -offset, sreg);
			pr_debug_type_name(&tsr->type, tsr->kind);
			return;
		}

		if (!has_reg_type(state, dreg) || !state->regs[dreg].ok)
			return;

		dst_tsr = state->regs[dreg];

		/* Dereference the pointer if it has one */
		if (dst_tsr.kind == TSR_KIND_TYPE &&
		    die_deref_ptr_type(&dst_tsr.type,
				       get_mem_offset(dst, dst_tsr.offset),
				       &type_die)) {
			tsr->type = type_die;
			tsr->kind = TSR_KIND_TYPE;
			tsr->offset = 0;
			tsr->addr = 0;
			tsr->ok = true;

			pr_debug_dtp("ldr [%x] %#x(reg%d) -> reg%d",
				     insn_offset, dst->offset, dreg, sreg);
			pr_debug_type_name(&tsr->type, tsr->kind);

			adjust_reg_index_state(state, dreg, dst, "ldr", insn_offset);
			return;
		}

		/* Or check if it's a global variable */
		if (dst_tsr.kind == TSR_KIND_GLOBAL_ADDR) {
			u64 ip = dloc->ms->sym->start + dl->al.offset;
			u64 addr = dst_tsr.addr + dst->offset;
			int offset;
			u8 kind;
			const char *var_name = NULL;

			/* it might be per-cpu offset */
			if (get_global_var_info(dloc, addr, &var_name, &offset) &&
			    !strcmp(var_name, "__per_cpu_offset"))
				kind = TSR_KIND_PERCPU_BASE;
			else
				kind = TSR_KIND_TYPE;

			if (!get_global_var_type(cu_die, dloc, ip, addr, &offset,
						 &type_die) ||
			    !die_get_member_type(&type_die, offset, &type_die)) {
				tsr->ok = false;
				return;
			}

			tsr->type = type_die;
			tsr->kind = kind;
			tsr->offset = offset;
			tsr->addr = 0;
			tsr->ok = true;

			pr_debug_dtp("ldr [%x] global (%"PRIx64") -> reg%d",
				     insn_offset, addr, sreg);
			pr_debug_type_name(&tsr->type, tsr->kind);
			return;
		}

		/* Or check if it's a per-cpu base address */
		if (dst_tsr.kind == TSR_KIND_PERCPU_BASE) {
			u64 ip = dloc->ms->sym->start + dl->al.offset;
			u64 addr;
			int offset;
			/*
			 * If reg2 is a global variable, this means reg1 is
			 * an index into the variable's per-cpu array, so
			 * dereference type from reg2.
			 */
			if (!dst->multi_regs || !has_reg_type(state, dst->reg2) ||
			    state->regs[dst->reg2].kind != TSR_KIND_GLOBAL_ADDR ||
			    !state->regs[dst->reg2].ok)
				return;

			addr = state->regs[dst->reg2].addr;
			if (!get_global_var_type(cu_die, dloc, ip, addr, &offset,
						 &type_die) ||
			    !die_get_member_type(&type_die, offset, &type_die)) {
				tsr->ok = false;
				return;
			}

			tsr->type = type_die;
			tsr->kind = TSR_KIND_TYPE;
			tsr->offset = offset;
			tsr->addr = 0;
			tsr->ok = true;

			pr_debug_dtp("ldr [%x] percpu (reg%d, reg%d) -> reg%d",
				     insn_offset, dreg, dst->reg2, sreg);
			pr_debug_type_name(&tsr->type, tsr->kind);
		}
		return;
	}

	/* Register to memory transfers */
	if (!strncmp(dl->ins.name, "st", 2)) {
		/* Check stack variables with offset */
		if (dreg == fbreg || dreg == state->stack_reg) {
			struct type_state_stack *stack;
			int offset = dst->offset - fboff;

			if (!has_reg_type(state, sreg) ||
			    !state->regs[sreg].ok)
				return;

			tsr = &state->regs[sreg];

			stack = find_stack_state(state, offset);
			if (stack) {
				if (!stack->compound)
					set_stack_state(stack, offset, tsr->kind,
							&tsr->type, tsr->offset,
							tsr->addr);
			} else {
				findnew_stack_state(state, offset, tsr->kind,
						    &tsr->type, tsr->offset,
						    tsr->addr);
			}

			pr_debug_dtp("str [%x] reg%d -> -%#x(stack)",
				     insn_offset, sreg, -offset);
			if (tsr->offset != 0) {
				pr_debug_dtp(" reg%d offset %#x ->",
					     sreg, tsr->offset);
			}
			pr_debug_type_name(&tsr->type, tsr->kind);
			return;
		}

		/*
		 * Store instructions do not change the register type,
		 * but the base register must be updated for pre/post-index
		 * modes.
		 */
		adjust_reg_index_state(state, dreg, dst, "str", insn_offset);
	}
}
#endif

const struct arch *arch__new_arm64(const struct e_machine_and_e_flags *id,
				   const char *cpuid __maybe_unused)
{
	int err;
	struct arch_arm64 *arm = zalloc(sizeof(*arm));
	struct arch *arch;

	if (!arm)
		return NULL;

	arch = &arm->arch;
	arch->name = "arm64";
	arch->id = *id;
	arch->objdump.comment_char	  = '/';
	arch->objdump.skip_functions_char = '+';
	arch->objdump.memory_ref_char	  = '[';
	arch->objdump.imm_char		  = '#';
	arch->associate_instruction_ops   = arm64__associate_instruction_ops;
	arch->extract_op_location	  = extract_op_location_arm64;
#ifdef HAVE_LIBDW_SUPPORT
	arch->update_insn_state		  = update_insn_state_arm64;
#endif

	/* bl, blr */
	err = regcomp(&arm->call_insn, "^blr?$", REG_EXTENDED);
	if (err)
		goto out_free_arm;

	/* b, b.cond, br, cbz/cbnz, tbz/tbnz */
	err = regcomp(&arm->jump_insn, "^[ct]?br?\\.?(cc|cs|eq|ge|gt|hi|hs|le|lo|ls|lt|mi|ne|pl|vc|vs)?n?z?$",
		      REG_EXTENDED);
	if (err)
		goto out_free_call;

	/*
	 * The ARM64 architecture has many variants of load/store instructions.
	 * It is quite challenging to match all of them completely. Here, we
	 * only match the prefixes of these instructions.
	 */
	err = regcomp(&arm->ldst_insn, "^(ld|st|cas|prf|swp)",
		      REG_EXTENDED);
	if (err)
		goto out_free_jump;

	return arch;

out_free_jump:
	regfree(&arm->jump_insn);
out_free_call:
	regfree(&arm->call_insn);
out_free_arm:
	free(arm);
	errno = SYMBOL_ANNOTATE_ERRNO__ARCH_INIT_REGEXP;
	return NULL;
}
