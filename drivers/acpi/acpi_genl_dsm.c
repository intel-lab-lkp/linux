// SPDX-License-Identifier: GPL-2.0
/*
 * acpi_genl_dsm.c - exports ACPI _DSM functions via netlink
 *
 *  Copyright (C) 2024  Wathsala Vithanage <wathsala.vithanage@arm.com>
 *
 */

#include <linux/acpi.h>
#include <linux/jhash.h>
#include <linux/hashtable.h>

#include "internal.h"

#define acpi_genl_dsm_hash_key(id)				\
	jhash2((u32 *)&(id), sizeof((id))/sizeof(u32), 0)

DEFINE_HASHTABLE(acpi_dsm_tbl, 4);

/**
 * acpi_genl_dsm_add_handle - add netlink handle to invoke _DSM via netlink.
 * @dsm_handle: _DSM handle
 *
 * Add handle to a _DSM method that will be invoked when user space calls it
 * via netlink.
 */
int acpi_genl_dsm_add_handle(struct acpi_genl_dsm_handle *handle)
{
	if (!handle)
		return -EINVAL;
	u32 id_hash = acpi_genl_dsm_hash_key(handle->id);

	hash_add_rcu(acpi_dsm_tbl, &handle->entry, id_hash);
	return 0;
}
EXPORT_SYMBOL(acpi_genl_dsm_add_handle);

/**
 * acpi_genl_dsm_get_handle - find netlink handle to a _DSM method.
 * @id: _DSM identifier
 *
 * Find netlink handle to _DSM method by _DSM identifier.
 */
struct acpi_genl_dsm_handle *
acpi_genl_dsm_get_handle(const struct acpi_genl_dsm_id *id)
{
	struct acpi_genl_dsm_handle *cur_obj;

	if (!id)
		return NULL;
	u32 id_hash = acpi_genl_dsm_hash_key(*id);

	hash_for_each_possible_rcu(acpi_dsm_tbl, cur_obj, entry, id_hash) {
		if (!memcmp(cur_obj, id, sizeof(struct acpi_genl_dsm_id)))
			return cur_obj;
	}
	return NULL;
}
EXPORT_SYMBOL(acpi_genl_dsm_get_handle);

/**
 * acpi_genl_dsm_del_handle - remove netlink handler to a _DSM method.
 * @id: _DSM identifier
 *
 * Remove netlink handler to a _DSM method by _DSM identifier.
 */
int acpi_genl_dsm_del_handle(const struct acpi_genl_dsm_id *id)
{
	if (!id)
		return -EINVAL;
	struct acpi_genl_dsm_handle *handle = acpi_genl_dsm_get_handle(id);

	if (!handle)
		return -ENOENT;
	hash_del_rcu(&handle->entry);
	return 0;
}
EXPORT_SYMBOL(acpi_genl_dsm_del_handle);

