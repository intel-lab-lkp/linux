// SPDX-License-Identifier: GPL-2.0
/*
 * The x86 operand parser: what is a memory reference and where it points.
 *
 * The absolute addressing forms are the ones perf annotate's data type
 * resolver has to follow, and the ones it must not mistake for a data
 * access: a direct branch target, an immediate and a 64-bit movabs that
 * does not survive the int offset.
 */
#include <elf.h>
#include <linux/compiler.h>
#include <stdlib.h>
#include <string.h>
#include "annotate.h"
#include "debug.h"
#include "disasm.h"
#include "tests.h"

struct op_case {
	const char		*insn;
	const struct ins_ops	*ops;
	const char		*operand;
	enum annotated_insn_ops	which;
	bool			mem_ref;
	int			offset;
};

static struct op_case op_cases[] = {
	/* 32-bit absolute, and the 64-bit sign extended disp32 both ways */
	{ "mov",    &mov_ops,  "0x8c73448",		INSN_OP_SOURCE, true,  0x8c73448 },
	{ "mov",    &mov_ops,  "0xffffffffff5fc300",	INSN_OP_SOURCE, true,  -0xa03d00 },
	{ "mov",    &mov_ops,  "-0xa03d00",		INSN_OP_SOURCE, true,  -0xa03d00 },
	/* An immediate and a direct branch target, with the same 0x */
	{ "mov",    &mov_ops,  "$0x10",			INSN_OP_SOURCE, false, 0 },
	{ "call",   &call_ops, "0x401020",		INSN_OP_TARGET, false, 0 },
	{ "jmp",    &jump_ops, "0x401020",		INSN_OP_TARGET, false, 0 },
	{ "loop",   NULL,      "0x401020",		INSN_OP_TARGET, false, 0 },
	/* A 64-bit absolute: kept only when the int offset is exact */
	{ "movabs", NULL,      "0x80001000",		INSN_OP_SOURCE, false, 0 },
	{ "movabs", NULL,      "0x1000",		INSN_OP_SOURCE, true,  0x1000 },
};

static int check_operand(const struct arch *arch, const struct op_case *c)
{
	struct disasm_line dl = {
		.ins = { .name = c->insn, .ops = c->ops },
	};
	struct annotated_insn_loc loc;
	struct annotated_op_loc *op;
	int ret = 0;

	if (c->which == INSN_OP_TARGET)
		dl.ops.target.raw = (char *)c->operand;
	else
		dl.ops.source.raw = (char *)c->operand;

	if (annotate_get_insn_location(arch, &dl, &loc) < 0)
		return TEST_FAIL;

	op = &loc.ops[c->which];
	if (op->mem_ref != c->mem_ref) {
		pr_debug("'%s %s': mem_ref %d, expected %d\n", c->insn,
			 c->operand, op->mem_ref, c->mem_ref);
		ret = TEST_FAIL;
	}
	if (op->mem_ref && op->offset != c->offset) {
		pr_debug("'%s %s': offset %#x, expected %#x\n", c->insn,
			 c->operand, op->offset, c->offset);
		ret = TEST_FAIL;
	}

	return ret;
}

static int test__annotate_ops(struct test_suite *test __maybe_unused,
			      int subtest __maybe_unused)
{
	struct e_machine_and_e_flags id = { .e_machine = EM_X86_64, };
	const struct arch *arch = arch__new_x86(&id, NULL);
	size_t i;
	int ret = 0;

	if (arch == NULL)
		return TEST_FAIL;

	for (i = 0; i < ARRAY_SIZE(op_cases); i++)
		ret |= check_operand(arch, &op_cases[i]);

	free((void *)arch);
	return ret;
}

DEFINE_SUITE("Annotate operand parsing", annotate_ops);
