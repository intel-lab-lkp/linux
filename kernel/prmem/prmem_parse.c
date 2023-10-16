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

/*
 * Syntax: prmem_meta=metadata_address
 *
 *	Specifies the address of a single page where the prmem metadata resides.
 *
 * On a kexec, the following will be appended to the kernel command line -
 * "prmem_meta=metadata_address". This is so that the metadata can be located
 * easily on kexec reboots.
 */
static int __init prmem_meta_parse(char *cmdline)
{
	char			*tmp, *cur = cmdline;
	unsigned long		addr;

	if (!cur)
		return -EINVAL;

	/* Get metadata address. */
	addr = memparse(cur, &tmp);
	if (cur == tmp || addr & (PAGE_SIZE - 1)) {
		pr_warn("%s: Incorrect address %lx\n", __func__, addr);
		return -EINVAL;
	}

	prmem_metadata = addr;
	return 0;
}
early_param("prmem_meta", prmem_meta_parse);
