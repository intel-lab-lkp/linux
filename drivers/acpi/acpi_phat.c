// SPDX-License-Identifier: GPL-2.0-only
/*
 * Helpers for parsing Platform Health Assessment Table (PHAT) records.
 */
#include <linux/kernel.h>
#include <linux/acpi.h>

#include <acpi/actbl2.h>

/*
 * __phat_get_firmware_health_data() - Internal helper to locate the
 * "Reset Reason Health Record" within the Platform Health Assessment
 * Table (PHAT).
 *
 * @table: Pointer to the beginning of PHAT.
 *
 * Return: A pointer within the @table if "Reset Reason Health Record"
 * is found; NULL otherwise.
 */
static struct acpi_phat_health_data *
__phat_get_firmware_health_data(struct acpi_table_phat *table)
{
	unsigned int length = table->header.length;
	void *header = table;

	/* No records. */
	if (length <= sizeof(struct acpi_table_header))
		return NULL;

	/*
	 * Advance to the end of the table header
	 * where the first record starts.
	 */
	header += sizeof(struct acpi_table_header);
	length -= sizeof(struct acpi_table_header);

	/*
	 * Search for PHAT firmware health data record header
	 * with type == ACPI_PHAT_TYPE_FW_HEALTH_DATA.
	 */
	while (length) {
		struct acpi_phat_health_data *data = header;

		if (data->header.type == ACPI_PHAT_TYPE_FW_HEALTH_DATA)
			return header;

		/* Move to the next header */
		header += data->header.length;
		length -= data->header.length;
	}

	return NULL;
}

/**
 * acpi_phat_get_vendor_reset_reason - Find a "Vendor Specific Reset Reason
 * Entry" with the matching @guid from the "Firmware Health Data Record". If
 * successfully located, the function will allocate an object of the size
 * "acpi_phat_vendor_element.length" and return a pointer populated with the
 * content of the record.
 *
 * @guid: The "Vendor Data ID" of the reset reason record.
 *
 * Return: A valid pointer to an allocated "acpi_phat_vendor_element" populated
 * with the data from the record with matching @guid; an ERR_PTR() otherwise if
 * no matching records were found, or if the element could not be allocated.
 * If a valid pointer was returned, the user must call
 * acpi_phat_put_vendor_reset_reason() for the object once done to reclaim the
 * allocated memory.
 */
struct acpi_phat_vendor_element *acpi_phat_get_vendor_reset_reason(guid_t *guid)
{
	struct acpi_table_header *phat_tbl __free(acpi_put_table) = NULL;
	struct acpi_phat_health_data *fw_health_data;
	struct acpi_phat_device_data *dev_data;
	acpi_status status;
	void *data;
	int i;

	status = acpi_get_table(ACPI_SIG_PHAT, 0, &phat_tbl);
	if (ACPI_FAILURE(status))
		return ERR_PTR(-ENODEV);

	fw_health_data =
		__phat_get_firmware_health_data((struct acpi_table_phat *)phat_tbl);
	if (!fw_health_data)
		return ERR_PTR(-ENODEV);

	/* Check if Device-specific data record is present. */
	if (!fw_health_data->device_specific_offset)
		return ERR_PTR(-ENODEV);

	dev_data = (void *)fw_health_data + fw_health_data->device_specific_offset;
	if (!dev_data->vendor_count)
		return ERR_PTR(-ENODEV);

	/* Vendor data starts after Device-specific data */
	data = (void *)dev_data + sizeof(*dev_data);

	for (i = 0; i <= dev_data->vendor_count; ++i) {
		struct acpi_phat_vendor_element *vendor_data = data;
		int length = vendor_data->length;

		/*
		 * Move to the next Vendor specific entry if
		 * the GUID of entry doesn't match.
		 */
		if (!guid_equal(guid, (guid_t *)vendor_data->vendor_guid)) {
			data += vendor_data->length;
			continue;
		}

		vendor_data = kmalloc(length, GFP_KERNEL);
		if (!vendor_data)
			return ERR_PTR(-ENOMEM);

		memcpy(vendor_data, data, length);
		return vendor_data;
	}

	return ERR_PTR(-ENODEV);
}

/**
 * acpi_phat_put_vendor_reset_reason - Reclaim the object allocated by
 * acpi_phat_get_vendor_reset_reason().
 *
 * @reason: A valid pointer returned by acpi_phat_get_vendor_reset_reason()
 */
void acpi_phat_put_vendor_reset_reason(struct acpi_phat_vendor_element *reason)
{
	kfree(reason);
}
