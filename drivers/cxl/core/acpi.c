// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */
#include <linux/acpi.h>
#include "cxl.h"
#include "core.h"

int cxl_acpi_get_extended_linear_cache_size(struct resource *backing_res,
					    int nid, resource_size_t *size)
{
	return hmat_get_extended_linear_cache_size(backing_res, nid, size);
}

int cxl_acpi_extended_linear_cache_address_xlat(u64 *address, u64 alias, int nid)
{
	return hmat_extended_linear_cache_address_xlat(address, alias, nid);
}

int cxl_acpi_extended_linear_cache_alias_xlat(u64 address, u64 *alias, int nid)
{
	return hmat_extended_linear_cache_alias_xlat(address, alias, nid);
}
