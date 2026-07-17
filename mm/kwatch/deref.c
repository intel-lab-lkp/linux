// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/kallsyms.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "kwatch.h"

int kwatch_deref_resolve(const struct kwatch_config *cfg, struct pt_regs *regs,
			 unsigned long *out_addr, u16 *out_len)
{
	unsigned long addr = 0;
	int i;

	/* 1. Resolve the Base Anchor */
	if (cfg->base == KWATCH_BASE_STACK) {
		addr = kernel_stack_pointer(regs);
		if (unlikely(!addr))
			return -EINVAL;
	} else if (cfg->base >= KWATCH_BASE_ARG1 &&
		   cfg->base <= KWATCH_BASE_ARG6) {
		int arg_idx = cfg->base - KWATCH_BASE_ARG1;

		addr = regs_get_kernel_argument(regs, arg_idx);
	} else if (cfg->base == KWATCH_BASE_ABS_ADDR ||
		   cfg->base == KWATCH_BASE_GLOBAL_SYM) {
		/* Zero-latency load of the static symbol location */
		addr = cfg->sym_addr;
	} else {
		return -EINVAL;
	}

	/* 2. The Pointer-Chasing FSM */
	for (i = 0; i < cfg->offset_count; i++) {
		addr += cfg->offsets[i];

		if (i < cfg->offset_count - 1) {
			unsigned long next_addr;

			/* Dynamically read the pointer contents at runtime */
			if (get_kernel_nofault(next_addr, (unsigned long *)addr))
				return -EFAULT;

			addr = next_addr;
		}
	}

	/* Enforce strict Kernel-Space boundary */
	if (unlikely(addr < TASK_SIZE_MAX))
		return -EINVAL;

	*out_addr = addr;
	*out_len = cfg->watch_len;
	return 0;
}

int kwatch_deref_parse(struct kwatch_config *cfg, const char *watch_expr)
{
	char *p, *sep, *dup_expr;
	char type = '\0';
	bool is_deref = false;
	int ret = 0;

	dup_expr = kstrdup(watch_expr, GFP_KERNEL);
	if (!dup_expr)
		return -ENOMEM;

	cfg->offset_count = 1;
	cfg->offsets[0] = 0;

	/* 1. Isolate and Resolve Base Anchor */
	p = dup_expr;
	sep = NULL;
	while (*p) {
		if (*p == '+') {
			sep = p;
			type = '+';
			break;
		}
		if (*p == '-') {
			sep = p;
			type = '-';
			if (p[1] == '>')
				is_deref = true;
			break;
		}
		p++;
	}

	if (type)
		*sep = '\0';

	if (!strcmp(dup_expr, "stack")) {
		cfg->base = KWATCH_BASE_STACK;
	} else if (!strncmp(dup_expr, "arg", 3) && strlen(dup_expr) == 4) {
		int arg_num;

		if (kstrtoint(dup_expr + 3, 10, &arg_num) || arg_num < 1 ||
		    arg_num > 6) {
			ret = -EINVAL;
			goto out;
		}
		cfg->base = KWATCH_BASE_ARG1 + (arg_num - 1);
	} else if (kstrtoul(dup_expr, 0, &cfg->sym_addr) == 0) {
		cfg->base = KWATCH_BASE_ABS_ADDR;
	} else {
#if IS_BUILTIN(CONFIG_KWATCH)
		cfg->sym_addr = kallsyms_lookup_name(dup_expr);
		if (!cfg->sym_addr) {
			pr_err("Failed to resolve symbol name: %s\n", dup_expr);
			ret = -EINVAL;
			goto out;
		}
		cfg->base = KWATCH_BASE_GLOBAL_SYM;
#else
		pr_err("cannot resolve symbol %s when built as a module, use a hex address\n",
		       dup_expr);
		ret = -EINVAL;
		goto out;
#endif
	}

	if (!type)
		goto out;

	/* 2. Resolve Base Offset (if + or - exists) */
	if (!is_deref) {
		char *next;

		*sep = type; /* Restore the '+' or '-' for kstrtol */
		next = strstr(sep, "->");
		if (next)
			*next = '\0';

		if (kstrtol(sep, 0, &cfg->offsets[0])) {
			ret = -EINVAL;
			goto out;
		}

		p = next ? next + 2 : NULL;
	} else {
		/* Jump directly to the first dereference after '->' */
		p = sep + 2;
	}

	/* 3. Resolve Dereference Chain */
	while (p) {
		char *next;

		if (cfg->offset_count >= MAX_DEREF_CHAIN) {
			ret = -E2BIG;
			goto out;
		}

		next = strstr(p, "->");
		if (next)
			*next = '\0';

		if (kstrtol(p, 0, &cfg->offsets[cfg->offset_count++])) {
			ret = -EINVAL;
			goto out;
		}

		p = next ? next + 2 : NULL;
	}

out:
	kfree(dup_expr);
	return ret;
}
