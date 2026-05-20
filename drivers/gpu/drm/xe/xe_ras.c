// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "xe_bo.h"
#include "xe_assert.h"
#include "xe_device_types.h"
#include "xe_device.h"
#include "xe_printk.h"
#include "xe_ras.h"
#include "xe_ras_types.h"
#include "xe_sysctrl.h"
#include "xe_sysctrl_event_types.h"
#include "xe_sysctrl_mailbox.h"
#include "xe_sysctrl_mailbox_types.h"

#define CORE_COMPUTE_UNCORR_TYPE	GENMASK(26, 25)
#define  GLOBAL_UNCORR_ERROR		2

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

static const int ras_status_to_errno_map[] = {
	[XE_RAS_STATUS_SUCCESS]			= 0,
	[XE_RAS_STATUS_INVALID_PARAM]		= -EINVAL,
	[XE_RAS_STATUS_OP_NOT_SUPPORTED]	= -EOPNOTSUPP,
	[XE_RAS_STATUS_TIMEOUT]			= -ETIMEDOUT,
	[XE_RAS_STATUS_HARDWARE_FAILURE]	= -EIO,
	[XE_RAS_STATUS_INSUFFICIENT_RESOURCES]	= -ENAVAIL,
	[XE_RAS_STATUS_UNKNOWN_ERROR]		= -ENODATA
};

static_assert(ARRAY_SIZE(ras_status_to_errno_map) == XE_RAS_STATUS_UNKNOWN_ERROR + 1);

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

static int ras_status_to_errno(enum xe_ras_response_status status)
{
	if (status > XE_RAS_STATUS_UNKNOWN_ERROR)
		status = XE_RAS_STATUS_UNKNOWN_ERROR;

	return ras_status_to_errno_map[status];
}

static void prepare_ras_command(struct xe_sysctrl_mailbox_command *command,
				u32 cmd_mask, void *request, size_t request_len,
				void *response, size_t response_len)
{
	struct xe_sysctrl_app_msg_hdr hdr = {0};

	hdr.data = FIELD_PREP(APP_HDR_GROUP_ID_MASK, XE_SYSCTRL_GROUP_GFSP) |
		   FIELD_PREP(APP_HDR_COMMAND_MASK, cmd_mask);

	command->header = hdr;
	command->data_in = request;
	command->data_in_len = request_len;
	command->data_out = response;
	command->data_out_len = response_len;
}

static int send_page_offline(struct xe_device *xe, enum xe_ras_page_action action, u64 page_address)
{
	struct xe_sysctrl_mailbox_command command = {0};
	struct xe_ras_page_offline_request request = {0};
	struct xe_ras_page_offline_response response = {0};
	size_t rlen;
	int ret;

	if (!xe->info.has_sysctrl)
		return 0;

	if (action >= XE_RAS_PAGE_ACTION_MAX) {
		xe_err(xe, "[RAS]: Invalid page offline action %d\n", action);
		return -EINVAL;
	}

	request.page_address = page_address;
	request.action = action;

	prepare_ras_command(&command, XE_SYSCTRL_CMD_PAGE_OFFLINE, &request,
			    sizeof(request), &response, sizeof(response));

	ret = xe_sysctrl_send_command(&xe->sc, &command, &rlen);
	if (ret) {
		xe_err(xe, "sysctrl: failed to send page offline command %d\n", ret);
		return ret;
	}

	if (rlen != sizeof(response)) {
		xe_err(xe, "sysctrl: unexpected page offline response length %zu (expected %zu)\n",
		       rlen, sizeof(response));
		return -EINVAL;
	}

	ret = ras_status_to_errno(response.status);
	if (ret)
		xe_err(xe, "sysctrl: page offline command failed with status %d\n",
		       response.status);

	return ret;
}

static int handle_page_offline(struct xe_device *xe, u64 page_address, bool send_offline_cmd)
{
	enum xe_ras_page_action action;
	int ret;

	if (!IS_ALIGNED(page_address, XE_PAGE_SIZE)) {
		xe_err(xe, "sysctrl: Unaligned page address: 0x%llx\n", page_address);
		return -EINVAL;
	}

	/*
	 * TODO: Call function to handle address fault
	 * ret = xe_ttm_vram_handle_addr_fault(xe, page_address);
	 */

	/*
	 * Handle return code from address fault handling function:
	 *  0: Address is valid and can be offlined
	 * -EIO: Address belongs to a critical BO that cannot be offlined
	 * -ENXIO: Invalid address
	 * -EOPNOTSUPP: Address is valid and can be offlined but user policy is not to offline
	 *
	 * For any other non-zero error code, skip offlining.
	 */

	switch (ret) {
	case 0:
		action = XE_RAS_PAGE_ACTION_OFFLINE;
		break;
	/* User policy set to decline page offlining */
	case -EOPNOTSUPP:
		action = XE_RAS_PAGE_ACTION_DECLINE;
		break;
	case -EIO:
		xe_err(xe, "[RAS]: Page address belongs to critical BO: 0x%llx\n",
		       page_address);
		return ret;
	default:
		xe_err(xe, "[RAS]: Failed to handle address fault 0x%llx: %d\n",
		       page_address, ret);
		return 0;
	}

	if (send_offline_cmd) {
		ret = send_page_offline(xe, action, page_address);
		if (ret)
			xe_err(xe, "sysctrl: Failed to offline page for address 0x%llx: %d\n",
			       page_address, ret);
		return ret;
	}

	return 0;
}

