// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

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
	[DRM_XE_RAS_ERR_COMP_FABRIC]		= XE_RAS_COMP_FABRIC
};
static_assert(ARRAY_SIZE(drm_to_xe_ras_components) == DRM_XE_RAS_ERR_COMP_MAX);

/* uAPI mapping */
static const int drm_to_xe_ras_severities[] = {
	[DRM_XE_RAS_ERR_SEV_CORRECTABLE]	= XE_RAS_SEV_CORRECTABLE,
	[DRM_XE_RAS_ERR_SEV_UNCORRECTABLE]	= XE_RAS_SEV_UNCORRECTABLE
};
static_assert(ARRAY_SIZE(drm_to_xe_ras_severities) == DRM_XE_RAS_ERR_SEV_MAX);

static inline const char *sev_to_str(u8 sev)
{
	if (sev >= XE_RAS_SEV_MAX)
		sev = XE_RAS_SEV_NOT_SUPPORTED;

	return xe_ras_severities[sev];
}

static inline const char *comp_to_str(u8 comp)
{
	if (comp >= XE_RAS_COMP_MAX)
		comp = XE_RAS_COMP_NOT_SUPPORTED;

	return xe_ras_components[comp];
}

void xe_ras_counter_threshold_crossed(struct xe_device *xe,
				      struct xe_sysctrl_event_response *response)
{
	struct xe_ras_threshold_crossed *pending = (void *)&response->data;
	struct xe_ras_error_class *errors = pending->counters;
	u32 counter_id, ncounters = pending->ncounters;

	if (!ncounters || ncounters > XE_RAS_NUM_COUNTERS) {
		xe_err(xe, "sysctrl: unexpected counter threshold crossed %u\n", ncounters);
		return;
	}

	BUILD_BUG_ON(sizeof(response->data) < sizeof(*pending));
	xe_warn(xe, "[RAS]: counter threshold crossed, %u new errors\n", ncounters);

	for (counter_id = 0; counter_id < ncounters; counter_id++) {
		u8 severity, component;

		severity = errors[counter_id].common.severity;
		component = errors[counter_id].common.component;

		xe_warn(xe, "[RAS]: %s %s detected\n",
			comp_to_str(component), sev_to_str(severity));
	}
}

static void ras_command_prepare(struct xe_sysctrl_mailbox_command *command,
				void *request, size_t request_len, void *response,
				size_t response_len, u8 hdr_cmd)
{
	struct xe_sysctrl_app_msg_hdr header = {};

	header.data = REG_FIELD_PREP(APP_HDR_GROUP_ID_MASK, XE_SYSCTRL_GROUP_GFSP) |
		      REG_FIELD_PREP(APP_HDR_COMMAND_MASK, hdr_cmd);

	command->header = header;
	command->data_in = request;
	command->data_in_len = request_len;
	command->data_out = response;
	command->data_out_len = response_len;
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

	ras_command_prepare(&command, &request, sizeof(request), &response,
			    sizeof(response), XE_SYSCTRL_CMD_GET_THRESHOLD);

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

	xe_dbg(xe, "[RAS]: Get threshold %u for %s %s\n", response.threshold,
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

	ras_command_prepare(&command, &request, sizeof(request), &response,
			    sizeof(response), XE_SYSCTRL_CMD_SET_THRESHOLD);

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
		return -EIO;
	}

	counter = response.counter;

	xe_dbg(xe, "[RAS]: Set threshold %u for %s %s\n", response.threshold,
	       comp_to_str(counter.common.component), sev_to_str(counter.common.severity));
	return 0;
}
