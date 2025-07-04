/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025 Intel Corporation
 */

#ifndef _LATE_BIND_MEI_INTERFACE_H_
#define _LATE_BIND_MEI_INTERFACE_H_

#include <linux/types.h>

struct device;
struct module;

/**
 * Late Binding flags
 * Persistent across warm reset
 */
#define CSC_LATE_BINDING_FLAGS_IS_PERSISTENT	BIT(0)

/**
 * xe_late_bind_fw_type - enum to determine late binding fw type
 */
enum late_bind_type {
	CSC_LATE_BINDING_TYPE_FAN_CONTROL = 1,
};

/**
 * Late Binding payload status
 */
enum csc_late_binding_status {
	CSC_LATE_BINDING_STATUS_SUCCESS           = 0,
	CSC_LATE_BINDING_STATUS_4ID_MISMATCH      = 1,
	CSC_LATE_BINDING_STATUS_ARB_FAILURE       = 2,
	CSC_LATE_BINDING_STATUS_GENERAL_ERROR     = 3,
	CSC_LATE_BINDING_STATUS_INVALID_PARAMS    = 4,
	CSC_LATE_BINDING_STATUS_INVALID_SIGNATURE = 5,
	CSC_LATE_BINDING_STATUS_INVALID_PAYLOAD   = 6,
	CSC_LATE_BINDING_STATUS_TIMEOUT           = 7,
};

/**
 * struct late_bind_component_ops - ops for Late Binding services.
 * @owner: Module providing the ops
 * @push_config: Sends a config to FW.
 */
struct late_bind_component_ops {
	/**
	 * @push_config: Sends a config to FW.
	 * @dev: device struct corresponding to the mei device
	 * @type: payload type
	 * @flags: payload flags
	 * @payload: payload buffer
	 * @payload_size: payload buffer size
	 *
	 * Return: 0 success, negative errno value on transport failure,
	 *         positive status returned by FW
	 */
	int (*push_config)(struct device *dev, u32 type, u32 flags,
			   const void *payload, size_t payload_size);
};

#endif /* _LATE_BIND_MEI_INTERFACE_H_ */
