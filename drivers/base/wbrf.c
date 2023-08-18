// SPDX-License-Identifier: GPL-2.0
/*
 * Wifi Band Exclusion Interface
 * Copyright (C) 2023 Advanced Micro Devices
 *
 */

#include <linux/wbrf.h>
#include <linux/acpi_amd_wbrf.h>

static BLOCKING_NOTIFIER_HEAD(wbrf_chain_head);

static DEFINE_MUTEX(wbrf_mutex);

static struct exclusion_range_pool {
	struct exclusion_range	band_list[MAX_NUM_OF_WBRF_RANGES];
	u64			ref_counter[MAX_NUM_OF_WBRF_RANGES];
} wbrf_pool;

enum WBRF_SUPPORT_CHECK {
	WBRF_SUPPORT_UNCHECKED,
	WBRF_SUPPORT_NONE,
	WBRF_SUPPORT_GENERIC,
	WBRF_SUPPORT_OTHERS,
};
static atomic_t wbrf_support_check = ATOMIC_INIT(WBRF_SUPPORT_UNCHECKED);

static enum WBRF_POLICY_MODE {
	WBRF_POLICY_FORCE_DISABLE,
	WBRF_POLICY_AUTO,
	WBRF_POLICY_FORCE_ENABLE,
} wbrf_policy = WBRF_POLICY_AUTO;

static int __init parse_wbrf_policy_mode(char *p)
{
	if (!strncmp(p, "auto", 4))
		wbrf_policy = WBRF_POLICY_AUTO;
	else if (!strncmp(p, "on", 2))
		wbrf_policy = WBRF_POLICY_FORCE_ENABLE;
	else if (!strncmp(p, "off", 3))
		wbrf_policy = WBRF_POLICY_FORCE_DISABLE;
	else
		return -EINVAL;

	return 0;
}
early_param("wbrf", parse_wbrf_policy_mode);

static int _wbrf_add_exclusion_ranges(struct wbrf_ranges_in *in)
{
	int i, j;

	for (i = 0; i < ARRAY_SIZE(in->band_list); i++) {
		if (!in->band_list[i].start &&
		    !in->band_list[i].end)
			continue;

		for (j = 0; j < ARRAY_SIZE(wbrf_pool.band_list); j++) {
			if (wbrf_pool.band_list[j].start == in->band_list[i].start &&
			    wbrf_pool.band_list[j].end == in->band_list[i].end) {
				wbrf_pool.ref_counter[j]++;
				break;
			}
		}
		if (j < ARRAY_SIZE(wbrf_pool.band_list))
			continue;

		for (j = 0; j < ARRAY_SIZE(wbrf_pool.band_list); j++) {
			if (!wbrf_pool.band_list[j].start &&
			    !wbrf_pool.band_list[j].end) {
				wbrf_pool.band_list[j].start = in->band_list[i].start;
				wbrf_pool.band_list[j].end = in->band_list[i].end;
				wbrf_pool.ref_counter[j] = 1;
				break;
			}
		}
		if (j >= ARRAY_SIZE(wbrf_pool.band_list))
			return -ENOSPC;
	}

	return 0;
}

static int _wbrf_remove_exclusion_ranges(struct wbrf_ranges_in *in)
{
	int i, j;

	for (i = 0; i < ARRAY_SIZE(in->band_list); i++) {
		if (!in->band_list[i].start &&
		    !in->band_list[i].end)
			continue;

		for (j = 0; j < ARRAY_SIZE(wbrf_pool.band_list); j++) {
			if (wbrf_pool.band_list[j].start == in->band_list[i].start &&
			    wbrf_pool.band_list[j].end == in->band_list[i].end) {
				wbrf_pool.ref_counter[j]--;
				if (!wbrf_pool.ref_counter[j]) {
					wbrf_pool.band_list[j].start = 0;
					wbrf_pool.band_list[j].end = 0;
				}
				break;
			}
		}
	}

	return 0;
}

static int _wbrf_retrieve_exclusion_ranges(struct wbrf_ranges_out *out)
{
	int out_idx = 0;
	int i;

	memset(out, 0, sizeof(*out));

	for (i = 0; i < ARRAY_SIZE(wbrf_pool.band_list); i++) {
		if (!wbrf_pool.band_list[i].start &&
		    !wbrf_pool.band_list[i].end)
			continue;

		out->band_list[out_idx].start = wbrf_pool.band_list[i].start;
		out->band_list[out_idx++].end = wbrf_pool.band_list[i].end;
	}

	out->num_of_ranges = out_idx;

	return 0;
}

/**
 * wbrf_supported_system - Determine if the system supports WBRF features
 *
 * WBRF is used to mitigate devices that cause harmonic interference.
 * This function will determine if the platform is able to support the
 * WBRF features. For example, for AMD ACPI implementation it should say
 * true only when the necessary AML code/logic supporting wbrf feature
 * available.
 */
static enum WBRF_SUPPORT_CHECK wbrf_supported_system(void)
{
	enum WBRF_SUPPORT_CHECK support_check;

	support_check = atomic_read(&wbrf_support_check);
	if (support_check != WBRF_SUPPORT_UNCHECKED)
		return support_check;

	support_check = WBRF_SUPPORT_NONE;

	switch (wbrf_policy) {
	case WBRF_POLICY_FORCE_ENABLE:
#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
		if (acpi_amd_wbrf_supported_system()) {
			support_check = WBRF_SUPPORT_OTHERS;
			break;
		}
		pr_warn_once("Force WBRF w/o acpi_amd_wbrf support\n");
		pr_warn_once("Fall back to generic version\n");
#endif
		support_check = WBRF_SUPPORT_GENERIC;
		break;
	case WBRF_POLICY_FORCE_DISABLE:
		break;
	case WBRF_POLICY_AUTO:
#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
		if (acpi_amd_wbrf_supported_system())
			support_check = WBRF_SUPPORT_OTHERS;
#endif
		break;
	}

