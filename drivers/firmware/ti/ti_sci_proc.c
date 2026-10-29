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
 * ti_sci_cmd_proc_request() - Command to request a physical processor control
 * @handle:	Pointer to TI SCI handle
 * @proc_id:	Processor ID this request is for
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_request(const struct ti_sci_handle *handle,
				   u8 proc_id)
{
	struct ti_sci_msg_req_proc_request req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_PROC_REQUEST,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_release() - Command to release a physical processor control
 * @handle:	Pointer to TI SCI handle
 * @proc_id:	Processor ID this request is for
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_release(const struct ti_sci_handle *handle,
				   u8 proc_id)
{
	struct ti_sci_msg_req_proc_release req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_PROC_RELEASE,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_handover() - Command to handover a physical processor
 *				control to a host in the processor's access
 *				control list.
 * @handle:	Pointer to TI SCI handle
 * @proc_id:	Processor ID this request is for
 * @host_id:	Host ID to get the control of the processor
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_handover(const struct ti_sci_handle *handle,
				    u8 proc_id, u8 host_id)
{
	struct ti_sci_msg_req_proc_handover req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;
	req.host_id = host_id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_PROC_HANDOVER,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_set_config() - Command to set the processor boot
 *				    configuration flags
 * @handle:		Pointer to TI SCI handle
 * @proc_id:		Processor ID this request is for
 * @bootvector:		Processor Boot vector (start address)
 * @config_flags_set:	Configuration flags to be set
 * @config_flags_clear:	Configuration flags to be cleared.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_set_config(const struct ti_sci_handle *handle,
				      u8 proc_id, u64 bootvector,
				      u32 config_flags_set,
				      u32 config_flags_clear)
{
	struct ti_sci_msg_req_set_config req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;
	req.bootvector_low = bootvector & TI_SCI_ADDR_LOW_MASK;
	req.bootvector_high = (bootvector & TI_SCI_ADDR_HIGH_MASK) >>
				TI_SCI_ADDR_HIGH_SHIFT;
	req.config_flags_set = config_flags_set;
	req.config_flags_clear = config_flags_clear;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_CONFIG,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_set_control() - Command to set the processor boot
 *				     control flags
 * @handle:			Pointer to TI SCI handle
 * @proc_id:			Processor ID this request is for
 * @control_flags_set:		Control flags to be set
 * @control_flags_clear:	Control flags to be cleared
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_set_control(const struct ti_sci_handle *handle,
				       u8 proc_id, u32 control_flags_set,
				       u32 control_flags_clear)
{
	struct ti_sci_msg_req_set_ctrl req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.processor_id = proc_id;
	req.control_flags_set = control_flags_set;
	req.control_flags_clear = control_flags_clear;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_CTRL,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_proc_get_status() - Command to get the processor boot status
 * @handle:	Pointer to TI SCI handle
 * @proc_id:	Processor ID this request is for
 * @bv:		Processor Boot vector (start address)
 * @cfg_flags:	Processor specific configuration flags
 * @ctrl_flags:	Processor specific control flags
 * @sts_flags:	Processor specific status flags
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_proc_get_status(const struct ti_sci_handle *handle,
				      u8 proc_id, u64 *bv, u32 *cfg_flags,
				      u32 *ctrl_flags, u32 *sts_flags)
{
	struct ti_sci_msg_resp_get_status resp;
	struct ti_sci_msg_req_get_status req;
	int ret = 0;

	req.processor_id = proc_id;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_GET_STATUS,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	*bv = (resp.bootvector_low & TI_SCI_ADDR_LOW_MASK) |
	      (((u64)resp.bootvector_high << TI_SCI_ADDR_HIGH_SHIFT) &
	       TI_SCI_ADDR_HIGH_MASK);
	*cfg_flags = resp.config_flags;
	*ctrl_flags = resp.control_flags;
	*sts_flags = resp.status_flags;

	return 0;
}

/*
 * ti_sci_setup_proc_ops() - Setup the processor operations structures
 * @pops:	pointer to TISCI processor ops structure
 */
void ti_sci_setup_proc_ops(struct ti_sci_proc_ops *pops)
{
	pops->request = ti_sci_cmd_proc_request;
	pops->release = ti_sci_cmd_proc_release;
	pops->handover = ti_sci_cmd_proc_handover;
	pops->set_config = ti_sci_cmd_proc_set_config;
	pops->set_control = ti_sci_cmd_proc_set_control;
	pops->get_status = ti_sci_cmd_proc_get_status;
}
