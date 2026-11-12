/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#ifndef FL2000_H
#define FL2000_H

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/usb.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper_vtables.h>

#include "fl2000_registers.h"

/* Known USB interfaces of FL2000 */
enum fl2000_interface {
	FL2000_USBIF_AVCONTROL = 0,
	FL2000_USBIF_STREAMING = 1,
	FL2000_USBIF_INTERRUPT = 2,
	FL2000_USBIF_NUM,
};

/* Called from URB completion handlers, i.e. in atomic context */
static inline int fl2000_urb_status(struct usb_device *usb_dev, int status)
{
	switch (status) {
	case 0:
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* Success, or the URB was unlinked / the device is going away */
		break;
	default:
		dev_err_ratelimited(&usb_dev->dev, "Nonzero urb status, %d\n", status);
		break;
	}

	return status;
}

struct fl2000_timings {
	u32 hactive;
	u32 htotal;
	u32 hsync_width;
	u32 hstart;
	u32 vactive;
	u32 vtotal;
	u32 vsync_width;
	u32 vstart;
};

struct fl2000_pll {
	u32 prescaler;
	u32 multiplier;
	u32 divisor;
	u32 function;
};

/* Polling interval and timeout in us for I2C read/write operations */
#define I2C_RDWR_INTERVAL 200
#define I2C_RDWR_TIMEOUT  (256 * 1000)

/* Streaming transfer task */
struct fl2000_stream;
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev);
void fl2000_stream_destroy(struct fl2000_stream *stream);

/* Streaming interface */
int fl2000_stream_mode_set(struct fl2000_stream *stream, unsigned int width,
			   unsigned int height, u32 bytes_pix);
struct drm_rect;
struct iosys_map;
void fl2000_stream_compress(struct fl2000_stream *stream, const struct iosys_map *src_map,
			    bool imported, unsigned int pitch,
			    const struct drm_rect *src, const struct drm_rect *clip);
void fl2000_stream_note_render(struct fl2000_stream *stream, unsigned int conv_us,
			       unsigned int damage_px);

int fl2000_stream_enable(struct fl2000_stream *stream);
void fl2000_stream_disable(struct fl2000_stream *stream);

/* Interrupt polling task */
struct fl2000_intr;
struct fl2000_intr *fl2000_intr_create(struct usb_device *usb_dev, struct regmap *regmap,
				       struct drm_device *drm);
void fl2000_intr_quiesce(struct fl2000_intr *intr);
void fl2000_intr_suspend(struct fl2000_intr *intr);
int fl2000_intr_start(struct fl2000_intr *intr);
int fl2000_intr_monitor_connected(struct fl2000_intr *intr);
void fl2000_intr_destroy(struct fl2000_intr *intr);

/* VGA output: a DAC encoder and connector next to the HDMI ones, on the same CRTC */
struct fl2000_vga {
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct i2c_adapter *adapter;
	struct fl2000_intr *intr;
	/* The HDMI transmitter shares the chip's monitor sense with this output */
	struct drm_bridge *hdmi_bridge;
};

int fl2000_vga_init(struct drm_device *drm, struct fl2000_vga *vga, struct drm_crtc *crtc,
		    struct drm_encoder *hdmi_encoder, struct i2c_adapter *adapter,
		    struct fl2000_intr *intr);

/* I2C adapter interface creation */
struct i2c_adapter *fl2000_i2c_init(struct usb_interface *interface, struct regmap *regmap);
int fl2000_ddc_read_block(struct i2c_adapter *adapter, u8 *buf, unsigned int block, size_t len);

/* Register map creation */
struct regmap *fl2000_regmap_init(struct device *dev, struct usb_device *usb_dev);
void fl2000_regmap_drop_cache(struct regmap *regmap);

/* Registers interface */
int fl2000_reset(struct regmap *regmap);
int fl2000_usb_magic(struct regmap *regmap);
int fl2000_afe_magic(struct regmap *regmap);
int fl2000_set_transfers(struct regmap *regmap);
int fl2000_set_pixfmt(struct regmap *regmap, u32 bytes_pix);
int fl2000_set_timings(struct regmap *regmap, struct fl2000_timings *timings);
int fl2000_set_pll(struct regmap *regmap, struct fl2000_pll *pll);
int fl2000_force_connect(struct regmap *regmap, bool force);
bool fl2000_force_connect_active(struct regmap *regmap);
int fl2000_enable_sink_interrupts(struct regmap *regmap);
int fl2000_stream_interrupts(struct regmap *regmap, bool enable);
int fl2000_read_status(struct regmap *regmap, u32 *status);
int fl2000_i2c_dword(struct regmap *regmap, bool read, u16 addr, u8 offset, u32 *data);

/* DRM device creation */
struct fl2000_device;
struct fl2000_device *fl2000_drm_create(struct usb_interface *interface, struct regmap *regmap,
					struct i2c_adapter *adapter, struct drm_bridge *bridge);
void fl2000_drm_destroy(struct fl2000_device *fdev);
void fl2000_drm_hotplug(struct drm_device *drm, u32 events);
int fl2000_drm_suspend(struct fl2000_device *fdev);
int fl2000_drm_resume(struct fl2000_device *fdev);

#endif /* FL2000_H */