	atomic_set(&wbrf_support_check, support_check);

	return support_check;
}

/**
 * wbrf_supported_producer - Determine if the device should report frequencies
 *
 * @dev: device pointer
 *
 * WBRF is used to mitigate devices that cause harmonic interference.
 * This function will determine if this device should report such frequencies.
 * For example, for AMD ACPI implementation it should say true only when the
 * necessary AML code/logic supporting wbrf feature available for this device.
 */
bool wbrf_supported_producer(struct device *dev)
{
	switch (wbrf_supported_system()) {
	case WBRF_SUPPORT_GENERIC:
		return true;
	case WBRF_SUPPORT_OTHERS:
#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
		return acpi_amd_wbrf_supported_producer(dev);
#endif
		fallthrough;
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(wbrf_supported_producer);

/**
 * wbrf_add_exclusion - Add frequency ranges to the exclusion list
 *
 * @dev: device pointer
 * @in: input structure containing the frequency ranges to be added
 *
 * Add frequencies into the exclusion list for supported consumers
 * to react to.
 */
int wbrf_add_exclusion(struct device *dev,
		       struct wbrf_ranges_in *in)
{
	int r = -ENODEV;

	mutex_lock(&wbrf_mutex);

	switch (wbrf_supported_system()) {
	case WBRF_SUPPORT_OTHERS:
#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
		r = acpi_amd_wbrf_add_exclusion(dev, in);
#endif
		break;
	case WBRF_SUPPORT_GENERIC:
		r = _wbrf_add_exclusion_ranges(in);
		break;
	default:
		break;
	}

	mutex_unlock(&wbrf_mutex);
	if (r)
		return r;

	blocking_notifier_call_chain(&wbrf_chain_head, WBRF_CHANGED, NULL);

	return 0;
}
EXPORT_SYMBOL_GPL(wbrf_add_exclusion);

/**
 * wbrf_remove_exclusion - Remove frequency ranges from the exclusion list
 *
 * @dev: device pointer
 * @in: input structure containing the frequency ranges to be removed
 *
 * Remove frequencies from the exclusion list for supported consumers
 * to react to.
 */
int wbrf_remove_exclusion(struct device *dev,
			  struct wbrf_ranges_in *in)
{
	int r = -ENODEV;

	mutex_lock(&wbrf_mutex);

	switch (wbrf_supported_system()) {
	case WBRF_SUPPORT_OTHERS:
#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
		r  = acpi_amd_wbrf_remove_exclusion(dev, in);
#endif
		break;
	case WBRF_SUPPORT_GENERIC:
		r = _wbrf_remove_exclusion_ranges(in);
		break;
	default:
		break;
	}

	mutex_unlock(&wbrf_mutex);
	if (r)
		return r;

	blocking_notifier_call_chain(&wbrf_chain_head, WBRF_CHANGED, NULL);

	return 0;
}
EXPORT_SYMBOL_GPL(wbrf_remove_exclusion);

/**
 * wbrf_supported_consumer - Determine if the device should react to frequencies
 *
 * @dev: device pointer
 *
 * WBRF is used to mitigate devices that cause harmonic interference.
 * This function will determine if this device should react to reports from
 * other devices for such frequencies. For example, for AMD ACPI implementation
 * it should say true only when the necessary AML code/logic supporting wbrf
 * feature available for this device.
 */
bool wbrf_supported_consumer(struct device *dev)
{
	switch (wbrf_supported_system()) {
	case WBRF_SUPPORT_GENERIC:
		return true;
	case WBRF_SUPPORT_OTHERS:
#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
		return acpi_amd_wbrf_supported_consumer(dev);
#endif
		fallthrough;
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(wbrf_supported_consumer);

/**
 * wbrf_register_notifier - Register for notifications of frequency changes
 *
 * @nb: driver notifier block
 *
 * WBRF is used to mitigate devices that cause harmonic interference.
 * This function will allow consumers to register for frequency notifications.
 */
int wbrf_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&wbrf_chain_head, nb);
}
EXPORT_SYMBOL_GPL(wbrf_register_notifier);

/**
 * wbrf_unregister_notifier - Unregister for notifications of frequency changes
 *
 * @nb: driver notifier block
 *
 * WBRF is used to mitigate devices that cause harmonic interference.
 * This function will allow consumers to unregister for frequency notifications.
 */
int wbrf_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&wbrf_chain_head, nb);
}
EXPORT_SYMBOL_GPL(wbrf_unregister_notifier);

/**
 * wbrf_retrieve_exclusions - Retrieve the exclusion list
 *
 * @dev: device pointer
 * @out: output structure containing the frequency ranges to be excluded
 *
 * Retrieve the current exclusion list
 */
int wbrf_retrieve_exclusions(struct device *dev,
			     struct wbrf_ranges_out *out)
{
	int r = -ENODEV;

	mutex_lock(&wbrf_mutex);

	switch (wbrf_supported_system()) {
	case WBRF_SUPPORT_OTHERS:
#if IS_ENABLED(CONFIG_WBRF_AMD_ACPI)
		r = acpi_amd_wbrf_retrieve_exclusions(dev, out);
#endif
		break;
	case WBRF_SUPPORT_GENERIC:
		r = _wbrf_retrieve_exclusion_ranges(out);
		break;
	default:
		break;
	}

	mutex_unlock(&wbrf_mutex);

	return r;
}
EXPORT_SYMBOL_GPL(wbrf_retrieve_exclusions);
