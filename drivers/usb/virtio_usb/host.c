// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio_usb: VirtIO USB device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include "controller.h"

#include "host.h"
#include "vq_common.h"

/**
 * virtio_usb_hc_reset() - Reset the host controller.
 * @hcd: USB host controller device.
 *
 * Context: Any context
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_hc_reset(struct usb_hcd *hcd)
{
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);

	if (hcd == vhcd_vp->hs) {
		hcd->speed = HCD_USB2;
		hcd->self.root_hub->speed = USB_SPEED_HIGH;
		hcd->has_tt = 1;
	} else {
		hcd->speed = HCD_USB3;
		hcd->self.root_hub->speed = USB_SPEED_SUPER;
	}

	hcd->self.sg_tablesize = ~0;
	return 0;
}

/**
 * virtio_usb_hc_start() - Start the host controller.
 * @hcd: USB host controller device
 *
 * Context: Any context
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_hc_start(struct usb_hcd *hcd)
{
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);
	unsigned long iflags;

	hcd->uses_new_polling = 1;
	clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);

	spin_lock_irqsave(&vhcd_vp->lock, iflags);
	hcd->state = HC_STATE_RUNNING;
	spin_unlock_irqrestore(&vhcd_vp->lock, iflags);
	hcd->self.no_sg_constraint = 1;

	return 0;
}

/**
 * virtio_usb_hc_stop() - Stop the host controller.
 * @hcd: USB host controller device
 *
 * Context: Any context
 */
static void virtio_usb_hc_stop(struct usb_hcd *hcd)
{
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);
	unsigned long iflags;

	spin_lock_irqsave(&vhcd_vp->lock, iflags);
	hcd->state = HC_STATE_HALT;
	spin_unlock_irqrestore(&vhcd_vp->lock, iflags);
}

/**
 * virtio_usb_hub_status_data() - Get the hub status data for the root hub.
 * @vhcd_vp: per-VP VirtIO USB host controller
 * @buffer: status buffer in which the hub status need to be updated
 * @ss_mode: indicates if the root hub is a super speed hub.
 *
 * Context: Any context
 * Return: 0 if the status hasn't changed, or the number of bytes in buffer.
 */
static int virtio_usb_hub_status_data(struct virtio_usb_hc_vp *vhcd_vp,
				      char *buffer, bool ss_mode)
{
	unsigned int i;
	unsigned int nbytes = DIV_ROUND_UP(VIRTIO_USB_VP_MAX_PORTS + 1, 8);
	int changed = 0;
	unsigned long iflags;
	struct usb_hcd *hcd = ss_mode ? vhcd_vp->ss : vhcd_vp->hs;

	memset(buffer, 0, nbytes);

	for (i = 0; i < VIRTIO_USB_VP_MAX_PORTS; i++) {
		struct virtio_usb_hc_port *port = &vhcd_vp->ports[i];
		u16 value;

		spin_lock_irqsave(&port->lock, iflags);
		value = ss_mode ? port->ss.change.value : port->hs.change.value;
		spin_unlock_irqrestore(&port->lock, iflags);

		if (value) {
			buffer[(i + 1) / 8] |= 1 << ((i + 1) % 8);
			changed = 1;
		}
	}
	if (changed) {
		spin_lock_irqsave(&vhcd_vp->lock, iflags);
		if (hcd->state == HC_STATE_SUSPENDED)
			usb_hcd_resume_root_hub(hcd);
		spin_unlock_irqrestore(&vhcd_vp->lock, iflags);
	}

	return changed ? nbytes : 0;
}

static int virtio_usb_hs_hub_status_data(struct usb_hcd *hcd, char *buffer)
{
	return virtio_usb_hub_status_data(vhcd_get(hcd), buffer, false);
}

static int virtio_usb_ss_hub_status_data(struct usb_hcd *hcd, char *buffer)
{
	return virtio_usb_hub_status_data(vhcd_get(hcd), buffer, true);
}

/**
 * virtio_usb_windex_to_port - Map wIndex to a host controller port.
 * @vhcd_vp: per-VP host controller
 * @wIndex:  low byte contains the 1-based port number
 *
 * Return: pointer to port on success, NULL if out of range.
 */
static struct virtio_usb_hc_port *
virtio_usb_windex_to_port(struct virtio_usb_hc_vp *vhcd_vp, u16 wIndex)
{
	u16 pIndex = wIndex & 0xFF;

	if (pIndex == 0 || pIndex > VIRTIO_USB_VP_MAX_PORTS)
		return NULL;
	return &vhcd_vp->ports[pIndex - 1];
}

/**
 * virtio_usb_hub_control() - Hub control request callback (shared HS/SS).
 */
static int virtio_usb_hub_control(struct usb_hcd *hcd, u16 type, u16 wValue,
				  u16 wIndex, char *buffer, u16 wLength,
				  bool ss_mode)
{
	struct virtio_usb_hc_port *port;
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);

	switch (type) {
	case GetHubDescriptor: {
		struct usb_hub_descriptor *dsc =
			(struct usb_hub_descriptor *)buffer;

		memset(dsc, 0, sizeof(struct usb_hub_descriptor));

		if (ss_mode) {
			dsc->bDescLength = USB_DT_SS_HUB_SIZE;
			dsc->bDescriptorType = USB_DT_SS_HUB;
		} else {
			dsc->bDescLength = 9;
			dsc->bDescriptorType = USB_DT_HUB;
		}

		/* Fixed port count per VP - always within USB_MAXCHILDREN (31)
		 * and USB_SS_MAXPORTS (15).
		 */
		dsc->bNbrPorts = VIRTIO_USB_VP_MAX_PORTS;

		return 0;
	}
	case GetHubStatus: {
		*((u32 *)buffer) = 0;
		return 0;
	}
	case GetPortStatus: {
		u16 *status = (u16 *)buffer;
		unsigned long iflags;

		memset(status, 0, wLength);

		port = virtio_usb_windex_to_port(vhcd_vp, wIndex);
		if (port) {
			spin_lock_irqsave(&port->lock, iflags);
			if (ss_mode) {
				status[0] = port->ss.status.value;
				status[1] = port->ss.change.value;
			} else {
				status[0] = port->hs.status.value;
				status[1] = port->hs.change.value;
			}
			spin_unlock_irqrestore(&port->lock, iflags);
		}

		return 0;
	}
	}

	return -EPIPE;
}

