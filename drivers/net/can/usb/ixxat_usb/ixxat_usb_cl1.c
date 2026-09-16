// SPDX-License-Identifier: GPL-2.0
/* CAN driver adapter for IXXAT USB-to-CAN CL1
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

#define IXXAT_USB_CLOCK			8000000

#define IXXAT_USB_BUFFER_SIZE_RX	512
#define IXXAT_USB_BUFFER_SIZE_TX	256

#define IXXAT_USB_MODES			(CAN_CTRLMODE_BERR_REPORTING | \
					 CAN_CTRLMODE_3_SAMPLES | \
					 CAN_CTRLMODE_LOOPBACK | \
					 CAN_CTRLMODE_LISTENONLY)

#define IXXAT_USB_BTMODE_TSM_CL1	0x80

/* USB endpoint mapping for CL1 */
#define IXXAT_USB2CAN_EP1_IN		(1 | USB_DIR_IN)
#define IXXAT_USB2CAN_EP2_IN		(2 | USB_DIR_IN)
#define IXXAT_USB2CAN_EP3_IN		(3 | USB_DIR_IN)
#define IXXAT_USB2CAN_EP4_IN		(4 | USB_DIR_IN)
#define IXXAT_USB2CAN_EP5_IN		(5 | USB_DIR_IN)

#define IXXAT_USB2CAN_EP1_OUT		(1 | USB_DIR_OUT)
#define IXXAT_USB2CAN_EP2_OUT		(2 | USB_DIR_OUT)
#define IXXAT_USB2CAN_EP3_OUT		(3 | USB_DIR_OUT)
#define IXXAT_USB2CAN_EP4_OUT		(4 | USB_DIR_OUT)
#define IXXAT_USB2CAN_EP5_OUT		(5 | USB_DIR_OUT)

#define IXXAT_USB_CAN_CMD_GETCAPS	0x320
#define IXXAT_USB_CAN_CMD_INIT		0x325

static const struct can_bittiming_const usb2can_bt = {
	.name = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 16,
	.tseg2_min = 1,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 64,
	.brp_inc = 1,
};

/* ixxat_usb_get_ctrl_caps - get controller capabilities
 * @dev: pointer to the IXXAT USB CAN device
 * @caps: pointer to the structure to store capabilities (can be NULL)
 * This function retrieves the capabilities of the IXXAT USB CAN controller.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int ixxat_usb_get_ctrl_caps(struct ixxat_usb_candevice *dev,
				   struct ixxat_cancaps2 *caps)
{
	struct ixxat_usb_getcaps_cl1_cmd cmd = { 0 };
	const u16 port = dev->ctrl_index;
	const u32 req_size = sizeof(cmd.req);
	const u32 rcv_size = sizeof(cmd) - req_size;
	const u32 cmd_size = req_size + sizeof(cmd.res);
	int err;

	ixxat_usb_setup_cmd(&cmd.req, &cmd.res);
	cmd.req.code = cpu_to_le32(IXXAT_USB_CAN_CMD_GETCAPS);
	cmd.req.port = cpu_to_le16(port);
	cmd.res.res_size = cpu_to_le32(rcv_size);
	memset(&cmd.caps, 0, sizeof(cmd.caps));

	err = ixxat_usb_send_cmd(dev, port,
				 &cmd.req, cmd_size,
				 &cmd.res, rcv_size,
				 IXXAT_USB_CMD_TIMEOUT);
	if (!err && caps) {
		memset(caps, 0, sizeof(*caps));

		caps->ctrltype = cmd.caps.ctrltype;
		caps->buscoupling = cmd.caps.buscoupling;
		caps->features = cmd.caps.features;
		caps->can_clock_freq = cmd.caps.can_clock_freq;

		/* these are not available in CL1
		 *  caps->sdr_range_min
		 *  caps->sdr_range_max
		 *  caps->fdr_range_min
		 *  caps->fdr_range_max
		 */
		caps->ts_clock_freq = cmd.caps.can_clock_freq;
		caps->ts_clock_divisor = cmd.caps.ts_clock_divisor;

		caps->cms_clock_freq = cmd.caps.can_clock_freq;
		caps->cms_clock_divisor = cmd.caps.cms_clock_divisor;
		caps->cms_max_ticks = cmd.caps.cms_max_ticks;

		caps->dtx_clock_freq = cmd.caps.can_clock_freq;
		caps->dtx_clock_divisor = cmd.caps.dtx_clock_divisor;
		caps->dtx_max_ticks = cmd.caps.dtx_max_ticks;
	}

	return err;
}

