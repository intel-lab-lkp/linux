// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "xe_assert.h"
#include "xe_device_types.h"
#include "xe_printk.h"
#include "xe_ras.h"
#include "xe_ras_types.h"
#include "xe_survivability_mode.h"
#include "xe_sysctrl_mailbox.h"
#include "xe_sysctrl_mailbox_types.h"

#define COMPUTE_ERROR_SEVERITY_MASK		GENMASK(26, 25)
#define GLOBAL_UNCORR_ERROR			2
/* Modify as needed */
#define XE_SYSCTRL_ERROR_FLOOD			16

/* Severity classification of detected errors */
enum xe_ras_severity {
	XE_RAS_SEVERITY_NOT_SUPPORTED = 0,
	XE_RAS_SEVERITY_CORRECTABLE,
	XE_RAS_SEVERITY_UNCORRECTABLE,
	XE_RAS_SEVERITY_INFORMATIONAL,
	XE_RAS_SEVERITY_MAX
};

/* major IP blocks where errors can originate */
enum xe_ras_component {
	XE_RAS_COMPONENT_NOT_SUPPORTED = 0,
	XE_RAS_COMPONENT_DEVICE_MEMORY,
	XE_RAS_COMPONENT_CORE_COMPUTE,
	XE_RAS_COMPONENT_RESERVED,
	XE_RAS_COMPONENT_PCIE,
	XE_RAS_COMPONENT_FABRIC,
	XE_RAS_COMPONENT_SOC_INTERNAL,
	XE_RAS_COMPONENT_MAX
};

static const char * const xe_ras_severities[] = {
	[XE_RAS_SEVERITY_NOT_SUPPORTED]		= "Not Supported",
	[XE_RAS_SEVERITY_CORRECTABLE]		= "Correctable",
	[XE_RAS_SEVERITY_UNCORRECTABLE]		= "Uncorrectable",
	[XE_RAS_SEVERITY_INFORMATIONAL]		= "Informational",
};

static_assert(ARRAY_SIZE(xe_ras_severities) == XE_RAS_SEVERITY_MAX);

static const char * const xe_ras_components[] = {
	[XE_RAS_COMPONENT_NOT_SUPPORTED]	= "Not Supported",
	[XE_RAS_COMPONENT_DEVICE_MEMORY]	= "Device Memory",
	[XE_RAS_COMPONENT_CORE_COMPUTE]		= "Core Compute",
	[XE_RAS_COMPONENT_RESERVED]		= "Reserved",
	[XE_RAS_COMPONENT_PCIE]			= "PCIe",
	[XE_RAS_COMPONENT_FABRIC]		= "Fabric",
	[XE_RAS_COMPONENT_SOC_INTERNAL]		= "SoC Internal",
};

static_assert(ARRAY_SIZE(xe_ras_components) == XE_RAS_COMPONENT_MAX);

static inline const char *severity_to_str(struct xe_device *xe, u32 severity)
{
	xe_assert(xe, severity < XE_RAS_SEVERITY_MAX);

	return severity < XE_RAS_SEVERITY_MAX ? xe_ras_severities[severity] : "Unknown";
}

static inline const char *comp_to_str(struct xe_device *xe, u32 comp)
{
	xe_assert(xe, comp < XE_RAS_COMPONENT_MAX);

	return comp < XE_RAS_COMPONENT_MAX ? xe_ras_components[comp] : "Unknown";
}

static enum xe_ras_recovery_action handle_compute_errors(struct xe_device *xe,
							 struct xe_ras_error_array *arr)
{
	struct xe_ras_compute_error *error_info = (struct xe_ras_compute_error *)arr->error_details;
	struct xe_ras_error_common common = arr->error_class.common;
	u8 uncorr_type;

	uncorr_type = FIELD_GET(COMPUTE_ERROR_SEVERITY_MASK, error_info->error_log_header);

	xe_err(xe, "[RAS]: %s %s Error detected", severity_to_str(xe, common.severity),
	       comp_to_str(xe, common.component));

	/* Request a RESET if error is global */
	if (uncorr_type == GLOBAL_UNCORR_ERROR)
		return XE_RAS_RECOVERY_ACTION_RESET;