/**
 * virtio_usb_hs_hub_control() - VirtIO USB High speed hub control request.
 */
static int virtio_usb_hs_hub_control(struct usb_hcd *hcd, u16 type, u16 wValue,
				     u16 wIndex, char *buffer, u16 wLength)
{
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);
	struct virtio_usb_hc_port *port;
	unsigned long iflags;
	int rc = 0;

	switch (type) {
	case ClearPortFeature:
		port = virtio_usb_windex_to_port(vhcd_vp, wIndex);
		if (port == NULL)
			return -EPIPE;

		spin_lock_irqsave(&port->lock, iflags);
		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
			if (port->hs.status.bits.enable) {
				port->hs.status.bits.enable = 0;
				port->hs.change.bits.enable = 1;
			}
			break;
		case USB_PORT_FEAT_C_CONNECTION:
			port->hs.change.bits.connect = 0;
			break;
		case USB_PORT_FEAT_C_ENABLE:
			port->hs.change.bits.enable = 0;
			break;
		case USB_PORT_FEAT_C_RESET:
			port->hs.change.bits.reset = 0;
			break;
		default:
			rc = -EPIPE;
			break;
		}
		spin_unlock_irqrestore(&port->lock, iflags);
		break;
	case SetPortFeature:
		port = virtio_usb_windex_to_port(vhcd_vp, wIndex);
		if (port == NULL)
			return -EPIPE;

		spin_lock_irqsave(&port->lock, iflags);
		switch (wValue) {
		case USB_PORT_FEAT_RESET:
			if (!port->hs.status.bits.enable) {
				port->hs.status.bits.enable = 1;
				port->hs.change.bits.enable = 1;
			}
			port->hs.change.bits.reset = 1;
			break;
		case USB_PORT_FEAT_POWER:
			port->hs.status.bits.power = 1;
			break;
		default:
			rc = -EPIPE;
			break;
		}
		spin_unlock_irqrestore(&port->lock, iflags);
		break;
	default:
		rc = virtio_usb_hub_control(hcd, type, wValue, wIndex, buffer,
					    wLength, false);
		break;
	}

	return rc;
}

/* Super speed root hub device descriptor */
static struct {
	struct usb_bos_descriptor bos;
	struct usb_ss_cap_descriptor ss_cap;
} __packed ss_bos_desc = {
	.bos = {
		.bLength = USB_DT_BOS_SIZE,
		.bDescriptorType = USB_DT_BOS,
		.wTotalLength = cpu_to_le16(sizeof(ss_bos_desc)),
		.bNumDeviceCaps = 1,
		},
	.ss_cap = {
		.bLength = USB_DT_USB_SS_CAP_SIZE,
		.bDescriptorType = USB_DT_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_SS_CAP_TYPE,
		.bmAttributes = 0x00,
		.wSpeedSupported = cpu_to_le16(USB_5GBPS_OPERATION),
		.bFunctionalitySupport = ilog2(USB_5GBPS_OPERATION),
		.bU1devExitLat = 0x00,
		.bU2DevExitLat = 0x00,
		},
};

/**
 * virtio_usb_ss_hub_control() - VirtIO USB Super speed hub control request.
 */
static int virtio_usb_ss_hub_control(struct usb_hcd *hcd, u16 type, u16 wValue,
				     u16 wIndex, char *buffer, u16 wLength)
{
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);
	struct virtio_usb_hc_port *port;
	unsigned long iflags;
	int rc = 0;

	switch (type) {
	case ClearPortFeature:
		port = virtio_usb_windex_to_port(vhcd_vp, wIndex);
		if (port == NULL)
			return -EPIPE;

		spin_lock_irqsave(&port->lock, iflags);
		switch (wValue) {
		case USB_PORT_FEAT_C_CONNECTION:
			port->ss.change.bits.connect = 0;
			break;
		case USB_PORT_FEAT_C_RESET:
			port->ss.change.bits.reset = 0;
			break;
		case USB_PORT_FEAT_C_PORT_LINK_STATE:
			port->ss.change.bits.link_state = 0;
			break;
		case USB_PORT_FEAT_C_BH_PORT_RESET:
			port->ss.change.bits.bh_reset = 0;
			break;
		default:
			rc = -EPIPE;
			break;
		}
		spin_unlock_irqrestore(&port->lock, iflags);
		break;
	case DeviceRequest | USB_REQ_GET_DESCRIPTOR: {
		u8 dtype = (wValue >> 8) & 0xff;

		if (dtype == USB_DT_BOS) {
			u16 bos_length = sizeof(ss_bos_desc);

			if (bos_length > wLength)
				bos_length = wLength;

			memcpy(buffer, &ss_bos_desc, bos_length);

			rc = bos_length;
		} else {
			rc = -EPIPE;
		}

		break;
	}
	case SetPortFeature: {
		port = virtio_usb_windex_to_port(vhcd_vp, wIndex);
		if (port == NULL)
			return -EPIPE;

		spin_lock_irqsave(&port->lock, iflags);
		switch (wValue) {
		case USB_PORT_FEAT_RESET:
		case USB_PORT_FEAT_BH_PORT_RESET:
			port->ss.status.bits.enable = 1;

			if (port->ss.status.bits.link_state != 0x00) {
				port->ss.status.bits.link_state = 0x00;
				port->ss.change.bits.link_state = 1;
			}

			port->ss.status.bits.reset = 0;
			port->ss.change.bits.reset = 1;

			if (wValue == USB_PORT_FEAT_BH_PORT_RESET)
				port->ss.change.bits.bh_reset = 1;

			break;
		case USB_PORT_FEAT_POWER:
			port->ss.status.bits.power = 1;
			break;
		default:
			rc = -EPIPE;
			break;
		}
		spin_unlock_irqrestore(&port->lock, iflags);
		break;
	}
	default:
		rc = virtio_usb_hub_control(hcd, type, wValue, wIndex, buffer,
					    wLength, true);
		break;
	}

	return rc;
}

