// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/zalloc.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <regex.h>
#include "../annotate.h"
#include "../disasm.h"
#include "../annotate-data.h"
#include "../debug.h"
#include "../map.h"
#include "../symbol.h"

struct arch_arm64 {
	struct arch arch;
	regex_t call_insn;
	regex_t jump_insn;
	regex_t ldst_insn; /* load and store instruction */
};

static bool arm64__is_reg(const char *op)
{
	if (!op || !*op)
		return false;

	/*
	 * General-purpose registers: x0-x30, w0-w30.
	 * Check for 'x' or 'w' prefix followed by a numeric index.
	 */
	if ((op[0] == 'x' || op[0] == 'w') && isdigit(op[1]))
		return true;

	/*
	 * Special-purpose registers:
	 * sp: stack pointer
	 * xzr/wzr: zero registers
	 */
	if (!strncmp(op, "sp", 2) || !strncmp(op, "xzr", 3) ||
	    !strncmp(op, "wzr", 3))
		return true;

	return false;
}

static bool arm64__check_multi_regs(const struct arch *arch, const char *op)
{
	const char *p = op;
	int reg_count = 0;

	while (p && *p) {
		p = skip_spaces(p);
		if (*p == arch->objdump.memory_ref_char)
			p++;

		if (arm64__is_reg(p))
			reg_count++;

		if (reg_count >= 2)
			return true;

		/* Move to next operand after comma */
		p = strchr(p, ',');
		if (p)
			p++;
	}

	return false;
}

static int arm64_mov__parse(const struct arch *arch,
			    struct ins_operands *ops,
			    struct map_symbol *ms __maybe_unused,
			    struct disasm_line *dl __maybe_unused)
{
	char *s = strchr(ops->raw, ','), *source, *endptr, *comment, prev;

	if (s == NULL)
		return -1;

	*s = '\0';
	ops->target.raw = strdup(ops->raw);
	*s = ',';

	if (ops->target.raw == NULL)
		return -1;

	/* Parse source, discarding comment if present */
	source = skip_spaces(++s);
	comment = strchr(s, arch->objdump.comment_char);

	if (comment != NULL)
		s = comment - 1;
	else
		s = strchr(s, '\0') - 1;

	while (s > source && isspace(s[0]))
		--s;
	s++;
	prev = *s;
	*s = '\0';
	ops->source.raw = strdup(source);
	*s = prev;

	if (ops->source.raw == NULL) {
		zfree(&ops->target.raw);
		return -1;
	}

	ops->source.multi_regs = arm64__check_multi_regs(arch, ops->source.raw);

	/* Parse address from source (if any) */
	ops->source.addr = strtoull(source, &endptr, 16);
	if (endptr == source)
		return 0;

	/* Parse a symbol followed by an address, if present */
	s = strchr(endptr, '<');
	if (s == NULL)
		return 0;
	endptr = strrchr(s + 1, '>');
	if (endptr == NULL)
		return 0;

	*endptr = '\0';
	*s = ' ';
	s = skip_spaces(++s);
	ops->source.name = strdup(s);
	*s = '<';
	*endptr = '>';

	return 0;
}

static int arm64_mov__scnprintf(const struct ins *ins, char *bf, size_t size,
				struct ins_operands *ops, int max_ins_name)
{
	return scnprintf(bf, size, "%-*s %s, %s", max_ins_name, ins->name,
			 ops->target.raw, ops->source.name ?: ops->source.raw);
}

static const struct ins_ops arm64_mov_ops = {
	.parse	   = arm64_mov__parse,
	.scnprintf = arm64_mov__scnprintf,
};

static bool arm64__insn_is_source_first(struct disasm_line *dl)
{
	/*
	 * Store instructions invert the standard syntax by placing the source
	 * register before the destination memory address.
	 */
	return !strncmp(dl->ins.name, "st", 2);
}

