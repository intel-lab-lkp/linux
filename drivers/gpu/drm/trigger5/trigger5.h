/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TRIGGER5_H__
#define __TRIGGER5_H__

#include <linux/completion.h>
#include <linux/scatterlist.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>

#define DRIVER_NAME		"trigger5"
#define DRIVER_DESC		"MCT Trigger 5 USB display adapter"

#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0

struct trigger5_transfer {
	struct trigger5_device *trigger5;

	void *frame_data;
	size_t frame_len;
	size_t frame_alloc_len;
	struct drm_rect transfer_rect;

	struct sg_table transfer_sgt;
	struct timer_list timer;
	struct usb_sg_request sgr;

	struct work_struct transfer_work;
	struct completion frame_complete;
};

struct trigger5_device {
	struct drm_device drm;
	struct usb_interface *intf;
	unsigned int bulk_pipe;

	struct drm_connector connector;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;

	u16 frame_counter;

	int current_transfer;
	struct drm_rect pending_rect;
	struct workqueue_struct *transfer_wq;
	bool display_enabled;
	struct delayed_work keepalive_work;
	struct trigger5_transfer transfers[2];
};

struct trigger5_pll {
	u8 prediv;
	u8 mul1;
	u8 mul2;
	u8 div1;
	u8 div2;
} __packed;

struct trigger5_mode_request {
	__be16 height;
	__be16 width;
	__be16 line_total_pixels; /* minus one */
	__be16 line_sync_pulse; /* minus one */
	__be16 line_back_porch; /* minus one */
	__be16 unknown1;
	__be16 unknown2;
	__be16 width_minus_one;
	__be16 frame_total_lines; /* minus one */
	__be16 frame_sync_pulse; /* minus one */
	__be16 frame_back_porch; /* minus one */
	__be16 unknown3;
	__be16 unknown4;
	__be16 height_minus_one;
	struct trigger5_pll pll;
	u8 hsync_polarity;
	u8 vsync_polarity;
} __packed;

/* gm12u320.c uses the same header format */
struct trigger5_bulk_header {
	u8 magic; /* 0xfb */
	u8 length; /* 0x14 */
	__le16 counter; /* lower 12-bit counter, upper 4-bit packet flags */
	__le16 horizontal_offset; /* lower 13-bit offset, upper 3-bit unknown */
	__le16 vertical_offset; /* lower 13-bit offset, upper 3-bit unknown */
	__le16 width; /* lower 13-bit width, upper 3-bit unknown */
	__le16 height; /* lower 13-bit height, upper 3-bit unknown */
	__le32 payload_length; /* lower 28-bit length, upper 4-bit flags */
	u8 flags; /* bit 0 must be set */
	u8 unknown1;
	u8 unknown2;
	u8 checksum;
} __packed;

#define TRIGGER5_REQUEST_KEEPALIVE		0x91
#define TRIGGER5_REQUEST_GET_REGISTER		0xA5
#define TRIGGER5_REQUEST_GET_STATUS		0xA6
#define TRIGGER5_REQUEST_GET_EDID		0xA8
#define TRIGGER5_REQUEST_SET_MODE		0xC3
#define TRIGGER5_REQUEST_SET_REGISTER		0xC4
#define TRIGGER5_REQUEST_SET_CURSOR_POSITION	0xC8
#define TRIGGER5_REQUEST_FIRMWARE_RESET		0xD1

#define TRIGGER5_KEEPALIVE_INTERVAL_MS	2000
#define TRIGGER5_BULK_TIMEOUT_MS		5000

#define to_trigger5(x) container_of(x, struct trigger5_device, drm)

int trigger5_connector_init(struct trigger5_device *trigger5,
			    int connector_type);
#endif /* __TRIGGER5_H__ */
