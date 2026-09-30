// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments System Control Interface Protocol Driver
 *
 * Copyright (C) 2015-2026 Texas Instruments Incorporated - https://www.ti.com/
 *	Nishanth Menon
 */

#include <linux/soc/ti/ti_sci_protocol.h>

#include "ti_sci.h"

/**
 * ti_sci_set_device_state() - Set device state helper
 * @handle:	pointer to TI SCI handle
 * @id:		Device identifier
 * @flags:	flags to setup for the device
 * @state:	State to move the device to
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_set_device_state(const struct ti_sci_handle *handle,
				   u32 id, u32 flags, u8 state)
{
	struct ti_sci_msg_req_set_device_state req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.id = id;
	req.state = state;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_DEVICE_STATE,
			     flags | TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_get_device_state() - Get device state helper
 * @handle:	Handle to the device
 * @id:		Device Identifier
 * @clcnt:	Pointer to Context Loss Count
 * @resets:	pointer to resets
 * @p_state:	pointer to p_state
 * @c_state:	pointer to c_state
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_get_device_state(const struct ti_sci_handle *handle,
				   u32 id,  u32 *clcnt,  u32 *resets,
				    u8 *p_state,  u8 *c_state)
{
	struct ti_sci_msg_req_get_device_state req;
	struct ti_sci_msg_resp_get_device_state resp;
	int ret = 0;

	if (!clcnt && !resets && !p_state && !c_state)
		return -EINVAL;

	req.id = id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_GET_DEVICE_STATE,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	if (clcnt)
		*clcnt = resp.context_loss_count;
	if (resets)
		*resets = resp.resets;
	if (p_state)
		*p_state = resp.programmed_state;
	if (c_state)
		*c_state = resp.current_state;

	return 0;
}

/**
 * ti_sci_cmd_get_device() - command to request for device managed by TISCI
 *			     that can be shared with other hosts.
 * @handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * @id:		Device Identifier
 *
 * Request for the device - NOTE: the client MUST maintain integrity of
 * usage count by balancing get_device with put_device. No refcounting is
 * managed by driver for that purpose.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_get_device(const struct ti_sci_handle *handle, u32 id)
{
	return ti_sci_set_device_state(handle, id, 0,
				       MSG_DEVICE_SW_STATE_ON);
}

/**
 * ti_sci_cmd_get_device_exclusive() - command to request for device managed by
 *				       TISCI that is exclusively owned by the
 *				       requesting host.
 * @handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * @id:		Device Identifier
 *
 * Request for the device - NOTE: the client MUST maintain integrity of
 * usage count by balancing get_device with put_device. No refcounting is
 * managed by driver for that purpose.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_get_device_exclusive(const struct ti_sci_handle *handle,
					   u32 id)
{
	return ti_sci_set_device_state(handle, id,
				       MSG_FLAG_DEVICE_EXCLUSIVE,
				       MSG_DEVICE_SW_STATE_ON);
}

/**
 * ti_sci_cmd_idle_device() - Command to idle a device managed by TISCI
 * @handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * @id:		Device Identifier
 *
 * Request for the device - NOTE: the client MUST maintain integrity of
 * usage count by balancing get_device with put_device. No refcounting is
 * managed by driver for that purpose.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_idle_device(const struct ti_sci_handle *handle, u32 id)
{
	return ti_sci_set_device_state(handle, id, 0,
				       MSG_DEVICE_SW_STATE_RETENTION);
}

/**
 * ti_sci_cmd_idle_device_exclusive() - Command to idle a device managed by
 *					TISCI that is exclusively owned by
 *					requesting host.
 * @handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * @id:		Device Identifier
 *
 * Request for the device - NOTE: the client MUST maintain integrity of
 * usage count by balancing get_device with put_device. No refcounting is
 * managed by driver for that purpose.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_idle_device_exclusive(const struct ti_sci_handle *handle,
					    u32 id)
{
	return ti_sci_set_device_state(handle, id,
				       MSG_FLAG_DEVICE_EXCLUSIVE,
				       MSG_DEVICE_SW_STATE_RETENTION);
}

/**
 * ti_sci_cmd_put_device() - command to release a device managed by TISCI
 * @handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * @id:		Device Identifier
 *
 * Request for the device - NOTE: the client MUST maintain integrity of
 * usage count by balancing get_device with put_device. No refcounting is
 * managed by driver for that purpose.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_put_device(const struct ti_sci_handle *handle, u32 id)
{
	return ti_sci_set_device_state(handle, id,
				       0, MSG_DEVICE_SW_STATE_AUTO_OFF);
}

/**
 * ti_sci_cmd_dev_is_valid() - Is the device valid
 * @handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * @id:		Device Identifier
 *
 * Return: 0 if all went fine and the device ID is valid, else return
 * appropriate error.
 */
static int ti_sci_cmd_dev_is_valid(const struct ti_sci_handle *handle, u32 id)
{
	u8 unused;

	/* check the device state which will also tell us if the ID is valid */
	return ti_sci_get_device_state(handle, id, NULL, NULL, NULL, &unused);
}

