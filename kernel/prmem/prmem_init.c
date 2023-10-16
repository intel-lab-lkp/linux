// SPDX-License-Identifier: GPL-2.0
/*
 * Persistent-Across-Kexec memory (prmem) - Initialization.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

bool			prmem_inited;

void __init prmem_init(void)
{
	if (!prmem)
		return;

	if (!prmem->metadata) {
		/* Cold boot. */
		prmem->metadata = prmem_metadata;
		prmem->size = prmem_size;
		INIT_LIST_HEAD(&prmem->regions);

		if (!prmem_add_region(prmem_pa, prmem_size))
			return;
	}
	prmem_inited = true;
}

void prmem_fini(void)
{
	if (!prmem_inited)
		return;

	/* Compute checksum over the metadata. */
	prmem->checksum = prmem_checksum(prmem, sizeof(*prmem));
}
