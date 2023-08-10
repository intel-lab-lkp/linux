// SPDX-License-Identifier: GPL-2.0
/*
 * Platform Health Assessment Table (PHAT) support
 *
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 *
 * Author: Avadhut Naik <avadhut.naik@amd.com>
 *
 * This file implements parsing of the Platform Health Assessment Table
 * through which a platform can expose an extensible set of platform
 * health related telemetry. The telemetry is exposed through Firmware
 * Version Data Records and Firmware Health Data Records. Additionally,
 * a platform, through system firmware, also exposes Reset Reason Health
 * Record to inform the operating system of the cause of last system
 * reset or boot.
 *
 * For more information on PHAT, please refer to ACPI specification
 * version 6.5, section 5.2.31
 */

#include <linux/acpi.h>

static int phat_disable __initdata;
static const char *prefix = "ACPI PHAT: ";

/* Reset Reason Health Record GUID */
static const guid_t reset_guid =
	GUID_INIT(0x7a014ce2, 0xf263, 0x4b77,
		  0xb8, 0x8a, 0xe6, 0x33, 0x6b, 0x78, 0x2c, 0x14);

static struct { u8 mask; const char *str; } const reset_sources[] = {
	{BIT(0), "Unknown source"},
	{BIT(1), "Hardware Source"},
	{BIT(2), "Firmware Source"},
	{BIT(3), "Software initiated reset"},
	{BIT(4), "Supervisor initiated reset"},
};

static struct { u8 val; const char *str; } const reset_reasons[] = {
	{0, "UNKNOWN"},
	{1, "COLD BOOT"},
	{2, "COLD RESET"},
	{3, "WARM RESET"},
	{4, "UPDATE"},
	{32, "UNEXPECTED RESET"},
	{33, "FAULT"},
	{34, "TIMEOUT"},
	{35, "THERMAL"},
	{36, "POWER LOSS"},
	{37, "POWER BUTTON"},
};

/*
 * Print the last PHAT Version Element associated with a Firmware
 * Version Data Record.
 * Firmware Version Data Record consists of an array of PHAT Version
 * Elements with each entry in the array representing a modification
 * undertaken on a given platform component.
 * In the event the array has multiple entries, minimize logs on the
 * console and print only the last version element since it denotes
 * the currently running instance of the component.
 */
static int phat_version_data_parse(const char *pfx,
				   struct acpi_phat_version_data *version)
{
	char newpfx[64];
	u32 num_elems = version->element_count - 1;
	struct acpi_phat_version_element *element;
	int offset = sizeof(struct acpi_phat_version_data);

	if (!version->element_count) {
		pr_info("%sNo PHAT Version Elements found.\n", prefix);
		return 0;
	}

	offset += num_elems * sizeof(struct acpi_phat_version_element);
	element = (void *)version + offset;

	pr_info("%sPHAT Version Element:\n", pfx);
	snprintf(newpfx, sizeof(newpfx), "%s ", pfx);
	pr_info("%sComponent ID: %pUl\n", newpfx, element->guid);
	pr_info("%sVersion: 0x%llx\n", newpfx, element->version_value);
	snprintf(newpfx, sizeof(newpfx), KERN_INFO "%s ", pfx);
	print_hex_dump(newpfx, "Producer ID: ", DUMP_PREFIX_NONE, 16, 4,
		       &element->producer_id, sizeof(element->producer_id), true);

	return 0;
}

/*
 * Print the Reset Reason Health Record
 */
static int phat_reset_reason_parse(const char *pfx,
				   struct acpi_phat_health_data *record)
{
	int idx;
	void *data;
	u32 data_len;
	char newpfx[64];
	struct acpi_phat_reset_reason *rr;
	struct acpi_phat_vendor_reset_data *vdata;

	rr = (void *)record + record->device_specific_offset;

	for (idx = 0; idx < ARRAY_SIZE(reset_sources); idx++) {
		if (!rr->reset_source) {
			pr_info("%sUnknown Reset Source.\n", pfx);
			break;
		}
		if (rr->reset_source & reset_sources[idx].mask) {
			pr_info("%sReset Source: 0x%x\t%s\n", pfx, reset_sources[idx].mask,
				reset_sources[idx].str);
			/* According to ACPI v6.5 Table 5.168, Sub-Source is
			 * defined only for Software initiated reset.
			 */
			if (idx == 0x3 && rr->reset_sub_source)
				pr_info("%sReset Sub-Source: %s\n", pfx,
					rr->reset_sub_source == 0x1 ?
					"Operating System" : "Hypervisor");
			break;
		}
	}

	for (idx = 0; idx < ARRAY_SIZE(reset_reasons); idx++) {
		if (rr->reset_reason == reset_reasons[idx].val) {
			pr_info("%sReset Reason: 0x%x\t%s\n", pfx, reset_reasons[idx].val,
				reset_reasons[idx].str);
			break;
		}
	}