/* virtio usb host controller priv structure */
struct virtio_usb_hc_priv {
	struct virtio_usb_hc_port *port;
	struct urb *urb;
	struct scatterlist *sgs;
};

/**
 * virtio_usb_hc_complete_urb() - Completes a URB
 * @vurb: virtio_usb_data message.
 *
 * Context: Process context.
 */
static void virtio_usb_hc_complete_urb(struct virtio_usb_data *vurb)
{
	struct virtio_usb_hc_priv *priv =
		(struct virtio_usb_hc_priv *)vurb->priv;
	struct virtio_usb_response *response;
	struct virtio_usb_iso_status *iso_urb_status;
	struct virtio_usb_hc_port *port = priv->port;
	struct urb *urb = priv->urb;
	struct usb_hcd *hcd;
	unsigned long flags;
	int i;
	unsigned int status;

	if (unlikely(!urb || !urb->dev || !urb->dev->bus)) {
		kfree(priv->sgs);
		virtio_usb_data_unref(vurb);
		return;
	}

	hcd = bus_to_hcd(urb->dev->bus);

	response = virtio_usb_data_response(vurb);

	status = le32_to_cpu(response->status);

	spin_lock_irqsave(&port->lock, flags);

	usb_hcd_unlink_urb_from_ep(hcd, urb);
	urb->hcpriv = NULL;
	list_del(&vurb->list);

	spin_unlock_irqrestore(&port->lock, flags);

	urb->status = virtio_error_to_usb(status);
	urb->actual_length = le32_to_cpu(response->actual_length);
	if (urb->status == -EINVAL)
		dev_err(&urb->dev->dev,
			"URB ep%02x status -EINVAL from virtio status %u actual=%d\n",
			urb->ep->desc.bEndpointAddress, status,
			urb->actual_length);

	if (usb_pipeisoc(urb->pipe) || usb_pipeint(urb->pipe))
		urb->interval = le32_to_cpu(response->interval);

	if (usb_pipeisoc(urb->pipe)) {
		iso_urb_status = (void *)response + sizeof(*response);

		for (i = 0; i < urb->number_of_packets; i++) {
			struct virtio_usb_iso_status *iso_status =
				&iso_urb_status[i];
			status = le32_to_cpu(iso_status->status);

			urb->iso_frame_desc[i].actual_length =
				le32_to_cpu(iso_status->actual_length);
			urb->iso_frame_desc[i].status =
				virtio_error_to_usb(status);

			if (iso_status->status)
				urb->error_count++;
		}

		urb->start_frame = le32_to_cpu(response->start_frame);
	}
	local_bh_disable();
	usb_hcd_giveback_urb(hcd, urb, urb->status);
	local_bh_enable();
	kfree(priv->sgs);
	virtio_usb_data_unref(vurb);
}

/**
 * virtio_usb_hc_set_urb_sgs() - Set urb sgs when total sg elements > 1.
 */
static struct scatterlist *
virtio_usb_hc_set_urb_sgs(struct virtio_usb_hc_priv *priv, gfp_t gfp)
{
	struct scatterlist *sg = NULL, *sgs = NULL;
	struct urb *urb = priv->urb;
	unsigned int nsgs, i = 0;
	unsigned int buffer_length = urb->transfer_buffer_length;
	unsigned int sg_length = 0;

	nsgs = urb->num_sgs;

	for_each_sg(urb->sg, sg, nsgs, i) {
		sg_length += sg->length;
	}
	if (sg_length > buffer_length) {
		sgs = kcalloc(nsgs, sizeof(*sgs), gfp);
		if (!sgs)
			return NULL;

		sg_init_table(sgs, nsgs);

		for_each_sg(urb->sg, sg, nsgs, i) {
			sg_length = sg->length;

			if (sg_length > buffer_length)
				sg_length = buffer_length;

			sg_set_page(&sgs[i], sg_page(sg), sg_length,
				    sg->offset);

			buffer_length -= sg_length;
			if (!buffer_length)
				break;
		}
		priv->sgs = sgs;
		return sgs;
	}
	return urb->sg;
}

/**
 * virtio_usb_hc_data_alloc() - Allocates a virtio usb data for the host
 * @port: VirtIO USB HC port
 * @urb: The urb request
 * @gfp: Kernel flags for memory allocation.
 */
