/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_RAS_TYPES_H_
#define _XE_RAS_TYPES_H_

#include <linux/types.h>

#define XE_RAS_NUM_ERROR_ARR			3
#define XE_RAS_NUM_COUNTERS			16
#define XE_RAS_SOC_IEH_PUNIT			BIT(1)
#define XE_RAS_MEMORY_ECC			BIT(1)
#define XE_RAS_NUM_PAGES			25

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
 * enum xe_ras_page_action - Page offline actions for page offline request
 *
 * @XE_RAS_PAGE_ACTION_OFFLINE: Instruct firmware to remove page from queue
 * @XE_RAS_PAGE_ACTION_DECLINE: Instruct firmware to mark page as not offline
 * @XE_RAS_PAGE_ACTION_MAX: Max value for validation
 */
enum xe_ras_page_action {
	XE_RAS_PAGE_ACTION_OFFLINE,
	XE_RAS_PAGE_ACTION_DECLINE,
	XE_RAS_PAGE_ACTION_MAX
};

/**
 * enum xe_ras_response_status - RAS response status codes
 *
 * @XE_RAS_STATUS_SUCCESS: Operation successful
 * @XE_RAS_STATUS_INVALID_PARAM: Invalid parameter
 * @XE_RAS_STATUS_OP_NOT_SUPPORTED: Operation not supported
 * @XE_RAS_STATUS_TIMEOUT: Operation timed out
 * @XE_RAS_STATUS_HARDWARE_FAILURE: Hardware failure
 * @XE_RAS_STATUS_INSUFFICIENT_RESOURCES: Insufficient resources
 * @XE_RAS_STATUS_UNKNOWN_ERROR: Unknown error
 */
enum xe_ras_response_status {
	XE_RAS_STATUS_SUCCESS = 0,
	XE_RAS_STATUS_INVALID_PARAM,
	XE_RAS_STATUS_OP_NOT_SUPPORTED,
	XE_RAS_STATUS_TIMEOUT,
	XE_RAS_STATUS_HARDWARE_FAILURE,
	XE_RAS_STATUS_INSUFFICIENT_RESOURCES,
	XE_RAS_STATUS_UNKNOWN_ERROR
};

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
	u32 error_details[XE_RAS_NUM_COUNTERS];
} __packed;

/**
 * struct xe_ras_get_soc_error - Response from get soc error command
 */
struct xe_ras_get_soc_error {
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
	/** @reserved: Reserved */
	u32 reserved[15];
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
	/** @error_source: Error source */
	struct xe_ras_soc_error_source error_source;
	/** @additional_details: Additional details */
	u32 additional_details[15];
} __packed;

/**
 * struct xe_ras_csc_error - CSC error details
 */
struct xe_ras_csc_error {
	/** @hec_uncorr_err_status: CSC hardware error status */
	u32 hec_uncorr_err_status;
	/** @hec_uncorr_fw_err_dw0: CSC firmware error */
	u32 hec_uncorr_fw_err_dw0;
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
	/** @additional_info: Additional information */
	u32 additional_info[10];
} __packed;

/**
 * struct xe_ras_memory_error - Device memory error details
 */
struct xe_ras_memory_error {
	/** @category: Device memory error category */
	u8 category;
	/** @reserved: Reserved for future use */
	u8 reserved[7];
	/** @hardware_address: Hardware physical address details */
	u64 hardware_address;
	/** @sw_address: Software address where error occurred */
	u64 sw_address;
	/** @reserved2: Reserved for future use */
	u32 reserved2[10];
} __packed;

/**
 * struct xe_ras_page_offline_list - Response from get offline list command
 */
struct xe_ras_page_offline_list {
	/** @max_entries: Total no of pages that can be stored in flash */
	u32 max_entries;
	/** @total_pages: Total number of permanently offlined pages */
	u32 total_pages;
	/** @pages_returned: Number of pages returned in this response */
	u32 pages_returned;
	/** @page_addresses: Array of permanently offlined page addresses (4KB aligned) */
	u64 page_addresses[XE_RAS_NUM_PAGES];
	/** @additional_data: Indicates if more data is available */
	u8 additional_data;
	/** @reserved: Reserved for future use */
	u8 reserved[3];
} __packed;

/**
 * struct xe_ras_page_offline_queue - Response from get offline queue command
 */
struct xe_ras_page_offline_queue {
	/** @total_pages: Total number of queued pages */
	u32 total_pages;
	/** @pages_returned: Number of pages returned in this response */
	u32 pages_returned;
	/** @page_addresses: Array of page addresses (4KB aligned) */
	u64 page_addresses[XE_RAS_NUM_PAGES];
	/** @additional_data: Indicates if more data is available */
	u8 additional_data;
	/** @reserved: Reserved for future use */
	u8 reserved[3];
} __packed;

/**
 * struct xe_ras_page_offline_request - Request for page offline command
 *
 * This structure provides the request format to offline/decline a page
 */
struct xe_ras_page_offline_request {
	/** @page_address: Page address (4KB aligned) */
	u64 page_address;
	/** @action: Action to be performed, see &enum xe_ras_page_action */
	u32 action;
	/** @reserved: Reserved for future use */
	u32 reserved;
} __packed;

/**
 * struct xe_ras_page_offline_response - Response from page offline command
 */
struct xe_ras_page_offline_response {
	/** @status: Status of the page offline request, see &enum xe_ras_response_status */
	u32 status;
	/** @reserved: Reserved for future use */
	u32 reserved;
} __packed;
#endif
