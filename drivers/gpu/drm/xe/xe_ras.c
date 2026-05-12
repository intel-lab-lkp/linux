// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "xe_device.h"
#include "xe_drm_ras.h"
#include "xe_pm.h"
#include "xe_printk.h"
#include "xe_ras.h"
#include "xe_ras_types.h"
#include "xe_sysctrl.h"
#include "xe_sysctrl_event_types.h"
#include "xe_sysctrl_mailbox.h"
#include "xe_sysctrl_mailbox_types.h"

/* Severity of detected errors  */
enum xe_ras_severity {
	XE_RAS_SEV_NOT_SUPPORTED = 0,
	XE_RAS_SEV_CORRECTABLE,
	XE_RAS_SEV_UNCORRECTABLE,
	XE_RAS_SEV_INFORMATIONAL,
	XE_RAS_SEV_MAX
};

/* Major IP blocks/components where errors can originate */
enum xe_ras_component {
	XE_RAS_COMP_NOT_SUPPORTED = 0,
	XE_RAS_COMP_DEVICE_MEMORY,
	XE_RAS_COMP_CORE_COMPUTE,
	XE_RAS_COMP_RESERVED,
	XE_RAS_COMP_PCIE,
	XE_RAS_COMP_FABRIC,
	XE_RAS_COMP_SOC_INTERNAL,
	XE_RAS_COMP_MAX
};

/* RAS operation status codes */
enum xe_ras_status {
	XE_RAS_STATUS_SUCCESS = 0,
	XE_RAS_STATUS_INVALID_PARAM,
	XE_RAS_STATUS_NOT_SUPPORTED,
	XE_RAS_STATUS_TIMEOUT,
	XE_RAS_STATUS_HARDWARE_FAILURE,
	XE_RAS_STATUS_INSUFFICIENT_RESOURCES,
	XE_RAS_STATUS_MAX
};

static const char *const xe_ras_severities[] = {
	[XE_RAS_SEV_NOT_SUPPORTED]		= "Not Supported",
	[XE_RAS_SEV_CORRECTABLE]		= "Correctable Error",
	[XE_RAS_SEV_UNCORRECTABLE]		= "Uncorrectable Error",
	[XE_RAS_SEV_INFORMATIONAL]		= "Informational Error",
};
static_assert(ARRAY_SIZE(xe_ras_severities) == XE_RAS_SEV_MAX);

static const char *const xe_ras_components[] = {
	[XE_RAS_COMP_NOT_SUPPORTED]		= "Not Supported",
	[XE_RAS_COMP_DEVICE_MEMORY]		= "Device Memory",
	[XE_RAS_COMP_CORE_COMPUTE]		= "Core Compute",
	[XE_RAS_COMP_RESERVED]			= "Reserved",
	[XE_RAS_COMP_PCIE]			= "PCIe",
	[XE_RAS_COMP_FABRIC]			= "Fabric",
	[XE_RAS_COMP_SOC_INTERNAL]		= "SoC Internal",
};
static_assert(ARRAY_SIZE(xe_ras_components) == XE_RAS_COMP_MAX);

/* uAPI mapping */
static const int drm_to_xe_ras_components[] = {
	[DRM_XE_RAS_ERR_COMP_CORE_COMPUTE]	= XE_RAS_COMP_CORE_COMPUTE,
	[DRM_XE_RAS_ERR_COMP_SOC_INTERNAL]	= XE_RAS_COMP_SOC_INTERNAL,
	[DRM_XE_RAS_ERR_COMP_DEVICE_MEMORY]	= XE_RAS_COMP_DEVICE_MEMORY,
	[DRM_XE_RAS_ERR_COMP_PCIE]		= XE_RAS_COMP_PCIE,
	[DRM_XE_RAS_ERR_COMP_FABRIC]		= XE_RAS_COMP_FABRIC,
};
static_assert(ARRAY_SIZE(drm_to_xe_ras_components) == DRM_XE_RAS_ERR_COMP_MAX);

/* uAPI mapping */
static const int drm_to_xe_ras_severities[] = {
	[DRM_XE_RAS_ERR_SEV_CORRECTABLE]	= XE_RAS_SEV_CORRECTABLE,
	[DRM_XE_RAS_ERR_SEV_UNCORRECTABLE]	= XE_RAS_SEV_UNCORRECTABLE,
};
static_assert(ARRAY_SIZE(drm_to_xe_ras_severities) == DRM_XE_RAS_ERR_SEV_MAX);

static int ras_status_to_errno(u32 status)
{
	switch (status) {
	case XE_RAS_STATUS_INVALID_PARAM:
		return -EINVAL;
	case XE_RAS_STATUS_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case XE_RAS_STATUS_TIMEOUT:
		return -ETIMEDOUT;
	case XE_RAS_STATUS_HARDWARE_FAILURE:
		return -EIO;
	case XE_RAS_STATUS_INSUFFICIENT_RESOURCES:
		return -ENOSPC;
	default:
		return -EPROTO;
	}
};