static struct virtio_usb_data *
virtio_usb_hc_data_alloc(struct virtio_usb_hc_port *port, struct urb *urb,
			 gfp_t gfp)
{
	struct virtio_usb_data *vurb;
	struct virtio_usb_hc_priv *priv;
	struct virtio_usb_request *request;
	struct virtio_usb_response *response;
	struct virtio_usb_iso_packet *iso_packet;
	struct virtio_usb_iso_status *iso_status;
	size_t request_size = sizeof(*request);
	size_t response_size = sizeof(*response);
	u16 ep, transfer_flags = 0;

	if (usb_pipeisoc(urb->pipe)) {
		request_size += sizeof(*iso_packet) * urb->number_of_packets;
		response_size += sizeof(*iso_status) * urb->number_of_packets;
	}

	vurb = virtio_usb_data_alloc(request_size, response_size,
				     sizeof(struct virtio_usb_hc_priv), gfp);

	if (!vurb)
		return NULL;

	priv = vurb->priv;
	priv->port = port;
	priv->urb = urb;
	vurb->msg.queue = port->vhcd_vp->hcqs[VIRTIO_USB_VQ_DATA_IDX];

	INIT_LIST_HEAD(&vurb->list);

	request = virtio_usb_data_request(vurb);
	response = virtio_usb_data_response(vurb);

	response->status = cpu_to_le32(VIRTIO_USB_S_ERR_CANCELLED);

	if (usb_pipeisoc(urb->pipe)) {
		iso_packet = (void *)request + sizeof(*request);
		iso_status = (void *)response + sizeof(*response);
	}
	request->tag = cpu_to_le64((uintptr_t)vurb);
	ep = usb_pipeendpoint(urb->pipe);
	if (usb_pipein(urb->pipe))
		ep |= VIRTIO_USB_EP_DIR_IN;

	request->endpoint = cpu_to_le16(ep);
	request->vp_idx = cpu_to_le16((u16)port->vhcd_vp->vp_idx);
	request->port = cpu_to_le16((u16)port->port_id);

	if (urb->transfer_flags & URB_SHORT_NOT_OK)
		transfer_flags |= VIRTIO_USB_FLAG_SHORT_NOT_OK;
	if (urb->transfer_flags & URB_ISO_ASAP)
		transfer_flags |= VIRTIO_USB_FLAG_ISO_ASAP;
	if (urb->transfer_flags & URB_ZERO_PACKET)
		transfer_flags |= VIRTIO_USB_FLAG_ZERO_PACKET;
	request->transfer_flags = cpu_to_le16(transfer_flags);

	switch (usb_pipetype(urb->pipe)) {
	case PIPE_ISOCHRONOUS: {
		int i;

		request->transfer_type = cpu_to_le16(VIRTIO_USB_EP_ISOCHRONOUS);
		request->iso.start_frame = cpu_to_le32(urb->start_frame);
		request->iso.interval = cpu_to_le32(urb->interval);
		request->iso.number_of_packets =
			cpu_to_le32(urb->number_of_packets);

		for (i = 0; i < urb->number_of_packets; i++) {
			struct virtio_usb_iso_packet *packet = &iso_packet[i];

			packet->offset =
				cpu_to_le32(urb->iso_frame_desc[i].offset);
			packet->length =
				cpu_to_le32(urb->iso_frame_desc[i].length);
		}

		break;
	}
	case PIPE_INTERRUPT:
		request->transfer_type = cpu_to_le16(VIRTIO_USB_EP_INTERRUPT);
		request->interrupt.interval = cpu_to_le32(urb->interval);
		break;
	case PIPE_CONTROL:
		request->transfer_type = cpu_to_le16(VIRTIO_USB_EP_CONTROL);
		memcpy(request->control.setup, urb->setup_packet, 8);
		break;
	case PIPE_BULK:
		request->transfer_type = cpu_to_le16(VIRTIO_USB_EP_BULK);
		break;
	}
	return vurb;
}

/**
 * virtio_usb_hc_cmd_alloc() - Allocate and initialize a host command message.
 */
static struct virtio_usb_cmd *virtio_usb_hc_cmd_alloc(struct virtio_usb *vusb,
						      unsigned int vp_idx,
						      unsigned int command,
						      gfp_t gfp)
{
	size_t request_size = sizeof(struct virtio_usb_host_cmd_hdr);
	size_t response_size = sizeof(struct virtio_usb_cmd_status);
	struct virtio_usb_cmd *cmd;

	switch (command) {
	case VIRTIO_USB_CMD_HOST_CANCEL:
		request_size = sizeof(struct virtio_usb_host_cmd_cancel);
		break;
	case VIRTIO_USB_CMD_HOST_STREAMS_ALLOC:
	case VIRTIO_USB_CMD_HOST_STREAMS_FREE:
		request_size = sizeof(struct virtio_usb_host_cmd_streams);
		break;
	default:
		break;
	}

	cmd = virtio_usb_cmd_alloc(request_size, response_size, gfp);
	if (cmd) {
		struct virtio_usb_host_cmd_hdr *hdr =
			virtio_usb_cmd_request(cmd);
		struct virtio_usb_cmd_status *status =
			virtio_usb_cmd_response(cmd);

		hdr->code = cpu_to_le32(command);
		cmd->msg.queue = vusb->vports[vp_idx]
					 .vhc->hcqs[VIRTIO_USB_VQ_COMMAND_IDX];
		status->code = cpu_to_le32(VIRTIO_USB_S_ERR_CANCELLED);
	}
	return cmd;
}

/**
 * virtio_usb_port_from_urb() - Look up the port for a URB.
 *
 * urb->dev->portnum is the 1-based port number on the root hub, which
 * equals port_id + 1. The HCD identifies which VP.
 *
 * Returns NULL if the slot is out of range.
 */
static struct virtio_usb_hc_port *virtio_usb_port_from_urb(struct usb_hcd *hcd,
							   struct urb *urb)
{
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);
	int slot = urb->dev->portnum - 1;

	if (slot < 0 || slot >= VIRTIO_USB_VP_MAX_PORTS)
		return NULL;
	return &vhcd_vp->ports[slot];
}

/**
 * virtio_usb_hc_enqueue() - Enqueue a URB.
 * @hcd: USB host controller device
 * @urb: URB
 * @mem_flags: Kernel flags for memory allocation.
 *
 * Context: Any context
 * Return: 0 on success -errno on failure
 */
