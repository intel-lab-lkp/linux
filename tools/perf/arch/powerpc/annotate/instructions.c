// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>

/*
 * powerpc instruction nmemonic table to associate load/store instructions with
 * move_ops. mov_ops is used to identify mem_type to associate instruction with
 * data type and offset.
 */
static struct ins powerpc__instructions[] = {
	{ .name = "lbz",	.ops = &mov_ops,  },
	{ .name = "lbzx",	.ops = &mov_ops,  },
	{ .name = "lbzu",	.ops = &mov_ops,  },
	{ .name = "lbzux",	.ops = &mov_ops,  },
	{ .name = "lhz",	.ops = &mov_ops,  },
	{ .name = "lhzx",	.ops = &mov_ops,  },
	{ .name = "lhzu",	.ops = &mov_ops,  },
	{ .name = "lhzux",	.ops = &mov_ops,  },
	{ .name = "lha",	.ops = &mov_ops,  },
	{ .name = "lhax",	.ops = &mov_ops,  },
	{ .name = "lhau",	.ops = &mov_ops,  },
	{ .name = "lhaux",	.ops = &mov_ops,  },
	{ .name = "lwz",	.ops = &mov_ops,  },
	{ .name = "lwzx",	.ops = &mov_ops,  },
	{ .name = "lwzu",	.ops = &mov_ops,  },
	{ .name = "lwzux",	.ops = &mov_ops,  },
	{ .name = "lwa",	.ops = &mov_ops,  },
	{ .name = "lwax",	.ops = &mov_ops,  },
	{ .name = "lwaux",	.ops = &mov_ops,  },
	{ .name = "ld",		.ops = &mov_ops,  },
	{ .name = "ldx",	.ops = &mov_ops,  },
	{ .name = "ldu",	.ops = &mov_ops,  },
	{ .name = "ldux",	.ops = &mov_ops,  },
	{ .name = "stb",	.ops = &mov_ops,  },
	{ .name = "stbx",	.ops = &mov_ops,  },
	{ .name = "stbu",	.ops = &mov_ops,  },
	{ .name = "stbux",	.ops = &mov_ops,  },
	{ .name = "sth",	.ops = &mov_ops,  },
	{ .name = "sthx",	.ops = &mov_ops,  },
	{ .name = "sthu",	.ops = &mov_ops,  },
	{ .name = "sthux",	.ops = &mov_ops,  },
	{ .name = "stw",	.ops = &mov_ops,  },
	{ .name = "stwx",	.ops = &mov_ops,  },
	{ .name = "stwu",	.ops = &mov_ops,  },
	{ .name = "stwux",	.ops = &mov_ops,  },
	{ .name = "std",	.ops = &mov_ops,  },
	{ .name = "stdx",	.ops = &mov_ops,  },
	{ .name = "stdu",	.ops = &mov_ops,  },
	{ .name = "stdux",	.ops = &mov_ops,  },
	{ .name = "lhbrx",	.ops = &mov_ops,  },
	{ .name = "sthbrx",	.ops = &mov_ops,  },
	{ .name = "lwbrx",	.ops = &mov_ops,  },
	{ .name = "stwbrx",	.ops = &mov_ops,  },
	{ .name = "ldbrx",	.ops = &mov_ops,  },
	{ .name = "stdbrx",	.ops = &mov_ops,  },
	{ .name = "lmw",	.ops = &mov_ops,  },
	{ .name = "stmw",	.ops = &mov_ops,  },
	{ .name = "lswi",	.ops = &mov_ops,  },
	{ .name = "lswx",	.ops = &mov_ops,  },
	{ .name = "stswi",	.ops = &mov_ops,  },
	{ .name = "stswx",	.ops = &mov_ops,  },
};

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

static int powerpc__annotate_init(struct arch *arch, char *cpuid __maybe_unused)
{
	if (!arch->initialized) {
		arch->nr_instructions = ARRAY_SIZE(powerpc__instructions);
		arch->instructions = calloc(arch->nr_instructions, sizeof(struct ins));
		if (arch->instructions == NULL)
			return -ENOMEM;

		memcpy(arch->instructions, (struct ins *)powerpc__instructions, sizeof(struct ins) * arch->nr_instructions);
		arch->nr_instructions_allocated = arch->nr_instructions;
		arch->initialized = true;
		arch->associate_instruction_ops = powerpc__associate_instruction_ops;
		arch->objdump.comment_char      = '#';
	}

	return 0;
}