	if (!rr->vendor_count)
		return 0;

	pr_info("%sReset Reason Vendor Data:\n", pfx);
	vdata = (void *)rr + sizeof(*rr);

	for (idx = 0; idx < rr->vendor_count; idx++) {
		snprintf(newpfx, sizeof(newpfx), "%s ", pfx);
		data_len = vdata->length - sizeof(*vdata);
		data = (void *)vdata + sizeof(*vdata);
		pr_info("%sVendor Data ID: %pUl\n", newpfx, vdata->vendor_id);
		pr_info("%sRevision: 0x%x\n", newpfx, vdata->revision);
		snprintf(newpfx, sizeof(newpfx), KERN_INFO "%s ", pfx);
		print_hex_dump(newpfx, "Data: ", DUMP_PREFIX_NONE, 16, 4,
			       data, data_len, false);
		vdata = (void *)vdata + vdata->length;
	}

	return 0;
}

/*
 * Print the Firmware Health Data Record.
 */
static int phat_health_data_parse(const char *pfx,
				  struct acpi_phat_health_data *record)
{
	void *data;
	u32 data_len;
	char newpfx[64];

	pr_info("%sHealth Records.\n", pfx);
	snprintf(newpfx, sizeof(newpfx), "%s ", pfx);
	pr_info("%sDevice Signature: %pUl\n", newpfx, record->device_guid);

	switch (record->health) {
	case ACPI_PHAT_ERRORS_FOUND:
		pr_info("%sAmHealthy: Errors found\n", newpfx);
		break;
	case ACPI_PHAT_NO_ERRORS:
		pr_info("%sAmHealthy: No errors found.\n", newpfx);
		break;
	case ACPI_PHAT_UNKNOWN_ERRORS:
		pr_info("%sAmHealthy: Unknown.\n", newpfx);
		break;
	case ACPI_PHAT_ADVISORY:
		pr_info("%sAmHealthy: Advisory – additional device-specific data exposed.\n",
			newpfx);
		break;
	default:
		break;
	}

	if (!record->device_specific_offset)
		return 0;

	/* Reset Reason Health Record has a unique GUID and is created as
	 * a Health Record in the PHAT table. Check if this Health Record
	 * is a Reset Reason Health Record.
	 */
	if (guid_equal((guid_t *)record->device_guid, &reset_guid)) {
		phat_reset_reason_parse(newpfx, record);
		return 0;
	}

	data = (void *)record + record->device_specific_offset;
	data_len = record->header.length - record->device_specific_offset;
	snprintf(newpfx, sizeof(newpfx), KERN_INFO "%s ", pfx);
	print_hex_dump(newpfx, "Device Data: ", DUMP_PREFIX_NONE, 16, 4,
		       data, data_len, false);

	return 0;
}

static int parse_phat_table(const char *pfx, struct acpi_table_phat *phat_tab)
{
	char newpfx[64];
	u32 offset = sizeof(*phat_tab);
	struct acpi_phat_header *phat_header;

	snprintf(newpfx, sizeof(newpfx), "%s ", pfx);

	while (offset < phat_tab->header.length) {
		phat_header = (void *)phat_tab + offset;
		switch (phat_header->type) {
		case ACPI_PHAT_TYPE_FW_VERSION_DATA:
			phat_version_data_parse(newpfx, (struct acpi_phat_version_data *)
			    phat_header);
			break;
		case ACPI_PHAT_TYPE_FW_HEALTH_DATA:
			phat_health_data_parse(newpfx, (struct acpi_phat_health_data *)
			    phat_header);
			break;
		default:
			break;
		}
		offset += phat_header->length;
	}
	return 0;
}

static int __init setup_phat_disable(char *str)
{
	phat_disable = 1;
	return 1;
}
__setup("phat_disable", setup_phat_disable);

static int __init acpi_phat_init(void)
{
	acpi_status status;
	struct acpi_table_phat *phat_tab;

	if (acpi_disabled)
		return 0;

	if (phat_disable) {
		pr_err("%sPHAT support has been disabled.\n", prefix);
		return 0;
	}

	status = acpi_get_table(ACPI_SIG_PHAT, 0,
				(struct acpi_table_header **)&phat_tab);

	if (status == AE_NOT_FOUND) {
		pr_info("%sPHAT Table not found.\n", prefix);
		return 0;
	} else if (ACPI_FAILURE(status)) {
		pr_err("%sFailed to get PHAT Table: %s.\n", prefix,
		       acpi_format_exception(status));
		return -EINVAL;
	}

	pr_info("%sPlatform Telemetry Records.\n", prefix);
	parse_phat_table(prefix, phat_tab);

	return 0;
}
late_initcall(acpi_phat_init);