static int virtio_usb_hc_enqueue(struct usb_hcd *hcd, struct urb *urb,
				 gfp_t mem_flags)
{
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);
	struct virtio_usb *vusb = vhcd_vp->vusb;
	struct virtio_usb_hc_port *port;
	struct virtio_usb_data *vurb;
	struct virtio_usb_hc_priv *priv;
	struct scatterlist sg;
	struct scatterlist *psg_data = &sg, *out_sgs = NULL, *in_sgs = NULL;
	unsigned long flags;
	int rc = 0;

	port = virtio_usb_port_from_urb(hcd, urb);
	if (!port)
		return -ENODEV;

	vurb = virtio_usb_hc_data_alloc(port, urb, mem_flags);
	if (!vurb)
		return -ENOMEM;
	priv = vurb->priv;

	if (urb->transfer_buffer) {
		sg_init_one(psg_data, urb->transfer_buffer,
			    urb->transfer_buffer_length);
	} else if (urb->num_sgs > 1) {
		psg_data = virtio_usb_hc_set_urb_sgs(priv, mem_flags);
		if (!psg_data) {
			rc = -ENOMEM;
			goto on_exit;
		}
	} else if (urb->transfer_buffer_length && urb->sg) {
		sg_init_one(psg_data, sg_virt(urb->sg),
			    urb->transfer_buffer_length);
	} else {
		psg_data = NULL;
	}

	spin_lock_irqsave(&port->lock, flags);
	rc = usb_hcd_link_urb_to_ep(port_vhcd_get(port), urb);
	if (rc) {
		spin_unlock_irqrestore(&port->lock, flags);
		goto on_exit;
	}
	urb->hcpriv = vurb;
	list_add_tail(&vurb->list, &port->pending_urb_list);
	spin_unlock_irqrestore(&port->lock, flags);

	if (usb_pipeout(urb->pipe))
		out_sgs = psg_data;
	else
		in_sgs = psg_data;

	rc = virtio_usb_data_send(vusb, vurb, out_sgs, in_sgs);
	if (rc)
		goto on_error_vq;

	return rc;

on_error_vq:
	spin_lock_irqsave(&port->lock, flags);
	usb_hcd_unlink_urb_from_ep(port_vhcd_get(port), urb);
	urb->hcpriv = NULL;
	list_del(&vurb->list);
	spin_unlock_irqrestore(&port->lock, flags);

on_exit:
	kfree(priv->sgs);
	virtio_usb_data_unref(vurb);
	return rc;
}

/**
 * virtio_usb_hc_dequeue() - Dequeue a URB.
 * @hcd: USB host controller device
 * @urb: URB
 * @status: status of the URB.
 *
 * Context: Any context
 * Return: 0 on success -errno on failure
 */
