// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/zalloc.h>
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

	/* Special-purpose registers: sp. */
	if (!strncmp(op, "sp", 2))
		return true;

	/* TODO: Support more registers. */
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

/*
 * Duplicate @insn, stripping the comment and trailing whitespace.
 * Returns a newly allocated string which the caller must free(),
 * or NULL on allocation failure or if @insn is NULL.
 */
static char *rstrip_space_and_comment(const char *insn, char comment_char)
{
	const char *end, *comment;
	size_t len;
	char *result;

	if (insn == NULL)
		return NULL;

	comment = strchr(insn, comment_char);
	if (comment != NULL)
		end = comment;
	else
		end = insn + strlen(insn);

	while (end > insn && isspace(end[-1]))
		--end;

	len = end - insn;
	result = malloc(len + 1);
	if (result == NULL)
		return NULL;

	memcpy(result, insn, len);
	result[len] = '\0';

	return result;
}

static int arm64_mov__parse(const struct arch *arch,
			    struct ins_operands *ops,
			    struct map_symbol *ms __maybe_unused,
			    struct disasm_line *dl __maybe_unused)
{
	char *s = strchr(ops->raw, ','), *source, *endptr;

	if (s == NULL)
		return -1;

	/* Parse target */
	*s = '\0';
	ops->target.raw = strdup(ops->raw);
	*s = ',';

	if (ops->target.raw == NULL)
		return -1;

	ops->target.multi_regs = arm64__check_multi_regs(arch, ops->target.raw);

	/* Parse source, stripping comment if present */
	source = skip_spaces(++s);
	ops->source.raw = rstrip_space_and_comment(source, arch->objdump.comment_char);
	if (ops->source.raw == NULL) {
		zfree(&ops->target.raw);
		return -1;
	}

	ops->source.multi_regs = arm64__check_multi_regs(arch, ops->source.raw);

	/* Parse 'addr <symbol>' from source (if any) */
	ops->source.addr = strtoull(ops->source.raw, &endptr, 16);
	if (endptr != ops->source.raw) {
		s = strchr(endptr, '<');
		if (s == NULL) {
			ops->source.addr = 0;
			return 0;
		}
		endptr = strrchr(s + 1, '>');
		if (endptr == NULL) {
			ops->source.addr = 0;
			return 0;
		}

		*endptr = '\0';
		ops->source.name = strdup(s + 1);
		*endptr = '>';
		if (ops->source.name == NULL) {
			ops->source.addr = 0;
			return 0;
		}
	}

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

static bool arm64__insn_is_source_on_left(struct disasm_line *dl)
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
 * 7. PC-relative loads (No bracket found):
 *    ldr   x0, ffff800080f40c68 <__kvm_nvhe_$d>  -> Fallback to default parser
 *
 * Parsing strategy:
 * Use the '[' bracket as the boundary to split the operands into left
 * and right sides. For non-store instructions, the left side is the
 * target and the right side is the source. For store instructions, the
 * roles are reversed.
 */
static int arm64_ldst__parse(const struct arch *arch,
			     struct ins_operands *ops,
			     struct map_symbol *ms,
			     struct disasm_line *dl)
{
	char *s, *left, *right;

	right = s = strchr(ops->raw, arch->objdump.memory_ref_char);
	if (!s) {
		/* Fallback to default parser for PC-relative loads. */
		return arm64_mov__parse(arch, ops, ms, dl);
	}

	while (s > ops->raw && *s != ',')
		--s;

	if (s == ops->raw)
		return -1;

	*s = '\0';
	left = strdup(ops->raw);

	*s = ',';
	if (!left)
		return -1;

	right = rstrip_space_and_comment(right, arch->objdump.comment_char);
	if (!right) {
		zfree(&left);
		return -1;
	}

	if (arm64__insn_is_source_on_left(dl)) {
		ops->source.raw = left;
		ops->source.mem_ref = false;
		ops->target.raw = right;
		ops->target.mem_ref = true;
	} else {
		ops->source.raw = right;
		ops->source.mem_ref = true;
		ops->target.raw = left;
		ops->target.mem_ref = false;
	}

