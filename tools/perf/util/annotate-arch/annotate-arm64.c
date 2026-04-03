// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/zalloc.h>
#include <regex.h>
#include "../annotate.h"
#include "../disasm.h"

struct arch_arm64 {
	struct arch arch;
	regex_t call_insn;
	regex_t jump_insn;
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

static const struct ins_ops *arm64__associate_instruction_ops(struct arch *arch, const char *name)
{
	struct arch_arm64 *arm = container_of(arch, struct arch_arm64, arch);
	const struct ins_ops *ops;
	regmatch_t match[2];

	if (!regexec(&arm->jump_insn, name, 2, match, 0))
		ops = &jump_ops;
	else if (!regexec(&arm->call_insn, name, 2, match, 0))
		ops = &call_ops;
	else if (!strcmp(name, "ret"))
		ops = &ret_ops;
	else
		ops = &arm64_mov_ops;

	arch__associate_ins_ops(arch, name, ops);
	return ops;
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
	arch->associate_instruction_ops   = arm64__associate_instruction_ops;

	/* bl, blr */
	err = regcomp(&arm->call_insn, "^blr?$", REG_EXTENDED);
	if (err)
		goto out_free_arm;

	/* b, b.cond, br, cbz/cbnz, tbz/tbnz */
	err = regcomp(&arm->jump_insn, "^[ct]?br?\\.?(cc|cs|eq|ge|gt|hi|hs|le|lo|ls|lt|mi|ne|pl|vc|vs)?n?z?$",
		      REG_EXTENDED);
	if (err)
		goto out_free_call;

	return arch;

out_free_call:
	regfree(&arm->call_insn);
out_free_arm:
	free(arm);
	errno = SYMBOL_ANNOTATE_ERRNO__ARCH_INIT_REGEXP;
	return NULL;
}