static int virtio_usb_hc_dequeue(struct usb_hcd *hcd, struct urb *urb,
				 int status)
{
	struct virtio_usb_hc_vp *vhcd_vp = vhcd_get(hcd);
	struct virtio_usb *vusb = vhcd_vp->vusb;
	struct virtio_usb_hc_port *port;
	struct virtio_usb_host_cmd_cancel *cancel;
	struct virtio_usb_data *vurb;
	struct virtio_usb_cmd *cmd;
	int rc;
	unsigned long flags;

	port = virtio_usb_port_from_urb(hcd, urb);
	if (!port)
		return -ENODEV;

	spin_lock_irqsave(&port->lock, flags);

	vurb = urb->hcpriv;
	if (!vurb) {
		spin_unlock_irqrestore(&port->lock, flags);
		return -EIDRM;
	}

	rc = usb_hcd_check_unlink_urb(port_vhcd_get(port), urb, status);
	spin_unlock_irqrestore(&port->lock, flags);

	if (rc)
		return rc;

	cmd = virtio_usb_hc_cmd_alloc(vusb, vhcd_vp->vp_idx,
				      VIRTIO_USB_CMD_HOST_CANCEL, GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	cancel = virtio_usb_cmd_request(cmd);
	cancel->hdr.port = cpu_to_le32(port->port_id);
	cancel->tag = cpu_to_le64((uintptr_t)vurb);

	return virtio_usb_cmd_send_async(vusb, NULL, NULL, cmd);
}

/**
 * virtio_usb_hc_get_frame() - Get the current hw frame number
 */
static int virtio_usb_hc_get_frame(struct usb_hcd *hcd)
{
	return 0;
}

/* Virtio USB high speed host controller driver */
static struct hc_driver virtio_usb_hs_hc_driver = {
	.description = "virtio-usb-hc",
	.product_desc = "VirtIO USB2 Host Controller",
	.hcd_priv_size = sizeof(void *),
	.flags = HCD_USB2 | HCD_SHARED,

	.reset = virtio_usb_hc_reset,
	.start = virtio_usb_hc_start,
	.stop = virtio_usb_hc_stop,

	.hub_status_data = virtio_usb_hs_hub_status_data,
	.hub_control = virtio_usb_hs_hub_control,

	.urb_enqueue = virtio_usb_hc_enqueue,
	.urb_dequeue = virtio_usb_hc_dequeue,

	.get_frame_number = virtio_usb_hc_get_frame,
};

/* Virtio USB super speed host controller driver */
static struct hc_driver virtio_usb_ss_hc_driver = {
	.description = "virtio-usb-hc",
	.product_desc = "VirtIO USB3 Host Controller",
	.hcd_priv_size = sizeof(void *),
	.flags = HCD_USB3 | HCD_SHARED,

	.reset = virtio_usb_hc_reset,
	.start = virtio_usb_hc_start,
	.stop = virtio_usb_hc_stop,

	.hub_status_data = virtio_usb_ss_hub_status_data,
	.hub_control = virtio_usb_ss_hub_control,

	.urb_enqueue = virtio_usb_hc_enqueue,
	.urb_dequeue = virtio_usb_hc_dequeue,

	.get_frame_number = virtio_usb_hc_get_frame,
};

/**
 * virtio_usb_add_hcd() - Add HS and SS HCDs for one VP.
 * @vusb:    VirtIO usb device.
 * @vhcd_vp: per-VP host controller to populate.
 */
static int virtio_usb_add_hcd(struct virtio_usb *vusb,
			      struct virtio_usb_hc_vp *vhcd_vp)
{
	struct virtio_device *vdev = vusb->vdev;
	struct device *dev = &vdev->dev;
	const char *name;
	int rc;

	/* usb_create_hcd()/usb_create_shared_hcd() store this pointer as-is
	 * in hcd->self.bus_name (no copy is made) - it must stay valid for
	 * the lifetime of the HCD, so it cannot be a stack buffer. Allocate
	 * it from the parent virtio device, which outlives the HCDs.
	 */
	name = devm_kasprintf(&vusb->vdev->dev, GFP_KERNEL, "%s-vp%u",
			      dev_name(dev), vhcd_vp->vp_idx);
	if (!name)
		return -ENOMEM;

	vhcd_vp->hs = usb_create_hcd(&virtio_usb_hs_hc_driver, dev, name);
	if (!vhcd_vp->hs)
		return -ENOMEM;

	vhcd_set(vhcd_vp->hs, vhcd_vp);

	vhcd_vp->hs->skip_phy_initialization = 1;
	rc = usb_add_hcd(vhcd_vp->hs, 0, 0);
	if (rc)
		goto on_put_hs;

	vhcd_vp->ss = usb_create_shared_hcd(&virtio_usb_ss_hc_driver, dev, name,
					    vhcd_vp->hs);
	if (!vhcd_vp->ss) {
		rc = -ENOMEM;
		goto on_remove_hs;
	}

	vhcd_set(vhcd_vp->ss, vhcd_vp);

	rc = usb_add_hcd(vhcd_vp->ss, 0, 0);
	if (rc)
		goto on_put_ss;

	return 0;

on_put_ss:
	usb_put_hcd(vhcd_vp->ss);
on_remove_hs:
	usb_remove_hcd(vhcd_vp->hs);
on_put_hs:
	usb_put_hcd(vhcd_vp->hs);

	return rc;
}

/**
 * virtio_usb_hc_event_populate() - Add events to the host event queue.
 * @vusb: VirtIO USB device
 *
 * Context: Any context.
 * Return: 0 on success -errno on failure
 */
int virtio_usb_hc_event_populate(struct virtio_usb *vusb)
{
	/* The host event queue is shared across every host-role VP and
	 * lives at the struct virtio_usb level - not owned by any single
	 * VP - since a host-role VP may not exist yet (e.g. a dual-role
	 * OTG instance where every port currently reports device role).
	 */
	struct virtio_usb_queue *evt_queue =
		&vusb->vqueues[vusb->host_vq_base + VIRTIO_USB_VQ_EVENT_IDX];
	struct virtio_usb_event *events;

	events = virtio_usb_events_alloc(
		vusb, evt_queue, sizeof(struct virtio_usb_host_port_event));

	return virtio_usb_events_populate(events);
}

/**
 * virtio_usb_hc_rx_work() - Worker to drain completed data messages.
 * @work: kernel work item embedded in struct virtio_usb.
 *
 * The data virtqueue is shared across all host-role VPs, so the work
 * item that drains it lives on struct virtio_usb itself instead of on
 * any single VP (mirrors vq_dev_data_rx_work on the device-role side).
 *
 * Context: Process context.
 */
void virtio_usb_hc_rx_work(struct work_struct *work)
{
	struct virtio_usb *vusb =
		container_of(work, struct virtio_usb, vq_host_data_rx_work);
	struct virtio_usb_queue *dataq =
		&vusb->vqueues[vusb->host_vq_base + VIRTIO_USB_VQ_DATA_IDX];
	u32 length;
	struct virtio_usb_data *vurb;

	spin_lock_irq(&dataq->lock);
	do {
		virtqueue_disable_cb(dataq->vqueue);
		while ((vurb = virtqueue_get_buf(dataq->vqueue, &length))) {
			spin_unlock_irq(&dataq->lock);
			virtio_usb_hc_complete_urb(vurb);
			spin_lock_irq(&dataq->lock);
		}
		if (unlikely(virtqueue_is_broken(dataq->vqueue)))
			break;
	} while (!virtqueue_enable_cb(dataq->vqueue));
	spin_unlock_irq(&dataq->lock);
}

/**
 * virtio_usb_hc_vp_init() - Initialize the host controller for one VP.
 * @vusb:    VirtIO USB device
 * @vp_idx:  VP index (0..nports-1)
 *
 * Context: Any context.
 * Return: 0 on success -errno on failure
 */
int virtio_usb_hc_vp_init(struct virtio_usb *vusb, unsigned int vp_idx)
{
	struct virtio_usb_hc_vp *vhcd_vp;
	unsigned int i;
	int rc;

	vhcd_vp = devm_kzalloc(&vusb->vdev->dev, sizeof(*vhcd_vp), GFP_KERNEL);
	if (!vhcd_vp)
		return -ENOMEM;

	vhcd_vp->vusb = vusb;
	vhcd_vp->vp_idx = vp_idx;
	spin_lock_init(&vhcd_vp->lock);

	/* Pre-allocate all port structs. Reused across connect/disconnect. */
	for (i = 0; i < VIRTIO_USB_VP_MAX_PORTS; i++) {
		struct virtio_usb_hc_port *port = &vhcd_vp->ports[i];

		port->vhcd_vp = vhcd_vp;
		INIT_LIST_HEAD(&port->pending_urb_list);
		spin_lock_init(&port->lock);
		port->port_id = i;
		port->ss.status.bits.link_state = 0x05; /* RX_DETECT */
	}
	for (i = 0; i < VIRTIO_USB_VQ_HOST_MAX; i++)
		vhcd_vp->hcqs[i] = &vusb->vqueues[vusb->host_vq_base + i];

	/* Install into the port before add_hcd so vhcd_vp->vusb is set */
	vusb->vports[vp_idx].vhc = vhcd_vp;

	/* Add HCDs first so hs/ss are valid before any PORT_CONNECTED event */
	rc = virtio_usb_add_hcd(vusb, vhcd_vp);
	if (rc) {
		vusb->vports[vp_idx].vhc = NULL;
		return rc;
	}

	return 0;
}

/**
 * virtio_usb_hc_vp_deinit() - Deinitialize the host controller for one VP.
 * @vusb:   VirtIO USB device
 * @vp_idx: VP index
 */
int virtio_usb_hc_vp_deinit(struct virtio_usb *vusb, unsigned int vp_idx)
{
	struct virtio_usb_hc_vp *vhcd_vp = vusb->vports[vp_idx].vhc;

	if (!vhcd_vp)
		return 0;

	usb_remove_hcd(vhcd_vp->ss);
	usb_put_hcd(vhcd_vp->ss);
	usb_remove_hcd(vhcd_vp->hs);
	usb_put_hcd(vhcd_vp->hs);

	vhcd_vp->ss = NULL;
	vhcd_vp->hs = NULL;

	vusb->vports[vp_idx].vhc = NULL;
	return 0;
}

/**
 * virtio_usb_hc_evt_process_one() - Process a single host port event.
 * @uevent: VirtIO usb host controller event.
 *
 * Context: Process context (called from virtio_usb_hc_evt_work()).
 */
static void virtio_usb_hc_evt_process_one(struct virtio_usb_event *uevent)
{
	struct virtio_usb *vusb = uevent->vusb;
	struct virtio_usb_host_port_event *evt;
	unsigned int vp_idx, port_id;
	struct virtio_usb_hc_vp *vhcd_vp;
	struct virtio_usb_hc_port *port;
	struct usb_hcd *hcd = NULL;
	unsigned long iflags;

	evt = (struct virtio_usb_host_port_event *)virtio_usb_event_buf(uevent);
	vp_idx = le32_to_cpu(evt->vp_idx);
	port_id = le32_to_cpu(evt->port_id);

	if (vp_idx >= vusb->nports || !vusb->vports) {
		dev_err_ratelimited(
			&vusb->vdev->dev,
			"virtio_usb: PORT event vp_idx %u invalid (nports=%u), ignoring\n",
			vp_idx, vusb->nports);
		return;
	}

	if (port_id >= VIRTIO_USB_VP_MAX_PORTS) {
		dev_err_ratelimited(
			&vusb->vdev->dev,
			"virtio_usb: PORT event port_id %u >= VP_MAX_PORTS %u, ignoring\n",
			port_id, VIRTIO_USB_VP_MAX_PORTS);
		return;
	}

	vhcd_vp = vusb->vports[vp_idx].vhc;
	if (!vhcd_vp) {
		dev_err_ratelimited(
			&vusb->vdev->dev,
			"virtio_usb: PORT event vp_idx %u not host-role, ignoring\n",
			vp_idx);
		return;
	}

	port = &vhcd_vp->ports[port_id];

	spin_lock_irqsave(&port->lock, iflags);
	switch (le32_to_cpu(evt->code)) {
	case VIRTIO_USB_EVT_HOST_PORT_CONNECTED:
		/* Reinitialize the pre-allocated port struct in place */
		memset(&port->hs, 0, sizeof(port->hs));
		memset(&port->ss, 0, sizeof(port->ss));
		port->ss.status.bits.link_state = 0x05; /* RX_DETECT */

		switch (le32_to_cpu(evt->speed)) {
		case USB_SPEED_SUPER_PLUS:
		case USB_SPEED_SUPER:
			port->ss.status.bits.connect = 1;
			port->ss.status.bits.enable = 1;
			port->ss.status.bits.link_state = 0x00; /* U0 */
			port->ss.change.bits.connect = 1;
			port->ss.change.bits.link_state = 1;
			hcd = vhcd_vp->ss;
			break;
		case USB_SPEED_HIGH:
		case USB_SPEED_FULL:
		case USB_SPEED_LOW:
			port->hs.status.bits.connect = 1;
			port->hs.change.bits.connect = 1;
			if (le32_to_cpu(evt->speed) == USB_SPEED_HIGH)
				port->hs.status.bits.high_speed = 1;
			if (le32_to_cpu(evt->speed) == USB_SPEED_LOW)
				port->hs.status.bits.low_speed = 1;
			hcd = vhcd_vp->hs;
			break;
		default:
			break;
		}
		port->speed = le32_to_cpu(evt->speed);
		break;

	case VIRTIO_USB_EVT_HOST_PORT_DISCONNECTED:
		switch (port->speed) {
		case USB_SPEED_SUPER_PLUS:
		case USB_SPEED_SUPER:
			port->ss.status.bits.connect = 0;
			port->ss.status.bits.enable = 0;
			port->ss.status.bits.link_state = 0x05; /* RX_DETECT */
			port->ss.change.bits.connect = 1;
			port->ss.change.bits.link_state = 1;
			hcd = vhcd_vp->ss;
			break;
		case USB_SPEED_HIGH:
		case USB_SPEED_FULL:
		case USB_SPEED_LOW:
			port->hs.status.bits.connect = 0;
			port->hs.status.bits.enable = 0;
			port->hs.status.bits.low_speed = 0;
			port->hs.status.bits.high_speed = 0;
			port->hs.change.bits.connect = 1;
			port->hs.change.bits.enable = 1;
			if (port->hs.status.bits.suspend) {
				port->hs.status.bits.suspend = 0;
				port->hs.change.bits.suspend = 1;
			}
			hcd = vhcd_vp->hs;
			break;
		default:
			break;
		}
		port->speed = USB_SPEED_UNKNOWN;
		break;
	}
	spin_unlock_irqrestore(&port->lock, iflags);

	if (hcd)
		usb_hcd_poll_rh_status(hcd);
}

/**
 * virtio_usb_hc_evt_work() - Host event queue receive worker.
 * @work: kernel work item embedded in struct virtio_usb.
 *
 * The host event queue is shared across all host-role VPs and its VP
 * may not even exist yet at probe time (e.g. a dual-role instance
 * where every port currently reports device role), so events are
 * drained and processed here, in process context, rather than
 * directly inside the interrupt-context notify callback.
 *
 * Context: Process context.
 */
void virtio_usb_hc_evt_work(struct work_struct *work)
{
	struct virtio_usb *vusb =
		container_of(work, struct virtio_usb, vq_host_evt_work);
	struct virtio_usb_queue *evtq =
		&vusb->vqueues[vusb->host_vq_base + VIRTIO_USB_VQ_EVENT_IDX];

	virtio_usb_evt_work(evtq, virtio_usb_hc_evt_process_one);
}

/**
 * virtio_usb_hc_dataq_stop_cb() - Stop data virtqueue, force-complete all
 * pending URBs with -ESHUTDOWN.
 */
static void virtio_usb_hc_dataq_stop_cb(struct virtio_usb *vusb,
					struct virtio_usb_queue *dataq)
{
	struct virtio_usb_data *vurb, *vurb_tmp;
	struct virtio_usb_hc_priv *priv;
	struct usb_hcd *hcd;
	unsigned int vp_idx, slot;
	unsigned long flags;

	virtio_usb_dataq_stop_cb(vusb, dataq);

	if (!vusb->vports)
		return;

	/* The data virtqueue is shared across all host-role VPs, so the
	 * work item that drains it lives on struct virtio_usb itself.
	 */
	cancel_work_sync(&vusb->vq_host_data_rx_work);

	for (vp_idx = 0; vp_idx < vusb->nports; vp_idx++) {
		struct virtio_usb_hc_vp *vhcd_vp = vusb->vports[vp_idx].vhc;

		if (!vhcd_vp)
			continue;

		for (slot = 0; slot < VIRTIO_USB_VP_MAX_PORTS; slot++) {
			struct virtio_usb_hc_port *port = &vhcd_vp->ports[slot];
			LIST_HEAD(giveback_list);

			spin_lock_irqsave(&port->lock, flags);
			list_for_each_entry_safe(
				vurb, vurb_tmp, &port->pending_urb_list, list) {
				priv = (struct virtio_usb_hc_priv *)vurb->priv;
				if (!priv->urb) {
					list_move_tail(&vurb->list,
						       &giveback_list);
					continue;
				}
				if (!priv->urb->dev) {
					usb_hcd_unlink_urb_from_ep(
						port_vhcd_get(port), priv->urb);
					priv->urb->hcpriv = NULL;
					list_move_tail(&vurb->list,
						       &giveback_list);
					continue;
				}
				hcd = bus_to_hcd(priv->urb->dev->bus);
				usb_hcd_unlink_urb_from_ep(hcd, priv->urb);
				priv->urb->hcpriv = NULL;
				list_move_tail(&vurb->list, &giveback_list);
			}
			spin_unlock_irqrestore(&port->lock, flags);

			list_for_each_entry_safe(vurb, vurb_tmp, &giveback_list,
						 list) {
				priv = (struct virtio_usb_hc_priv *)vurb->priv;
				if (!priv->urb || !priv->urb->dev) {
					list_del(&vurb->list);
					kfree(priv->sgs);
					virtio_usb_data_unref(vurb);
					continue;
				}
				hcd = bus_to_hcd(priv->urb->dev->bus);
				priv->urb->status = -ESHUTDOWN;
				list_del(&vurb->list);
				local_bh_disable();
				usb_hcd_giveback_urb(hcd, priv->urb,
						     priv->urb->status);
				local_bh_enable();
				kfree(priv->sgs);
				virtio_usb_data_unref(vurb);
			}
		}
	}
}

/**
 * virtio_usb_hc_evt_notify_cb() - Event virtqueue notification callback.
 *
 * Just schedules virtio_usb_hc_evt_work() - the actual event processing
 * needs process context, since it may end up reading vhc concurrently
 * with an OTG-triggered virtio_usb_hc_vp_init()/_deinit(), which sleep.
 *
 * Context: Interrupt context.
 */
static void virtio_usb_hc_evt_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_usb *vusb = vqueue->vdev->priv;

	schedule_work(&vusb->vq_host_evt_work);
}

