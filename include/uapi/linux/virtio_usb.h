/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _UAPI_LINUX_VIRTIO_USB_H
#define _UAPI_LINUX_VIRTIO_USB_H

#include <linux/virtio_types.h>

/*****************************************************************************
 * FEATURE BITS
 */
enum {
	/* Host controller is supported */
	VIRTIO_USB_F_HOST = 0,
	/* Device controller is supported */
	VIRTIO_USB_F_DEVICE,
	/* Role switching is supported */
	VIRTIO_USB_F_SWITCH_ROLE,
};

/*****************************************************************************
 * CONFIGURATION SPACE
 */
struct virtio_usb_config {
	/* # of available ports */
	__le32 ports;
};

/*****************************************************************************
 * COMMON DEFINITIONS
 */
enum {
	VIRTIO_USB_VQ_HOST_COMMAND = 0,
	VIRTIO_USB_VQ_HOST_EVENT,
	VIRTIO_USB_VQ_HOST_DATA,
	VIRTIO_USB_VQ_DEV_COMMAND,
	VIRTIO_USB_VQ_DEV_EVENT,
	VIRTIO_USB_VQ_DEV_DATA,
	VIRTIO_USB_VQ_OTG_COMMAND,
	VIRTIO_USB_VQ_OTG_EVENT,
	VIRTIO_USB_VQ_MAX,
};

enum {
	VIRTIO_USB_EP_CONTROL = 0,
	VIRTIO_USB_EP_INTERRUPT,
	VIRTIO_USB_EP_BULK,
	VIRTIO_USB_EP_ISOCHRONOUS,
};

enum {
	VIRTIO_USB_S_OK = 0,
	VIRTIO_USB_S_ERR_BAD_MSG,
	VIRTIO_USB_S_ERR_INTERNAL,
	VIRTIO_USB_S_ERR_NO_DEVICE,
	VIRTIO_USB_S_ERR_ISO_XFER,
	VIRTIO_USB_S_ERR_ISO_BIG,
	VIRTIO_USB_S_ERR_DATA_IN,
	VIRTIO_USB_S_ERR_DATA_OUT,
	VIRTIO_USB_S_ERR_MSG_SIZE,
	VIRTIO_USB_S_ERR_OVERFLOW,
	VIRTIO_USB_S_ERR_STALL,
	VIRTIO_USB_S_ERR_SHORT_PKT,
	VIRTIO_USB_S_ERR_CANCELLED,
};

struct virtio_usb_cmd_status {
	__le32 code; /* VIRTIO_USB_S_XXX */
} __packed;

/*****************************************************************************
 * HOST COMMAND MESSAGES
 */
enum {
	VIRTIO_USB_CMD_HOST_CANCEL = 0,
	VIRTIO_USB_CMD_HOST_STREAMS_ALLOC,
	VIRTIO_USB_CMD_HOST_STREAMS_FREE,
};

struct virtio_usb_host_cmd_hdr {
	__le32 code; /* VIRTIO_USB_CMD_HOST_XXX */
	__le32 port; /* Port ID */
};

/* VIRTIO_USB_CMD_HOST_CANCEL */
struct virtio_usb_host_cmd_cancel {
	struct virtio_usb_host_cmd_hdr hdr;
	__le64 tag;
};

/* VIRTIO_USB_CMD_HOST_STREAMS_ALLOC/FREE */
struct virtio_usb_host_cmd_streams {
	struct virtio_usb_host_cmd_hdr hdr;
	__le32 number_of_streams;
	__le32 number_of_eps;
};

/*****************************************************************************
 * HOST EVENT MESSAGES
 */
enum {
	VIRTIO_USB_EVT_HOST_PORT_CONNECTED = 0,
	VIRTIO_USB_EVT_HOST_PORT_DISCONNECTED,
};

/* Maximum number of leaf-device slots per virtual port (physical socket).
 * Each VP's root hub advertises exactly this many ports to the guest hub
 * driver. Leaf devices (direct or behind a physical hub) are flattened
 * into slots 0..VIRTIO_USB_VP_MAX_PORTS-1 of their VP's root hub.
 * Must be <= USB_MAXCHILDREN (31) and <= USB_SS_MAXPORTS (15).
 */
#define VIRTIO_USB_VP_MAX_PORTS 8

