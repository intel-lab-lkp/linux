/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_MEI_LATE_BIND_TYPES_H_
#define _XE_MEI_LATE_BIND_TYPES_H_

#include <linux/iosys-map.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define MAX_PAYLOAD_SIZE (1024 * 4)

struct xe_bo;
struct xe_late_bind_component;

/**
 * xe_late_bind_fw_type - enum to determine late binding fw type
 */
enum xe_late_bind_type {
	CSC_LATE_BINDING_TYPE_FAN_CONTROL = 1,
	CSC_LATE_BINDING_TYPE_VOLTAGE_REGULATOR
};

/**
 * Late Binding flags
 */
enum csc_late_binding_flags {
	/** Persistent across warm reset */
	CSC_LATE_BINDING_FLAGS_IS_PERSISTENT = 0x1
};

/**
 * xe_late_bind_fw_id - enum to determine late binding fw index
 */
enum xe_late_bind_fw_id {
	FAN_CONTROL_ID = 0,
	VOLTAGE_REGULATOR_ID,
	MAX_ID
};

/**
 * struct xe_late_bind_fw
 */
struct xe_late_bind_fw {
	/** @late_bind_fw.valid */
	bool valid;

	/** @late_bind_fw.id */
	u32 id;

	/** @late_bind_fw.blob_path: late binding fw blob path */
	char blob_path[PATH_MAX];

	/** @late_bind_fw.type */
	u32  type;

	/** @late_bind_fw.type */
	u32  flags;

	/** @late_bind_fw.payload: to store the late binding blob */
	u8  payload[MAX_PAYLOAD_SIZE];

	/** @late_bind_fw.payload_size: late binding blob payload_size */
	size_t payload_size;

	/** @late_bind_fw.work: worker to upload latebind blob */
	struct work_struct work;
};

/**
 * struct xe_late_bind
 */
struct xe_late_bind {
	/** @late_bind.component: struct for communication with mei component */
	struct xe_late_bind_component *component;

	/** @late_bind.component_added: whether the component has been added */
	bool component_added;

	/** @late_bind.wq: workqueue to submit request to download late bind blob */
	struct workqueue_struct *wq;

	/** @late_bind.mutex: protects the component binding and usage */
	struct mutex mutex;

	/** @late_bind.late_bind_fw: late binding firmwares */
	struct xe_late_bind_fw late_bind_fw[MAX_ID];

	/** @late_bind.disable to block late binding reload during pm resume flow*/
	bool disable;
};

#endif
