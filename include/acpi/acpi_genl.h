/* SPDX-License-Identifier: GPL-2.0 */
/*
 * acpi_genl_dsm.c - exports ACPI _DSM functions via netlink
 *
 *  Copyright (C) 2024  Wathsala Vithanage <wathsala.vithanage@arm.com>
 *
 */

#ifndef __ACPI_GENL_H__
#define __ACPI_GENL_H__

/* _DSM method identifier */
struct acpi_genl_dsm_id {
	guid_t	guid;	/* ACPI _DSM GUID */
	u64	rev;	/* _DSM method revision ID */
	u64	func;	/* _DSM method index */
} __packed;

/* _DSM method handle */
struct acpi_genl_dsm_handle {
	struct acpi_genl_dsm_id id;	/* Unique _DSM method identifier */
	u16 arg_len;	/* Size of arg passed into dsm_fn */
	u16 ret_len;	/* Size of ret passed into dsm_fn */
	/*
	 * Callback to invoke the _DSM method.
	 */
	int (*dsm_cb)(struct acpi_genl_dsm_id *arg,
		      struct acpi_genl_dsm_id *ret);
	int cap;	/* Min cap for invoking _DSM from the user space */
	struct hlist_node entry;
};

#ifdef CONFIG_ACPI

int acpi_genl_dsm_add_handle(struct acpi_genl_dsm_handle *handle);

struct acpi_genl_dsm_handle *
acpi_genl_dsm_get_handle(const struct acpi_genl_dsm_id *id);

int acpi_genl_dsm_del_handle(const struct acpi_genl_dsm_id *id);

#else

int acpi_genl_dsm_add_handle(const struct acpi_genl_dsm_handle *handle)
{
	return -ENOTSUP;
}

struct acpi_genl_dsm_handle *
acpi_genl_dsm_get_handle(const struct acpi_genl_dsm_id *id)
{
	return NULL;
}

int acpi_genl_dsm_del_handle(struct acpi_genl_dsm_id *id)
{
	return -ENOTSUP;
}

#endif //CONFIG_ACPI
#endif //__ACPI_GENL_H__