/* VIRTIO_USB_EVT_HOST_PORT_CONNECTED/DISCONNECTED */
enum {
	VIRTIO_USB_SPEED_UNKNOWN = 0,
	VIRTIO_USB_SPEED_LOW,
	VIRTIO_USB_SPEED_FULL, /* usb 1.1 */
	VIRTIO_USB_SPEED_HIGH, /* usb 2.0 */
	VIRTIO_USB_SPEED_WIRELESS, /* wireless (usb 2.5) */
	VIRTIO_USB_USB_SPEED_SUPER, /* usb 3.0 */
	VIRTIO_USB_SPEED_SUPER_PLUS, /* usb 3.1 */
};

struct virtio_usb_host_port_event {
	__le32 code; /* VIRTIO_USB_EVT_HOST_PORT_XXX */
	__le32 vp_idx; /* virtual port (physical socket) index, 0..nports-1 */
	__le32 port_id; /* leaf slot within VP, 0..VIRTIO_USB_VP_MAX_PORTS-1 */
	__le32 speed; /* VIRTIO_USB_SPEED_XXX */
};

/*****************************************************************************
 * DEVICE COMMAND MESSAGES
 */
enum {
	VIRTIO_USB_CMD_DEV_GET_ENDPOINT_COUNT = 0,
	VIRTIO_USB_CMD_DEV_GET_ENDPOINT_INFO,
	VIRTIO_USB_CMD_DEV_PULLUP,
	VIRTIO_USB_CMD_DEV_VBUS_DRAW,
	VIRTIO_USB_CMD_DEV_EP_ENABLE,
	VIRTIO_USB_CMD_DEV_EP_DISABLE,
	VIRTIO_USB_CMD_DEV_EP_SET_HALT,
	VIRTIO_USB_CMD_DEV_EP_SET_WEDGE,
	VIRTIO_USB_CMD_DEV_CANCEL,
	VIRTIO_USB_CMD_DEV_GET_FRAME_NUMBER,
	VIRTIO_USB_CMD_DEV_SET_SELF_POWERED,
};

struct virtio_usb_dev_cmd_hdr {
	__le32 code; /* VIRTIO_USB_CMD_DEV_XXX */
	__le16 port; /* Port ID */
	__le16 endpoint; /* Endpoint ID */
};

/* VIRTIO_USB_CMD_DEV_CANCEL */
struct virtio_usb_dev_cmd_cancel {
	struct virtio_usb_dev_cmd_hdr hdr;
	__le64 tag;
};

/* VIRTIO_USB_CMD_DEV_GET_ENDPOINT_COUNT */
struct virtio_usb_dev_ep_count {
	struct virtio_usb_cmd_status status;
	__le32 count; /* # of supported endpoints */
};

struct virtio_usb_dev_cmd_set_value {
	struct virtio_usb_dev_cmd_hdr hdr;
	__le32 value;
	__le32 padding;
};

/* VIRTIO_USB_CMD_DEV_GET_FRAME_NUMBER */
struct virtio_usb_dev_frame_number {
	struct virtio_usb_cmd_status status;
	__le32 frame_number;
};

enum {
	VIRTIO_USB_DIR_OUT = 0,
	VIRTIO_USB_DIR_IN,
};

enum {
	VIRTIO_USB_EP_DIR_OUT = 0,
	VIRTIO_USB_EP_DIR_IN = 0x80,
};

/**
 * VIRTIO_USB_CMD_DEV_GET_ENDPOINT_INFO
 *
 * Response:
 *     struct virtio_usb_cmd_status
 *     struct virtio_usb_dev_cmd_ep_info [count]
 */
struct virtio_usb_dev_ep_info {
	__le16 types; /* supported type bit map (1 << VIRTIO_USB_EP_XXX) */
	__le16 directions /* supported direction bit map (1 << VIRTIO_USB_DIR_XXX) */;
	__le16 maxpacket_limit;
	__le16 max_streams;
	__u8 name[16];
};

/*******************************************************************************
 * DEVICE EVENT MESSAGES
 */
enum {
	VIRTIO_USB_EVT_DEV_BIND = 0,
	VIRTIO_USB_EVT_DEV_DISCONNECTED,
	VIRTIO_USB_EVT_DEV_SETUP,
	VIRTIO_USB_EVT_DEV_RESET,
	VIRTIO_USB_EVT_DEV_SUSPEND,
	VIRTIO_USB_EVT_DEV_RESUME,
	VIRTIO_USB_EVT_DEV_UNBIND,
};

