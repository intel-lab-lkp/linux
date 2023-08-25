// SPDX-License-Identifier: GPL-2.0
/*
 * Wifi Band Exclusion Interface (AMD ACPI Implementation)
 * Copyright (C) 2023 Advanced Micro Devices
 *
 * Due to electrical and mechanical constraints in certain platform designs
 * there may be likely interference of relatively high-powered harmonics of
 * the (G-)DDR memory clocks with local radio module frequency bands used
 * by Wifi 6/6e/7.
 *
 * To mitigate this, AMD has introduced an ACPI based mechanism to support
 * WBRF(Wifi Band RFI mitigation Feature) for platforms with AMD dGPU + WLAN.
 * This needs support from BIOS equipped with necessary AML implementations
 * and dGPU firmwares.
 *
 * Some general terms:
 * Producer: such component who can produce high-powered radio frequency
 * Consumer: such component who can adjust its in-use frequency in
 *           response to the radio frequencies of other components to
 *           mitigate the possible RFI.
 *
 * To make the mechanism function, those producers should notify active use
 * of their particular frequencies so that other consumers can make relative
 * internal adjustments as necessary to avoid this resonance.
 */

#ifndef _ACPI_AMD_WBRF_H
#define _ACPI_AMD_WBRF_H

#include <linux/device.h>
#include <linux/notifier.h>

/*
 * A wbrf range is defined as a frequency band with start and end
 * frequency point specified(in Hz). And a vaild range should have
 * its start and end frequency point filled with non-zero values.
 * Meanwhile, the maximum number of wbrf ranges is limited as
 * `MAX_NUM_OF_WBRF_RANGES`.
 */
#define MAX_NUM_OF_WBRF_RANGES		11

struct exclusion_range {
	u64		start;
	u64		end;
};

struct wbrf_ranges_in_out {
	u64			num_of_ranges;
	struct exclusion_range	band_list[MAX_NUM_OF_WBRF_RANGES];
};

/*
 * The notification types for the consumers are defined as below.
 * The consumers may need to take different actions in response to
 * different notifications.
 * WBRF_CHANGED: there was some frequency band updates. The consumers
 *               should retrieve the latest active frequency bands.
 */
enum wbrf_notifier_actions {
	WBRF_CHANGED,
};

#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
/*
 * The expected flow for the producers:
 * 1) During probe, call `acpi_amd_wbrf_supported_producer` to check
 *    if WBRF can be enabled for the device.
 * 2) On using some frequency band, call `acpi_amd_wbrf_add_exclusion`
 *    to get other consumers properly notified.
 * 3) Or on stopping using some frequency band, call
 *    `acpi_amd_wbrf_remove_exclusion` to get other consumers notified.
 */
bool acpi_amd_wbrf_supported_producer(struct device *dev);
int acpi_amd_wbrf_remove_exclusion(struct device *dev,
				   struct wbrf_ranges_in_out *in);
int acpi_amd_wbrf_add_exclusion(struct device *dev,
				struct wbrf_ranges_in_out *in);

/*
 * The expected flow for the consumers:
 * 1) During probe, call `acpi_amd_wbrf_supported_consumer` to check if WBRF
 *    can be enabled for the device.
 * 2) Call `acpi_amd_wbrf_register_notifier` to register for notification
 *    of frequency band change(add or remove) from other producers.
 * 3) Call the `acpi_amd_wbrf_retrieve_exclusions` intentionally to retrieve
 *    current active frequency bands considering some producers may broadcast
 *    such information before the consumer is up.
 * 4) On receiving a notification for frequency band change, run
 *    `acpi_amd_wbrf_retrieve_exclusions` again to retrieve the latest
 *    active frequency bands.
 * 5) During driver cleanup, call `acpi_amd_wbrf_unregister_notifier` to
 *    unregister the notifier.
 */
bool acpi_amd_wbrf_supported_consumer(struct device *dev);
int acpi_amd_wbrf_retrieve_exclusions(struct device *dev,
				      struct wbrf_ranges_in_out *out);
int acpi_amd_wbrf_register_notifier(struct notifier_block *nb);
int acpi_amd_wbrf_unregister_notifier(struct notifier_block *nb);
#else
static inline
bool acpi_amd_wbrf_supported_consumer(struct device *dev)
{
	return false;
}
static inline
int acpi_amd_wbrf_remove_exclusion(struct device *dev,
				   struct wbrf_ranges_in_out *in)
{
	return -ENODEV;
}
static inline
int acpi_amd_wbrf_add_exclusion(struct device *dev,
				struct wbrf_ranges_in_out *in)
{
	return -ENODEV;
}
static inline
bool acpi_amd_wbrf_supported_producer(struct device *dev)
{
	return false;
}
static inline
int acpi_amd_wbrf_retrieve_exclusions(struct device *dev,
				      struct wbrf_ranges_in_out *out)
{
	return -ENODEV;
}
static inline
int acpi_amd_wbrf_register_notifier(struct notifier_block *nb)
{
	return -ENODEV;
}
static inline
int acpi_amd_wbrf_unregister_notifier(struct notifier_block *nb)
{
	return -ENODEV;
}
#endif

#endif /* _ACPI_AMD_WBRF_H */
