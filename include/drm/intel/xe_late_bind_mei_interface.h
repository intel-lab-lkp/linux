/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025 Intel Corporation
 */

#ifndef _XE_LATE_BIND_MEI_INTERFACE_H_
#define _XE_LATE_BIND_MEI_INTERFACE_H_

#include <linux/types.h>

struct device;
struct module;

/**
 * struct xe_late_bind_component_ops - ops for Late Binding services.
 * @owner: Module providing the ops
 * @push_config: Sends a config to FW.
 */
struct xe_late_bind_component_ops {
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

/**
 * struct xe_late_bind_component - Late Binding services component
 * @mei_dev: device that provide Late Binding service.
 * @ops: Ops implemented by Late Binding driver, used by Xe driver.
 *
 * Communication between Xe and MEI drivers for Late Binding services
 */
struct xe_late_bind_component {
	struct device *mei_dev;
	const struct xe_late_bind_component_ops *ops;
};

#endif /* _XE_LATE_BIND_MEI_INTERFACE_H_ */