/**
 * ti_sci_cmd_dev_get_clcnt() - Get context loss counter
 * @handle:	Pointer to TISCI handle
 * @id:		Device Identifier
 * @count:	Pointer to Context Loss counter to populate
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_dev_get_clcnt(const struct ti_sci_handle *handle, u32 id,
				    u32 *count)
{
	return ti_sci_get_device_state(handle, id, count, NULL, NULL, NULL);
}

/**
 * ti_sci_cmd_dev_is_idle() - Check if the device is requested to be idle
 * @handle:	Pointer to TISCI handle
 * @id:		Device Identifier
 * @r_state:	true if requested to be idle
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_dev_is_idle(const struct ti_sci_handle *handle, u32 id,
				  bool *r_state)
{
	int ret;
	u8 state;

	if (!r_state)
		return -EINVAL;

	ret = ti_sci_get_device_state(handle, id, NULL, NULL, &state, NULL);
	if (ret)
		return ret;

	*r_state = (state == MSG_DEVICE_SW_STATE_RETENTION);

	return 0;
}

/**
 * ti_sci_cmd_dev_is_stop() - Check if the device is requested to be stopped
 * @handle:	Pointer to TISCI handle
 * @id:		Device Identifier
 * @r_state:	true if requested to be stopped
 * @curr_state:	true if currently stopped.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_dev_is_stop(const struct ti_sci_handle *handle, u32 id,
				  bool *r_state,  bool *curr_state)
{
	int ret;
	u8 p_state, c_state;

	if (!r_state && !curr_state)
		return -EINVAL;

	ret =
	    ti_sci_get_device_state(handle, id, NULL, NULL, &p_state, &c_state);
	if (ret)
		return ret;

	if (r_state)
		*r_state = (p_state == MSG_DEVICE_SW_STATE_AUTO_OFF);
	if (curr_state)
		*curr_state = (c_state == MSG_DEVICE_HW_STATE_OFF);

	return 0;
}

/**
 * ti_sci_cmd_dev_is_on() - Check if the device is requested to be ON
 * @handle:	Pointer to TISCI handle
 * @id:		Device Identifier
 * @r_state:	true if requested to be ON
 * @curr_state:	true if currently ON and active
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_dev_is_on(const struct ti_sci_handle *handle, u32 id,
				bool *r_state,  bool *curr_state)
{
	int ret;
	u8 p_state, c_state;

	if (!r_state && !curr_state)
		return -EINVAL;

	ret =
	    ti_sci_get_device_state(handle, id, NULL, NULL, &p_state, &c_state);
	if (ret)
		return ret;

	if (r_state)
		*r_state = (p_state == MSG_DEVICE_SW_STATE_ON);
	if (curr_state)
		*curr_state = (c_state == MSG_DEVICE_HW_STATE_ON);

	return 0;
}

/**
 * ti_sci_cmd_dev_is_trans() - Check if the device is currently transitioning
 * @handle:	Pointer to TISCI handle
 * @id:		Device Identifier
 * @curr_state:	true if currently transitioning.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_dev_is_trans(const struct ti_sci_handle *handle, u32 id,
				   bool *curr_state)
{
	int ret;
	u8 state;

	if (!curr_state)
		return -EINVAL;

	ret = ti_sci_get_device_state(handle, id, NULL, NULL, NULL, &state);
	if (ret)
		return ret;

	*curr_state = (state == MSG_DEVICE_HW_STATE_TRANS);

	return 0;
}

/**
 * ti_sci_cmd_set_device_resets() - command to set resets for device managed
 *				    by TISCI
 * @handle:	Pointer to TISCI handle as retrieved by *ti_sci_get_handle
 * @id:		Device Identifier
 * @reset_state: Device specific reset bit field
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_set_device_resets(const struct ti_sci_handle *handle,
					u32 id, u32 reset_state)
{
	struct ti_sci_msg_req_set_device_resets req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.id = id;
	req.resets = reset_state;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_DEVICE_RESETS,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_get_device_resets() - Get reset state for device managed
 *				    by TISCI
 * @handle:		Pointer to TISCI handle
 * @id:			Device Identifier
 * @reset_state:	Pointer to reset state to populate
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_get_device_resets(const struct ti_sci_handle *handle,
					u32 id, u32 *reset_state)
{
	return ti_sci_get_device_state(handle, id, NULL, reset_state, NULL,
				       NULL);
}

/*
 * ti_sci_setup_device_ops() - Setup the device operations structures
 * @dops:	pointer to TISCI device ops structure
 */
void ti_sci_setup_device_ops(struct ti_sci_dev_ops *dops)
{
	dops->get_device = ti_sci_cmd_get_device;
	dops->get_device_exclusive = ti_sci_cmd_get_device_exclusive;
	dops->idle_device = ti_sci_cmd_idle_device;
	dops->idle_device_exclusive = ti_sci_cmd_idle_device_exclusive;
	dops->put_device = ti_sci_cmd_put_device;

	dops->is_valid = ti_sci_cmd_dev_is_valid;
	dops->get_context_loss_count = ti_sci_cmd_dev_get_clcnt;
	dops->is_idle = ti_sci_cmd_dev_is_idle;
	dops->is_stop = ti_sci_cmd_dev_is_stop;
	dops->is_on = ti_sci_cmd_dev_is_on;
	dops->is_transitioning = ti_sci_cmd_dev_is_trans;
	dops->set_device_resets = ti_sci_cmd_set_device_resets;
	dops->get_device_resets = ti_sci_cmd_get_device_resets;
}