	ops->source.multi_regs = arm64__check_multi_regs(arch, ops->source.raw);
	ops->target.multi_regs = arm64__check_multi_regs(arch, ops->target.raw);

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
	 * Handle memory references, identify arm64 specific addressing modes.
	 * Reference: Arm Architecture Reference Manual
	 *            (DDI 0487), Chapter C1.3.3: Load/store addressing modes.
	 */
	if (*s == arch->objdump.memory_ref_char) {
		op_loc->mem_ref = true;

		p = strchr(s, ']');
		if (p == NULL)
			return -1;

		/* Pre-index: [base, #imm]! */
		if (p[1] == '!')
			op_loc->addr_mode = PERF_ADDR_MODE_PRE_INDEX;
		/* Post-index: [base], #imm|reg */
		else if (p[1] == ',' &&
			 (strchr(p + 1, arch->objdump.imm_char) ||
			  arm64__is_reg(skip_spaces(p + 2))))
			op_loc->addr_mode = PERF_ADDR_MODE_POST_INDEX;
		/* Signed offset: [base{, #imm|reg}] */
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

	/* Extract secondary register or immediate offset */
	if (op_loc->multi_regs)
		op_loc->reg2 = arch__dwarf_regnum(arch, s);
	else if (*s == arch->objdump.imm_char)
		op_loc->offset = strtol(s + 1, &p, 0);

	return 0;
}

#ifdef HAVE_LIBDW_SUPPORT
static void update_insn_state_arm64(struct type_state *state,
				    struct data_loc_info *dloc, Dwarf_Die *cu_die,
				    struct disasm_line *dl)
{
	struct annotated_insn_loc loc;
	struct annotated_op_loc *dst = &loc.ops[INSN_OP_TARGET];
	u32 insn_offset = dl->al.offset;

	if (annotate_get_insn_location(dloc->arch, dl, &loc) < 0)
		return;

	/*
	 * Invalidate caller-saved registers on function calls per ARM64 AAPCS64
	 * ABI, unless DWARF location info indicates the register remains valid
	 * beyond the call address.
	 */
	if (ins__is_call(&dl->ins)) {
		struct symbol *func = dl->ops.target.sym;
		const char *call_name;
		u64 call_addr;
		struct type_state_reg *tsr;
		Dwarf_Die type_die;

		call_name = func ? func->name : dl->ops.target.name;
		pr_debug_dtp("call [%x] %s\n", insn_offset, call_name ?: "<unknown>");

		/* Invalidate caller-saved registers after call */
		call_addr = map__rip_2objdump(dloc->ms->map,
					      dloc->ms->sym->start + dl->al.offset);
		for (unsigned int i = 0; i < ARRAY_SIZE(state->regs); i++) {
			struct type_state_reg *reg = &state->regs[i];

			if (!reg->caller_saved)
				continue;
			/* Keep register valid within DWARF location lifetime */
			if (reg->lifetime_active && call_addr < reg->lifetime_end)
				continue;
			invalidate_reg_state(reg);
		}

		/* Update register with the return type (if any) */
		if (call_name && die_find_func_rettype(cu_die, call_name, &type_die)) {
			tsr = &state->regs[state->ret_reg];
			tsr->copied_from = -1;
			tsr->type = type_die;
			tsr->kind = TSR_KIND_TYPE;
			tsr->offset = 0;
			tsr->ok = true;

			pr_debug_dtp("call [%x] return -> reg%d",
				     insn_offset, state->ret_reg);
			pr_debug_type_name(&type_die, tsr->kind);
		}
		return;
	}

	/*
	 * Invalidate destination register(s) for unsupported instructions to
	 * prevent stale type info from propagating to subsequent instructions.
	 */
	if (has_reg_type(state, dst->reg1)) {
		pr_debug_dtp("%s [%x] invalidate reg%d",
			     dl->ins.name, insn_offset, dst->reg1);
		invalidate_reg_state(&state->regs[dst->reg1]);
		if (dst->multi_regs) {
			pr_debug_dtp(" and reg%d", dst->reg2);
			invalidate_reg_state(&state->regs[dst->reg2]);
		}
		pr_debug_dtp("\n");
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
