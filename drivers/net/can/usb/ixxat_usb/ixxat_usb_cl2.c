// SPDX-License-Identifier: GPL-2.0
/* CAN driver adapter for IXXAT USB-to-CAN CL2
 *
 * Copyright (C) 2018-2024 HMS Industrial Networks <socketcan@hms-networks.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>

#include "ixxat_usb_core.h"

#define IXXAT_USB_CLOCK			80000000
#define IXXAT_USBV2_CLOCK		36000000

#define IXXAT_USBFD_BUFFER_SIZE_RX	512
#define IXXAT_USBFD_BUFFER_SIZE_TX	512

#define IXXAT_USBV2_BUFFER_SIZE_RX	512
#define IXXAT_USBV2_BUFFER_SIZE_TX	512

#define IXXAT_USBIDM_BUFFER_SIZE_RX	512
#define IXXAT_USBIDM_BUFFER_SIZE_TX	512

#define IXXAT_USB_MODES_FD		(CAN_CTRLMODE_LISTENONLY | \
					 CAN_CTRLMODE_3_SAMPLES | \
					 CAN_CTRLMODE_LOOPBACK | \
					 CAN_CTRLMODE_BERR_REPORTING | \
					 CAN_CTRLMODE_FD | \
					 CAN_CTRLMODE_FD_NON_ISO | \
					 CAN_CTRLMODE_TDC_AUTO)

#define IXXAT_USB_MODES			(CAN_CTRLMODE_LISTENONLY | \
					 CAN_CTRLMODE_3_SAMPLES | \
					 CAN_CTRLMODE_LOOPBACK | \
					 CAN_CTRLMODE_BERR_REPORTING)

/* USB endpoint mapping for USB-to-CAN V2 */
#define IXXAT_USB2CANV2_EP1_IN		(1 | USB_DIR_IN)
#define IXXAT_USB2CANV2_EP2_IN		(2 | USB_DIR_IN)
#define IXXAT_USB2CANV2_EP3_IN		(3 | USB_DIR_IN)
#define IXXAT_USB2CANV2_EP4_IN		(4 | USB_DIR_IN)
#define IXXAT_USB2CANV2_EP5_IN		(5 | USB_DIR_IN)

#define IXXAT_USB2CANV2_EP1_OUT		(1 | USB_DIR_OUT)
#define IXXAT_USB2CANV2_EP2_OUT		(2 | USB_DIR_OUT)
#define IXXAT_USB2CANV2_EP3_OUT		(3 | USB_DIR_OUT)
#define IXXAT_USB2CANV2_EP4_OUT		(4 | USB_DIR_OUT)
#define IXXAT_USB2CANV2_EP5_OUT		(5 | USB_DIR_OUT)

/* USB endpoint mapping for USB-to-CAN FD */
#define IXXAT_USB2CANFD_EP1_IN		(1 | USB_DIR_IN)
#define IXXAT_USB2CANFD_EP2_IN		(2 | USB_DIR_IN)
#define IXXAT_USB2CANFD_EP3_IN		(3 | USB_DIR_IN)
#define IXXAT_USB2CANFD_EP4_IN		(4 | USB_DIR_IN)
#define IXXAT_USB2CANFD_EP5_IN		(5 | USB_DIR_IN)

#define IXXAT_USB2CANFD_EP1_OUT		(1 | USB_DIR_OUT)
#define IXXAT_USB2CANFD_EP2_OUT		(2 | USB_DIR_OUT)
#define IXXAT_USB2CANFD_EP3_OUT		(3 | USB_DIR_OUT)
#define IXXAT_USB2CANFD_EP4_OUT		(4 | USB_DIR_OUT)
#define IXXAT_USB2CANFD_EP5_OUT		(5 | USB_DIR_OUT)

/* USB endpoint mapping for CAN IDM */
#define IXXAT_CANIDM_EP1_IN		(2 | USB_DIR_IN)
#define IXXAT_CANIDM_EP2_IN		(4 | USB_DIR_IN)
#define IXXAT_CANIDM_EP3_IN		(6 | USB_DIR_IN)
#define IXXAT_CANIDM_EP4_IN		(8 | USB_DIR_IN)
#define IXXAT_CANIDM_EP5_IN		(10 | USB_DIR_IN)

#define IXXAT_CANIDM_EP1_OUT		(1 | USB_DIR_OUT)
#define IXXAT_CANIDM_EP2_OUT		(3 | USB_DIR_OUT)
#define IXXAT_CANIDM_EP3_OUT		(5 | USB_DIR_OUT)
#define IXXAT_CANIDM_EP4_OUT		(7 | USB_DIR_OUT)
#define IXXAT_CANIDM_EP5_OUT		(9 | USB_DIR_OUT)