static inline const char *sev_to_str(u8 severity)
{
	if (severity >= XE_RAS_SEV_MAX)
		severity = XE_RAS_SEV_NOT_SUPPORTED;

	return xe_ras_severities[severity];
}

static inline const char *comp_to_str(u8 component)
{
	if (component >= XE_RAS_COMP_MAX)
		component = XE_RAS_COMP_NOT_SUPPORTED;

	return xe_ras_components[component];
}

void xe_ras_counter_threshold_crossed(struct xe_device *xe,
				      struct xe_sysctrl_event_response *response)
{
	struct xe_ras_threshold_crossed *pending = (void *)&response->data;
	struct xe_ras_error_class *errors = pending->counters;
	u32 id, ncounters = pending->ncounters;

	BUILD_BUG_ON(sizeof(response->data) < sizeof(*pending));
	xe_device_assert_mem_access(xe);

	if (!ncounters || ncounters > XE_RAS_NUM_COUNTERS)
		xe_err(xe, "sysctrl: unexpected counter threshold crossed %u\n", ncounters);
	else
		xe_warn(xe, "[RAS]: counter threshold crossed, %u new errors\n", ncounters);

	for (id = 0; id < ncounters && id < XE_RAS_NUM_COUNTERS; id++) {
		u8 severity, component;

		severity = errors[id].common.severity;
		component = errors[id].common.component;

		xe_warn(xe, "[RAS]: %s %s detected\n",
			comp_to_str(component), sev_to_str(severity));
	}
}

int xe_ras_get_threshold(struct xe_device *xe, u32 severity, u32 component, u32 *threshold)
{
	struct xe_ras_get_threshold_response response = {};
	struct xe_ras_get_threshold_request request = {};
	struct xe_sysctrl_mailbox_command command = {};
	struct xe_ras_error_class counter = {};
	size_t len;
	int ret;

	counter.common.severity = drm_to_xe_ras_severities[severity];
	counter.common.component = drm_to_xe_ras_components[component];
	request.counter = counter;

	xe_sysctrl_populate_command(&command, &request, &response, sizeof(request),
				    sizeof(response), XE_SYSCTRL_GROUP_GFSP,
				    XE_SYSCTRL_CMD_GET_THRESHOLD);

	guard(xe_pm_runtime)(xe);
	ret = xe_sysctrl_send_command(&xe->sc, &command, &len);
	if (ret) {
		xe_err(xe, "sysctrl: failed to get threshold %d\n", ret);
		return ret;
	}

	if (len != sizeof(response)) {
		xe_err(xe, "sysctrl: unexpected get threshold response length %zu (expected %zu)\n",
		       len, sizeof(response));
		return -EIO;
	}

	counter = response.counter;
	*threshold = response.threshold;

	xe_dbg(xe, "[RAS]: get threshold %u for %s %s\n", response.threshold,
	       comp_to_str(counter.common.component), sev_to_str(counter.common.severity));
	return 0;
}

int xe_ras_set_threshold(struct xe_device *xe, u32 severity, u32 component, u32 threshold)
{
	struct xe_ras_set_threshold_response response = {};
	struct xe_ras_set_threshold_request request = {};
	struct xe_sysctrl_mailbox_command command = {};
	struct xe_ras_error_class counter = {};
	size_t len;
	int ret;

	counter.common.severity = drm_to_xe_ras_severities[severity];
	counter.common.component = drm_to_xe_ras_components[component];
	request.counter = counter;
	request.threshold = threshold;

	xe_sysctrl_populate_command(&command, &request, &response, sizeof(request),
				    sizeof(response), XE_SYSCTRL_GROUP_GFSP,
				    XE_SYSCTRL_CMD_SET_THRESHOLD);

	guard(xe_pm_runtime)(xe);
	ret = xe_sysctrl_send_command(&xe->sc, &command, &len);
	if (ret) {
		xe_err(xe, "sysctrl: failed to set threshold %d\n", ret);
		return ret;
	}

	if (len != sizeof(response)) {
		xe_err(xe, "sysctrl: unexpected set threshold response length %zu (expected %zu)\n",
		       len, sizeof(response));
		return -EIO;
	}

	if (response.status) {
		xe_err(xe, "sysctrl: set threshold operation failed %#x\n", response.status);
		return ras_status_to_errno(response.status);
	}

	counter = response.counter;

	xe_dbg(xe, "[RAS]: set threshold %u for %s %s\n", response.threshold,
	       comp_to_str(counter.common.component), sev_to_str(counter.common.severity));
	return 0;
}

/**
 * xe_ras_init - Initialize Xe RAS
 * @xe: xe device instance
 *
 * Initialize Xe RAS
 */
void xe_ras_init(struct xe_device *xe)
{
	int ret;

	if (!xe->info.has_drm_ras)
		return;

	ret = xe_drm_ras_init(xe);
	if (ret)
		drm_err(&xe->drm, "Failed to initialize xe_drm_ras %d\n", ret);
}

