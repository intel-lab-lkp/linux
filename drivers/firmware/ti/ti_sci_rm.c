// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments System Control Interface Protocol Driver
 *
 * Copyright (C) 2015-2026 Texas Instruments Incorporated - https://www.ti.com/
 *	Nishanth Menon
 */

#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/soc/ti/ti_sci_protocol.h>

#include "ti_sci.h"

/**
 * ti_sci_get_resource_range - Helper to get a range of resources assigned
 *			       to a host. Resource is uniquely identified by
 *			       type and subtype.
 * @handle:		Pointer to TISCI handle.
 * @dev_id:		TISCI device ID.
 * @subtype:		Resource assignment subtype that is being requested
 *			from the given device.
 * @s_host:		Host processor ID to which the resources are allocated
 * @desc:		Pointer to ti_sci_resource_desc to be updated with the
 *			resource range start index and number of resources
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_get_resource_range(const struct ti_sci_handle *handle,
				     u32 dev_id, u8 subtype, u8 s_host,
				     struct ti_sci_resource_desc *desc)
{
	struct ti_sci_msg_resp_get_resource_range resp;
	struct ti_sci_msg_req_get_resource_range req;
	int ret = 0;

	if (!desc)
		return -EINVAL;

	req.secondary_host = s_host;
	req.type = dev_id & MSG_RM_RESOURCE_TYPE_MASK;
	req.subtype = subtype & MSG_RM_RESOURCE_SUBTYPE_MASK;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_GET_RESOURCE_RANGE,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	if (!resp.range_num && !resp.range_num_sec) {
		/* Neither of the two resource range is valid */
		ret = -ENODEV;
	} else {
		desc->start = resp.range_start;
		desc->num = resp.range_num;
		desc->start_sec = resp.range_start_sec;
		desc->num_sec = resp.range_num_sec;
	}

	return ret;
}

/**
 * ti_sci_cmd_get_resource_range - Get a range of resources assigned to host
 *				   that is same as ti sci interface host.
 * @handle:		Pointer to TISCI handle.
 * @dev_id:		TISCI device ID.
 * @subtype:		Resource assignment subtype that is being requested
 *			from the given device.
 * @desc:		Pointer to ti_sci_resource_desc to be updated with the
 *			resource range start index and number of resources
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_get_resource_range(const struct ti_sci_handle *handle,
					 u32 dev_id, u8 subtype,
					 struct ti_sci_resource_desc *desc)
{
	return ti_sci_get_resource_range(handle, dev_id, subtype,
					 TI_SCI_IRQ_SECONDARY_HOST_INVALID,
					 desc);
}

/**
 * ti_sci_cmd_get_resource_range_from_shost - Get a range of resources
 *					      assigned to a specified host.
 * @handle:		Pointer to TISCI handle.
 * @dev_id:		TISCI device ID.
 * @subtype:		Resource assignment subtype that is being requested
 *			from the given device.
 * @s_host:		Host processor ID to which the resources are allocated
 * @desc:		Pointer to ti_sci_resource_desc to be updated with the
 *			resource range start index and number of resources
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static
int ti_sci_cmd_get_resource_range_from_shost(const struct ti_sci_handle *handle,
					     u32 dev_id, u8 subtype, u8 s_host,
					     struct ti_sci_resource_desc *desc)
{
	return ti_sci_get_resource_range(handle, dev_id, subtype, s_host, desc);
}

/**
 * ti_sci_manage_irq() - Helper api to configure/release the irq route between
 *			 the requested source and destination
 * @handle:		Pointer to TISCI handle.
 * @valid_params:	Bit fields defining the validity of certain params
 * @src_id:		Device ID of the IRQ source
 * @src_index:		IRQ source index within the source device
 * @dst_id:		Device ID of the IRQ destination
 * @dst_host_irq:	IRQ number of the destination device
 * @ia_id:		Device ID of the IA, if the IRQ flows through this IA
 * @vint:		Virtual interrupt to be used within the IA
 * @global_event:	Global event number to be used for the requesting event
 * @vint_status_bit:	Virtual interrupt status bit to be used for the event
 * @s_host:		Secondary host ID to which the irq/event is being
 *			requested for.
 * @type:		Request type irq set or release.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
int ti_sci_manage_irq(const struct ti_sci_handle *handle,
		      u32 valid_params, u16 src_id, u16 src_index,
		      u16 dst_id, u16 dst_host_irq, u16 ia_id, u16 vint,
		      u16 global_event, u8 vint_status_bit, u8 s_host,
		      u16 type)
{
	struct ti_sci_msg_req_manage_irq req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.valid_params = valid_params;
	req.src_id = src_id;
	req.src_index = src_index;
	req.dst_id = dst_id;
	req.dst_host_irq = dst_host_irq;
	req.ia_id = ia_id;
	req.vint = vint;
	req.global_event = global_event;
	req.vint_status_bit = vint_status_bit;
	req.secondary_host = s_host;

	ret = ti_sci_do_xfer(handle, type, TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_irq_hash() - Helper API to compute irq hash for the hash table.
 * @irq:	irq to hash
 *
 * Return: the computed hash value.
 */
