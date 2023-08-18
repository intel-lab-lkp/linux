// SPDX-License-Identifier: GPL-2.0
/*
 * Wifi Band Exclusion Interface (AMD ACPI Implementation)
 * Copyright (C) 2023 Advanced Micro Devices
 *
 */

#include <linux/acpi.h>
#include <linux/acpi_amd_wbrf.h>

#define ACPI_AMD_WBRF_METHOD	"\\WBRF"

/*
 * Functions bit vector for WBRF method
 *
 * Bit 0: Supported for any functions other than function 0.
 * Bit 1: Function 1 (Add / Remove frequency) is supported.
 * Bit 2: Function 2 (Get frequency list) is supported.
 */
#define WBRF_ENABLED				0x0
#define WBRF_RECORD				0x1
#define WBRF_RETRIEVE				0x2

/* record actions */
#define WBRF_RECORD_ADD		0x0
#define WBRF_RECORD_REMOVE	0x1

#define WBRF_REVISION		0x1

/*
 * The data structure used for WBRF_RETRIEVE is not natually aligned.
 * And unfortunately the design has been settled down.
 */
struct amd_wbrf_ranges_out {
	u32			num_of_ranges;
	struct exclusion_range	band_list[MAX_NUM_OF_WBRF_RANGES];
} __packed;

static const guid_t wifi_acpi_dsm_guid =
	GUID_INIT(0x7b7656cf, 0xdc3d, 0x4c1c,
		  0x83, 0xe9, 0x66, 0xe7, 0x21, 0xde, 0x30, 0x70);

static int wbrf_dsm(struct acpi_device *adev, u8 fn,
		    union acpi_object *argv4,
		    union acpi_object **out)
{
	union acpi_object *obj;
	int rc;

	obj = acpi_evaluate_dsm(adev->handle, &wifi_acpi_dsm_guid,
				WBRF_REVISION, fn, argv4);
	if (!obj)
		return -ENXIO;

	switch (obj->type) {
	case ACPI_TYPE_BUFFER:
		*out = obj;
		return 0;

	case ACPI_TYPE_INTEGER:
		rc =  obj->integer.value ? -EINVAL : 0;
		break;

	default:
		rc = -EOPNOTSUPP;
	}

	ACPI_FREE(obj);

	return rc;
}

static int wbrf_record(struct acpi_device *adev, uint8_t action,
		       struct wbrf_ranges_in *in)
{
	union acpi_object argv4;
	union acpi_object *tmp;
	u32 num_of_ranges = 0;
	u32 num_of_elements;
	u32 arg_idx = 0;
	u32 loop_idx;
	int ret;

	if (!in)
		return -EINVAL;

	for (loop_idx = 0; loop_idx < ARRAY_SIZE(in->band_list);
	     loop_idx++)
		if (in->band_list[loop_idx].start &&
		    in->band_list[loop_idx].end)
			num_of_ranges++;

	/*
	 * Every range comes with two end points(start and end) and
	 * each of them is accounted as an element. Meanwhile the range
	 * count and action type are accounted as an element each.
	 * So, the total element count = 2 * num_of_ranges + 1 + 1.
	 */
	num_of_elements = 2 * num_of_ranges + 1 + 1;

	tmp = kcalloc(num_of_elements, sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	argv4.package.type = ACPI_TYPE_PACKAGE;
	argv4.package.count = num_of_elements;
	argv4.package.elements = tmp;

	tmp[arg_idx].integer.type = ACPI_TYPE_INTEGER;
	tmp[arg_idx++].integer.value = num_of_ranges;
	tmp[arg_idx].integer.type = ACPI_TYPE_INTEGER;
	tmp[arg_idx++].integer.value = action;

	for (loop_idx = 0; loop_idx < ARRAY_SIZE(in->band_list);
	     loop_idx++) {
		if (!in->band_list[loop_idx].start ||
		    !in->band_list[loop_idx].end)
			continue;

		tmp[arg_idx].integer.type = ACPI_TYPE_INTEGER;
		tmp[arg_idx++].integer.value = in->band_list[loop_idx].start;
		tmp[arg_idx].integer.type = ACPI_TYPE_INTEGER;
		tmp[arg_idx++].integer.value = in->band_list[loop_idx].end;
	}

	ret = wbrf_dsm(adev, WBRF_RECORD, &argv4, NULL);

	kfree(tmp);

	return ret;
}

int acpi_amd_wbrf_add_exclusion(struct device *dev,
				struct wbrf_ranges_in *in)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);

	if (!adev)
		return -ENODEV;

	return wbrf_record(adev, WBRF_RECORD_ADD, in);
}

