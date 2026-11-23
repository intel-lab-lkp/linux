// SPDX-License-Identifier: GPL-2.0
/*
 * The x86 operand parser: what is a memory reference and where it points.
 *
 * The data type resolver follows absolute memory operands; an immediate
 * is not a memory reference.
 */
#include <elf.h>
#include <linux/compiler.h>
#include <stdlib.h>
#include <string.h>
#include "annotate.h"
#include "debug.h"
#include "dso.h"
#include "disasm.h"
#include "tests.h"

struct op_case {
	const char		*insn;
	const struct ins_ops	*ops;
	const char		*operand;
	bool			mem_ref;
	int			offset;
};

static struct op_case op_cases[] = {
	/* 32-bit absolute, and the 64-bit sign extended disp32 both ways */
	{ "mov",    &mov_ops,  "0x8c73448",		true,  0x8c73448 },
	{ "mov",    &mov_ops,  "0xffffffffff5fc300",	true,  -0xa03d00 },
	{ "mov",    &mov_ops,  "-0xa03d00",		true,  -0xa03d00 },
	/* An immediate is not a memory reference */
	{ "mov",    &mov_ops,  "$0x10",			false, 0 },
	/* Zero and values wider than a disp32 are not absolute addresses. */
	{ "mov",    &mov_ops,  "0x0",			false, 0 },
	{ "mov",    &mov_ops,  "0x100000000",		false, 0 },
};

static int check_operand(const struct arch *arch, const struct op_case *c,
			 struct dso *dso64, struct dso *dso32, struct dso *dsorel)
{
	struct disasm_line dl = {
		.ins = { .name = c->insn, .ops = c->ops },
	};
	struct annotated_insn_loc loc;
	struct annotated_op_loc *op;
	int ret = 0;

	dl.ops.source.raw = (char *)c->operand;

	if (annotate_get_insn_location(arch, &dl, &loc) < 0)
		return TEST_FAIL;

	op = &loc.ops[INSN_OP_SOURCE];
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
	if (c->mem_ref &&
	    (!annotated_op_loc__is_abs(op, dso64) ||
	     !annotated_op_loc__is_abs(op, dso32) ||
	     annotated_op_loc__abs_addr(op, dso64) != (u64)(s64)c->offset ||
	     annotated_op_loc__abs_addr(op, dso32) != (u32)c->offset)) {
		pr_debug("'%s %s': wrong absolute address\n", c->insn, c->operand);
		ret = TEST_FAIL;
	}
	/* The same operand in a relocatable object is an addend, not an address */
	if (c->mem_ref && annotated_op_loc__is_abs(op, dsorel)) {
		pr_debug("'%s %s': absolute address in a relocatable object\n",
			 c->insn, c->operand);
		ret = TEST_FAIL;
	}

	return ret;
}

struct movabs_case {
	const char	*name;
	const char	*operand;
	s64		imm;
};

/* The KASAN shadow offset movabs, as both disassemblers print it */
static const struct movabs_case movabs_cases[] = {
	{ "movabsq", "$-0x2000040000000000, %r15 # imm = 0xDFFFFC0000000000",
	  -0x2000040000000000LL },
	{ "movabs",  "$0xdffffc0000000000,%r15", -0x2000040000000000LL },
};

static int check_movabs(const struct arch *arch, const struct movabs_case *c)
{
	struct disasm_line dl = { .ins = { .name = c->name } };
	struct annotated_insn_loc loc;
	int ret = TEST_FAIL;

	dl.ops.raw = strdup(c->operand);
	if (dl.ops.raw == NULL)
		return TEST_FAIL;

	dl.ins.ops = ins__find(arch, c->name, &dl);
	if (dl.ins.ops != &mov_ops || dl.ins.ops->parse(arch, &dl.ops, NULL, &dl) < 0) {
		pr_debug("'%s %s': not parsed as a mov\n", c->name, c->operand);
		goto out;
	}

	/*
	 * get_dwarf_regnum() is a stub returning -1 when perf is built
	 * without libdw, the target register cannot be resolved then.
	 */
#ifdef HAVE_LIBDW_SUPPORT
	if (annotate_get_insn_location(arch, &dl, &loc) == 0 &&
	    !loc.ops[INSN_OP_SOURCE].mem_ref && loc.ops[INSN_OP_SOURCE].imm &&
	    loc.ops[INSN_OP_SOURCE].imm_value == c->imm &&
	    loc.ops[INSN_OP_TARGET].reg1 != -1)
#else
	if (annotate_get_insn_location(arch, &dl, &loc) == 0 &&
	    !loc.ops[INSN_OP_SOURCE].mem_ref && loc.ops[INSN_OP_SOURCE].imm &&
	    loc.ops[INSN_OP_SOURCE].imm_value == c->imm)
#endif
		ret = TEST_OK;
	else
		pr_debug("'%s %s': not an immediate to a register\n",
			 c->name, c->operand);
out:
	free(dl.ops.source.raw);
	free(dl.ops.target.raw);
	free(dl.ops.raw);
	return ret;
}

static int test__annotate_ops(struct test_suite *test __maybe_unused,
			      int subtest __maybe_unused)
{
	struct e_machine_and_e_flags id = { .e_machine = EM_X86_64, };
	const struct arch *arch = arch__new_x86(&id, NULL);
	struct dso *dso64 = dso__new("annotate-ops-64");
	struct dso *dso32 = dso__new("annotate-ops-32");
	struct dso *dsorel = dso__new("annotate-ops-rel");
	size_t i;
	int ret = 0;

	if (arch == NULL || dso64 == NULL || dso32 == NULL || dsorel == NULL) {
		dso__put(dso64);
		dso__put(dso32);
		dso__put(dsorel);
		free((void *)arch);
		return TEST_FAIL;
	}

	dso__set_is_64_bit(dso64, true);
	dso__set_is_64_bit(dso32, false);
	dso__set_rel(dsorel, ET_REL);

	for (i = 0; i < ARRAY_SIZE(op_cases); i++)
		ret |= check_operand(arch, &op_cases[i], dso64, dso32, dsorel);

	for (i = 0; i < ARRAY_SIZE(movabs_cases); i++)
		ret |= check_movabs(arch, &movabs_cases[i]);

	dso__put(dso64);
	dso__put(dso32);
	dso__put(dsorel);
	free((void *)arch);
	return ret;
}

DEFINE_SUITE("Annotate operand parsing", annotate_ops);
