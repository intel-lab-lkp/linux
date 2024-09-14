// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: nsparse - namespace interface to AML parser
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "acparser.h"
#include "acdispat.h"
#include "actables.h"
#include "acinterp.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nsparse")

/*******************************************************************************
 *
 * FUNCTION:    ns_execute_table
 *
 * PARAMETERS:  table_desc      - An ACPI table descriptor for table to parse
 *              start_node      - Where to enter the table into the namespace
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load ACPI/AML table by executing the entire table as a single
 *              large control method.
 *
 * NOTE: The point of this is to execute any module-level code in-place
 * as the table is parsed. Some AML code depends on this behavior.
 *
 * It is a run-time option at this time, but will eventually become
 * the default.
 *
 * Note: This causes the table to only have a single-pass parse.
 * However, this is compatible with other ACPI implementations.
 *
 ******************************************************************************/
acpi_status
acpi_ns_execute_table(u32 table_index, struct acpi_namespace_node *start_node)
{
	acpi_status status;
	struct acpi_table_header *table;
	acpi_owner_id owner_id;
	struct acpi_evaluate_info *info = NULL;
	u32 aml_length;
	u8 *aml_start;
	union acpi_operand_object *method_obj = NULL;

	ACPI_FUNCTION_TRACE(ns_execute_table);

	status = acpi_get_table_by_index(table_index, &table);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Table must consist of at least a complete header */

	if (table->length < sizeof(struct acpi_table_header)) {
		return_ACPI_STATUS(AE_BAD_HEADER);
	}

	aml_start = (u8 *)table + sizeof(struct acpi_table_header);
	aml_length = table->length - sizeof(struct acpi_table_header);

	status = acpi_tb_get_owner_id(table_index, &owner_id);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Create, initialize, and link a new temporary method object */

	method_obj = acpi_ut_create_internal_object(ACPI_TYPE_METHOD);
	if (!method_obj) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Allocate the evaluation information block */

	info = ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_evaluate_info));
	if (!info) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_PARSE,
			      "%s: Create table pseudo-method for [%4.4s] @%p, method %p\n",
			      ACPI_GET_FUNCTION_NAME, table->signature, table,
			      method_obj));

	method_obj->method.aml_start = aml_start;
	method_obj->method.aml_length = aml_length;
	method_obj->method.owner_id = owner_id;
	method_obj->method.info_flags |= ACPI_METHOD_MODULE_LEVEL;

	info->pass_number = ACPI_IMODE_EXECUTE;
	info->node = start_node;
	info->obj_desc = method_obj;
	info->node_flags = info->node->flags;
	info->full_pathname = acpi_ns_get_normalized_pathname(info->node, TRUE);
	if (!info->full_pathname) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/* Optional object evaluation log */

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_EVALUATION,
			      "%-26s:  (Definition Block level)\n",
			      "Module-level evaluation"));

	status = acpi_ps_execute_table(info);

	/* Optional object evaluation log */

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_EVALUATION,
			      "%-26s:  (Definition Block level)\n",
			      "Module-level complete"));

cleanup:
	if (info) {
		ACPI_FREE(info->full_pathname);
		info->full_pathname = NULL;
	}
	ACPI_FREE(info);
	acpi_ut_remove_reference(method_obj);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_parse_table
 *
 * PARAMETERS:  table_desc      - An ACPI table descriptor for table to parse
 *              start_node      - Where to enter the table into the namespace
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Parse AML within an ACPI table and return a tree of ops
 *
 ******************************************************************************/

acpi_status
acpi_ns_parse_table(u32 table_index, struct acpi_namespace_node *start_node)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ns_parse_table);

	/*
	 * Executes the AML table as one large control method.
	 * The point of this is to execute any module-level code in-place
	 * as the table is parsed. Some AML code depends on this behavior.
	 *
	 * Note: This causes the table to only have a single-pass parse.
	 * However, this is compatible with other ACPI implementations.
	 */
	ACPI_DEBUG_PRINT_RAW((ACPI_DB_PARSE,
			      "%s: **** Start table execution pass\n",
			      ACPI_GET_FUNCTION_NAME));

	status = acpi_ns_execute_table(table_index, start_node);

	return_ACPI_STATUS(status);
}
