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
 * struct late_bind_component_ops - ops for Late Binding services.
 * @owner: Module providing the ops
 * @push_config: Sends a config to FW.
 */
struct late_bind_component_ops {
	struct module *owner;

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