static int ti_sci_irq_hash(struct ti_sci_msg_req_manage_irq *irq)
{
	return irq->src_id ^ irq->src_index;
}

/**
 * ti_sci_irq_equal() - Helper API to compare two irqs (generic headers are not
 *                       compared)
 * @irq_a:	irq_a to compare
 * @irq_b:	irq_b to compare
 *
 * Return: true if the two irqs are equal, else false.
 */
static bool ti_sci_irq_equal(struct ti_sci_msg_req_manage_irq *irq_a,
			     struct ti_sci_msg_req_manage_irq *irq_b)
{
	return !memcmp(&irq_a->valid_params, &irq_b->valid_params,
		       sizeof(*irq_a) - sizeof(irq_a->hdr));
}

/**
 * ti_sci_set_irq() - Helper api to configure the irq route between the
 *		      requested source and destination
 * @handle:		Pointer to TISCI handle.
 * @valid_params:	Bit fields defining the validity of certain params
 * @src_id:		Device ID of the IRQ source
 * @src_index:		IRQ source index within the source device
 * @dst_id:		Device ID of the IRQ destination
 * @dst_host_irq:	IRQ number of the destination device
 * @ia_id:		Device ID of the IA, if the IRQ flows through this IA
 * @vint:		Virtual interrupt to be used within the IA
 * @global_event:	Global event number to be used for the requesting event
 * @vint_status_bit:	Virtual interrupt status bit to be used for the event
 * @s_host:		Secondary host ID to which the irq/event is being
 *			requested for.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_set_irq(const struct ti_sci_handle *handle, u32 valid_params,
			  u16 src_id, u16 src_index, u16 dst_id,
			  u16 dst_host_irq, u16 ia_id, u16 vint,
			  u16 global_event, u8 vint_status_bit, u8 s_host)
{
	struct ti_sci_info *info = handle_to_ti_sci_info(handle);
	struct ti_sci_msg_req_manage_irq *desc;
	struct ti_sci_irq *irq;
	int ret;

	/* Lock for set_irq() and free_irq() to keep the IRQ hash list consistent */
	guard(mutex)(&info->irq_lock);

	pr_debug("%s: IRQ set with valid_params = 0x%x from src = %d, index = %d, to dst = %d, irq = %d,via ia_id = %d, vint = %d, global event = %d,status_bit = %d\n",
		 __func__, valid_params, src_id, src_index,
		 dst_id, dst_host_irq, ia_id, vint, global_event,
		 vint_status_bit);

	ret = ti_sci_manage_irq(handle, valid_params, src_id, src_index,
				dst_id, dst_host_irq, ia_id, vint,
				global_event, vint_status_bit, s_host,
				TI_SCI_MSG_SET_IRQ);

	if (ret || !(info->fw_caps & MSG_FLAG_CAPS_LPM_IRQ_CONTEXT_LOST))
		goto end;

	irq = kzalloc_obj(*irq);
	if (!irq) {
		ti_sci_manage_irq(handle, valid_params, src_id, src_index,
				  dst_id, dst_host_irq, ia_id, vint,
				  global_event, vint_status_bit, s_host,
				  TI_SCI_MSG_FREE_IRQ);
		ret = -ENOMEM;
		goto end;
	}

	desc = &irq->desc;
	desc->valid_params = valid_params;
	desc->src_id = src_id;
	desc->src_index = src_index;
	desc->dst_id = dst_id;
	desc->dst_host_irq = dst_host_irq;
	desc->ia_id = ia_id;
	desc->vint = vint;
	desc->global_event = global_event;
	desc->vint_status_bit = vint_status_bit;
	desc->secondary_host = s_host;

	hash_add(info->irqs, &irq->node, ti_sci_irq_hash(desc));

