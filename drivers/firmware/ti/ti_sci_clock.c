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
 * ti_sci_set_clock_state() - Set clock state helper
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @flags:	Header flags as needed
 * @state:	State to request for the clock.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_set_clock_state(const struct ti_sci_handle *handle,
				  u32 dev_id, u32 clk_id,
				  u32 flags, u8 state)
{
	struct ti_sci_msg_req_set_clock_state req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.dev_id = dev_id;
	if (clk_id < 255) {
		req.clk_id = clk_id;
	} else {
		req.clk_id = 255;
		req.clk_id_32 = clk_id;
	}
	req.request_state = state;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_CLOCK_STATE,
			     flags | TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_get_clock_state() - Get clock state helper
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @programmed_state:	State requested for clock to move to
 * @current_state:	State that the clock is currently in
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_get_clock_state(const struct ti_sci_handle *handle,
				      u32 dev_id, u32 clk_id,
				      u8 *programmed_state, u8 *current_state)
{
	struct ti_sci_msg_req_get_clock_state req;
	struct ti_sci_msg_resp_get_clock_state resp;
	int ret = 0;

	if (!programmed_state && !current_state)
		return -EINVAL;

	req.dev_id = dev_id;
	if (clk_id < 255) {
		req.clk_id = clk_id;
	} else {
		req.clk_id = 255;
		req.clk_id_32 = clk_id;
	}

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_GET_CLOCK_STATE,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	if (programmed_state)
		*programmed_state = resp.programmed_state;
	if (current_state)
		*current_state = resp.current_state;

	return 0;
}

/**
 * ti_sci_cmd_get_clock() - Get control of a clock from TI SCI
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @needs_ssc: 'true' if Spread Spectrum clock is desired, else 'false'
 * @can_change_freq: 'true' if frequency change is desired, else 'false'
 * @enable_input_term: 'true' if input termination is desired, else 'false'
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_get_clock(const struct ti_sci_handle *handle, u32 dev_id,
				u32 clk_id, bool needs_ssc,
				bool can_change_freq, bool enable_input_term)
{
	u32 flags = 0;

	flags |= needs_ssc ? MSG_FLAG_CLOCK_ALLOW_SSC : 0;
	flags |= can_change_freq ? MSG_FLAG_CLOCK_ALLOW_FREQ_CHANGE : 0;
	flags |= enable_input_term ? MSG_FLAG_CLOCK_INPUT_TERM : 0;

	return ti_sci_set_clock_state(handle, dev_id, clk_id, flags,
				      MSG_CLOCK_SW_STATE_REQ);
}

/**
 * ti_sci_cmd_idle_clock() - Idle a clock which is in our control
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 *
 * NOTE: This clock must have been requested by get_clock previously.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_idle_clock(const struct ti_sci_handle *handle,
				 u32 dev_id, u32 clk_id)
{
	return ti_sci_set_clock_state(handle, dev_id, clk_id,
				      MSG_FLAG_CLOCK_ALLOW_FREQ_CHANGE,
				      MSG_CLOCK_SW_STATE_UNREQ);
}

/**
 * ti_sci_cmd_put_clock() - Release a clock from our control back to TISCI
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 *
 * NOTE: This clock must have been requested by get_clock previously.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_put_clock(const struct ti_sci_handle *handle,
				u32 dev_id, u32 clk_id)
{
	return ti_sci_set_clock_state(handle, dev_id, clk_id,
				      MSG_FLAG_CLOCK_ALLOW_FREQ_CHANGE,
				      MSG_CLOCK_SW_STATE_AUTO);
}

/**
 * ti_sci_cmd_clk_is_auto() - Is the clock being auto managed
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @req_state: state indicating if the clock is auto managed
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_is_auto(const struct ti_sci_handle *handle,
				  u32 dev_id, u32 clk_id, bool *req_state)
{
	u8 state = 0;
	int ret;

	if (!req_state)
		return -EINVAL;

	ret = ti_sci_cmd_get_clock_state(handle, dev_id, clk_id, &state, NULL);
	if (ret)
		return ret;

	*req_state = (state == MSG_CLOCK_SW_STATE_AUTO);
	return 0;
}

/**
 * ti_sci_cmd_clk_is_on() - Is the clock ON
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @req_state: state indicating if the clock is managed by us and enabled
 * @curr_state: state indicating if the clock is ready for operation
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_is_on(const struct ti_sci_handle *handle, u32 dev_id,
				u32 clk_id, bool *req_state, bool *curr_state)
{
	u8 c_state = 0, r_state = 0;
	int ret;

	if (!req_state && !curr_state)
		return -EINVAL;

	ret = ti_sci_cmd_get_clock_state(handle, dev_id, clk_id,
					 &r_state, &c_state);
	if (ret)
		return ret;

	if (req_state)
		*req_state = (r_state == MSG_CLOCK_SW_STATE_REQ);
	if (curr_state)
		*curr_state = (c_state == MSG_CLOCK_HW_STATE_READY);
	return 0;
}

/**
 * ti_sci_cmd_clk_is_off() - Is the clock OFF
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @req_state: state indicating if the clock is managed by us and disabled
 * @curr_state: state indicating if the clock is NOT ready for operation
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_is_off(const struct ti_sci_handle *handle, u32 dev_id,
				 u32 clk_id, bool *req_state, bool *curr_state)
{
	u8 c_state = 0, r_state = 0;
	int ret;

	if (!req_state && !curr_state)
		return -EINVAL;

	ret = ti_sci_cmd_get_clock_state(handle, dev_id, clk_id,
					 &r_state, &c_state);
	if (ret)
		return ret;

	if (req_state)
		*req_state = (r_state == MSG_CLOCK_SW_STATE_UNREQ);
	if (curr_state)
		*curr_state = (c_state == MSG_CLOCK_HW_STATE_NOT_READY);
	return 0;
}

/**
 * ti_sci_cmd_clk_set_parent() - Set the clock source of a specific device clock
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @parent_id:	Parent clock identifier to set
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_set_parent(const struct ti_sci_handle *handle,
				     u32 dev_id, u32 clk_id, u32 parent_id)
{
	struct ti_sci_msg_req_set_clock_parent req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.dev_id = dev_id;
	if (clk_id < 255) {
		req.clk_id = clk_id;
	} else {
		req.clk_id = 255;
		req.clk_id_32 = clk_id;
	}
	if (parent_id < 255) {
		req.parent_id = parent_id;
	} else {
		req.parent_id = 255;
		req.parent_id_32 = parent_id;
	}

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_CLOCK_PARENT,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_clk_get_parent() - Get current parent clock source
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @parent_id:	Current clock parent
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_get_parent(const struct ti_sci_handle *handle,
				     u32 dev_id, u32 clk_id, u32 *parent_id)
{
	struct ti_sci_msg_req_get_clock_parent req;
	struct ti_sci_msg_resp_get_clock_parent resp;
	int ret = 0;

	if (!parent_id)
		return -EINVAL;

	req.dev_id = dev_id;
	if (clk_id < 255) {
		req.clk_id = clk_id;
	} else {
		req.clk_id = 255;
		req.clk_id_32 = clk_id;
	}

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_GET_CLOCK_PARENT,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	if (resp.parent_id < 255)
		*parent_id = resp.parent_id;
	else
		*parent_id = resp.parent_id_32;

	return 0;
}

/**
 * ti_sci_cmd_clk_get_num_parents() - Get num parents of the current clk source
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @num_parents: Returns he number of parents to the current clock.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_get_num_parents(const struct ti_sci_handle *handle,
					  u32 dev_id, u32 clk_id,
					  u32 *num_parents)
{
	struct ti_sci_msg_req_get_clock_num_parents req;
	struct ti_sci_msg_resp_get_clock_num_parents resp;
	int ret = 0;

	if (!num_parents)
		return -EINVAL;

	req.dev_id = dev_id;
	if (clk_id < 255) {
		req.clk_id = clk_id;
	} else {
		req.clk_id = 255;
		req.clk_id_32 = clk_id;
	}

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_GET_NUM_CLOCK_PARENTS,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	if (resp.num_parents < 255)
		*num_parents = resp.num_parents;
	else
		*num_parents = resp.num_parents_32;

	return 0;
}

/**
 * ti_sci_cmd_clk_get_match_freq() - Find a good match for frequency
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @min_freq:	The minimum allowable frequency in Hz. This is the minimum
 *		allowable programmed frequency and does not account for clock
 *		tolerances and jitter.
 * @target_freq: The target clock frequency in Hz. A frequency will be
 *		processed as close to this target frequency as possible.
 * @max_freq:	The maximum allowable frequency in Hz. This is the maximum
 *		allowable programmed frequency and does not account for clock
 *		tolerances and jitter.
 * @match_freq:	Frequency match in Hz response.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_get_match_freq(const struct ti_sci_handle *handle,
					 u32 dev_id, u32 clk_id, u64 min_freq,
					 u64 target_freq, u64 max_freq,
					 u64 *match_freq)
{
	struct ti_sci_msg_req_query_clock_freq req;
	struct ti_sci_msg_resp_query_clock_freq resp;
	int ret = 0;

	if (!match_freq)
		return -EINVAL;

	req.dev_id = dev_id;
	if (clk_id < 255) {
		req.clk_id = clk_id;
	} else {
		req.clk_id = 255;
		req.clk_id_32 = clk_id;
	}
	req.min_freq_hz = min_freq;
	req.target_freq_hz = target_freq;
	req.max_freq_hz = max_freq;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_QUERY_CLOCK_FREQ,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	*match_freq = resp.freq_hz;

	return 0;
}

/**
 * ti_sci_cmd_clk_set_freq() - Set a frequency for clock
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @min_freq:	The minimum allowable frequency in Hz. This is the minimum
 *		allowable programmed frequency and does not account for clock
 *		tolerances and jitter.
 * @target_freq: The target clock frequency in Hz. A frequency will be
 *		processed as close to this target frequency as possible.
 * @max_freq:	The maximum allowable frequency in Hz. This is the maximum
 *		allowable programmed frequency and does not account for clock
 *		tolerances and jitter.
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_set_freq(const struct ti_sci_handle *handle,
				   u32 dev_id, u32 clk_id, u64 min_freq,
				   u64 target_freq, u64 max_freq)
{
	struct ti_sci_msg_req_set_clock_freq req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.dev_id = dev_id;
	if (clk_id < 255) {
		req.clk_id = clk_id;
	} else {
		req.clk_id = 255;
		req.clk_id_32 = clk_id;
	}
	req.min_freq_hz = min_freq;
	req.target_freq_hz = target_freq;
	req.max_freq_hz = max_freq;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_SET_CLOCK_FREQ,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_clk_get_freq() - Get current frequency
 * @handle:	pointer to TI SCI handle
 * @dev_id:	Device identifier this request is for
 * @clk_id:	Clock identifier for the device for this request.
 *		Each device has it's own set of clock inputs. This indexes
 *		which clock input to modify.
 * @freq:	Currently frequency in Hz
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_clk_get_freq(const struct ti_sci_handle *handle,
				   u32 dev_id, u32 clk_id, u64 *freq)
{
	struct ti_sci_msg_req_get_clock_freq req;
	struct ti_sci_msg_resp_get_clock_freq resp;
	int ret = 0;

	if (!freq)
		return -EINVAL;

	req.dev_id = dev_id;
	if (clk_id < 255) {
		req.clk_id = clk_id;
	} else {
		req.clk_id = 255;
		req.clk_id_32 = clk_id;
	}

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_GET_CLOCK_FREQ,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	*freq = resp.freq_hz;

	return 0;
}

/*
 * ti_sci_setup_clock_ops() - Setup the clock operations structures
 * @cops:	pointer to TISCI clock operations structure
 */
void ti_sci_setup_clock_ops(struct ti_sci_clk_ops *cops)
{
	cops->get_clock = ti_sci_cmd_get_clock;
	cops->idle_clock = ti_sci_cmd_idle_clock;
	cops->put_clock = ti_sci_cmd_put_clock;
	cops->is_auto = ti_sci_cmd_clk_is_auto;
	cops->is_on = ti_sci_cmd_clk_is_on;
	cops->is_off = ti_sci_cmd_clk_is_off;

	cops->set_parent = ti_sci_cmd_clk_set_parent;
	cops->get_parent = ti_sci_cmd_clk_get_parent;
	cops->get_num_parents = ti_sci_cmd_clk_get_num_parents;

	cops->get_best_match_freq = ti_sci_cmd_clk_get_match_freq;
	cops->set_freq = ti_sci_cmd_clk_set_freq;
	cops->get_freq = ti_sci_cmd_clk_get_freq;
}
