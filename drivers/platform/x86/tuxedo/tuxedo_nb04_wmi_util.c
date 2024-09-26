// SPDX-License-Identifier: GPL-2.0
/*
 * This code gives functions to avoid code duplication while interacting with
 * the TUXEDO NB04 wmi interfaces.
 *
 * Copyright (C) 2024 Werner Sembach wse@tuxedocomputers.com
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "tuxedo_nb04_wmi_ab_init.h"

#include "tuxedo_nb04_wmi_util.h"

static int __wmi_method_acpi_object_out(struct wmi_device *wdev, uint32_t wmi_method_id,
					uint8_t *in, acpi_size in_len, union acpi_object **out)
{
	struct tuxedo_nb04_wmi_driver_data_t *driver_data = wdev->dev.driver_data;
	struct acpi_buffer acpi_buffer_in = { in_len, in };
	struct acpi_buffer acpi_buffer_out = { ACPI_ALLOCATE_BUFFER, NULL };

	pr_debug("Evaluate WMI method: %u in:\n", wmi_method_id);
	print_hex_dump_bytes("", DUMP_PREFIX_OFFSET, in, in_len);

	mutex_lock(&driver_data->wmi_access_mutex);
	acpi_status status = wmidev_evaluate_method(wdev, 0, wmi_method_id, &acpi_buffer_in,
						    &acpi_buffer_out);
	mutex_unlock(&driver_data->wmi_access_mutex);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to evaluate WMI method.\n");
		return -EIO;
	}
	if (!acpi_buffer_out.pointer) {
		pr_err("Unexpected empty out buffer.\n");
		return -ENODATA;
	}

	*out = acpi_buffer_out.pointer;

	return 0;
}

static int __wmi_method_buffer_out(struct wmi_device *wdev, uint32_t wmi_method_id, uint8_t *in,
				   acpi_size in_len, uint8_t *out, acpi_size out_len)
{
	int ret;
	union acpi_object *acpi_object_out = NULL;

	ret = __wmi_method_acpi_object_out(wdev, wmi_method_id, in, in_len, &acpi_object_out);
	if (ret)
		return ret;

	if (acpi_object_out->type != ACPI_TYPE_BUFFER) {
		pr_err("Unexpected out buffer type. Expected: %u Got: %u\n", ACPI_TYPE_BUFFER,
		       acpi_object_out->type);
		kfree(acpi_object_out);
		return -EIO;
	}
	if (acpi_object_out->buffer.length != out_len) {
		pr_err("Unexpected out buffer length.\n");
		kfree(acpi_object_out);
		return -EIO;
	}

	memcpy(out, acpi_object_out->buffer.pointer, out_len);
	kfree(acpi_object_out);

	return ret;
}

int tuxedo_nb04_wmi_8_b_in_80_b_out(struct wmi_device *wdev,
				    enum tuxedo_nb04_wmi_8_b_in_80_b_out_methods method,
				    union tuxedo_nb04_wmi_8_b_in_80_b_out_input *input,
				    union tuxedo_nb04_wmi_8_b_in_80_b_out_output *output)
{
	return __wmi_method_buffer_out(wdev, method, input->raw, 8, output->raw, 80);
}

int tuxedo_nb04_wmi_496_b_in_80_b_out(struct wmi_device *wdev,
				      enum tuxedo_nb04_wmi_496_b_in_80_b_out_methods method,
				      union tuxedo_nb04_wmi_496_b_in_80_b_out_input *input,
				      union tuxedo_nb04_wmi_496_b_in_80_b_out_output *output)
{
	return __wmi_method_buffer_out(wdev, method, input->raw, 496, output->raw, 80);
}