static enum xe_ras_recovery_action handle_core_compute_errors(struct xe_device *xe,
							      struct xe_ras_error_array *arr)
{
	struct xe_ras_compute_error *error_info = (struct xe_ras_compute_error *)arr->error_details;
	u8 uncorr_type;

	uncorr_type = FIELD_GET(CORE_COMPUTE_UNCORR_TYPE, error_info->error_log_header);

	/* Request a reset if error is global */
	if (uncorr_type == GLOBAL_UNCORR_ERROR)
		return XE_RAS_RECOVERY_ACTION_RESET;

	/* Local errors are recovered using an engine reset by GuC */
	return XE_RAS_RECOVERY_ACTION_RECOVERED;
}

#ifdef CONFIG_PCIEAER
static bool pcie_slot_is_hotplug_capable(struct pci_dev *usp)
{
	struct pci_dev *root_port = pci_upstream_bridge(usp);
	u32 sltcap;
	u16 flags;

	if (!root_port)
		return false;

	/*
	 * Per PCIe spec, the Slot Capabilities register contents are
	 * undefined unless the Slot Implemented bit in the PCI Express
	 * Capabilities register is set. Check it before reading SLTCAP.
	 */
	if (pcie_capability_read_word(root_port, PCI_EXP_FLAGS, &flags))
		return false;

	if (!(flags & PCI_EXP_FLAGS_SLOT))
		return false;

	if (pcie_capability_read_dword(root_port, PCI_EXP_SLTCAP, &sltcap))
		return false;

	return (sltcap & (PCI_EXP_SLTCAP_HPC | PCI_EXP_SLTCAP_PCP)) ==
		(PCI_EXP_SLTCAP_HPC | PCI_EXP_SLTCAP_PCP);
}

static void pcie_suppress_surprise_link_down(struct pci_dev *usp)
{
	u32 aer_uncorr_mask;
	u16 aer_cap;

	aer_cap = usp->aer_cap;
	if (!aer_cap) {
		dev_dbg(&usp->dev,
			"AER capability not present; cannot mask Surprise Link Down for cold reset\n");
		return;
	}

	pci_read_config_dword(usp, aer_cap + PCI_ERR_UNCOR_MASK, &aer_uncorr_mask);
	aer_uncorr_mask |= PCI_ERR_UNC_SURPDN;
	pci_write_config_dword(usp, aer_cap + PCI_ERR_UNCOR_MASK, aer_uncorr_mask);
	dev_dbg(&usp->dev, "Non-hotplug slot: Surprise Link Down masked for cold reset\n");
}
#endif /* CONFIG_PCIEAER */

static void punit_error_handler(struct xe_device *xe)
{
#ifdef CONFIG_PCIEAER
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct pci_dev *vsp, *usp;

	/*
	 * Device Hierarchy:
	 *
	 * Root Port --> Upstream Switch Port (USP) --> Virtual Switch Port (VSP) --> SGunit
	 *
	 * Cold reset power-cycles the slot, dropping the PCIe link. On a non-hotplug
	 * slot this triggers a spurious Surprise Link Down AER event on the USP.
	 * Suppress it if the slot is not hotplug capable.
	 */
	vsp = pci_upstream_bridge(pdev);
	usp = vsp ? pci_upstream_bridge(vsp) : NULL;

	if (usp && !pcie_slot_is_hotplug_capable(usp))
		pcie_suppress_surprise_link_down(usp);
#endif
	xe_device_set_wedged_method(xe, DRM_WEDGE_RECOVERY_COLD_RESET);
	xe_device_declare_wedged(xe);
}

static enum xe_ras_recovery_action handle_soc_internal_errors(struct xe_device *xe,
							      struct xe_ras_error_array *arr)
{
	struct xe_ras_soc_error *error_info = (struct xe_ras_soc_error *)arr->error_details;
	struct xe_ras_soc_error_source *source = &error_info->error_source;
	struct xe_ras_error_class *error_class = &arr->error_class;
	u8 tile_id = error_class->product.unit.tile;
	struct xe_tile *tile;

