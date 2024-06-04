// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2024 Intel Corporation. */

#include "iavf.h"

/**
 * iavf_ptp_cap_supported - Check if a PTP capability is supported
 * @adapter: private adapter structure
 * @cap: the capability bitmask to check
 *
 * Return: true if every capability set in cap is also set in the enabled
 *         capabilities reported by the PF, false otherwise.
 */
bool iavf_ptp_cap_supported(struct iavf_adapter *adapter, u32 cap)
{
	if (!PTP_ALLOWED(adapter))
		return false;

	/* Only return true if every bit in cap is set in hw_caps.caps */
	return (adapter->ptp.hw_caps.caps & cap) == cap;
}

/**
 * iavf_ptp_register_clock - Register a new PTP for userspace
 * @adapter: private adapter structure
 *
 * Allocate and register a new PTP clock device if necessary.
 *
 * Return: 0 if success, error otherwise
 */
static int iavf_ptp_register_clock(struct iavf_adapter *adapter)
{
	struct ptp_clock_info *ptp_info = &adapter->ptp.info;
	struct device *dev = &adapter->pdev->dev;

	memset(ptp_info, 0, sizeof(*ptp_info));

	snprintf(ptp_info->name, sizeof(ptp_info->name) - 1, "%s-%s-clk",
		 dev_driver_string(dev),
		 dev_name(dev));
	ptp_info->owner = THIS_MODULE;

	adapter->ptp.clock = ptp_clock_register(ptp_info, dev);
	if (IS_ERR(adapter->ptp.clock))
		return PTR_ERR(adapter->ptp.clock);

	dev_info(&adapter->pdev->dev, "PTP clock %s registered\n",
		 adapter->ptp.info.name);
	return 0;
}

/**
 * iavf_ptp_init - Initialize PTP support if capability was negotiated
 * @adapter: private adapter structure
 *
 * Initialize PTP functionality, based on the capabilities that the PF has
 * enabled for this VF.
 */
void iavf_ptp_init(struct iavf_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	int err;

	if (WARN_ON(adapter->ptp.initialized)) {
		dev_err(dev, "PTP functionality was already initialized!\n");
		return;
	}

	if (!iavf_ptp_cap_supported(adapter, VIRTCHNL_1588_PTP_CAP_READ_PHC)) {
		dev_dbg(dev, "Device does not have PTP clock support\n");
		return;
	}

	err = iavf_ptp_register_clock(adapter);
	if (err) {
		dev_warn(dev, "Failed to register PTP clock device (%d)\n",
			 err);
		return;
	}

	adapter->ptp.initialized = true;
}

/**
 * iavf_ptp_release - Disable PTP support
 * @adapter: private adapter structure
 *
 * Release all PTP resources that were previously initialized.
 */
void iavf_ptp_release(struct iavf_adapter *adapter)
{
	adapter->ptp.initialized = false;

	if (!IS_ERR_OR_NULL(adapter->ptp.clock)) {
		dev_info(&adapter->pdev->dev, "removing PTP clock %s\n",
			 adapter->ptp.info.name);
		ptp_clock_unregister(adapter->ptp.clock);
		adapter->ptp.clock = NULL;
	}
}

/**
 * iavf_ptp_process_caps - Handle change in PTP capabilities
 * @adapter: private adapter structure
 *
 * Handle any state changes necessary due to change in PTP capabilities, such
 * as after a device reset or change in configuration from the PF.
 */
void iavf_ptp_process_caps(struct iavf_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;

	dev_dbg(dev, "PTP capabilities changed at runtime\n");

	/* Check if the device gained or lost necessary access to support the
	 * PTP hardware clock. If so, driver must respond appropriately by
	 * creating or destroying the PTP clock device.
	 */
	if (adapter->ptp.initialized &&
	    !iavf_ptp_cap_supported(adapter, VIRTCHNL_1588_PTP_CAP_READ_PHC))
		iavf_ptp_release(adapter);
	else if (!adapter->ptp.initialized &&
		 iavf_ptp_cap_supported(adapter, VIRTCHNL_1588_PTP_CAP_READ_PHC))
		iavf_ptp_init(adapter);
}