/* ixxat_usb_init_ctrl - initialize the controller
 * @dev: pointer to the IXXAT USB CAN device
 * This function initializes the IXXAT USB CAN controller with the specified
 * bittiming parameters and control modes.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int ixxat_usb_init_ctrl(struct ixxat_usb_candevice *dev)
{
	struct ixxat_usb_init_cl1_cmd cmd = { 0 };
	const u16 port = dev->ctrl_index;
	const u32 rcv_size = sizeof(cmd.res);
	const u32 cmd_size = sizeof(cmd);
	const struct can_bittiming *bt = &dev->can.bittiming;
	u8 opmode = IXXAT_USB_OPMODE_EXTENDED | IXXAT_USB_OPMODE_STANDARD;
	u8 btr0 = ((bt->brp - 1) & 0x3f) | (((bt->sjw - 1) & 0x3) << 6);
	u8 btr1 = ((bt->prop_seg + bt->phase_seg1 - 1) & 0xf) |
		  (((bt->phase_seg2 - 1) & 0x7) << 4);

	dev->loopback = ((dev->can.ctrlmode & CAN_CTRLMODE_LOOPBACK) > 0);

	if (dev->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES)
		btr1 |= IXXAT_USB_BTMODE_TSM_CL1;
	if (dev->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING)
		opmode |= IXXAT_USB_OPMODE_ERRFRAME;
	if (dev->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		opmode |= IXXAT_USB_OPMODE_LISTONLY;

	ixxat_usb_setup_cmd(&cmd.req, &cmd.res);
	cmd.req.size = cpu_to_le32(cmd_size - rcv_size);
	cmd.req.code = cpu_to_le32(IXXAT_USB_CAN_CMD_INIT);
	cmd.req.port = cpu_to_le16(port);
	cmd.mode = opmode;
	cmd.btr0 = btr0;
	cmd.btr1 = btr1;

	return ixxat_usb_send_cmd(dev, port,
				  &cmd.req, cmd_size,
				  &cmd.res, rcv_size,
				  IXXAT_USB_CMD_TIMEOUT);
}

const struct ixxat_usb_adapter usb2can_cl1 = {
	.clock = IXXAT_USB_CLOCK,
	.bt = &usb2can_bt,
	.modes = IXXAT_USB_MODES,
	.buffer_size_rx = IXXAT_USB_BUFFER_SIZE_RX,
	.buffer_size_tx = IXXAT_USB_BUFFER_SIZE_TX,
	.cl1_msg_format = true,
	.ep_msg_in = {
		IXXAT_USB2CAN_EP1_IN,
		IXXAT_USB2CAN_EP2_IN,
		IXXAT_USB2CAN_EP3_IN,
		IXXAT_USB2CAN_EP4_IN,
		IXXAT_USB2CAN_EP5_IN
	},
	.ep_msg_out = {
		IXXAT_USB2CAN_EP1_OUT,
		IXXAT_USB2CAN_EP2_OUT,
		IXXAT_USB2CAN_EP3_OUT,
		IXXAT_USB2CAN_EP4_OUT,
		IXXAT_USB2CAN_EP5_OUT
	},
	.get_ctrl_caps = ixxat_usb_get_ctrl_caps,
	.init_ctrl = ixxat_usb_init_ctrl
};