/*
 * This function is used to parse arm64 load/store instructions into
 * instruction operands.
 *
 * Typical instructions and their parsing logic:
 *
 * 1. Immediate offset:
 *    ldr   x2, [x0]                -> target="x2", source="[x0]"
 *    ldr   x2, [x0, #24]           -> target="x2", source="[x0, #24]"
 *    ldp   x19, x20, [sp, #16]     -> target="x19, x20", source="[sp, #16]"
 *
 * 2. Pre-index addressing:
 *    stp   x29, x30, [sp, #-64]!   -> target="[sp, #-64]!", source="x29, x30"
 *
 * 3. Post-index addressing:
 *    str   x1, [x0], #8            -> target="[x0], #8", source="x1"
 *    ldr   w1, [x21], #4           -> target="w1", source="[x21], #4"
 *    ldp   x29, x30, [sp], #32     -> target="x29, x30", source="[sp], #32"
 *
 * 4. Register offset / extension:
 *    ldr   x0, [x1, w0, sxtw #3]   -> target="x0", source="[x1, w0, sxtw #3]"
 *    ldr   x0, [x1, x0, lsl #3]    -> target="x0", source="[x1, x0, lsl #3]"
 *
 * 5. Atomic operations:
 *    cas   w3, w1, [x0]            -> target="w3, w1", source="[x0]"
 *    swp   x3, x0, [x2]            -> target="x3, x0", source="[x2]"
 *
 * 6. Prefetch memory:
 *    prfm  pstl1strm, [x4]         -> target="pstl1strm", source="[x4]"
 *
 * Parsing strategy:
 * Use the '[' bracket as the boundary to split the operands into left
 * and right sides. For non-store instructions, the left side is the
 * target and the right side is the source. For store instructions, the
 * roles are reversed.
 */
static int arm64_ldst__parse(const struct arch *arch,
			     struct ins_operands *ops,
			     struct map_symbol *ms __maybe_unused,
			     struct disasm_line *dl)
{
	char *s, *left, *right;

	right = s = strchr(ops->raw, arch->objdump.memory_ref_char);
	if (!s)
		return -1;

	while (s > ops->raw && *s != ',')
		--s;

	if (s == ops->raw)
		return -1;

	*s = '\0';
	left = strdup(ops->raw);

	*s = ',';
	if (!left)
		return -1;

	right = strdup(right);
	if (!right) {
		zfree(&left);
		return -1;
	}

	if (arm64__insn_is_source_first(dl)) {
		ops->source.raw = left;
		ops->source.mem_ref = false;

		ops->target.raw = right;
		ops->target.mem_ref = true;
		ops->target.multi_regs = arm64__check_multi_regs(arch, ops->target.raw);
	} else {
		ops->source.raw = right;
		ops->source.mem_ref = true;
		ops->source.multi_regs = arm64__check_multi_regs(arch, ops->source.raw);

		ops->target.raw = left;
		ops->target.mem_ref = false;
	}

	return 0;
}

static struct ins_ops arm64_ldst_ops = {
	.parse	   = arm64_ldst__parse,
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
			op_loc->addr_mode = PERF_ADDR_MODE_PRE_INDEX;
		/* Post-index: [base], #imm */
		else if (p[1] == ',' && strchr(p + 1, arch->objdump.imm_char))
			op_loc->addr_mode = PERF_ADDR_MODE_POST_INDEX;
		/* Signed offset: [base{, #imm}] */
		else
			op_loc->addr_mode = PERF_ADDR_MODE_SIGNED_OFFSET;

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
static int get_reg_index_offset(struct annotated_op_loc *op_loc)
{
	return op_loc->addr_mode == PERF_ADDR_MODE_POST_INDEX ? 0 : op_loc->offset;
}

static void adjust_reg_index_state(struct type_state *state,
				   struct annotated_op_loc *op_loc,
				   const char *insn_name, u32 insn_offset)
{
	struct type_state_reg *tsr;
	int reg = op_loc->reg1;

	if (op_loc->addr_mode != PERF_ADDR_MODE_PRE_INDEX &&
	    op_loc->addr_mode != PERF_ADDR_MODE_POST_INDEX)
		return;

	if (!has_reg_type(state, reg) || !state->regs[reg].ok)
		return;

	tsr = &state->regs[reg];
	tsr->copied_from = -1;
	tsr->offset = op_loc->offset + tsr->offset;

	pr_debug_dtp("%s [%x] %s-index %#x(reg%d) -> reg%d", insn_name,
		     insn_offset, op_loc->addr_mode == PERF_ADDR_MODE_PRE_INDEX ?
		     "pre" : "post", op_loc->offset, reg, reg);
	pr_debug_type_name(&tsr->type, tsr->kind);
}

static void update_load_insn_state(struct type_state *state,
				   struct data_loc_info *dloc,
				   struct disasm_line *dl,
				   struct annotated_op_loc *src,
				   struct annotated_op_loc *dst)
{
	struct type_state_reg *tsr;
	struct type_state_reg src_tsr;
	Dwarf_Die type_die;
	u32 insn_offset = dl->al.offset;
	int reg_offset;
	int sreg = src->reg1;
	int dreg = dst->reg1;
	int fbreg = dloc->fbreg;
	int fboff = 0;