int acpi_amd_wbrf_remove_exclusion(struct device *dev,
				   struct wbrf_ranges_in *in)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);

	if (!adev)
		return -ENODEV;

	return wbrf_record(adev, WBRF_RECORD_REMOVE, in);
}

bool acpi_amd_wbrf_supported_system(void)
{
	acpi_status status;
	acpi_handle handle;

	status = acpi_get_handle(NULL, ACPI_AMD_WBRF_METHOD, &handle);

	return ACPI_SUCCESS(status);
}

bool acpi_amd_wbrf_supported_producer(struct device *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);

	if (!adev)
		return false;

	return acpi_check_dsm(adev->handle, &wifi_acpi_dsm_guid,
			      WBRF_REVISION,
			      BIT(WBRF_RECORD));
}

static union acpi_object *
acpi_evaluate_wbrf(acpi_handle handle, u64 rev, u64 func)
{
	acpi_status ret;
	struct acpi_buffer buf = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object params[4];
	struct acpi_object_list input = {
		.count = 4,
		.pointer = params,
	};

	params[0].type = ACPI_TYPE_INTEGER;
	params[0].integer.value = rev;
	params[1].type = ACPI_TYPE_INTEGER;
	params[1].integer.value = func;
	params[2].type = ACPI_TYPE_PACKAGE;
	params[2].package.count = 0;
	params[2].package.elements = NULL;
	params[3].type = ACPI_TYPE_STRING;
	params[3].string.length = 0;
	params[3].string.pointer = NULL;

	ret = acpi_evaluate_object(handle, "WBRF", &input, &buf);
	if (ACPI_SUCCESS(ret))
		return (union acpi_object *)buf.pointer;

	return NULL;
}

static bool check_acpi_wbrf(acpi_handle handle, u64 rev, u64 funcs)
{
	int i;
	u64 mask = 0;
	union acpi_object *obj;

	if (funcs == 0)
		return false;

	obj = acpi_evaluate_wbrf(handle, rev, 0);
	if (!obj)
		return false;

	if (obj->type != ACPI_TYPE_BUFFER)
		return false;

	/*
	 * Bit vector providing supported functions information.
	 * Each bit marks support for one specific function of the WBRF method.
	 */
	for (i = 0; i < obj->buffer.length && i < 8; i++)
		mask |= (((u64)obj->buffer.pointer[i]) << (i * 8));

	ACPI_FREE(obj);

	if ((mask & BIT(WBRF_ENABLED)) &&
	    (mask & funcs) == funcs)
		return true;

	return false;
}

bool acpi_amd_wbrf_supported_consumer(struct device *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);

	if (!adev)
		return false;

	return check_acpi_wbrf(adev->handle,
			       WBRF_REVISION,
			       BIT(WBRF_RETRIEVE));
}

int acpi_amd_wbrf_retrieve_exclusions(struct device *dev,
				      struct wbrf_ranges_out *out)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);
	struct amd_wbrf_ranges_out acpi_out = {0};
	union acpi_object *obj;

	if (!adev)
		return -ENODEV;

	obj = acpi_evaluate_wbrf(adev->handle,
				 WBRF_REVISION,
				 WBRF_RETRIEVE);
	if (!obj)
		return -EINVAL;

	/*
	 * The return buffer is with variable length and the format below:
	 * number_of_entries(1 DWORD):       Number of entries
	 * start_freq of 1st entry(1 QWORD): Start frequency of the 1st entry
	 * end_freq of 1st entry(1 QWORD):   End frequency of the 1st entry
	 * ...
	 * ...
	 * start_freq of the last entry(1 QWORD)
	 * end_freq of the last entry(1 QWORD)
	 *
	 * Thus the buffer length is determined by the number of entries.
	 * - For zero entry scenario, the buffer length will be 4 bytes.
	 * - For one entry scenario, the buffer length will be 20 bytes.
	 */
	if (obj->buffer.length > sizeof(acpi_out) ||
	    obj->buffer.length < 4) {
		dev_err(dev, "BIOS FUBAR, ignoring wrong sized WBRT information");
		ACPI_FREE(obj);
		return -EINVAL;
	}
	memcpy(&acpi_out, obj->buffer.pointer, obj->buffer.length);

	out->num_of_ranges = acpi_out.num_of_ranges;
	memcpy(out->band_list, acpi_out.band_list, sizeof(acpi_out.band_list));

	ACPI_FREE(obj);

	return 0;
}