	if (tile_id >= xe->info.tile_count) {
		xe_err(xe, "sysctrl: SOC internal error reported from invalid tile %u\n", tile_id);
		return XE_RAS_RECOVERY_ACTION_RESET;
	}

	tile = &xe->tiles[tile_id];

	if (source->csc) {
		struct xe_ras_csc_error *csc_error =
			(struct xe_ras_csc_error *)error_info->additional_details;

		/*
		 * CSC uncorrectable errors are classified as hardware errors and firmware errors.
		 * CSC firmware errors are critical errors that can be recovered only by firmware
		 * update via SPI driver. On a CSC firmware error, PCODE enables FDO mode and sets
		 * the bit in the capability register. On receiving this error, the driver enables
		 * runtime survivability mode which notifies userspace that a firmware update
		 * is required.
		 */
		if (csc_error->hec_uncorr_fw_err_dw0) {
			xe_err(xe, "[RAS]: CSC %s detected: 0x%x\n",
			       sev_to_str(error_class->common.severity),
			       csc_error->hec_uncorr_fw_err_dw0);
			schedule_work(&tile->csc_hw_error_work);
			return XE_RAS_RECOVERY_ACTION_DISCONNECT;
		}
	} else if (source->ieh) {
		struct xe_ras_ieh_error *ieh_error =
			(struct xe_ras_ieh_error *)error_info->additional_details;

		if (ieh_error->global_error_status & XE_RAS_SOC_IEH_PUNIT) {
			xe_err(xe, "[RAS]: PUNIT %s detected: 0x%x\n",
			       sev_to_str(error_class->common.severity),
			       ieh_error->global_error_status);
			punit_error_handler(xe);
			return XE_RAS_RECOVERY_ACTION_DISCONNECT;
		}
	}

	/* For other SOC internal errors, request a reset as recovery mechanism */
	return XE_RAS_RECOVERY_ACTION_RESET;
}

static enum xe_ras_recovery_action handle_device_memory_errors(struct xe_device *xe,
							       struct xe_ras_error_array *arr)
{
	struct xe_ras_memory_error *error_info = (struct xe_ras_memory_error *)arr->error_details;
	int ret;

	if (error_info->category & XE_RAS_MEMORY_ECC) {
		xe_err(xe, "[RAS]: double-bit ECC error detected at sw address 0x%llx\n",
		       error_info->sw_address);
		ret = handle_page_offline(xe, error_info->sw_address, true);
		if (!ret)
			return XE_RAS_RECOVERY_ACTION_RECOVERED;
	}

	/* Request a reset for other device memory errors and if page offlining failed */
	return XE_RAS_RECOVERY_ACTION_RESET;
}

static void get_queued_pages(struct xe_device *xe)
{
	struct xe_sysctrl_mailbox_command command = {0};
	struct xe_ras_page_offline_queue response = {0};
	u32 count = 0;
	size_t rlen;
	int ret, i;

	/* Supported only on platforms with system controller */
	if (!xe->info.has_sysctrl)
		return;

	prepare_ras_command(&command, XE_SYSCTRL_CMD_GET_OFFLINE_QUEUE, NULL, 0,
			    &response, sizeof(response));

	do {
		memset(&response, 0, sizeof(response));

		ret = xe_sysctrl_send_command(&xe->sc, &command, &rlen);
		if (ret) {
			xe_err(xe, "sysctrl: failed to get page offline queue %d\n", ret);
			return;
		}

		if (rlen != sizeof(response)) {
			xe_err(xe, "sysctrl: unexpected page offline queue response length %zu (expected %zu)\n",
			       rlen, sizeof(response));
			return;
		}

		for (i = 0; i < response.pages_returned && i < XE_RAS_NUM_PAGES; i++)
			handle_page_offline(xe, response.page_addresses[i], true);

		count += response.pages_returned;
		if (count > response.total_pages) {
			xe_err(xe, "sysctrl: Pages returned from queue exceed total pages %u, returned %u\n",
			       response.total_pages, count);
			return;
		}
	} while (response.additional_data);
}

