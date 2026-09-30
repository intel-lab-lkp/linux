// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments System Control Interface Protocol Driver
 *
 * Copyright (C) 2015-2026 Texas Instruments Incorporated - https://www.ti.com/
 *	Nishanth Menon
 */

#include <linux/errno.h>
#include <linux/soc/ti/ti_sci_protocol.h>

#include "ti_sci.h"

/**
 * ti_sci_msg_cmd_lpm_wake_reason() - Get the wakeup source from LPM
 * @handle:		Pointer to TI SCI handle
 * @source:		The wakeup source that woke the SoC from LPM
 * @timestamp:		Timestamp of the wakeup event
 * @pin:		The pin that has triggered wake up
 * @mode:		The last entered low power mode
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_msg_cmd_lpm_wake_reason(const struct ti_sci_handle *handle,
					  u32 *source, u64 *timestamp, u8 *pin, u8 *mode)
{
	struct ti_sci_msg_hdr req;
	struct ti_sci_msg_resp_lpm_wake_reason resp;
	struct device *dev;
	int ret = 0;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	dev = handle_to_ti_sci_info(handle)->dev;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_LPM_WAKE_REASON,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret) {
		dev_err(dev, "Failed to get wake reason\n");
		return ret;
	}

	if (source)
		*source = resp.wake_source;
	if (timestamp)
		*timestamp = resp.wake_timestamp;
	if (pin)
		*pin = resp.wake_pin;
	if (mode)
		*mode = resp.mode;

	return 0;
}

/**
 * ti_sci_cmd_set_device_constraint() - Set LPM constraint on behalf of a device
 * @handle:	pointer to TI SCI handle
 * @id:	Device identifier
 * @state:	The desired state of device constraint: set or clear
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_set_device_constraint(const struct ti_sci_handle *handle,
					    u32 id, u8 state)
{
	struct ti_sci_msg_req_lpm_set_device_constraint req;
	struct ti_sci_msg_hdr resp;
	struct device *dev;
	int ret = 0;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	dev = handle_to_ti_sci_info(handle)->dev;

	req.id = id;
	req.state = state;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_LPM_SET_DEVICE_CONSTRAINT,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret) {
		dev_err(dev, "Failed to set device constraint\n");
		return ret;
	}

	return 0;
}

/**
 * ti_sci_cmd_set_latency_constraint() - Set LPM resume latency constraint
 * @handle:	pointer to TI SCI handle
 * @latency:	maximum acceptable latency (in ms) to wake up from LPM
 * @state:	The desired state of latency constraint: set or clear
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_set_latency_constraint(const struct ti_sci_handle *handle,
					     u16 latency, u8 state)
{
	struct ti_sci_msg_req_lpm_set_latency_constraint req;
	struct ti_sci_msg_hdr resp;
	struct device *dev;
	int ret = 0;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	dev = handle_to_ti_sci_info(handle)->dev;

	req.latency = latency;
	req.state = state;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_LPM_SET_LATENCY_CONSTRAINT,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret) {
		dev_err(dev, "Failed to set device constraint\n");
		return ret;
	}

	return 0;
}

/*
 * ti_sci_setup_pm_ops() - Setup the pm operations structures
 * @pmops:	pointer to TISCI pm ops structure
 */
void ti_sci_setup_pm_ops(struct ti_sci_pm_ops *pmops)
{
	pmops->lpm_wake_reason = ti_sci_msg_cmd_lpm_wake_reason;
	pmops->set_device_constraint = ti_sci_cmd_set_device_constraint;
	pmops->set_latency_constraint = ti_sci_cmd_set_latency_constraint;
}