struct virtio_usb_dev_event {
	__le32 code; /* VIRTIO_USB_EVT_DEV_XXX */
	__le16 port; /* Device ID */
	__u8 padding[10];
};

/* VIRTIO_USB_EVT_DEV_BIND */
struct virtio_usb_dev_bind_event {
	__le32 code; /* VIRTIO_USB_EVT_DEV_BIND */
	__le16 port; /* Device ID */
	__u8 max_speed; /* enum usb_device_speed */
	__u8 padding[9];
};

/* VIRTIO_USB_EVT_DEV_SETUP */
struct virtio_usb_dev_setup_event {
	__le32 code; /* VIRTIO_USB_EVT_DEV_SETUP */
	__le16 port; /* Device ID */
	__u8 setup[8]; /* setup packet contents */
	__u8 padding[2];
};

/*******************************************************************************
 * HOST/DEVICE DATA MESSAGES
 */
enum {
	VIRTIO_USB_FLAG_SHORT_NOT_OK = (1 << 0),
	VIRTIO_USB_FLAG_ISO_ASAP = (1 << 1),
	VIRTIO_USB_FLAG_ZERO_PACKET = (1 << 2),
};

struct virtio_usb_request {
	__le64 tag;
	__le16 vp_idx; /* virtual port (physical socket) index, host role only */
	__le16 port; /* Port ID (leaf slot within vp_idx for host role) */
	__le16 endpoint; /* Endpoint ID */
	__le16 transfer_type; /* VIRTIO_USB_EP_XXX */
	__le16 transfer_flags; /* VIRTIO_USB_FLAG_XXX */
	__le16 padding;
	union {
		/* transfer_type = VIRTIO_USB_EP_CONTROL */
		struct {
			__u8 setup[8];
		} control;
		/* transfer_type = VIRTIO_USB_EP_INTERRUPT */
		struct {
			__le32 interval;
		} interrupt;
		/* transfer_type = VIRTIO_USB_EP_BULK */
		struct {
			__le32 stream_id;
		} bulk;
		/* transfer_type = VIRTIO_USB_EP_ISOCHRONOUS */
		struct {
			__le32 interval;
			__le32 start_frame;
			__le32 number_of_packets;
			__le32 padding;
		} iso;
	};
};

struct virtio_usb_response {
	__le32 status; /* VIRTIO_USB_S_XXX */
	__le32 actual_length;
	__le32 interval; /* type = VIRTIO_USB_EP_INTERRUPT/ISOCHRONOUS */
	__le32 start_frame; /* type = VIRTIO_USB_EP_ISOCHRONOUS */
};

/**
 * ISO request:
 *     struct virtio_usb_request
 *     struct virtio_usb_iso_packet [number_of_packets]
 * ISO response:
 *     struct virtio_usb_response
 *     struct virtio_usb_iso_status [number_of_packets]
 */
struct virtio_usb_iso_packet {
	__le32 offset;
	__le32 length;
};

struct virtio_usb_iso_status {
	__le32 status; /* VIRTIO_USB_S_XXX */
	__le32 actual_length;
};

/*****************************************************************************
 * OTG COMMAND MESSAGES
 */
enum {
	VIRTIO_USB_CMD_OTG_GET_ROLE = 0,
	/* If VIRTIO_USB_F_SWITCH_ROLE is negotiated */
	VIRTIO_USB_CMD_OTG_SWITCH_ROLE,
};

struct virtio_usb_otg_cmd_hdr {
	__le32 code; /* VIRTIO_USB_CMD_OTG_XXX */
	__le32 port; /* Port ID */
};

/* VIRTIO_USB_CMD_OTG_GET_ROLE */
enum {
	VIRTIO_USB_ROLE_HOST = 0,
	VIRTIO_USB_ROLE_DEVICE,
};

struct virtio_usb_otg_cmd_role {
	struct virtio_usb_cmd_status status;
	__le32 role; /* VIRTIO_USB_ROLE_XXX */
	__le32 supported_role; /* VIRTIO_USB_F_[HOST|DEVICE|SWITCH_ROLE] */
};

/*****************************************************************************
 * OTG EVENT MESSAGES
 */
enum {
	VIRTIO_USB_EVT_OTG_CHANGE_ROLE = 0,
};

struct virtio_usb_otg_event {
	__le32 code; /* VIRTIO_USB_EVT_OTG_XXX */
	__le32 port; /* Port ID */
};

#endif /* _UAPI_LINUX_VIRTIO_USB_H */