	if (!has_reg_type(state, dreg))
		goto out_adjust;

	tsr = &state->regs[dreg];
	tsr->copied_from = -1;

	if (dloc->fb_cfa) {
		u64 ip = dloc->ms->sym->start + dl->al.offset;
		u64 pc = map__rip_2objdump(dloc->ms->map, ip);

		if (die_get_cfa(dloc->di->dbg, pc, &fbreg, &fboff) < 0)
			fbreg = -1;
	}

retry:
	/* Check stack variables with offset */
	if (sreg == fbreg || sreg == state->stack_reg) {
		struct type_state_stack *stack;
		int offset = src->offset - fboff;

		stack = find_stack_state(state, offset);
		if (stack == NULL) {
			invalidate_reg_state(tsr);
			goto out_adjust;
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
			invalidate_reg_state(tsr);
			goto out_adjust;
		}

		if (sreg == fbreg) {
			pr_debug_dtp("ldr [%x] -%#x(stack) -> reg%d",
				     insn_offset, -offset, dreg);
		} else {
			pr_debug_dtp("ldr [%x] %#x(reg%d) -> reg%d",
				     insn_offset, offset, sreg, dreg);
		}
		pr_debug_type_name(&tsr->type, tsr->kind);
		goto out_adjust;
	}

	if (!has_reg_type(state, sreg) || !state->regs[sreg].ok) {
		invalidate_reg_state(tsr);
		return;
	}

	src_tsr = state->regs[sreg];
	reg_offset = get_reg_index_offset(src);

	/* Dereference the pointer if it has one */
	if (src_tsr.kind == TSR_KIND_TYPE &&
	    die_deref_ptr_type(&src_tsr.type,
			       src_tsr.offset + reg_offset, &type_die)) {
		tsr->type = type_die;
		tsr->kind = TSR_KIND_TYPE;
		tsr->offset = 0;
		tsr->ok = true;

		if (src->multi_regs) {
			pr_debug_dtp("ldr [%x] %#x(reg%d, reg%d) -> reg%d",
				     insn_offset, reg_offset, src->reg1,
				     src->reg2, dreg);
		} else {
			pr_debug_dtp("ldr [%x] %#x(reg%d) -> reg%d",
				     insn_offset, reg_offset, sreg, dreg);
		}
		pr_debug_type_name(&tsr->type, tsr->kind);
	}
	/* Or try another register if any */
	else if (src->multi_regs && src->reg1 != src->reg2 && sreg != src->reg2) {
		sreg = src->reg2;
		goto retry;
	}

out_adjust:
	adjust_reg_index_state(state, src, "ldr", insn_offset);
}

static void update_store_insn_state(struct type_state *state,
				    struct data_loc_info *dloc,
				    struct disasm_line *dl,
				    struct annotated_op_loc *src,
				    struct annotated_op_loc *dst)
{
	struct type_state_reg *tsr;
	u32 insn_offset = dl->al.offset;
	int sreg = src->reg1;
	int dreg = dst->reg1;
	int fbreg = dloc->fbreg;
	int fboff = 0;

	if (!has_reg_type(state, sreg) || !state->regs[sreg].ok)
		goto out_adjust;

	if (dloc->fb_cfa) {
		u64 ip = dloc->ms->sym->start + dl->al.offset;
		u64 pc = map__rip_2objdump(dloc->ms->map, ip);

		if (die_get_cfa(dloc->di->dbg, pc, &fbreg, &fboff) < 0)
			fbreg = -1;
	}