/**
 * virtio_usb_hc_evtq_stop_cb() - Stop the host event virtqueue.
 *
 * Do not process host port events during teardown - the vhc they'd
 * reference may already be gone. Just cancel the (idempotent)
 * work item and drain the used ring so del_vqs() finds it empty.
 */
static void virtio_usb_hc_evtq_stop_cb(struct virtio_usb *vusb,
				       struct virtio_usb_queue *vq)
{
	virtio_usb_evt_drain_stop_cb(vq, &vusb->vq_host_evt_work);
}

/**
 * virtio_usb_host_data_notify_cb() - Data virtqueue notification callback.
 *
 * The data virtqueue is shared across all host-role VPs; the work item
 * that drains it lives on struct virtio_usb, not on any specific VP.
 */
static void virtio_usb_host_data_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_usb *vusb = vqueue->vdev->priv;

	schedule_work(&vusb->vq_host_data_rx_work);
}

const struct virtio_usb_vq_desc host_vqueues[VIRTIO_USB_VQ_HOST_MAX] = {
	[VIRTIO_USB_VQ_COMMAND_IDX] = {
			.callback = virtio_usb_cmd_notify_cb,
			.name = "virtusb-host-cmd",
			.process = virtio_usb_cmd_process_cb,
			.stop = virtio_usb_cmdq_stop_cb,
			},
	[VIRTIO_USB_VQ_EVENT_IDX] = {
			.callback = virtio_usb_hc_evt_notify_cb,
			.name = "virtusb-host-evt",
			.process = NULL,
			.stop = virtio_usb_hc_evtq_stop_cb,
			},
	[VIRTIO_USB_VQ_DATA_IDX] = {
			.callback = virtio_usb_host_data_notify_cb,
			.name = "virtusb-host-data",
			.process = NULL,
			.stop = virtio_usb_hc_dataq_stop_cb,
			},
};
