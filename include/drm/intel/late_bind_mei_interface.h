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