	/* Check stack variables with offset */
	if (dreg == fbreg || dreg == state->stack_reg) {
		struct type_state_stack *stack;
		int offset = dst->offset - fboff;

		tsr = &state->regs[sreg];

		stack = find_stack_state(state, offset);
		if (stack) {
			if (!stack->compound)
				set_stack_state(stack, offset, tsr->kind,
						&tsr->type, tsr->offset);
		} else {
			findnew_stack_state(state, offset, tsr->kind,
					    &tsr->type, tsr->offset);
		}

		if (dst->reg1 == fbreg) {
			pr_debug_dtp("str [%x] reg%d -> -%#x(stack)",
				     insn_offset, sreg, -offset);
		} else {
			pr_debug_dtp("str [%x] reg%d -> %#x(reg%d)",
				     insn_offset, sreg, offset, dreg);
		}
		if (tsr->offset != 0) {
			pr_debug_dtp(" reg%d offset %#x ->",
				     sreg, tsr->offset);
		}
		pr_debug_type_name(&tsr->type, tsr->kind);
	}

out_adjust:
	/*
	 * Store instructions do not change the register type,
	 * but the base register must be updated for pre/post-index
	 * modes.
	 */
	adjust_reg_index_state(state, dst, "str", insn_offset);
}

static void update_mov_insn_state(struct type_state *state,
				  struct disasm_line *dl,
				  struct annotated_op_loc *src,
				  struct annotated_op_loc *dst)
{
	struct type_state_reg *tsr;
	u32 insn_offset = dl->al.offset;
	int sreg = src->reg1;
	int dreg = dst->reg1;

	if (!has_reg_type(state, dreg))
		return;

	tsr = &state->regs[dreg];
	tsr->copied_from = -1;

	if (src->imm) {
		tsr->kind = TSR_KIND_CONST;
		tsr->imm_value = src->offset;
		tsr->offset = 0;
		tsr->ok = true;

		pr_debug_dtp("mov [%x] imm=%#x -> reg%d\n",
			     insn_offset, tsr->imm_value, dreg);
		return;
	}

	if (!has_reg_type(state, sreg) || !state->regs[sreg].ok) {
		invalidate_reg_state(tsr);
		return;
	}

	tsr->type = state->regs[sreg].type;
	tsr->kind = state->regs[sreg].kind;
	tsr->imm_value = state->regs[sreg].imm_value;
	tsr->offset = state->regs[sreg].offset;
	tsr->ok = state->regs[sreg].ok;

	if (tsr->kind == TSR_KIND_TYPE || tsr->kind == TSR_KIND_POINTER)
		tsr->copied_from = sreg;

	pr_debug_dtp("mov [%x] reg%d -> reg%d", insn_offset, sreg, dreg);
	pr_debug_type_name(&tsr->type, tsr->kind);
}

static void update_insn_state_arm64(struct type_state *state,
				    struct data_loc_info *dloc, Dwarf_Die *cu_die __maybe_unused,
				    struct disasm_line *dl)
{
	struct annotated_insn_loc loc;
	struct annotated_op_loc *src = &loc.ops[INSN_OP_SOURCE];
	struct annotated_op_loc *dst = &loc.ops[INSN_OP_TARGET];
	u32 insn_offset = dl->al.offset;

	if (annotate_get_insn_location(dloc->arch, dl, &loc) < 0)
		return;

	/*
	 * For unsupported instructions with a destination register, invalidate
	 * the destination register itself to prevent incorrect type propagation.
	 */
	if (has_reg_type(state, dst->reg1) &&
	    strcmp(dl->ins.name, "mov") &&
	    strncmp(dl->ins.name, "ld", 2) && strncmp(dl->ins.name, "st", 2)) {
		pr_debug_dtp("%s [%x] invalidate reg%d\n",
			     dl->ins.name, insn_offset, dst->reg1);
		invalidate_reg_state(&state->regs[dst->reg1]);
		return;
	}

	/* Register to register or imm value to register transfers */
	if (!strcmp(dl->ins.name, "mov")) {
		update_mov_insn_state(state, dl, src, dst);
		return;
	}

	/* Memory to register transfers */
	if (!strncmp(dl->ins.name, "ld", 2)) {
		update_load_insn_state(state, dloc, dl, src, dst);
		return;
	}

	/* Register to memory transfers */
	if (!strncmp(dl->ins.name, "st", 2)) {
		update_store_insn_state(state, dloc, dl, src, dst);
		return;
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
