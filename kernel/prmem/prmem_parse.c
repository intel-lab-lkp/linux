// SPDX-License-Identifier: GPL-2.0-only
/*
 * Persistent-Across-Kexec memory (prmem) - Process prmem cmdline parameter.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

/*
 * Syntax: prmem=size[KMG]
 *
 *	Specifies the size of the initial memory to be allocated to prmem.
 */
static int __init prmem_size_parse(char *cmdline)
{
	char			*tmp, *cur = cmdline;
	unsigned long		size;

	if (!cur)
		return -EINVAL;

	/* Get initial size. */
	size = memparse(cur, &tmp);
	if (cur == tmp || !size || size & (PAGE_SIZE - 1)) {
		pr_warn("%s: Incorrect size %lx\n", __func__, size);
		return -EINVAL;
	}

	prmem_size = size;
	return 0;
}
early_param("prmem", prmem_size_parse);