	/* Local errors are recovered using a engine reset by GuC */
	return XE_RAS_RECOVERY_ACTION_RECOVERED;
}

static enum xe_ras_recovery_action handle_soc_internal_errors(struct xe_device *xe,
							      struct xe_ras_error_array *arr)
{
	struct xe_ras_soc_error *error_info =
		(struct xe_ras_soc_error *)arr->error_details;
	struct xe_ras_soc_error_source source = error_info->error_source;
	struct xe_ras_error_common common = arr->error_class.common;

	xe_err(xe, "[RAS]: %s %s Error detected", severity_to_str(xe, common.severity),
	       comp_to_str(xe, common.component));

	if (source.csc) {
		struct xe_ras_csc_error *csc_error =
			(struct xe_ras_csc_error *)error_info->additional_details;

		/*
		 * CSC uncorrectable errors are classified as hardware errors and firmware errors.
		 * CSC firmware errors are critical errors that can be recovered only by firmware
		 * update via SPI driver. PCODE enables FDO mode and sets the bit in the capability
		 * register. On receiving this error, the driver enables runtime survivability mode
		 * which notifies userspace that a firmware update is required.
		 */
		if (csc_error->hec_uncorr_fw_err_dw0) {
			xe_err(xe, "[RAS]: CSC %s error detected: 0x%x\n",
			       severity_to_str(xe, common.severity),
			       csc_error->hec_uncorr_fw_err_dw0);
			xe_survivability_mode_runtime_enable(xe);
			return XE_RAS_RECOVERY_ACTION_DISCONNECT;
		}
	}

	if (source.soc) {
		struct xe_ras_ieh_error *ieh_error =
			(struct xe_ras_ieh_error *)error_info->additional_details;

		if (ieh_error->global_error_status & XE_RAS_IEH_PUNIT_ERROR) {
			xe_err(xe, "[RAS]: PUNIT %s error detected: 0x%x\n",
			       severity_to_str(xe, common.severity),
			       ieh_error->global_error_status);
			/** TODO: Add PUNIT error handling */
			return XE_RAS_RECOVERY_ACTION_DISCONNECT;
		}
	}

	/* For other SOC internal errors, request a reset as recovery mechanism */
	return XE_RAS_RECOVERY_ACTION_RESET;
}

static void prepare_sysctrl_command(struct xe_sysctrl_mailbox_command *command,
				    u32 cmd_mask, void *request, size_t request_len,
				    void *response, size_t response_len)
{
	struct xe_sysctrl_app_msg_hdr hdr = {0};
	u32 req_hdr;

	req_hdr = FIELD_PREP(APP_HDR_GROUP_ID_MASK, XE_SYSCTRL_GROUP_GFSP) |
		  FIELD_PREP(APP_HDR_COMMAND_MASK, cmd_mask);

	hdr.data = req_hdr;
	command->header = hdr;
	command->data_in = request;
	command->data_in_len = request_len;
	command->data_out = response;
	command->data_out_len = response_len;
}

/**
 * xe_ras_process_errors - Process and contain hardware errors
 * @xe: xe device instance
 *
 * Get error details from system controller and return recovery
 * method. Called only from PCI error handling.
 *
 * Returns: recovery action to be taken
 */
enum xe_ras_recovery_action xe_ras_process_errors(struct xe_device *xe)
{
	struct xe_sysctrl_mailbox_command command = {0};
	struct xe_ras_get_error_response response;
	enum xe_ras_recovery_action final_action;
	u32 count = 0;
	size_t rlen;
	int ret;

	/* Default action */
	final_action = XE_RAS_RECOVERY_ACTION_RECOVERED;

	if (!xe->info.has_sysctrl)
		return XE_RAS_RECOVERY_ACTION_RESET;

	prepare_sysctrl_command(&command, XE_SYSCTRL_CMD_GET_SOC_ERROR, NULL, 0,
				&response, sizeof(response));

