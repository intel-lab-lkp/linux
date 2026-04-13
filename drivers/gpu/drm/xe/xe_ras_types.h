/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_RAS_TYPES_H_
#define _XE_RAS_TYPES_H_

#include <linux/types.h>

#define XE_RAS_NUM_ERROR_ARR		3
#define XE_RAS_MAX_ERROR_DETAILS	16
#define XE_RAS_IEH_PUNIT_ERROR		BIT(1)

/**
 * enum xe_ras_recovery_action - RAS recovery actions
 *
 * @XE_RAS_RECOVERY_ACTION_RECOVERED: Error recovered
 * @XE_RAS_RECOVERY_ACTION_RESET: Requires reset
 * @XE_RAS_RECOVERY_ACTION_DISCONNECT: Requires disconnect
 * @XE_RAS_RECOVERY_ACTION_MAX: Max action value
 *
 * This enum defines the possible recovery actions that can be taken in response
 * to RAS errors.
 */
enum xe_ras_recovery_action {
	XE_RAS_RECOVERY_ACTION_RECOVERED = 0,
	XE_RAS_RECOVERY_ACTION_RESET,
	XE_RAS_RECOVERY_ACTION_DISCONNECT,
	XE_RAS_RECOVERY_ACTION_MAX
};

/**
 * struct xe_ras_error_common - Common RAS error class
 *
 * This structure contains error severity and component information
 * across all products
 */
struct xe_ras_error_common {
	/** @severity: Error Severity */
	u8 severity;
	/** @component: IP where the error originated */
	u8 component;
} __packed;

/**
 * struct xe_ras_error_unit - Error unit information
 */
struct xe_ras_error_unit {
	/** @tile: Tile identifier */
	u8 tile;
	/** @instance: Instance identifier within a component */
	u32 instance;
} __packed;

/**
 * struct xe_ras_error_cause - Error cause information
 */
struct xe_ras_error_cause {
	/** @cause: Cause */
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
	/** @error_cause: Cause/checker */
	struct xe_ras_error_cause error_cause;
} __packed;

/**
 * struct xe_ras_error_class - Complete RAS Error Class
 *
 * This structure provides the complete error classification by combining
 * the common error class with the product-specific error class.
 */
struct xe_ras_error_class {
	/** @common: Common error severity and component */
	struct xe_ras_error_common common;
	/** @product: Product-specific unit and cause */
	struct xe_ras_error_product product;
} __packed;

/**
 * struct xe_ras_error_array - Details of the error types
 */
struct xe_ras_error_array {
	/** @counter_value: Counter value of the returned error */
	u32 counter_value;
	/** @error_class: Error class */
	struct xe_ras_error_class error_class;
	/** @timestamp: Timestamp */
	u64 timestamp;
	/** @error_details: Error details specific to the class */
	u32 error_details[XE_RAS_MAX_ERROR_DETAILS];
} __packed;

/**
 * struct xe_ras_get_error_response - Response for XE_SYSCTRL_GET_SOC_ERROR
 */
struct xe_ras_get_error_response {
	/** @num_errors: Number of errors reported in this response */
	u8 num_errors;
	/** @additional_errors: Indicates if the errors are pending */
	u8 additional_errors;
	/** @error_arr: Array of up to 3 errors */
	struct xe_ras_error_array error_arr[XE_RAS_NUM_ERROR_ARR];
} __packed;

/**
 * struct xe_ras_compute_error - Error details of Core Compute error
 */
struct xe_ras_compute_error {
	/** @error_log_header: Error Source and type */
	u32 error_log_header;
	/** @internal_error_log: Internal Error log */
	u32 internal_error_log;
	/** @fabric_log: Fabric Error log */
	u32 fabric_log;
	/** @internal_error_addr_log0: Internal Error addr log */
	u32 internal_error_addr_log0;
	/** @internal_error_addr_log1: Internal Error addr log */
	u32 internal_error_addr_log1;
	/** @packet_log0: Packet log */
	u32 packet_log0;
	/** @packet_log1: Packet log */
	u32 packet_log1;
	/** @packet_log2: Packet log */
	u32 packet_log2;
	/** @packet_log3: Packet log */
	u32 packet_log3;
	/** @packet_log4: Packet log */
	u32 packet_log4;
	/** @misc_log0: Misc log */
	u32 misc_log0;
	/** @misc_log1: Misc log */
	u32 misc_log1;
	/** @spare_log0: Spare log */
	u32 spare_log0;
	/** @spare_log1: Spare log */
	u32 spare_log1;
	/** @spare_log2: Spare log */
	u32 spare_log2;
	/** @spare_log3: Spare log */
	u32 spare_log3;
} __packed;

/**
 * struct xe_ras_soc_error_source - Source of SOC error
 */
struct xe_ras_soc_error_source {
	/** @csc: CSC error */
	u32 csc:1;
	/** @soc: SOC error */
	u32 soc:1;
	/** @reserved: Reserved for future use */
	u32 reserved:30;
} __packed;

/**
 * struct xe_ras_soc_error - SOC error details
 */
struct xe_ras_soc_error {
	/** @error_source: Error Source */
	struct xe_ras_soc_error_source error_source;
	/** @additional_details: Additional details */
	u32 additional_details[15];
} __packed;

/**
 * struct xe_ras_csc_error - CSC error details
 */
struct xe_ras_csc_error {
	/** @hec_uncorr_err_status: CSC error */
	u32 hec_uncorr_err_status;
	/** @hec_uncorr_fw_err_dw0: CSC f/w error */
	u32 hec_uncorr_fw_err_dw0;
} __packed;

/**
 * struct xe_ras_ieh_error - SoC IEH (Integrated Error Handler) details
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
	/** @additional_info: Additional information */
	u32 additional_info[10];
} __packed;

#endif