end:
	return ret;
}

/**
 * ti_sci_free_irq() - Helper api to free the irq route between the
 *			   requested source and destination
 * @handle:		Pointer to TISCI handle.
 * @valid_params:	Bit fields defining the validity of certain params
 * @src_id:		Device ID of the IRQ source
 * @src_index:		IRQ source index within the source device
 * @dst_id:		Device ID of the IRQ destination
 * @dst_host_irq:	IRQ number of the destination device
 * @ia_id:		Device ID of the IA, if the IRQ flows through this IA
 * @vint:		Virtual interrupt to be used within the IA
 * @global_event:	Global event number to be used for the requesting event
 * @vint_status_bit:	Virtual interrupt status bit to be used for the event
 * @s_host:		Secondary host ID to which the irq/event is being
 *			requested for.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_free_irq(const struct ti_sci_handle *handle, u32 valid_params,
			   u16 src_id, u16 src_index, u16 dst_id,
			   u16 dst_host_irq, u16 ia_id, u16 vint,
			   u16 global_event, u8 vint_status_bit, u8 s_host)
{
	struct ti_sci_info *info = handle_to_ti_sci_info(handle);
	struct ti_sci_msg_req_manage_irq irq_desc;
	struct device *dev = info->dev;
	struct ti_sci_irq *this_irq;
	struct hlist_node *tmp_node;
	int ret;

	guard(mutex)(&info->irq_lock);

	pr_debug("%s: IRQ release with valid_params = 0x%x from src = %d, index = %d, to dst = %d, irq = %d,via ia_id = %d, vint = %d, global event = %d,status_bit = %d\n",
		 __func__, valid_params, src_id, src_index,
		 dst_id, dst_host_irq, ia_id, vint, global_event,
		 vint_status_bit);

	ret = ti_sci_manage_irq(handle, valid_params, src_id, src_index,
				dst_id, dst_host_irq, ia_id, vint,
				global_event, vint_status_bit, s_host,
				TI_SCI_MSG_FREE_IRQ);

	if (ret || !(info->fw_caps & MSG_FLAG_CAPS_LPM_IRQ_CONTEXT_LOST))
		goto end;

	irq_desc.valid_params = valid_params;
	irq_desc.src_id = src_id;
	irq_desc.src_index = src_index;
	irq_desc.dst_id = dst_id;
	irq_desc.dst_host_irq = dst_host_irq;
	irq_desc.ia_id = ia_id;
	irq_desc.vint = vint;
	irq_desc.global_event = global_event;
	irq_desc.vint_status_bit = vint_status_bit;
	irq_desc.secondary_host = s_host;

	hash_for_each_possible_safe(info->irqs, this_irq, tmp_node, node,
				    ti_sci_irq_hash(&irq_desc)) {
		if (ti_sci_irq_equal(&irq_desc, &this_irq->desc)) {
			hlist_del(&this_irq->node);
			kfree(this_irq);
			goto end;
		}
	}

	dev_warn(dev, "%s: should not be here, IRQ was not found in hash list\n",
		 __func__);

end:
	return ret;
}

/**
 * ti_sci_cmd_set_irq() - Configure a host irq route between the requested
 *			  source and destination.
 * @handle:		Pointer to TISCI handle.
 * @src_id:		Device ID of the IRQ source
 * @src_index:		IRQ source index within the source device
 * @dst_id:		Device ID of the IRQ destination
 * @dst_host_irq:	IRQ number of the destination device
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_set_irq(const struct ti_sci_handle *handle, u16 src_id,
			      u16 src_index, u16 dst_id, u16 dst_host_irq)
{
	u32 valid_params = MSG_FLAG_DST_ID_VALID | MSG_FLAG_DST_HOST_IRQ_VALID;

	return ti_sci_set_irq(handle, valid_params, src_id, src_index, dst_id,
			      dst_host_irq, 0, 0, 0, 0, 0);
}

/**
 * ti_sci_cmd_set_event_map() - Configure an event based irq route between the
 *				requested source and Interrupt Aggregator.
 * @handle:		Pointer to TISCI handle.
 * @src_id:		Device ID of the IRQ source
 * @src_index:		IRQ source index within the source device
 * @ia_id:		Device ID of the IA, if the IRQ flows through this IA
 * @vint:		Virtual interrupt to be used within the IA
 * @global_event:	Global event number to be used for the requesting event
 * @vint_status_bit:	Virtual interrupt status bit to be used for the event
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_set_event_map(const struct ti_sci_handle *handle,
				    u16 src_id, u16 src_index, u16 ia_id,
				    u16 vint, u16 global_event,
				    u8 vint_status_bit)
{
	u32 valid_params = MSG_FLAG_IA_ID_VALID | MSG_FLAG_VINT_VALID |
			   MSG_FLAG_GLB_EVNT_VALID |
			   MSG_FLAG_VINT_STS_BIT_VALID;

	return ti_sci_set_irq(handle, valid_params, src_id, src_index, 0, 0,
			      ia_id, vint, global_event, vint_status_bit, 0);
}

/**
 * ti_sci_cmd_free_irq() - Free a host irq route between the between the
 *			   requested source and destination.
 * @handle:		Pointer to TISCI handle.
 * @src_id:		Device ID of the IRQ source
 * @src_index:		IRQ source index within the source device
 * @dst_id:		Device ID of the IRQ destination
 * @dst_host_irq:	IRQ number of the destination device
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_free_irq(const struct ti_sci_handle *handle, u16 src_id,
			       u16 src_index, u16 dst_id, u16 dst_host_irq)
{
	u32 valid_params = MSG_FLAG_DST_ID_VALID | MSG_FLAG_DST_HOST_IRQ_VALID;

	return ti_sci_free_irq(handle, valid_params, src_id, src_index, dst_id,
			       dst_host_irq, 0, 0, 0, 0, 0);
}

/**
 * ti_sci_cmd_free_event_map() - Free an event map between the requested source
 *				 and Interrupt Aggregator.
 * @handle:		Pointer to TISCI handle.
 * @src_id:		Device ID of the IRQ source
 * @src_index:		IRQ source index within the source device
 * @ia_id:		Device ID of the IA, if the IRQ flows through this IA
 * @vint:		Virtual interrupt to be used within the IA
 * @global_event:	Global event number to be used for the requesting event
 * @vint_status_bit:	Virtual interrupt status bit to be used for the event
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_free_event_map(const struct ti_sci_handle *handle,
				     u16 src_id, u16 src_index, u16 ia_id,
				     u16 vint, u16 global_event,
				     u8 vint_status_bit)
{
	u32 valid_params = MSG_FLAG_IA_ID_VALID |
			   MSG_FLAG_VINT_VALID | MSG_FLAG_GLB_EVNT_VALID |
			   MSG_FLAG_VINT_STS_BIT_VALID;

	return ti_sci_free_irq(handle, valid_params, src_id, src_index, 0, 0,
			       ia_id, vint, global_event, vint_status_bit, 0);
}

/**
 * ti_sci_cmd_rm_ring_cfg() - Configure a NAVSS ring
 * @handle:	Pointer to TI SCI handle.
 * @params:	Pointer to ti_sci_msg_rm_ring_cfg ring config structure
 *
 * Return: 0 if all went well, else returns appropriate error value.
 *
 * See @ti_sci_msg_rm_ring_cfg and @ti_sci_msg_rm_ring_cfg_req for
 * more info.
 */
