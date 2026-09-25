/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   ALSA driver for Reloop Jockey 3 devices
 *   Ploytec USB Protocol Handling
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 *
 *   Comments here and in ploytec_proto.c cite documents by paths beginning
 *   "re/". Those are the reverse-engineering notes this protocol layer was
 *   derived from; they are not part of the kernel tree but live in the driver's
 *   development repository, linked from its MAINTAINERS entry.
 */

#ifndef __SOUND_USB_JOCKEY3_PLOYTEC_PROTO_H
#define __SOUND_USB_JOCKEY3_PLOYTEC_PROTO_H

#include <linux/types.h>
#include <linux/usb.h>

#define USB_XFER_BUF_SIZE		64	// temporary buffer for USB control transfers

/* Packet format/structure */
#define PLOYTEC_PKT_SIZE		512
#define PLOYTEC_MIDI_OUT_OFFSET		480	// location of MIDI data in the playback packet
#define PLOYTEC_MIDI_IDLE_BYTE		0xFD	// send when no MIDI data is available
#define PLOYTEC_SYNC_BYTE_OFFSET	481
#define PLOYTEC_SYNC_BYTE_VALUE		0xFF

/* USB Endpoint numbers*/
#define PLOYTEC_EP_NUM_PCM_OUT		0x05	// Playback & MIDI Out (EP 0x05)
#define PLOYTEC_EP_NUM_PCM_IN		0x06	// Capture (EP 0x86)
#define PLOYTEC_EP_NUM_MIDI_IN		0x03	// MIDI In (EP 0x83)

/* Protocol Commands */
#define PLOYTEC_SET_RATE		0x01	// bRequest to set sample rate
#define PLOYTEC_SET_RATE_TYPE		0x22	// bmRequestType to set sample rate
#define PLOYTEC_SET_STATUS		0x49	// bRequest to set device status
#define PLOYTEC_SET_STATUS_TYPE		0x40	// bmRequestType to set device status
#define PLOYTEC_REQ_STATUS		0x49	// bRequest to get device status
#define PLOYTEC_REQ_STATUS_TYPE		0xC0	// bmRequestType to get device status
#define PLOYTEC_REQ_FIRMWARE		0x56	// bRequest to get firmware version
#define PLOYTEC_REQ_FIRMWARE_TYPE	0xC0	// bmRequestType to get firmware version
#define PLOYTEC_REQ_GET_RATE		0x81	// bRequest to get current sample rate
#define PLOYTEC_REQ_GET_RATE_TYPE	0xA2	// bmRequestType to get current sample rate

/*
 * wIndex values for the rate requests: zero addresses the device as a whole,
 * an endpoint address addresses one stream. Both forms are needed -- see
 * re/protocol_analysis.md.
 */
#define PLOYTEC_RATE_IDX_DEVICE		0x0000	// device-wide, used before programming
#define PLOYTEC_RATE_IDX_PCM_IN		(PLOYTEC_EP_NUM_PCM_IN | USB_DIR_IN)	// 0x86
#define PLOYTEC_RATE_IDX_PCM_OUT	(PLOYTEC_EP_NUM_PCM_OUT)		// 0x05

/* Status Bits (bits 0-4 are observed but not understood, and unused) */
#define PLOYTEC_STATUS_STREAMING	0x20

/* Timeout for the EP0 control transfers above, in milliseconds */
#define PLOYTEC_CTRL_TIMEOUT_MS		2000

/*
 * Protocol Helpers
 *
 * All of these take the USB interface rather than the USB device so that
 * dev_dbg()/dev_err()/dev_warn() attribute log messages to our driver
 * (e.g. "snd-reloop-jockey3 1-13:1.0: ...") instead of to usbcore's generic
 * per-device node (e.g. "usb 1-13: ..."), which is what struct usb_device's
 * embedded struct device is bound to.
 */
int ploytec_initialize_device(struct usb_interface *intf, void *xfer_buf,
			      bool bounce_alt0, u32 *fw_version);
int ploytec_start_streaming(struct usb_interface *intf, void *xfer_buf);
int ploytec_get_rate(struct usb_interface *intf, void *xfer_buf, u16 index, u32 *rate);
int ploytec_set_rate(struct usb_interface *intf, void *xfer_buf, u32 rate, bool cold_init);
int ploytec_get_firmware(struct usb_interface *intf, void *xfer_buf, u32 *fw_version);
int ploytec_get_status(struct usb_interface *intf, void *xfer_buf, u8 *status);

#endif /* __SOUND_USB_JOCKEY3_PLOYTEC_PROTO_H */
