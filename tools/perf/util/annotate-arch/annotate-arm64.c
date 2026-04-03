// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <linux/zalloc.h>
#include <linux/string.h>
#include <regex.h>
#include "../annotate.h"
#include "../disasm.h"

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
