/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_LATE_BIND_TYPES_H_
#define _XE_LATE_BIND_TYPES_H_

#include <linux/iosys-map.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define MAX_PAYLOAD_SIZE (1024 * 4)

/**
 * xe_late_bind_fw_id - enum to determine late binding fw index
 */
enum xe_late_bind_fw_id {
	FAN_CONTROL_FW_ID = 0,
	MAX_FW_ID
};

/**
 * struct xe_late_bind_fw
 */
struct xe_late_bind_fw {
	/** @late_bind_fw.valid: to check if fw is valid */
	bool valid;
	/** @late_bind_fw.id: firmware index */
	u32 id;
	/** @late_bind_fw.blob_path: firmware binary path */
	char blob_path[PATH_MAX];
	/** @late_bind_fw.type: firmware type */
	u32  type;
	/** @late_bind_fw.flags: firmware flags */
	u32  flags;
	/** @late_bind_fw.payload: to store the late binding blob */
	u8  payload[MAX_PAYLOAD_SIZE];
	/** @late_bind_fw.payload_size: late binding blob payload_size */
	size_t payload_size;
};

/**
 * struct xe_late_bind_component - Late Binding services component
 * @mei_dev: device that provide Late Binding service.
 * @ops: Ops implemented by Late Binding driver, used by Xe driver.
 *
 * Communication between Xe and MEI drivers for Late Binding services
 */
struct xe_late_bind_component {
	/** @late_bind_component.mei_dev: mei device */
	struct device *mei_dev;
	/** @late_bind_component.ops: late binding ops */
	const struct late_bind_component_ops *ops;
};

/**
 * struct xe_late_bind
 */
struct xe_late_bind {
	/** @late_bind.component: struct for communication with mei component */
	struct xe_late_bind_component component;
	/** @late_bind.component_added: whether the component has been added */
	bool component_added;
	/** @late_bind.mutex: protects the component binding and usage */
	struct mutex mutex;
	/** @late_bind.late_bind_fw: late binding firmware array */
	struct xe_late_bind_fw late_bind_fw[MAX_FW_ID];
};

#endif
