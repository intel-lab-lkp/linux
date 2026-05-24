// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023-2024, Ventana Micro Systems Inc
 *	Author: Sunil V L <sunilvl@ventanamicro.com>
 */

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include "init.h"
#include "rqsc.h"

void __init acpi_arch_init(void)
{
	riscv_acpi_init_gsi_mapping();

	if (IS_ENABLED(CONFIG_ACPI_RIMT))
		riscv_acpi_rimt_init();

	if (IS_ENABLED(CONFIG_RISCV_CBQRI_DRIVER)) {
		struct acpi_table_header *rqsc __free(acpi_put_table) = NULL;
		acpi_status status = acpi_get_table(ACPI_SIG_RQSC, 0, &rqsc);

		if (status == AE_NOT_FOUND) {
			/* RQSC is optional. Silence on systems without it. */
		} else if (ACPI_FAILURE(status)) {
			pr_err("RQSC: failed to get table: %s\n",
			       acpi_format_exception(status));
		} else {
			int rc = acpi_parse_rqsc(rqsc);

			if (rc < 0)
				pr_err("RQSC: failed to parse table: %d\n",
				       rc);
		}
	}
}