#define IXXAT_USB_CAN_CMD_GETCAPS2	0x335
#define IXXAT_USB_CAN_CMD_INIT2		0x337

static const struct can_bittiming_const usb2can_v2_bt = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 16,
	.tseg2_min = 1,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 1024,
	.brp_inc = 1,
};

static const struct can_bittiming_const usb2can_fd_bt = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 256,
	.tseg2_min = 1,
	.tseg2_max = 256,
	.sjw_max = 128,
	.brp_min = 2,
	.brp_max = 513,
	.brp_inc = 1,
};

static const struct can_bittiming_const usb2can_fd_btd = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 256,
	.tseg2_min = 1,
	.tseg2_max = 256,
	.sjw_max = 128,
	.brp_min = 2,
	.brp_max = 513,
	.brp_inc = 1,
};

static const struct can_tdc_const usb2can_fd_tdc = {
	.tdcv_min = 0,
	.tdcv_max = 0,	/* Manual mode not supported */
	.tdco_min = 0,
	.tdco_max = 127,
};

static const struct can_bittiming_const canidm_bt = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 256,
	.tseg2_min = 1,
	.tseg2_max = 128,
	.sjw_max = 128,
	.brp_min = 1,
	.brp_max = 512,
	.brp_inc = 1
};

static const struct can_bittiming_const canidm_btd = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 32,
	.tseg2_min = 1,
	.tseg2_max = 16,
	.sjw_max = 8,
	.brp_min = 1,
	.brp_max = 32,
	.brp_inc = 1
};

static int ixxat_usb_get_ctrl_caps(struct ixxat_usb_candevice *dev,
				   struct ixxat_cancaps2 *caps)
{
	struct ixxat_usb_getcaps_cl2_cmd cmd = { 0 };
	const u16 port = dev->ctrl_index;
	const u32 req_size = sizeof(cmd.req);
	const u32 rcv_size = sizeof(cmd) - req_size;
	const u32 cmd_size = req_size + sizeof(cmd.res);
	int err;

	ixxat_usb_setup_cmd(&cmd.req, &cmd.res);
	cmd.req.code = cpu_to_le32(IXXAT_USB_CAN_CMD_GETCAPS2);
	cmd.req.port = cpu_to_le16(port);
	cmd.res.res_size = cpu_to_le32(rcv_size);

	err = ixxat_usb_send_cmd(dev, port,
				 &cmd.req, cmd_size,
				 &cmd.res, rcv_size,
				 IXXAT_USB_CMD_TIMEOUT);
	if (!err && caps)
		memcpy(caps, &cmd.caps, sizeof(*caps));

	return err;
}

