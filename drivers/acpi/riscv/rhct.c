// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022-2023, Ventana Micro Systems Inc
 *	Author: Sunil V L <sunilvl@ventanamicro.com>
 *
 */

#define pr_fmt(fmt)     "ACPI: RHCT: " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/overflow.h>
#include <linux/string.h>

static bool rhct_node_valid(struct acpi_table_rhct *rhct,
			    struct acpi_rhct_node_header *node)
{
	struct acpi_rhct_node_header *end;
	size_t remaining;

	end = ACPI_ADD_PTR(struct acpi_rhct_node_header, rhct,
			   rhct->header.length);
	if (node >= end)
		return false;

	remaining = (char *)end - (char *)node;
	if (remaining < sizeof(*node) ||
	    node->length < sizeof(*node) ||
	    node->length > remaining) {
		pr_err(FW_BUG "Invalid RHCT node length\n");
		return false;
	}

	return true;
}

static struct acpi_rhct_node_header *rhct_node_from_offset(struct acpi_table_rhct *rhct,
							   u32 offset)
{
	struct acpi_rhct_node_header *node;

	if (offset > rhct->header.length)
		return NULL;

	node = ACPI_ADD_PTR(struct acpi_rhct_node_header, rhct, offset);
	return rhct_node_valid(rhct, node) ? node : NULL;
}

static bool rhct_node_has_data(struct acpi_rhct_node_header *node,
			       size_t data_size)
{
	if (node->length < sizeof(*node) + data_size) {
		pr_err(FW_BUG "Truncated RHCT node type %u\n", node->type);
		return false;
	}

	return true;
}

static bool rhct_hart_info_valid(struct acpi_rhct_node_header *node)
{
	struct acpi_rhct_hart_info *hart_info;
	size_t offsets_size;

	if (!rhct_node_has_data(node, sizeof(*hart_info)))
		return false;

	hart_info = ACPI_ADD_PTR(struct acpi_rhct_hart_info, node,
				 sizeof(*node));
	if (check_mul_overflow(hart_info->num_offsets, sizeof(u32),
			       &offsets_size) ||
	    offsets_size > node->length - sizeof(*node) -
			   sizeof(*hart_info)) {
		pr_err(FW_BUG "Invalid RHCT hart-info offset array\n");
		return false;
	}

	return true;
}

static bool rhct_isa_string_valid(struct acpi_rhct_node_header *node)
{
	struct acpi_rhct_isa_string *isa_node;
	size_t remaining;

	if (!rhct_node_has_data(node, sizeof(*isa_node)))
		return false;

	isa_node = ACPI_ADD_PTR(struct acpi_rhct_isa_string, node,
				sizeof(*node));
	remaining = node->length - sizeof(*node) - sizeof(*isa_node);
	if (isa_node->isa_length > remaining ||
	    !memchr(isa_node->isa, '\0', isa_node->isa_length)) {
		pr_err(FW_BUG "Invalid RHCT ISA string\n");
		return false;
	}

	return true;
}

static struct acpi_table_rhct *acpi_get_rhct(void)
{
	static struct acpi_table_header *rhct;
	acpi_status status;

	/*
	 * RHCT will be used at runtime on every CPU, so we
	 * don't need to call acpi_put_table() to release the table mapping.
	 */
	if (!rhct) {
		status = acpi_get_table(ACPI_SIG_RHCT, 0, &rhct);
		if (ACPI_FAILURE(status)) {
			pr_warn_once("No RHCT table found\n");
			return NULL;
		}
	}

	return (struct acpi_table_rhct *)rhct;
}

/*
 * During early boot, the caller should call acpi_get_table() and pass its pointer to
 * these functions(and free up later). At run time, since this table can be used
 * multiple times, NULL may be passed in order to use the cached table.
 */