static void get_offlined_list(struct xe_device *xe)
{
	struct xe_sysctrl_mailbox_command command = {0};
	struct xe_ras_page_offline_list response = {0};
	u32 count = 0;
	size_t rlen;
	int ret, i;

	/* Supported only on platforms with system controller */
	if (!xe->info.has_sysctrl)
		return;

	prepare_ras_command(&command, XE_SYSCTRL_CMD_GET_OFFLINE_LIST, NULL, 0,
			    &response, sizeof(response));

	do {
		memset(&response, 0, sizeof(response));

		ret = xe_sysctrl_send_command(&xe->sc, &command, &rlen);
		if (ret) {
			xe_err(xe, "sysctrl: failed to get page offline list %d\n", ret);
			return;
		}

		if (rlen != sizeof(response)) {
			xe_err(xe, "sysctrl: unexpected page offline list response length %zu (expected %zu)\n",
			       rlen, sizeof(response));
			return;
		}

		for (i = 0; i < response.pages_returned && i < XE_RAS_NUM_PAGES; i++)
			handle_page_offline(xe, response.page_addresses[i], false);

		count += response.pages_returned;
		if (count > response.total_pages) {
			xe_err(xe, "sysctrl: Pages returned from list exceed total pages %u, returned %u\n",
			       response.total_pages, count);
			return;
		}
	} while (response.additional_data);
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

/**
 * xe_ras_process_errors() - Process and contain hardware errors
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
	struct xe_ras_get_soc_error response;
	enum xe_ras_recovery_action final_action;
	u32 count = XE_SYSCTRL_FLOOD;
	size_t rlen;
	int ret;

	if (!xe->info.has_sysctrl)
		return XE_RAS_RECOVERY_ACTION_RESET;

	/* Default action */
	final_action = XE_RAS_RECOVERY_ACTION_RECOVERED;

	prepare_ras_command(&command, XE_SYSCTRL_CMD_GET_SOC_ERROR, NULL, 0,
			    &response, sizeof(response));

	do {
		memset(&response, 0, sizeof(response));

		ret = xe_sysctrl_send_command(&xe->sc, &command, &rlen);
		if (ret) {
			xe_err(xe, "sysctrl: failed to get soc error %d\n", ret);
			goto err;
		}

		if (rlen != sizeof(response)) {
			xe_err(xe, "sysctrl: unexpected get soc error response length %zu (expected %zu)\n",
			       rlen, sizeof(response));
			goto err;
		}

		/* Report if number of errors exceeds the maximum errors supported */
		if (response.num_errors > XE_RAS_NUM_ERROR_ARR)
			xe_err(xe, "sysctrl: number of errors received %d out of bound (%d)\n",
			       response.num_errors, XE_RAS_NUM_ERROR_ARR);

		for (int i = 0; i < response.num_errors && i < XE_RAS_NUM_ERROR_ARR; i++) {
			struct xe_ras_error_array *arr = &response.error_arr[i];
			enum xe_ras_recovery_action action;
			struct xe_ras_error_class error_class;
			u8 component, severity;

			error_class = arr->error_class;
			component = error_class.common.component;
			severity = error_class.common.severity;

			xe_err(xe, "[RAS]: %s %s detected\n", comp_to_str(component),
			       sev_to_str(severity));

			switch (component) {
			case XE_RAS_COMP_CORE_COMPUTE:
				action = handle_core_compute_errors(xe, arr);
				break;
			case XE_RAS_COMP_SOC_INTERNAL:
				action = handle_soc_internal_errors(xe, arr);
				break;
			case XE_RAS_COMP_DEVICE_MEMORY:
				action = handle_device_memory_errors(xe, arr);
				break;
			default:
				/* For any other component, reset */
				action = XE_RAS_RECOVERY_ACTION_RESET;
				break;
			}

			/* Process and log all errors and then trigger highest recovery action */
			if (action > final_action)
				final_action = action;
		}

		/* Treat flooding as an system controller error */
		if (!--count) {
			xe_err(xe, "[RAS]: sysctrl: get soc error response flooding\n");
			return XE_RAS_RECOVERY_ACTION_RESET;
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

	/*
	 * Device Hierarchy:
	 *
	 * Upstream Switch Port (USP)--> Virtual Switch Port (VSP)--> SGunit (GPU endpoint)
	 */
	vsp = pci_upstream_bridge(pdev);
	if (!vsp)
		return;

	usp = pci_upstream_bridge(vsp);
	if (!usp)
		return;

	aer_cap = usp->aer_cap;

	if (!aer_cap) {
		dev_info(&usp->dev, "USP doesn't support AER capability\n");
		return;
	}

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
	dev_dbg(&usp->dev, "Uncorrectable Internal Errors downgraded and unmasked\n");
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
	if (!xe->info.has_sysctrl || IS_SRIOV_VF(xe))
		return;

#ifdef CONFIG_PCIEAER
	aer_unmask_and_downgrade_internal_error(xe);
#endif

	get_queued_pages(xe);
	get_offlined_list(xe);

	/*
	 * On init, process and log any errors detected by firmware before driver load.
	 * Critical errors such as Punit, CSC are reported through PCode init failure,
	 * causing the driver to enter survivability mode.
	 */
	xe_ras_process_errors(xe);
}