static int ixxat_usb_init_ctrl(struct ixxat_usb_candevice *dev)
{
	struct ixxat_usb_init_cl2_cmd cmd = { 0 };
	const struct can_bittiming *bt = &dev->can.bittiming;
	const struct can_bittiming *btd = &dev->can.fd.data_bittiming;
	const u16 port = dev->ctrl_index;
	const u32 rcv_size = sizeof(cmd.res);
	const u32 cmd_size = sizeof(cmd);
	u32 btmode = IXXAT_USB_BTMODE_NAT;
	u8 opmode = IXXAT_USB_OPMODE_EXTENDED | IXXAT_USB_OPMODE_STANDARD;
	u8 exmode = 0;

	dev->loopback = ((dev->can.ctrlmode & CAN_CTRLMODE_LOOPBACK) > 0);

	if (dev->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES)
		btmode = IXXAT_USB_BTMODE_TSM;
	if (dev->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING)
		opmode |= IXXAT_USB_OPMODE_ERRFRAME;
	if (dev->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		opmode |= IXXAT_USB_OPMODE_LISTONLY;
	if (dev->can.ctrlmode & CAN_CTRLMODE_FD) {
		exmode |= IXXAT_USB_EXMODE_EXTDATA | IXXAT_USB_EXMODE_FASTDATA;

		if (!(dev->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO))
			exmode |= IXXAT_USB_EXMODE_ISOFD;
	}

	ixxat_usb_setup_cmd(&cmd.req, &cmd.res);
	cmd.req.size = cpu_to_le32(cmd_size - rcv_size);
	cmd.req.code = cpu_to_le32(IXXAT_USB_CAN_CMD_INIT2);
	cmd.req.port = cpu_to_le16(port);
	cmd.opmode = opmode;
	cmd.exmode = exmode;
	cmd.sdr.mode = cpu_to_le32(btmode);
	cmd.sdr.bps = cpu_to_le32(bt->brp);
	cmd.sdr.ts1 = cpu_to_le16(bt->prop_seg + bt->phase_seg1);
	cmd.sdr.ts2 = cpu_to_le16(bt->phase_seg2);
	cmd.sdr.sjw = cpu_to_le16(bt->sjw);
	cmd.sdr.tdo = 0;

	if (exmode) {
		cmd.fdr.tdo = cpu_to_le16(dev->can.fd.tdc.tdco);
		cmd.fdr.mode = cpu_to_le32(btmode);
		cmd.fdr.bps = cpu_to_le32(btd->brp);
		cmd.fdr.ts1 = cpu_to_le16(btd->prop_seg + btd->phase_seg1);
		cmd.fdr.ts2 = cpu_to_le16(btd->phase_seg2);
		cmd.fdr.sjw = cpu_to_le16(btd->sjw);
	}

	return ixxat_usb_send_cmd(dev, port,
				  &cmd.req, cmd_size,
				  &cmd.res, rcv_size,
				  IXXAT_USB_CMD_TIMEOUT);
}

const struct ixxat_usb_adapter usb2can_fd = {
	.clock = IXXAT_USB_CLOCK,
	.bt = &usb2can_fd_bt,
	.btd = &usb2can_fd_btd,
	.tdc = &usb2can_fd_tdc,
	.modes = IXXAT_USB_MODES_FD,
	.buffer_size_rx = IXXAT_USBFD_BUFFER_SIZE_RX,
	.buffer_size_tx = IXXAT_USBFD_BUFFER_SIZE_TX,
	.ep_msg_in = {
		IXXAT_USB2CANFD_EP1_IN,
		IXXAT_USB2CANFD_EP2_IN,
		IXXAT_USB2CANFD_EP3_IN,
		IXXAT_USB2CANFD_EP4_IN,
		IXXAT_USB2CANFD_EP5_IN
	},
	.ep_msg_out = {
		IXXAT_USB2CANFD_EP1_OUT,
		IXXAT_USB2CANFD_EP2_OUT,
		IXXAT_USB2CANFD_EP3_OUT,
		IXXAT_USB2CANFD_EP4_OUT,
		IXXAT_USB2CANFD_EP5_OUT
	},
	.ep_offs = 1,
	.get_ctrl_caps = ixxat_usb_get_ctrl_caps,
	.init_ctrl = ixxat_usb_init_ctrl
};

const struct ixxat_usb_adapter usb2can_v2 = {
	.clock = IXXAT_USBV2_CLOCK,
	.bt = &usb2can_v2_bt,
	.modes = IXXAT_USB_MODES,
	.buffer_size_rx = IXXAT_USBV2_BUFFER_SIZE_RX,
	.buffer_size_tx = IXXAT_USBV2_BUFFER_SIZE_TX,
	.ep_msg_in = {
		IXXAT_USB2CANV2_EP1_IN,
		IXXAT_USB2CANV2_EP2_IN,
		IXXAT_USB2CANV2_EP3_IN,
		IXXAT_USB2CANV2_EP4_IN,
		IXXAT_USB2CANV2_EP5_IN
	},
	.ep_msg_out = {
		IXXAT_USB2CANV2_EP1_OUT,
		IXXAT_USB2CANV2_EP2_OUT,
		IXXAT_USB2CANV2_EP3_OUT,
		IXXAT_USB2CANV2_EP4_OUT,
		IXXAT_USB2CANV2_EP5_OUT
	},
	.get_ctrl_caps = ixxat_usb_get_ctrl_caps,
	.init_ctrl = ixxat_usb_init_ctrl
};

const struct ixxat_usb_adapter can_fd_idm = {
	.clock = IXXAT_USB_CLOCK,
	.bt = &canidm_bt,
	.btd = &canidm_btd,
	.modes = IXXAT_USB_MODES_FD,
	.buffer_size_rx = IXXAT_USBIDM_BUFFER_SIZE_RX,
	.buffer_size_tx = IXXAT_USBIDM_BUFFER_SIZE_TX,
	.ep_msg_in = {
		IXXAT_CANIDM_EP1_IN,
		IXXAT_CANIDM_EP2_IN,
		IXXAT_CANIDM_EP3_IN,
		IXXAT_CANIDM_EP4_IN,
		IXXAT_CANIDM_EP5_IN
	},
	.ep_msg_out = {
		IXXAT_CANIDM_EP1_OUT,
		IXXAT_CANIDM_EP2_OUT,
		IXXAT_CANIDM_EP3_OUT,
		IXXAT_CANIDM_EP4_OUT,
		IXXAT_CANIDM_EP5_OUT
	},
	.get_ctrl_caps = ixxat_usb_get_ctrl_caps,
	.init_ctrl = ixxat_usb_init_ctrl
};
