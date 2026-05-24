/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Local definitions for the RISC-V Quality of Service Controller (RQSC)
 * ACPI table. Will move to ACPICA's include/acpi/actbl2.h once the spec
 * is ratified.
 */
#ifndef _DRIVERS_ACPI_RISCV_RQSC_H
#define _DRIVERS_ACPI_RISCV_RQSC_H

#include <linux/types.h>
#include <acpi/actbl.h>

#define ACPI_SIG_RQSC	"RQSC"	/* RISC-V Quality of Service Controller */

/* RQSC v0.9.2 Table 1: current revision number. */
#define ACPI_RQSC_REVISION	1

/* RQSC v0.9.2 Table 4: Resource Type values for acpi_rqsc_resource.type. */
#define ACPI_RQSC_RESOURCE_TYPE_CACHE	0
#define ACPI_RQSC_RESOURCE_TYPE_MEMORY	1

/* RQSC v0.9.2 Table 4: Resource ID Type values for .id_type. */
#define ACPI_RQSC_RESOURCE_ID_TYPE_PROCESSOR_CACHE	0
#define ACPI_RQSC_RESOURCE_ID_TYPE_MEMORY_RANGE		1

/*
 * Byte-packed: u64 id1 would otherwise pad to 8-byte alignment and inflate
 * sizeof(*res) from the spec's 20 bytes to 24, mis-sizing resource subtables.
 */
struct acpi_rqsc_resource {
	u8 type;
	u8 resv;
	u16 length;
	u16 flags;
	u8 resv2;
	u8 id_type;
	u64 id1;
	u32 id2;
} __packed;

struct acpi_rqsc_node {
	u8 type;
	u8 resv;
	u16 length;
	/* RQSC v0.9.2 section 2 Table 2: 12-byte GAS-format register interface address */
	struct acpi_generic_address reg;
	u16 rcid;
	u16 mcid;
	u16 flags;
	u16 nres;
	/*
	 * Followed by nres acpi_rqsc_resource subtables. Walk them via
	 * each resource's own length field so a future RQSC revision that
	 * extends the resource layout cannot misalign older parsers.
	 */
} __packed;

struct acpi_table_rqsc {
	struct acpi_table_header header;	/* Common ACPI table header */
	u32 num;
} __packed;

#endif /* _DRIVERS_ACPI_RISCV_RQSC_H */