	do {
		memset(&response, 0, sizeof(response));
		rlen = 0;

		ret = xe_sysctrl_send_command(&xe->sc, &command, &rlen);
		if (ret) {
			xe_err(xe, "[RAS]: Sysctrl error ret %d\n", ret);
			goto err;
		}

		if (rlen != sizeof(response)) {
			xe_err(xe, "[RAS]: Sysctrl response size mismatch. Expected %zu, got %zu\n",
			       sizeof(response), rlen);
			goto err;
		}

		for (int i = 0; i < response.num_errors &&  i < XE_RAS_NUM_ERROR_ARR; i++) {
			struct xe_ras_error_array arr = response.error_arr[i];
			enum xe_ras_recovery_action action;
			struct xe_ras_error_class error_class;
			u8 component;

			error_class = arr.error_class;
			component = error_class.common.component;

			switch (component) {
			case XE_RAS_COMPONENT_CORE_COMPUTE:
				action = handle_compute_errors(xe, &arr);
				break;
			case XE_RAS_COMPONENT_SOC_INTERNAL:
				action = handle_soc_internal_errors(xe, &arr);
				break;
			default:
				xe_err(xe, "[RAS]: Unknown error component %u\n", component);
				action = XE_RAS_RECOVERY_ACTION_RESET;
				break;
			}

			/*
			 * Retain the highest severity action. Process and log all errors
			 * and then take appropriate recovery action.
			 */
			if (action > final_action)
				final_action = action;
		}

		/* Break if system controller floods responses */
		if (++count > XE_SYSCTRL_ERROR_FLOOD) {
			xe_err(xe, "[RAS]: Sysctrl response flooding\n");
			break;
		}

	} while (response.additional_errors);

	return final_action;

err:
	return XE_RAS_RECOVERY_ACTION_RESET;
}

#ifdef CONFIG_PCIEAER
static void aer_unmask_and_downgrade_internal_error(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct pci_dev *vsp, *usp;
	u32 aer_uncorr_mask, aer_uncorr_sev, aer_uncorr_status;
	u16 aer_cap;

	 /* Gfx Device Hierarchy: USP-->VSP-->SGunit */
	vsp = pci_upstream_bridge(pdev);
	if (!vsp)
		return;

	usp = pci_upstream_bridge(vsp);
	if (!usp)
		return;

	aer_cap = usp->aer_cap;

	if (!aer_cap)
		return;

	/*
	 * Clear any stale Uncorrectable Internal Error Status event in Uncorrectable Error
	 * Status Register.
	 */
	pci_read_config_dword(usp, aer_cap + PCI_ERR_UNCOR_STATUS, &aer_uncorr_status);
	if (aer_uncorr_status & PCI_ERR_UNC_INTN)
		pci_write_config_dword(usp, aer_cap + PCI_ERR_UNCOR_STATUS, PCI_ERR_UNC_INTN);

	/*
	 * All errors are steered to USP which is a PCIe AER Compliant device.
	 * Downgrade all the errors to non-fatal to prevent PCIe bus driver
	 * from triggering a Secondary Bus Reset (SBR). This allows error
	 * detection, containment and recovery in the driver.
	 *
	 * The Uncorrectable Error Severity Register has the 'Uncorrectable
	 * Internal Error Severity' set to fatal by default. Set this to
	 * non-fatal and unmask the error.
	 */

	/* Initialize Uncorrectable Error Severity Register */
	pci_read_config_dword(usp, aer_cap + PCI_ERR_UNCOR_SEVER, &aer_uncorr_sev);
	aer_uncorr_sev &= ~PCI_ERR_UNC_INTN;
	pci_write_config_dword(usp, aer_cap + PCI_ERR_UNCOR_SEVER, aer_uncorr_sev);

	/* Initialize Uncorrectable Error Mask Register */
	pci_read_config_dword(usp, aer_cap + PCI_ERR_UNCOR_MASK, &aer_uncorr_mask);
	aer_uncorr_mask &= ~PCI_ERR_UNC_INTN;
	pci_write_config_dword(usp, aer_cap + PCI_ERR_UNCOR_MASK, aer_uncorr_mask);

	pci_save_state(usp);
}
#endif

/**
 * xe_ras_init - Initialize Xe RAS
 * @xe: xe device instance
 *
 * Initialize Xe RAS
 */
void xe_ras_init(struct xe_device *xe)
{
	if (!xe->info.has_sysctrl)
		return;

#ifdef CONFIG_PCIEAER
	aer_unmask_and_downgrade_internal_error(xe);
#endif
}
