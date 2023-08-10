// SPDX-License-Identifier: GPL-2.0
/*
 * Wifi Band Exclusion Interface (AMD ACPI Implementation)
 * Copyright (C) 2023 Advanced Micro Devices
 *
 */

#ifndef _ACPI_AMD_WBRF_H
#define _ACPI_AMD_WBRF_H

#include <linux/wbrf.h>

#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
bool acpi_amd_wbrf_supported_system(void);
bool acpi_amd_wbrf_supported_consumer(struct device *dev);
bool acpi_amd_wbrf_supported_producer(struct device *dev);
int acpi_amd_wbrf_remove_exclusion(struct device *dev,
				   struct wbrf_ranges_in *in);
int acpi_amd_wbrf_add_exclusion(struct device *dev,
				struct wbrf_ranges_in *in);
int acpi_amd_wbrf_retrieve_exclusions(struct device *dev,
				      struct wbrf_ranges_out *out);
#endif

#endif /* _ACPI_AMD_WBRF_H */