int acpi_get_riscv_isa(struct acpi_table_header *table, unsigned int cpu, const char **isa)
{
	struct acpi_rhct_node_header *node, *ref_node, *end;
	struct acpi_rhct_hart_info *hart_info;
	struct acpi_rhct_isa_string *isa_node;
	struct acpi_table_rhct *rhct;
	u32 *hart_info_node_offset;
	u32 acpi_cpu_id;
	int ret;

	BUG_ON(acpi_disabled);

	ret = acpi_get_cpu_uid(cpu, &acpi_cpu_id);
	if (ret != 0)
		return ret;

	if (!table) {
		rhct = acpi_get_rhct();
		if (!rhct)
			return -ENOENT;
	} else {
		rhct = (struct acpi_table_rhct *)table;
	}

	end = ACPI_ADD_PTR(struct acpi_rhct_node_header, rhct, rhct->header.length);

	for (node = ACPI_ADD_PTR(struct acpi_rhct_node_header, rhct, rhct->node_offset);
	     node < end;
	     node = ACPI_ADD_PTR(struct acpi_rhct_node_header, node, node->length)) {
		if (!rhct_node_valid(rhct, node))
			return -EINVAL;

		if (node->type == ACPI_RHCT_NODE_TYPE_HART_INFO) {
			if (!rhct_hart_info_valid(node))
				return -EINVAL;

			hart_info = ACPI_ADD_PTR(struct acpi_rhct_hart_info, node,
						 sizeof(*node));
			hart_info_node_offset = ACPI_ADD_PTR(u32, hart_info,
							     sizeof(*hart_info));
			if (acpi_cpu_id != hart_info->uid)
				continue;

			for (int i = 0; i < hart_info->num_offsets; i++) {
				ref_node = rhct_node_from_offset(rhct,
								 hart_info_node_offset[i]);
				if (!ref_node)
					return -EINVAL;

				if (ref_node->type == ACPI_RHCT_NODE_TYPE_ISA_STRING) {
					if (!rhct_isa_string_valid(ref_node))
						return -EINVAL;

					isa_node = ACPI_ADD_PTR(struct acpi_rhct_isa_string,
								ref_node, sizeof(*ref_node));
					*isa = isa_node->isa;
					return 0;
				}
			}
		}
	}

	return -1;
}

static void acpi_parse_hart_info_cmo_node(struct acpi_table_rhct *rhct,
					  struct acpi_rhct_node_header *node,
					  u32 *cbom_size, u32 *cboz_size, u32 *cbop_size)
{
	struct acpi_rhct_node_header *ref_node;
	struct acpi_rhct_hart_info *hart_info;
	struct acpi_rhct_cmo_node *cmo_node;
	u32 *hart_info_node_offset;

	if (!rhct_hart_info_valid(node))
		return;

	hart_info = ACPI_ADD_PTR(struct acpi_rhct_hart_info, node,
				 sizeof(*node));
	hart_info_node_offset = ACPI_ADD_PTR(u32, hart_info,
					     sizeof(*hart_info));
	for (int i = 0; i < hart_info->num_offsets; i++) {
		ref_node = rhct_node_from_offset(rhct, hart_info_node_offset[i]);
		if (!ref_node)
			return;

		if (ref_node->type == ACPI_RHCT_NODE_TYPE_CMO) {
			if (!rhct_node_has_data(ref_node, sizeof(*cmo_node)))
				return;

			cmo_node = ACPI_ADD_PTR(struct acpi_rhct_cmo_node,
						ref_node, sizeof(*ref_node));
			if (cbom_size && cmo_node->cbom_size <= 30) {
				if (!*cbom_size)
					*cbom_size = BIT(cmo_node->cbom_size);
				else if (*cbom_size != BIT(cmo_node->cbom_size))
					pr_warn("CBOM size is not the same across harts\n");
			}

			if (cboz_size && cmo_node->cboz_size <= 30) {
				if (!*cboz_size)
					*cboz_size = BIT(cmo_node->cboz_size);
				else if (*cboz_size != BIT(cmo_node->cboz_size))
					pr_warn("CBOZ size is not the same across harts\n");
			}

			if (cbop_size && cmo_node->cbop_size <= 30) {
				if (!*cbop_size)
					*cbop_size = BIT(cmo_node->cbop_size);
				else if (*cbop_size != BIT(cmo_node->cbop_size))
					pr_warn("CBOP size is not the same across harts\n");
			}
		}
	}
}

/*
 * During early boot, the caller should call acpi_get_table() and pass its pointer to
 * these functions (and free up later). At run time, since this table can be used
 * multiple times, pass NULL so that the table remains in memory.
 */
void acpi_get_cbo_block_size(struct acpi_table_header *table, u32 *cbom_size,
			     u32 *cboz_size, u32 *cbop_size)
{
	struct acpi_rhct_node_header *node, *end;
	struct acpi_table_rhct *rhct;

	if (acpi_disabled)
		return;

	if (table) {
		rhct = (struct acpi_table_rhct *)table;
	} else {
		rhct = acpi_get_rhct();
		if (!rhct)
			return;
	}

	if (cbom_size)
		*cbom_size = 0;

	if (cboz_size)
		*cboz_size = 0;

	if (cbop_size)
		*cbop_size = 0;

	end = ACPI_ADD_PTR(struct acpi_rhct_node_header, rhct, rhct->header.length);
	for (node = ACPI_ADD_PTR(struct acpi_rhct_node_header, rhct, rhct->node_offset);
	     node < end;
	     node = ACPI_ADD_PTR(struct acpi_rhct_node_header, node, node->length)) {
		if (!rhct_node_valid(rhct, node))
			return;

		if (node->type == ACPI_RHCT_NODE_TYPE_HART_INFO) {
			acpi_parse_hart_info_cmo_node(rhct, node, cbom_size,
						      cboz_size, cbop_size);
		}
	}
}
