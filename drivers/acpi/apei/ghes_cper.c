// SPDX-License-Identifier: GPL-2.0
/*
 * Shared GHES helpers for firmware-first CPER error handling.
 *
 * This file holds the GHES helper code that is shared by the in-tree GHES
 * driver and by other firmware-first error sources that reuse the same CPER
 * handling flow.
 *
 * Derived from the ACPI APEI GHES driver.
 *
 * Copyright 2010,2011 Intel Corp.
 *   Author: Huang Ying <ying.huang@intel.com>
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>

#include <acpi/apei.h>
#include <acpi/ghes_cper.h>

#include <asm/fixmap.h>
#include <asm/tlbflush.h>

#include "apei-internal.h"

static void __iomem *ghes_map(u64 pfn, enum fixed_addresses fixmap_idx)
{
	phys_addr_t paddr;
	pgprot_t prot;

	paddr = PFN_PHYS(pfn);
	prot = arch_apei_get_mem_attribute(paddr);
	__set_fixmap(fixmap_idx, paddr, prot);

	return (void __iomem *) __fix_to_virt(fixmap_idx);
}

static void ghes_unmap(void __iomem *vaddr, enum fixed_addresses fixmap_idx)
{
	int _idx = virt_to_fix((unsigned long)vaddr);

	WARN_ON_ONCE(fixmap_idx != _idx);
	clear_fixmap(fixmap_idx);
}

static void ghes_ack_error(struct acpi_hest_generic_v2 *gv2)
{
	int rc;
	u64 val = 0;

	rc = apei_read(&val, &gv2->read_ack_register);
	if (rc)
		return;

	val &= gv2->read_ack_preserve << gv2->read_ack_register.bit_offset;
	val |= gv2->read_ack_write    << gv2->read_ack_register.bit_offset;

	apei_write(val, &gv2->read_ack_register);
}

static void ghes_copy_tofrom_phys(void *buffer, u64 paddr, u32 len,
				  int from_phys,
				  enum fixed_addresses fixmap_idx)
{
	void __iomem *vaddr;
	u64 offset;
	u32 trunk;

	while (len > 0) {
		offset = paddr - (paddr & PAGE_MASK);
		vaddr = ghes_map(PHYS_PFN(paddr), fixmap_idx);
		trunk = PAGE_SIZE - offset;
		trunk = min(trunk, len);
		if (from_phys)
			memcpy_fromio(buffer, vaddr + offset, trunk);
		else
			memcpy_toio(vaddr + offset, buffer, trunk);
		len -= trunk;
		paddr += trunk;
		buffer += trunk;
		ghes_unmap(vaddr, fixmap_idx);
	}
}

/* Check the top-level record header has an appropriate size. */
int __ghes_check_estatus(struct ghes *ghes,
			 struct acpi_hest_generic_status *estatus)
{
	u32 len = cper_estatus_len(estatus);
	u32 max_len = min(ghes->generic->error_block_length,
			  ghes->estatus_length);

	if (len < sizeof(*estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Truncated error status block!\n");
		return -EIO;
	}

	if (!len || len > max_len) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Invalid error status block length!\n");
		return -EIO;
	}

	if (cper_estatus_check_header(estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Invalid CPER header!\n");
		return -EIO;
	}

	return 0;
}

/* Read the CPER block, returning its address, and header in estatus. */
int __ghes_peek_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 *buf_paddr, enum fixed_addresses fixmap_idx)
{
	struct acpi_hest_generic *g = ghes->generic;
	int rc;

	rc = apei_read(buf_paddr, &g->error_status_address);
	if (rc) {
		*buf_paddr = 0;
		pr_warn_ratelimited(FW_WARN GHES_PFX
				    "Failed to read error status block address for hardware error source: %d.\n",
				   g->header.source_id);
		return -EIO;
	}
	if (!*buf_paddr)
		return -ENOENT;

	ghes_copy_tofrom_phys(estatus, *buf_paddr, sizeof(*estatus), 1,
			      fixmap_idx);
	if (!estatus->block_status) {
		*buf_paddr = 0;
		return -ENOENT;
	}

	return 0;
}

int __ghes_read_estatus(struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx,
			size_t buf_len)
{
	ghes_copy_tofrom_phys(estatus, buf_paddr, buf_len, 1, fixmap_idx);
	if (cper_estatus_check(estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX
				    "Failed to read error status block!\n");
		return -EIO;
	}

	return 0;
}

int ghes_read_estatus(struct ghes *ghes,
		      struct acpi_hest_generic_status *estatus,
		      u64 *buf_paddr, enum fixed_addresses fixmap_idx)
{
	int rc;

	rc = __ghes_peek_estatus(ghes, estatus, buf_paddr, fixmap_idx);
	if (rc)
		return rc;

	rc = __ghes_check_estatus(ghes, estatus);
	if (rc)
		return rc;

	return __ghes_read_estatus(estatus, *buf_paddr, fixmap_idx,
				   cper_estatus_len(estatus));
}

void ghes_clear_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx)
{
	estatus->block_status = 0;

	if (!buf_paddr)
		return;

	ghes_copy_tofrom_phys(estatus, buf_paddr,
			      sizeof(estatus->block_status), 0,
			      fixmap_idx);

	/*
	 * GHESv2 type HEST entries introduce support for error acknowledgment,
	 * so only acknowledge the error if this support is present.
	 */
	if (is_hest_type_generic_v2(ghes))
		ghes_ack_error(ghes->generic_v2);
}
