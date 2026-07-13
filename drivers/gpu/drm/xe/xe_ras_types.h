/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_RAS_TYPES_H_
#define _XE_RAS_TYPES_H_

#include <linux/types.h>

#define XE_RAS_NUM_COUNTERS			16
#define XE_RAS_SOC_IEH_PUNIT			BIT(1)

/**
 * struct xe_ras_error_common - Error fields that are common across all products
 */
struct xe_ras_error_common {
	/** @severity: Error severity */
	u8 severity;
	/** @component: IP block where error originated */
	u8 component;
} __packed;

/**
 * struct xe_ras_error_unit - Error unit information
 */
struct xe_ras_error_unit {
	/** @tile: Tile identifier */
	u8 tile;
	/** @instance: Instance identifier specific to IP */
	u32 instance;
} __packed;

/**
 * struct xe_ras_error_cause - Error cause information
 */
struct xe_ras_error_cause {
	/** @cause: Cause/checker */
	u32 cause;
	/** @reserved: For future use */
	u8 reserved;
} __packed;

/**
 * struct xe_ras_error_product - Error fields that are specific to the product
 */
struct xe_ras_error_product {
	/** @unit: Unit within IP block */
	struct xe_ras_error_unit unit;
	/** @cause: Cause/checker */
	struct xe_ras_error_cause cause;
} __packed;

/**
 * struct xe_ras_error_class - Combines common and product-specific parts
 */
struct xe_ras_error_class {
	/** @common: Common error type and component */
	struct xe_ras_error_common common;
	/** @product: Product-specific unit and cause */
	struct xe_ras_error_product product;
} __packed;

/**
 * struct xe_ras_threshold_crossed - Data for threshold crossed event
 */
struct xe_ras_threshold_crossed {
	/** @ncounters: Number of error counters that crossed thresholds */
	u32 ncounters;
	/** @counters: Array of error counters that crossed threshold */
	struct xe_ras_error_class counters[XE_RAS_NUM_COUNTERS];
} __packed;

/**
 * struct xe_ras_get_counter_request - Request structure for get counter
 */
struct xe_ras_get_counter_request {
	/** @counter: Error counter to be queried */
	struct xe_ras_error_class counter;
	/** @reserved: Reserved for future use */
	u32 reserved;
} __packed;

/**
 * struct xe_ras_get_counter_response - Response structure for get counter
 */
struct xe_ras_get_counter_response {
	/** @counter: Error counter that was queried */
	struct xe_ras_error_class counter;
	/** @value: Current counter value */
	u32 value;
	/** @timestamp: Timestamp when counter was last updated */
	u64 timestamp;
	/** @threshold: Threshold value for the counter */
	u32 threshold;
	/** @reserved: Reserved  */
	u32 reserved[57];
} __packed;

/**
 * struct xe_ras_clear_counter_request - Request structure for clear counter
 */
struct xe_ras_clear_counter_request {
	/** @counter: Counter class to be cleared */
	struct xe_ras_error_class counter;
	/** @reserved: Reserved for future use */
	u32 reserved;
} __packed;

/**
 * struct xe_ras_clear_counter_response - Response structure for clear counter
 */
struct xe_ras_clear_counter_response {
	/** @counter: Counter class that was cleared */
	struct xe_ras_error_class counter;
	/** @reserved: Reserved */
	u32 reserved;
	/** @timestamp: Timestamp when the counter was cleared */
	u64 timestamp;
	/** @status: Status of the clear operation */
	u32 status;
	/** @reserved1: Reserved for future use */
	u32 reserved1[3];
} __packed;

/**
 * enum xe_ras_recovery_action - RAS recovery actions
 *
 * @XE_RAS_RECOVERY_ACTION_RECOVERED: Error recovered
 * @XE_RAS_RECOVERY_ACTION_RESET: Requires reset
 * @XE_RAS_RECOVERY_ACTION_DISCONNECT: Requires disconnect
 * @XE_RAS_RECOVERY_ACTION_MAX: Max action value
 */
enum xe_ras_recovery_action {
	XE_RAS_RECOVERY_ACTION_RECOVERED = 0,
	XE_RAS_RECOVERY_ACTION_RESET,
	XE_RAS_RECOVERY_ACTION_DISCONNECT,
	XE_RAS_RECOVERY_ACTION_MAX
};

/**
 * struct xe_ras_error_array - Details of the error types
 */
struct xe_ras_error_array {
	/** @counter_value: Counter value of the returned error */
	u32 counter_value;
	/** @counter: Error counter */
	struct xe_ras_error_class counter;
	/** @timestamp: Timestamp */
	u64 timestamp;
	/** @details: Error details specific to the counter */
	u32 details[XE_RAS_NUM_COUNTERS];
} __packed;

/**
 * struct xe_ras_soc_error_source - Source of SoC error
 */
struct xe_ras_soc_error_source {
	/** @csc: CSC */
	u32 csc:1;
	/** @ieh: IEH (Integrated Error Handler) */
	u32 ieh:1;
	/** @reserved: Reserved for future use */
	u32 reserved:30;
} __packed;

/**
 * struct xe_ras_soc_error - Error details of SoC internal error
 */
struct xe_ras_soc_error {
	/** @source: Error source */
	struct xe_ras_soc_error_source source;
	/** @details: Error details specific to the error source */
	u32 details[15];
} __packed;

/**
 * struct xe_ras_csc_error - CSC error details
 */
struct xe_ras_csc_error {
	/** @reserved: Reserved */
	u32 reserved;
	/** @hec_fw_error: CSC firmware error */
	u32 hec_fw_error;
} __packed;

/**
 * struct xe_ras_ieh_error - SoC IEH (Integrated Error Handler) error details
 */
struct xe_ras_ieh_error {
	/** @ieh_instance: IEH instance */
	u32 ieh_instance:2;
	/** @reserved: Reserved for future use */
	u32 reserved:30;
	/** @global_error_status: Global error status */
	u32 global_error_status;
	/** @local_error_status: Local error status */
	u32 local_error_status;
	/** @gerr_mask: Global error mask */
	u32 gerr_mask;
	/** @info: Additional information */
	u32 info[10];
} __packed;
#endif