static int ti_sci_cmd_rm_ring_cfg(const struct ti_sci_handle *handle,
				  const struct ti_sci_msg_rm_ring_cfg *params)
{
	struct ti_sci_msg_rm_ring_cfg_req req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.valid_params = params->valid_params;
	req.nav_id = params->nav_id;
	req.index = params->index;
	req.addr_lo = params->addr_lo;
	req.addr_hi = params->addr_hi;
	req.count = params->count;
	req.mode = params->mode;
	req.size = params->size;
	req.order_id = params->order_id;
	req.virtid = params->virtid;
	req.asel = params->asel;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_RM_RING_CFG,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_rm_psil_pair() - Pair PSI-L source to destination thread
 * @handle:	Pointer to TI SCI handle.
 * @nav_id:	Device ID of Navigator Subsystem which should be used for
 *		pairing
 * @src_thread:	Source PSI-L thread ID
 * @dst_thread: Destination PSI-L thread ID
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_rm_psil_pair(const struct ti_sci_handle *handle,
				   u32 nav_id, u32 src_thread, u32 dst_thread)
{
	struct ti_sci_msg_psil_pair req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.nav_id = nav_id;
	req.src_thread = src_thread;
	req.dst_thread = dst_thread;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_RM_PSIL_PAIR,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_rm_psil_unpair() - Unpair PSI-L source from destination thread
 * @handle:	Pointer to TI SCI handle.
 * @nav_id:	Device ID of Navigator Subsystem which should be used for
 *		unpairing
 * @src_thread:	Source PSI-L thread ID
 * @dst_thread:	Destination PSI-L thread ID
 *
 * Return: 0 if all went well, else returns appropriate error value.
 */
static int ti_sci_cmd_rm_psil_unpair(const struct ti_sci_handle *handle,
				     u32 nav_id, u32 src_thread, u32 dst_thread)
{
	struct ti_sci_msg_psil_unpair req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.nav_id = nav_id;
	req.src_thread = src_thread;
	req.dst_thread = dst_thread;

	ret = ti_sci_do_xfer(handle, TI_SCI_MSG_RM_PSIL_UNPAIR,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_rm_udmap_tx_ch_cfg() - Configure a UDMAP TX channel
 * @handle:	Pointer to TI SCI handle.
 * @params:	Pointer to ti_sci_msg_rm_udmap_tx_ch_cfg TX channel config
 *		structure
 *
 * Return: 0 if all went well, else returns appropriate error value.
 *
 * See @ti_sci_msg_rm_udmap_tx_ch_cfg and @ti_sci_msg_rm_udmap_tx_ch_cfg_req for
 * more info.
 */
static int ti_sci_cmd_rm_udmap_tx_ch_cfg(const struct ti_sci_handle *handle,
					 const struct ti_sci_msg_rm_udmap_tx_ch_cfg *params)
{
	struct ti_sci_msg_rm_udmap_tx_ch_cfg_req req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.valid_params = params->valid_params;
	req.nav_id = params->nav_id;
	req.index = params->index;
	req.tx_pause_on_err = params->tx_pause_on_err;
	req.tx_filt_einfo = params->tx_filt_einfo;
	req.tx_filt_pswords = params->tx_filt_pswords;
	req.tx_atype = params->tx_atype;
	req.tx_chan_type = params->tx_chan_type;
	req.tx_supr_tdpkt = params->tx_supr_tdpkt;
	req.tx_fetch_size = params->tx_fetch_size;
	req.tx_credit_count = params->tx_credit_count;
	req.txcq_qnum = params->txcq_qnum;
	req.tx_priority = params->tx_priority;
	req.tx_qos = params->tx_qos;
	req.tx_orderid = params->tx_orderid;
	req.fdepth = params->fdepth;
	req.tx_sched_priority = params->tx_sched_priority;
	req.tx_burst_size = params->tx_burst_size;
	req.tx_tdtype = params->tx_tdtype;
	req.extended_ch_type = params->extended_ch_type;

	ret = ti_sci_do_xfer(handle, TISCI_MSG_RM_UDMAP_TX_CH_CFG,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_rm_udmap_rx_ch_cfg() - Configure a UDMAP RX channel
 * @handle:	Pointer to TI SCI handle.
 * @params:	Pointer to ti_sci_msg_rm_udmap_rx_ch_cfg RX channel config
 *		structure
 *
 * Return: 0 if all went well, else returns appropriate error value.
 *
 * See @ti_sci_msg_rm_udmap_rx_ch_cfg and @ti_sci_msg_rm_udmap_rx_ch_cfg_req for
 * more info.
 */
static int ti_sci_cmd_rm_udmap_rx_ch_cfg(const struct ti_sci_handle *handle,
					 const struct ti_sci_msg_rm_udmap_rx_ch_cfg *params)
{
	struct ti_sci_msg_rm_udmap_rx_ch_cfg_req req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.valid_params = params->valid_params;
	req.nav_id = params->nav_id;
	req.index = params->index;
	req.rx_fetch_size = params->rx_fetch_size;
	req.rxcq_qnum = params->rxcq_qnum;
	req.rx_priority = params->rx_priority;
	req.rx_qos = params->rx_qos;
	req.rx_orderid = params->rx_orderid;
	req.rx_sched_priority = params->rx_sched_priority;
	req.flowid_start = params->flowid_start;
	req.flowid_cnt = params->flowid_cnt;
	req.rx_pause_on_err = params->rx_pause_on_err;
	req.rx_atype = params->rx_atype;
	req.rx_chan_type = params->rx_chan_type;
	req.rx_ignore_short = params->rx_ignore_short;
	req.rx_ignore_long = params->rx_ignore_long;
	req.rx_burst_size = params->rx_burst_size;

	ret = ti_sci_do_xfer(handle, TISCI_MSG_RM_UDMAP_RX_CH_CFG,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_cmd_rm_udmap_rx_flow_cfg() - Configure UDMAP RX FLOW
 * @handle:	Pointer to TI SCI handle.
 * @params:	Pointer to ti_sci_msg_rm_udmap_flow_cfg RX FLOW config
 *		structure
 *
 * Return: 0 if all went well, else returns appropriate error value.
 *
 * See @ti_sci_msg_rm_udmap_flow_cfg and @ti_sci_msg_rm_udmap_flow_cfg_req for
 * more info.
 */
static int ti_sci_cmd_rm_udmap_rx_flow_cfg(const struct ti_sci_handle *handle,
					   const struct ti_sci_msg_rm_udmap_flow_cfg *params)
{
	struct ti_sci_msg_rm_udmap_flow_cfg_req req;
	struct ti_sci_msg_hdr resp;
	int ret = 0;

	req.valid_params = params->valid_params;
	req.nav_id = params->nav_id;
	req.flow_index = params->flow_index;
	req.rx_einfo_present = params->rx_einfo_present;
	req.rx_psinfo_present = params->rx_psinfo_present;
	req.rx_error_handling = params->rx_error_handling;
	req.rx_desc_type = params->rx_desc_type;
	req.rx_sop_offset = params->rx_sop_offset;
	req.rx_dest_qnum = params->rx_dest_qnum;
	req.rx_src_tag_hi = params->rx_src_tag_hi;
	req.rx_src_tag_lo = params->rx_src_tag_lo;
	req.rx_dest_tag_hi = params->rx_dest_tag_hi;
	req.rx_dest_tag_lo = params->rx_dest_tag_lo;
	req.rx_src_tag_hi_sel = params->rx_src_tag_hi_sel;
	req.rx_src_tag_lo_sel = params->rx_src_tag_lo_sel;
	req.rx_dest_tag_hi_sel = params->rx_dest_tag_hi_sel;
	req.rx_dest_tag_lo_sel = params->rx_dest_tag_lo_sel;
	req.rx_fdq0_sz0_qnum = params->rx_fdq0_sz0_qnum;
	req.rx_fdq1_qnum = params->rx_fdq1_qnum;
	req.rx_fdq2_qnum = params->rx_fdq2_qnum;
	req.rx_fdq3_qnum = params->rx_fdq3_qnum;
	req.rx_ps_location = params->rx_ps_location;

	ret = ti_sci_do_xfer(handle, TISCI_MSG_RM_UDMAP_FLOW_CFG,
			     TI_SCI_FLAG_REQ_ACK_ON_PROCESSED,
			     &req, sizeof(req),
			     &resp, sizeof(resp));
	if (ret)
		return ret;

	return 0;
}

/**
 * ti_sci_get_free_resource() - Get a free resource from TISCI resource.
 * @res:	Pointer to the TISCI resource
 *
 * Return: resource num if all went ok else TI_SCI_RESOURCE_NULL.
 */
u16 ti_sci_get_free_resource(struct ti_sci_resource *res)
{
	unsigned long flags;
	u16 set, free_bit;

	raw_spin_lock_irqsave(&res->lock, flags);
	for (set = 0; set < res->sets; set++) {
		struct ti_sci_resource_desc *desc = &res->desc[set];
		int res_count = desc->num + desc->num_sec;

		free_bit = find_first_zero_bit(desc->res_map, res_count);
		if (free_bit != res_count) {
			__set_bit(free_bit, desc->res_map);
			raw_spin_unlock_irqrestore(&res->lock, flags);

			if (desc->num && free_bit < desc->num)
				return desc->start + free_bit;
			else
				return desc->start_sec + free_bit;
		}
	}
	raw_spin_unlock_irqrestore(&res->lock, flags);

	return TI_SCI_RESOURCE_NULL;
}
EXPORT_SYMBOL_GPL(ti_sci_get_free_resource);

/**
 * ti_sci_release_resource() - Release a resource from TISCI resource.
 * @res:	Pointer to the TISCI resource
 * @id:		Resource id to be released.
 */
void ti_sci_release_resource(struct ti_sci_resource *res, u16 id)
{
	unsigned long flags;
	u16 set;

	raw_spin_lock_irqsave(&res->lock, flags);
	for (set = 0; set < res->sets; set++) {
		struct ti_sci_resource_desc *desc = &res->desc[set];

		if (desc->num && desc->start <= id &&
		    (desc->start + desc->num) > id)
			__clear_bit(id - desc->start, desc->res_map);
		else if (desc->num_sec && desc->start_sec <= id &&
			 (desc->start_sec + desc->num_sec) > id)
			__clear_bit(id - desc->start_sec, desc->res_map);
	}
	raw_spin_unlock_irqrestore(&res->lock, flags);
}
EXPORT_SYMBOL_GPL(ti_sci_release_resource);

/**
 * ti_sci_get_num_resources() - Get the number of resources in TISCI resource
 * @res:	Pointer to the TISCI resource
 *
 * Return: Total number of available resources.
 */
u32 ti_sci_get_num_resources(struct ti_sci_resource *res)
{
	u32 set, count = 0;

	for (set = 0; set < res->sets; set++)
		count += res->desc[set].num + res->desc[set].num_sec;

	return count;
}
EXPORT_SYMBOL_GPL(ti_sci_get_num_resources);

/**
 * devm_ti_sci_get_resource_sets() - Get a TISCI resources assigned to a device
 * @handle:	TISCI handle
 * @dev:	Device pointer to which the resource is assigned
 * @dev_id:	TISCI device id to which the resource is assigned
 * @sub_types:	Array of sub_types assigned corresponding to device
 * @sets:	Number of sub_types
 *
 * Return: Pointer to ti_sci_resource if all went well else appropriate
 *	   error pointer.
 */
static struct ti_sci_resource *
devm_ti_sci_get_resource_sets(const struct ti_sci_handle *handle,
			      struct device *dev, u32 dev_id, u32 *sub_types,
			      u32 sets)
{
	struct ti_sci_resource *res;
	bool valid_set = false;
	int i, ret, res_count;

	res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
	if (!res)
		return ERR_PTR(-ENOMEM);

	res->sets = sets;
	res->desc = devm_kcalloc(dev, res->sets, sizeof(*res->desc),
				 GFP_KERNEL);
	if (!res->desc)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < res->sets; i++) {
		ret = handle->ops.rm_core_ops.get_range(handle, dev_id,
							sub_types[i],
							&res->desc[i]);
		if (ret) {
			dev_dbg(dev, "dev = %d subtype %d not allocated for this host\n",
				dev_id, sub_types[i]);
			memset(&res->desc[i], 0, sizeof(res->desc[i]));
			continue;
		}

		dev_dbg(dev, "dev/sub_type: %d/%d, start/num: %d/%d | %d/%d\n",
			dev_id, sub_types[i], res->desc[i].start,
			res->desc[i].num, res->desc[i].start_sec,
			res->desc[i].num_sec);

		valid_set = true;
		res_count = res->desc[i].num + res->desc[i].num_sec;
		res->desc[i].res_map = devm_bitmap_zalloc(dev, res_count,
							  GFP_KERNEL);
		if (!res->desc[i].res_map)
			return ERR_PTR(-ENOMEM);
	}
	raw_spin_lock_init(&res->lock);

	if (valid_set)
		return res;

	return ERR_PTR(-EINVAL);
}

/**
 * devm_ti_sci_get_of_resource() - Get a TISCI resource assigned to a device
 * @handle:	TISCI handle
 * @dev:	Device pointer to which the resource is assigned
 * @dev_id:	TISCI device id to which the resource is assigned
 * @of_prop:	property name by which the resource are represented
 *
 * Return: Pointer to ti_sci_resource if all went well else appropriate
 *	   error pointer.
 */
struct ti_sci_resource *
devm_ti_sci_get_of_resource(const struct ti_sci_handle *handle,
			    struct device *dev, u32 dev_id, char *of_prop)
{
	struct ti_sci_resource *res;
	u32 *sub_types;
	int sets;

	sets = of_property_count_elems_of_size(dev_of_node(dev), of_prop,
					       sizeof(u32));
	if (sets < 0) {
		dev_err(dev, "%s resource type ids not available\n", of_prop);
		return ERR_PTR(sets);
	}

	sub_types = kcalloc(sets, sizeof(*sub_types), GFP_KERNEL);
	if (!sub_types)
		return ERR_PTR(-ENOMEM);

	of_property_read_u32_array(dev_of_node(dev), of_prop, sub_types, sets);
	res = devm_ti_sci_get_resource_sets(handle, dev, dev_id, sub_types,
					    sets);

	kfree(sub_types);
	return res;
}
EXPORT_SYMBOL_GPL(devm_ti_sci_get_of_resource);

/**
 * devm_ti_sci_get_resource() - Get a resource range assigned to the device
 * @handle:	TISCI handle
 * @dev:	Device pointer to which the resource is assigned
 * @dev_id:	TISCI device id to which the resource is assigned
 * @sub_type:	TISCI resource subytpe representing the resource.
 *
 * Return: Pointer to ti_sci_resource if all went well else appropriate
 *	   error pointer.
 */
struct ti_sci_resource *
devm_ti_sci_get_resource(const struct ti_sci_handle *handle, struct device *dev,
			 u32 dev_id, u32 sub_type)
{
	return devm_ti_sci_get_resource_sets(handle, dev, dev_id, &sub_type, 1);
}
EXPORT_SYMBOL_GPL(devm_ti_sci_get_resource);

/*
 * ti_sci_setup_rm_core_ops() - Setup the Resource Management core operations structures
 * @rm_core_ops:	pointer to TISCI rm core ops structure
 */
void ti_sci_setup_rm_core_ops(struct ti_sci_rm_core_ops *rm_core_ops)
{
	rm_core_ops->get_range = ti_sci_cmd_get_resource_range;
	rm_core_ops->get_range_from_shost =
		ti_sci_cmd_get_resource_range_from_shost;
}

/*
 * ti_sci_setup_rm_irq_ops() - Setup the Resource Management IRQ operations structures
 * @iops:	pointer to TISCI rm irq ops structure
 */
void ti_sci_setup_rm_irq_ops(struct ti_sci_rm_irq_ops *iops)
{
	iops->set_irq = ti_sci_cmd_set_irq;
	iops->set_event_map = ti_sci_cmd_set_event_map;
	iops->free_irq = ti_sci_cmd_free_irq;
	iops->free_event_map = ti_sci_cmd_free_event_map;
}

/*
 * ti_sci_setup_rm_ringacc_ops() - Setup the Resource Management RINGACC operations structures
 * @rops:	pointer to TISCI rm ringacc ops structure
 */
void ti_sci_setup_rm_ringacc_ops(struct ti_sci_rm_ringacc_ops *rops)
{
	rops->set_cfg = ti_sci_cmd_rm_ring_cfg;
}

/*
 * ti_sci_setup_rm_psil_ops() - Setup the Resource Management PSIL operations structures
 * @psilops:	pointer to TISCI rm psil ops structure
 */
void ti_sci_setup_rm_psil_ops(struct ti_sci_rm_psil_ops *psilops)
{
	psilops->pair = ti_sci_cmd_rm_psil_pair;
	psilops->unpair = ti_sci_cmd_rm_psil_unpair;
}

/*
 * ti_sci_setup_rm_udmap_ops() - Setup the Resource Management UDMAP operations structures
 * @udmap_ops:	pointer to TISCI rm udmap ops structure
 */
void ti_sci_setup_rm_udmap_ops(struct ti_sci_rm_udmap_ops *udmap_ops)
{
	udmap_ops->tx_ch_cfg = ti_sci_cmd_rm_udmap_tx_ch_cfg;
	udmap_ops->rx_ch_cfg = ti_sci_cmd_rm_udmap_rx_ch_cfg;
	udmap_ops->rx_flow_cfg = ti_sci_cmd_rm_udmap_rx_flow_cfg;
}
