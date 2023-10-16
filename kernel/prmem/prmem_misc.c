// SPDX-License-Identifier: GPL-2.0-only
/*
 * Persistent-Across-Kexec memory (prmem) - Miscellaneous functions.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

#define MAX_META_LENGTH	31

/*
 * On a kexec, modify the kernel command line to include the boot parameter
 * "prmem_meta=" so that the metadata can be found on the next boot. If the
 * parameter is already present in cmdline, overwrite it. Else, add it.
 */
void prmem_cmdline(char *cmdline)
{
	char		meta[MAX_META_LENGTH], *str;
	unsigned long	metadata;

	metadata = prmem_inited ? prmem->metadata : 0;
	snprintf(meta, MAX_META_LENGTH, " prmem_meta=0x%.16lx", metadata);

	str = strstr(cmdline, " prmem_meta");
	if (str) {
		/*
		 * Boot parameter already exists. Overwrite it. We deliberately
		 * use strncpy() and rely on the fact that it will not NULL
		 * terminate the copy.
		 */
		strncpy(str, meta, MAX_META_LENGTH - 1);
		return;
	}
	if (prmem_inited) {
		/* Boot parameter does not exist. Add it. */
		strcat(cmdline, meta);
	}
}

/*
 * Make sure that the kexec command line can accommodate the prmem_meta
 * command line parameter.
 */
int prmem_cmdline_size(void)
{
	return MAX_META_LENGTH;
}

unsigned long prmem_checksum(void *start, size_t size)
{
	unsigned long	checksum = 0;
	unsigned long	*ptr;
	void		*end;

	end = start + size;
	for (ptr = start; (void *) ptr < end; ptr++)
		checksum += *ptr;
	return checksum;
}

/*
 * Check if the metadata is sane. It would not be sane on a cold boot or if the
 * metadata has been corrupted. In the latter case, we treat it as a cold boot.
 */
bool __init prmem_validate(void)
{
	unsigned long		checksum;

	/* Sanity check the boot parameter. */
	if (prmem_metadata != prmem->metadata || prmem_size != prmem->size) {
		pr_warn("%s: Boot parameter mismatch\n", __func__);
		return false;
	}

	/* Compute and check the checksum of the metadata. */
	checksum = prmem->checksum;
	prmem->checksum = 0;

	if (checksum != prmem_checksum(prmem, sizeof(*prmem))) {
		pr_warn("%s: Checksum mismatch\n", __func__);
		return false;
	}
	return true;
}
