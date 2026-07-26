// SPDX-License-Identifier: GPL-2.0-only
/*
 * Himax HM1092 monochrome infrared image sensor driver
 *
 * The sensor has a 1280x720 pixel array and streams a single 648x368 10-bit
 * mode at 30fps over a 1-lane MIPI CSI-2 link. It provides the infrared
 * stream used for face authentication.
 *
 * The device is ACPI-enumerated with HID HIMX1092 and is reached over I2C.
 * It was developed against a Dell laptop with an Intel Panther Lake IPU7,
 * where the sensor sits behind a Synaptics SVP7500 vision bridge. That
 * bridge is a control-plane device only: it transfers sensor ownership away
 * from the firmware and applies the ACPI SSDB MIPI configuration at its own
 * probe time, but it is not a member of the media graph and this driver has
 * no dependency on it. The sensor was verified to stream with the bridge
 * driver's configuration call disabled.
 *
 * Copyright (C) 2026 Jake Steinman <j@metarealtyinc.ca>
 */

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>

#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Chip identification. The silicon reports 0x1091, not 0x1092. */
#define HM1092_CHIP_ID			0x1091
#define HM1092_REG_CHIP_ID_H		CCI_REG8(0x0000)
#define HM1092_REG_CHIP_ID_L		CCI_REG8(0x0001)

/* Mode control */
#define HM1092_REG_MODE_SELECT		CCI_REG8(0x0100)
#define HM1092_MODE_STANDBY		0x00
#define HM1092_MODE_STREAMING		0x01

/* Group hold: every runtime control group is committed by writing 1 here. */
#define HM1092_REG_COMMAND_UPDATE	CCI_REG8(0x0104)

/* Exposure, 16-bit coarse integration time split over two 8-bit registers. */
#define HM1092_REG_EXPOSURE_H		CCI_REG8(0x0202)
#define HM1092_REG_EXPOSURE_L		CCI_REG8(0x0203)
#define HM1092_EXPOSURE_MIN		2
#define HM1092_EXPOSURE_MAX		720
/*
 * Defaults MUST match the state the init table's tail leaves behind
 * (0x0202/0x0203 = 0x00/0xb4, 0x0205 = 0x00, 0x020e/0x020f = 0x01/0x00).
 * The control handler setup at stream-on writes these to the sensor, so any
 * other value silently changes the configuration away from the only one this
 * silicon is known to stream with.
 */
#define HM1092_EXPOSURE_DEFAULT		180
#define HM1092_EXPOSURE_STEP		1

/* Analog gain */
#define HM1092_REG_ANALOG_GAIN		CCI_REG8(0x0205)
#define HM1092_AGAIN_MIN		0
#define HM1092_AGAIN_MAX		0xff
#define HM1092_AGAIN_DEFAULT		0
#define HM1092_AGAIN_STEP		1

/* Digital gain, 16-bit split over two 8-bit registers. */
#define HM1092_REG_DIGITAL_GAIN_H	CCI_REG8(0x020e)
#define HM1092_REG_DIGITAL_GAIN_L	CCI_REG8(0x020f)
#define HM1092_DGAIN_MIN		0x0100
#define HM1092_DGAIN_MAX		0x0fff
#define HM1092_DGAIN_DEFAULT		0x0100
#define HM1092_DGAIN_STEP		1

/* Test pattern */
#define HM1092_REG_TEST_PATTERN		CCI_REG8(0x0601)
#define HM1092_TEST_PATTERN_OFF		0x00
#define HM1092_TEST_PATTERN_ON		0x01

/*
 * Fixed output geometry.
 *
 * Every shipping module variant of this sensor streams 648x368 and nothing
 * else; the larger resolutions advertised by the vendor reference board are
 * not validated by production firmware and are not reachable through the
 * bridge. The exposed mode is the 2x2-binned window plus overscan:
 *
 *   648 = 640 + 8 columns of overscan past the binned 1280
 *   368 = 360 + 8 rows of overscan past the binned 720
 */
#define HM1092_NATIVE_WIDTH		1280
#define HM1092_NATIVE_HEIGHT		720
#define HM1092_WIDTH			648
#define HM1092_HEIGHT			368

/*
 * Line length and frame length programmed by the initialisation sequence:
 * HTS is 0x0654 in 0x0342/0x0343 and VTS is 0x02e5 in 0x0340/0x0341. The
 * blanking controls are derived from these so that userspace computing a frame
 * duration from HBLANK/VBLANK gets the timing the sensor actually runs at.
 */
#define HM1092_HTS			1620
#define HM1092_VTS			741
#define HM1092_HBLANK			(HM1092_HTS - HM1092_WIDTH)
#define HM1092_VBLANK			(HM1092_VTS - HM1092_HEIGHT)

/*
 * The sensor is physically monochrome and MEDIA_BUS_FMT_Y10_1X10 would be the
 * honest description of its output. SGRBG10 is reported instead because the
 * vendor pipeline configuration for this part declares bayer_order=GRBG, and
 * the IPU firmware graph is selected from that declaration; the mono data is
 * carried unchanged either way, and consumers ignore the Bayer order.
 *
 * This is a compatibility choice, not a hardware constraint: the IPU CSI-2
 * receiver does accept Y10. If a maintainer prefers Y10 here, the change is
 * mechanical on the driver side, but it needs a matching change wherever the
 * vendor graph configuration is consumed.
 */
#define HM1092_MBUS_CODE		MEDIA_BUS_FMT_SGRBG10_1X10

/* MIPI CSI-2 */
#define HM1092_LANES			1

/*
 * V4L2_CID_LINK_FREQ is the DDR clock, i.e. half the per-lane bit rate.
 *
 * The sensor's mode descriptor declares 360,960,000 as the per-lane BIT RATE
 * for 648x368 at 30fps over one RAW10 lane, so the link frequency is
 * 180,480,000. Publishing the bit rate instead makes the receiver program the
 * D-PHY at roughly 721 Mbps against a sensor transmitting at roughly
 * 361 Mbps: the clock lane toggles but no packet ever validates and no frame
 * is ever received. This constant is load bearing.
 */
#define HM1092_LINK_FREQ_180MHZ		180480000ULL

/* pixel_rate = link_freq * 2 (DDR) * lanes / bits per pixel */
#define HM1092_PIXEL_RATE \
	(HM1092_LINK_FREQ_180MHZ * 2 * HM1092_LANES / 10)

/* Time the sensor needs after power-on before it answers on the bus. */
#define HM1092_POWER_ON_DELAY_MS	20

/*
 * Sensor initialisation sequence, 238 entries, reproduced verbatim - both
 * values and order.
 *
 * Provenance: the sequence was extracted from the register table embedded in
 * the Dell-supplied Windows driver hm1092.sys
 * (Intel-2D-Imaging-USB-IO-Vision-Driver-for-Camera, version 81.26100.0.13),
 * at offset 0x26B34 of its .rdata section, where it is stored as 16-byte
 * entries. It was cross-checked against a USB/I2C bus capture of that driver
 * programming this silicon, and the two agree.
 *
 * It configures the 648x368 30fps mode and leaves the sensor in standby, ready
 * for the single terminal stream-on in hm1092_enable_streams().
 *
 * Entries that look redundant are not: the 0x0104 write pairs, the repeated
 * trailing auto-exposure groups and the second write of 0x4b20 are all part of
 * the captured sequence. Do not deduplicate, reorder, or fold the consecutive
 * 8-bit halves of 16-bit registers (0x0202/0x0203, 0x0340/0x0341, 0x0342/
 * 0x0343, 0x0344..0x034f, 0x020e/0x020f) into CCI_REG16 entries: that would
 * coalesce two I2C transactions into one auto-incrementing burst and no longer
 * match the sequence the sensor is known to accept.
 */
static const struct cci_reg_sequence hm1092_init_regs[] = {
	{ CCI_REG8(0x0100), 0x00 },
	{ CCI_REG8(0x0103), 0x00 },
	{ CCI_REG8(0x0000), 0x00 },
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x0202), 0x02 },
	{ CCI_REG8(0x0203), 0xe5 },
	{ CCI_REG8(0x0307), 0x00 },
	{ CCI_REG8(0x0309), 0x01 },
	{ CCI_REG8(0x030a), 0x05 },
	{ CCI_REG8(0x030d), 0x0a },
	{ CCI_REG8(0x030f), 0x5e },
	{ CCI_REG8(0x0310), 0x00 },
	{ CCI_REG8(0x0340), 0x03 },
	{ CCI_REG8(0x0341), 0x1e },
	{ CCI_REG8(0x0342), 0x05 },
	{ CCI_REG8(0x0343), 0xe4 },
	{ CCI_REG8(0x0350), 0x53 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x3110), 0x02 },
	{ CCI_REG8(0x3735), 0xe2 },
	{ CCI_REG8(0x3704), 0x04 },
	{ CCI_REG8(0x4001), 0x00 },
	{ CCI_REG8(0x4002), 0x2b },
	{ CCI_REG8(0x4024), 0x40 },
	{ CCI_REG8(0x4131), 0x01 },
	{ CCI_REG8(0x4132), 0x20 },
	{ CCI_REG8(0x4265), 0x02 },
	{ CCI_REG8(0x4b04), 0x01 },
	{ CCI_REG8(0x4b0e), 0x0e },
	{ CCI_REG8(0x4b18), 0x00 },
	{ CCI_REG8(0x4b20), 0x9e },
	{ CCI_REG8(0x4b31), 0x06 },
	{ CCI_REG8(0x4b3b), 0x02 },
	{ CCI_REG8(0x4b3e), 0x00 },
	{ CCI_REG8(0x4b44), 0x0c },
	{ CCI_REG8(0x4b45), 0x01 },
	{ CCI_REG8(0x4b47), 0x00 },
	{ CCI_REG8(0x5004), 0x40 },
	{ CCI_REG8(0x5005), 0x28 },
	{ CCI_REG8(0x5006), 0x40 },
	{ CCI_REG8(0x5007), 0x28 },
	{ CCI_REG8(0x5010), 0x20 },
	{ CCI_REG8(0x5011), 0x00 },
	{ CCI_REG8(0x5013), 0x03 },
	{ CCI_REG8(0x5015), 0xb3 },
	{ CCI_REG8(0x501d), 0x4c },
	{ CCI_REG8(0x5098), 0x00 },
	{ CCI_REG8(0x5099), 0x11 },
	{ CCI_REG8(0x509b), 0x03 },
	{ CCI_REG8(0x50a0), 0x30 },
	{ CCI_REG8(0x50a2), 0x0b },
	{ CCI_REG8(0x50a6), 0x00 },
	{ CCI_REG8(0x50a7), 0x00 },
	{ CCI_REG8(0x50aa), 0x22 },
	{ CCI_REG8(0x50ab), 0x07 },
	{ CCI_REG8(0x50ac), 0x24 },
	{ CCI_REG8(0x50ad), 0x07 },
	{ CCI_REG8(0x50ae), 0x20 },
	{ CCI_REG8(0x50af), 0x40 },
	{ CCI_REG8(0x50b3), 0x04 },
	{ CCI_REG8(0x50b4), 0x00 },
	{ CCI_REG8(0x50b7), 0x00 },
	{ CCI_REG8(0x50b8), 0x70 },
	{ CCI_REG8(0x50b9), 0xff },
	{ CCI_REG8(0x50ba), 0xff },
	{ CCI_REG8(0x50bb), 0x14 },
	{ CCI_REG8(0x50cb), 0x21 },
	{ CCI_REG8(0x50d5), 0xe0 },
	{ CCI_REG8(0x50d7), 0x12 },
	{ CCI_REG8(0x50dd), 0x00 },
	{ CCI_REG8(0x50e8), 0x00 },
	{ CCI_REG8(0x50ea), 0x74 },
	{ CCI_REG8(0x50fa), 0x02 },
	{ CCI_REG8(0x5100), 0x03 },
	{ CCI_REG8(0x5101), 0x13 },
	{ CCI_REG8(0x5102), 0x23 },
	{ CCI_REG8(0x5103), 0x33 },
	{ CCI_REG8(0x5104), 0x43 },
	{ CCI_REG8(0x5105), 0x42 },
	{ CCI_REG8(0x5106), 0x40 },
	{ CCI_REG8(0x5118), 0x00 },
	{ CCI_REG8(0x5119), 0x00 },
	{ CCI_REG8(0x511a), 0x00 },
	{ CCI_REG8(0x511b), 0x00 },
	{ CCI_REG8(0x511c), 0x00 },
	{ CCI_REG8(0x511d), 0x00 },
	{ CCI_REG8(0x511e), 0x00 },
	{ CCI_REG8(0x5130), 0x13 },
	{ CCI_REG8(0x5131), 0x23 },
	{ CCI_REG8(0x5132), 0x33 },
	{ CCI_REG8(0x5133), 0x43 },
	{ CCI_REG8(0x5134), 0x42 },
	{ CCI_REG8(0x5135), 0x40 },
	{ CCI_REG8(0x5136), 0x40 },
	{ CCI_REG8(0x5148), 0x01 },
	{ CCI_REG8(0x5149), 0x01 },
	{ CCI_REG8(0x514a), 0x01 },
	{ CCI_REG8(0x514b), 0x01 },
	{ CCI_REG8(0x514c), 0x01 },
	{ CCI_REG8(0x514d), 0x01 },
	{ CCI_REG8(0x514e), 0x01 },
	{ CCI_REG8(0x51c0), 0x00 },
	{ CCI_REG8(0x51c1), 0x81 },
	{ CCI_REG8(0x51c2), 0xec },
	{ CCI_REG8(0x51c3), 0x00 },
	{ CCI_REG8(0x51c4), 0x55 },
	{ CCI_REG8(0x51c5), 0x44 },
	{ CCI_REG8(0x51c6), 0x00 },
	{ CCI_REG8(0x51c7), 0x81 },
	{ CCI_REG8(0x51c8), 0xec },
	{ CCI_REG8(0x51c9), 0x00 },
	{ CCI_REG8(0x51ca), 0x55 },
	{ CCI_REG8(0x51cb), 0x24 },
	{ CCI_REG8(0x51cc), 0x00 },
	{ CCI_REG8(0x51cd), 0x81 },
	{ CCI_REG8(0x51ce), 0xec },
	{ CCI_REG8(0x51cf), 0x00 },
	{ CCI_REG8(0x51d0), 0x54 },
	{ CCI_REG8(0x51d1), 0x24 },
	{ CCI_REG8(0x51d2), 0x00 },
	{ CCI_REG8(0x51d3), 0x81 },
	{ CCI_REG8(0x51d4), 0xec },
	{ CCI_REG8(0x51d5), 0x00 },
	{ CCI_REG8(0x51d6), 0x53 },
	{ CCI_REG8(0x51d7), 0x14 },
	{ CCI_REG8(0x51d8), 0x00 },
	{ CCI_REG8(0x51d9), 0x81 },
	{ CCI_REG8(0x51da), 0xec },
	{ CCI_REG8(0x51db), 0x00 },
	{ CCI_REG8(0x51dc), 0x53 },
	{ CCI_REG8(0x51dd), 0x14 },
	{ CCI_REG8(0x51e0), 0x09 },
	{ CCI_REG8(0x51e1), 0x03 },
	{ CCI_REG8(0x51e2), 0x04 },
	{ CCI_REG8(0x51e3), 0x03 },
	{ CCI_REG8(0x51e4), 0x08 },
	{ CCI_REG8(0x51e5), 0x07 },
	{ CCI_REG8(0x51e6), 0x08 },
	{ CCI_REG8(0x51e7), 0x07 },
	{ CCI_REG8(0x51e8), 0x04 },
	{ CCI_REG8(0x51e9), 0x46 },
	{ CCI_REG8(0x51ea), 0x43 },
	{ CCI_REG8(0x51eb), 0x62 },
	{ CCI_REG8(0x51ec), 0x61 },
	{ CCI_REG8(0x51ed), 0x00 },
	{ CCI_REG8(0x51ee), 0x00 },
	{ CCI_REG8(0x5200), 0x60 },
	{ CCI_REG8(0x5201), 0x80 },
	{ CCI_REG8(0x5202), 0x00 },
	{ CCI_REG8(0x5203), 0x01 },
	{ CCI_REG8(0x5206), 0x80 },
	{ CCI_REG8(0x5208), 0x0b },
	{ CCI_REG8(0x5209), 0x0c },
	{ CCI_REG8(0x520c), 0x15 },
	{ CCI_REG8(0x520d), 0x40 },
	{ CCI_REG8(0x5214), 0x28 },
	{ CCI_REG8(0x5215), 0x04 },
	{ CCI_REG8(0x5216), 0x02 },
	{ CCI_REG8(0x5217), 0x01 },
	{ CCI_REG8(0x5218), 0x07 },
	{ CCI_REG8(0x521e), 0x01 },
	{ CCI_REG8(0x5282), 0xff },
	{ CCI_REG8(0x5283), 0x03 },
	{ CCI_REG8(0x0202), 0x01 },
	{ CCI_REG8(0x0203), 0x68 },
	{ CCI_REG8(0x0340), 0x02 },
	{ CCI_REG8(0x0341), 0xe4 },
	{ CCI_REG8(0x0342), 0x06 },
	{ CCI_REG8(0x0343), 0x54 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x05 },
	{ CCI_REG8(0x0349), 0x0d },
	{ CCI_REG8(0x034a), 0x02 },
	{ CCI_REG8(0x034b), 0xdd },
	{ CCI_REG8(0x034c), 0x02 },
	{ CCI_REG8(0x034d), 0x88 },
	{ CCI_REG8(0x034e), 0x01 },
	{ CCI_REG8(0x034f), 0x70 },
	{ CCI_REG8(0x0383), 0x00 },
	{ CCI_REG8(0x0387), 0x10 },
	{ CCI_REG8(0x0390), 0x03 },
	{ CCI_REG8(0x4800), 0xac },
	{ CCI_REG8(0x0104), 0x01 },
	{ CCI_REG8(0x0104), 0x00 },
	{ CCI_REG8(0x4801), 0xae },
	{ CCI_REG8(0x4b20), 0x9e },
	{ CCI_REG8(0x0101), 0x03 },
	{ CCI_REG8(0x0340), 0x02 },
	{ CCI_REG8(0x0341), 0xe5 },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0xae },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0104), 0x01 },
	{ CCI_REG8(0x0340), 0x02 },
	{ CCI_REG8(0x0341), 0xe5 },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0xa1 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0104), 0x01 },
	{ CCI_REG8(0x0340), 0x02 },
	{ CCI_REG8(0x0341), 0xe5 },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0x85 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0104), 0x01 },
	{ CCI_REG8(0x0340), 0x02 },
	{ CCI_REG8(0x0341), 0xe5 },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0x80 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0104), 0x01 },
	{ CCI_REG8(0x0340), 0x02 },
	{ CCI_REG8(0x0341), 0xe5 },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0x91 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0104), 0x01 },
	{ CCI_REG8(0x0340), 0x02 },
	{ CCI_REG8(0x0341), 0xe5 },
	{ CCI_REG8(0x0202), 0x00 },
	{ CCI_REG8(0x0203), 0xb4 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0104), 0x01 },
};

/**
 * struct hm1092 - HM1092 sensor device instance
 * @sd: V4L2 sub-device
 * @pad: media pad for the single source pad
 * @ctrl_handler: V4L2 control handler, whose lock also serialises the subdev
 *		  state and therefore every register access
 * @dev: the underlying I2C device, used for logging and runtime PM
 * @regmap: CCI register map for 16-bit register addresses
 * @dvdd: digital core supply, may be NULL
 * @avdd: analogue supply, may be NULL
 * @dovdd: I/O supply, may be NULL
 * @extclk: external reference clock, may be NULL
 * @ir_led: IR flood illuminator GPIO, may be NULL
 */
struct hm1092 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;

	struct device *dev;
	struct regmap *regmap;

	struct regulator *dvdd;
	struct regulator *avdd;
	struct regulator *dovdd;

	struct clk *extclk;

	struct gpio_desc *ir_led;
};

static inline struct hm1092 *to_hm1092(struct v4l2_subdev *sd)
{
	return container_of(sd, struct hm1092, sd);
}

static int hm1092_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hm1092 *hm = container_of(ctrl->handler, struct hm1092,
					 ctrl_handler);
	int pm_ref;
	int ret = 0;

	/*
	 * Control values are only written to the sensor while it is powered.
	 * Values set while it is suspended are applied by the control handler
	 * setup that hm1092_enable_streams() runs after resuming it.
	 */
	pm_ref = pm_runtime_get_if_in_use(hm->dev);
	if (pm_ref <= 0)
		return 0;

	/*
	 * The 16-bit controls are written as two 8-bit transactions rather
	 * than one CCI_REG16 burst, matching the vendor sequence.
	 */
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(hm->regmap, HM1092_REG_EXPOSURE_H,
			  ctrl->val >> 8, &ret);
		cci_write(hm->regmap, HM1092_REG_EXPOSURE_L,
			  ctrl->val & 0xff, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(hm->regmap, HM1092_REG_ANALOG_GAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(hm->regmap, HM1092_REG_DIGITAL_GAIN_H,
			  ctrl->val >> 8, &ret);
		cci_write(hm->regmap, HM1092_REG_DIGITAL_GAIN_L,
			  ctrl->val & 0xff, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(hm->regmap, HM1092_REG_TEST_PATTERN,
			  ctrl->val ? (ctrl->val << 1) | HM1092_TEST_PATTERN_ON
				    : HM1092_TEST_PATTERN_OFF, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	/*
	 * Commit the group. cci_write() skips the write when @ret is already
	 * set, so a failed data write never commits a partial group.
	 */
	cci_write(hm->regmap, HM1092_REG_COMMAND_UPDATE, 0x01, &ret);

	pm_runtime_put(hm->dev);

	/*
	 * Never fail a stop. v4l2_subdev_disable_streams() only clears
	 * enabled_pads when this returns 0, so reporting the error would leave
	 * the core believing the pad still streams while the PM reference has
	 * already been dropped: the next enable is rejected with -EALREADY and
	 * a second disable underflows the reference count.
	 */
	return 0;
}

static const struct v4l2_ctrl_ops hm1092_ctrl_ops = {
	.s_ctrl = hm1092_set_ctrl,
};

static const s64 hm1092_link_freqs[] = {
	HM1092_LINK_FREQ_180MHZ,
};

static const char * const hm1092_test_pattern_menu[] = {
	"Disabled",
	"Solid Color",
};

static int hm1092_init_controls(struct hm1092 *hm)
{
	struct v4l2_ctrl_handler *handler = &hm->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_ctrl_handler_init(handler, 10);
	if (ret)
		return ret;

	/*
	 * The sensor runs one fixed mode, so the blanking and rate controls
	 * describe that mode and are read-only.
	 */
	ctrl = v4l2_ctrl_new_std(handler, &hm1092_ctrl_ops, V4L2_CID_HBLANK,
				 HM1092_HBLANK, HM1092_HBLANK, 1,
				 HM1092_HBLANK);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(handler, &hm1092_ctrl_ops, V4L2_CID_VBLANK,
				 HM1092_VBLANK, HM1092_VBLANK, 1,
				 HM1092_VBLANK);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_int_menu(handler, &hm1092_ctrl_ops,
				      V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(hm1092_link_freqs) - 1, 0,
				      hm1092_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(handler, &hm1092_ctrl_ops,
				 V4L2_CID_PIXEL_RATE, HM1092_PIXEL_RATE,
				 HM1092_PIXEL_RATE, 1, HM1092_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(handler, &hm1092_ctrl_ops, V4L2_CID_EXPOSURE,
			  HM1092_EXPOSURE_MIN, HM1092_EXPOSURE_MAX,
			  HM1092_EXPOSURE_STEP, HM1092_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(handler, &hm1092_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  HM1092_AGAIN_MIN, HM1092_AGAIN_MAX,
			  HM1092_AGAIN_STEP, HM1092_AGAIN_DEFAULT);

	v4l2_ctrl_new_std(handler, &hm1092_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  HM1092_DGAIN_MIN, HM1092_DGAIN_MAX,
			  HM1092_DGAIN_STEP, HM1092_DGAIN_DEFAULT);

	v4l2_ctrl_new_std_menu_items(handler, &hm1092_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(hm1092_test_pattern_menu) - 1,
				     0, 0, hm1092_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(hm->dev, &props);
	if (ret)
		goto err_free;

	ret = v4l2_ctrl_new_fwnode_properties(handler, &hm1092_ctrl_ops,
					      &props);
	if (ret)
		goto err_free;

	if (handler->error) {
		ret = handler->error;
		goto err_free;
	}

	hm->sd.ctrl_handler = handler;

	return 0;

err_free:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int hm1092_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = HM1092_MBUS_CODE;

	return 0;
}

static int hm1092_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index > 0 || fse->code != HM1092_MBUS_CODE)
		return -EINVAL;

	fse->min_width = HM1092_WIDTH;
	fse->max_width = HM1092_WIDTH;
	fse->min_height = HM1092_HEIGHT;
	fse->max_height = HM1092_HEIGHT;

	return 0;
}

static int hm1092_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *mf;

	mf = v4l2_subdev_state_get_format(state, fmt->pad);
	if (!mf)
		return -EINVAL;

	/* The sensor has exactly one mode; the requested format is ignored. */
	mf->width = HM1092_WIDTH;
	mf->height = HM1092_HEIGHT;
	mf->code = HM1092_MBUS_CODE;
	mf->field = V4L2_FIELD_NONE;
	mf->colorspace = V4L2_COLORSPACE_RAW;
	mf->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	mf->quantization = V4L2_QUANTIZATION_DEFAULT;
	mf->xfer_func = V4L2_XFER_FUNC_NONE;

	fmt->format = *mf;

	return 0;
}

static int hm1092_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct hm1092 *hm = to_hm1092(sd);
	int ret;

	ret = pm_runtime_resume_and_get(hm->dev);
	if (ret)
		return ret;

	ret = cci_multi_reg_write(hm->regmap, hm1092_init_regs,
				  ARRAY_SIZE(hm1092_init_regs), NULL);
	if (ret) {
		dev_err(hm->dev, "init sequence failed: %d\n", ret);
		goto err_rpm;
	}

	ret = __v4l2_ctrl_handler_setup(&hm->ctrl_handler);
	if (ret) {
		dev_err(hm->dev, "control setup failed: %d\n", ret);
		goto err_rpm;
	}

	ret = cci_write(hm->regmap, HM1092_REG_MODE_SELECT,
			HM1092_MODE_STREAMING, NULL);
	if (ret) {
		dev_err(hm->dev, "stream start failed: %d\n", ret);
		goto err_rpm;
	}

	/* The illuminator is only powered while the sensor is streaming. */
	gpiod_set_value_cansleep(hm->ir_led, 1);

	return 0;

err_rpm:
	pm_runtime_put(hm->dev);

	return ret;
}

static int hm1092_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct hm1092 *hm = to_hm1092(sd);
	int ret;

	gpiod_set_value_cansleep(hm->ir_led, 0);

	ret = cci_write(hm->regmap, HM1092_REG_MODE_SELECT,
			HM1092_MODE_STANDBY, NULL);
	if (ret)
		dev_err(hm->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put(hm->dev);

	return ret;
}

static int hm1092_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
	};

	return hm1092_set_format(sd, state, &fmt);
}

/*
 * libcamera queries the selection rectangles to learn the sensor geometry;
 * without them it reports that the kernel driver needs fixing and falls back
 * to guessed defaults.
 *
 * NATIVE_SIZE is the full pixel array. The crop targets describe the streamed
 * window in the binned coordinate space the sensor outputs, which is the same
 * 648x368 the source pad reports.
 */
static int hm1092_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	if (sel->pad != 0)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = HM1092_WIDTH;
		sel->r.height = HM1092_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = HM1092_NATIVE_WIDTH;
		sel->r.height = HM1092_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_video_ops hm1092_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops hm1092_pad_ops = {
	.enum_mbus_code = hm1092_enum_mbus_code,
	.enum_frame_size = hm1092_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = hm1092_set_format,
	.get_selection = hm1092_get_selection,
	.enable_streams = hm1092_enable_streams,
	.disable_streams = hm1092_disable_streams,
};

static const struct v4l2_subdev_ops hm1092_subdev_ops = {
	.video = &hm1092_video_ops,
	.pad = &hm1092_pad_ops,
};

static const struct v4l2_subdev_internal_ops hm1092_internal_ops = {
	.init_state = hm1092_init_state,
};

static int hm1092_check_chip_id(struct hm1092 *hm)
{
	u64 hi, lo;
	u16 chip_id;
	int ret = 0;

	/*
	 * Read as two 8-bit transactions rather than one CCI_REG16 burst, so
	 * that the very first access this driver performs keeps the wire
	 * format the sensor is known to answer.
	 */
	cci_read(hm->regmap, HM1092_REG_CHIP_ID_H, &hi, &ret);
	cci_read(hm->regmap, HM1092_REG_CHIP_ID_L, &lo, &ret);
	if (ret) {
		/*
		 * Return -ENODEV for ANY I2C failure, never -ETIMEDOUT or
		 * -EPROBE_DEFER. The kernel retries deferred probes, and on the
		 * USB-attached I2C bridge this sensor sits behind, repeated I2C
		 * timeouts wedge the bus and take down every other device on
		 * it, including the working RGB camera.
		 */
		dev_err(hm->dev,
			"failed to read chip ID (sensor may not be powered): %d\n",
			ret);
		return -ENODEV;
	}

	chip_id = (hi << 8) | lo;
	if (chip_id != HM1092_CHIP_ID) {
		dev_err(hm->dev, "chip ID mismatch: expected 0x%04x, got 0x%04x\n",
			HM1092_CHIP_ID, chip_id);
		return -ENODEV;
	}

	dev_dbg(hm->dev, "chip ID confirmed: 0x%04x\n", chip_id);

	return 0;
}

static int hm1092_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hm1092 *hm = to_hm1092(sd);
	int ret;

	if (hm->dovdd) {
		ret = regulator_enable(hm->dovdd);
		if (ret)
			return ret;
	}

	if (hm->avdd) {
		ret = regulator_enable(hm->avdd);
		if (ret)
			goto err_dovdd;
	}

	if (hm->dvdd) {
		ret = regulator_enable(hm->dvdd);
		if (ret)
			goto err_avdd;
	}

	ret = clk_prepare_enable(hm->extclk);
	if (ret)
		goto err_dvdd;

	/* Let the sensor boot after power-on before touching its registers. */
	msleep(HM1092_POWER_ON_DELAY_MS);

	return 0;

err_dvdd:
	if (hm->dvdd)
		regulator_disable(hm->dvdd);
err_avdd:
	if (hm->avdd)
		regulator_disable(hm->avdd);
err_dovdd:
	if (hm->dovdd)
		regulator_disable(hm->dovdd);

	return ret;
}

static int hm1092_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hm1092 *hm = to_hm1092(sd);

	clk_disable_unprepare(hm->extclk);

	if (hm->dvdd)
		regulator_disable(hm->dvdd);
	if (hm->avdd)
		regulator_disable(hm->avdd);
	if (hm->dovdd)
		regulator_disable(hm->dovdd);

	return 0;
}

/*
 * The supplies are optional: which of them exist depends on the camera module
 * variant and on what the platform's power-control device actually registers.
 * Only -ENODEV means "this board does not have it"; every other error is real.
 */
static int hm1092_get_regulator(struct hm1092 *hm, const char *id,
				struct regulator **out)
{
	struct regulator *reg;

	reg = devm_regulator_get_optional(hm->dev, id);
	if (IS_ERR(reg)) {
		if (PTR_ERR(reg) != -ENODEV)
			return dev_err_probe(hm->dev, PTR_ERR(reg),
					     "failed to get %s supply\n", id);
		reg = NULL;
	}

	*out = reg;

	return 0;
}

/*
 * Validate what firmware says about the CSI-2 link against what this driver is
 * built to drive. Without this the driver publishes its own lane count and link
 * frequency unconditionally and probes "successfully" against a firmware
 * description that disagrees -- which is exactly how this part came to be
 * driven at twice its real line rate for three months, clock lane up and not a
 * single frame ever framed.
 */
static int hm1092_check_hwcfg(struct device *dev)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep, *fwnode = dev_fwnode(dev);
	unsigned long link_freq_bitmap;
	int ret;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "no endpoint found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != HM1092_LANES) {
		ret = dev_err_probe(dev, -EINVAL,
				    "only %u data lane is supported, got %u\n",
				    HM1092_LANES,
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       hm1092_link_freqs,
				       ARRAY_SIZE(hm1092_link_freqs),
				       &link_freq_bitmap);

out:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int hm1092_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct hm1092 *hm;
	int ret;

	ret = hm1092_check_hwcfg(dev);
	if (ret)
		return ret;

	hm = devm_kzalloc(dev, sizeof(*hm), GFP_KERNEL);
	if (!hm)
		return -ENOMEM;

	hm->dev = dev;

	v4l2_i2c_subdev_init(&hm->sd, client, &hm1092_subdev_ops);
	hm->sd.internal_ops = &hm1092_internal_ops;

	hm->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(hm->regmap))
		return dev_err_probe(dev, PTR_ERR(hm->regmap),
				     "failed to init CCI regmap\n");

	ret = hm1092_get_regulator(hm, "dvdd", &hm->dvdd);
	if (ret)
		return ret;

	ret = hm1092_get_regulator(hm, "avdd", &hm->avdd);
	if (ret)
		return ret;

	ret = hm1092_get_regulator(hm, "dovdd", &hm->dovdd);
	if (ret)
		return ret;

	hm->extclk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(hm->extclk))
		return dev_err_probe(dev, PTR_ERR(hm->extclk),
				     "failed to get external clock\n");

	/*
	 * IR flood illuminator. On some platforms the GPIO is claimed by the
	 * camera power-control device and exposed as an LED class device
	 * instead, in which case this resolves to NULL and userspace drives the
	 * illuminator. Where the sensor does own it, turning it on only while
	 * streaming is the eye-safety and power gate.
	 */
	hm->ir_led = devm_gpiod_get_optional(dev, "ir-led", GPIOD_OUT_LOW);
	if (IS_ERR(hm->ir_led))
		return dev_err_probe(dev, PTR_ERR(hm->ir_led),
				     "failed to get ir-led GPIO\n");

	ret = hm1092_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power on sensor\n");

	ret = hm1092_check_chip_id(hm);
	if (ret)
		goto err_power;

	/*
	 * Leave the sensor in the state the first entry of the initialisation
	 * sequence assumes.
	 */
	ret = cci_write(hm->regmap, HM1092_REG_MODE_SELECT,
			HM1092_MODE_STANDBY, NULL);
	if (ret) {
		dev_err(dev, "failed to put sensor in standby: %d\n", ret);
		goto err_power;
	}

	ret = hm1092_init_controls(hm);
	if (ret) {
		dev_err(dev, "failed to init controls: %d\n", ret);
		goto err_power;
	}

	/*
	 * Share the control handler lock with the subdev state. The CCI regmap
	 * has locking disabled, so every register access must be serialised by
	 * the driver: this makes the control path and the streaming path take
	 * the same mutex.
	 */
	hm->sd.state_lock = hm->ctrl_handler.lock;

	hm->pad.flags = MEDIA_PAD_FL_SOURCE;
	hm->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	hm->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&hm->sd.entity, 1, &hm->pad);
	if (ret) {
		dev_err(dev, "failed to init media entity: %d\n", ret);
		goto err_ctrl;
	}

	ret = v4l2_subdev_init_finalize(&hm->sd);
	if (ret) {
		dev_err(dev, "failed to finalize subdev init: %d\n", ret);
		goto err_media;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&hm->sd);
	if (ret) {
		dev_err(dev, "failed to register v4l2 subdev: %d\n", ret);
		goto err_pm;
	}

	pm_runtime_idle(dev);

	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&hm->sd);
err_media:
	media_entity_cleanup(&hm->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&hm->ctrl_handler);
err_power:
	hm1092_power_off(dev);

	return ret;
}

static void hm1092_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct hm1092 *hm = to_hm1092(sd);

	gpiod_set_value_cansleep(hm->ir_led, 0);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&hm->ctrl_handler);

	/*
	 * Release the rails runtime PM left enabled. Without this, every module
	 * reload leaks a use count on the digital supply: the devm-managed
	 * consumer is freed while still enabled, after which regulator_disable()
	 * can never reach zero and the rail never physically drops.
	 */
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		hm1092_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(hm1092_pm_ops, hm1092_power_off,
				 hm1092_power_on, NULL);

static const struct acpi_device_id hm1092_acpi_ids[] = {
	{ "HIMX1092" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, hm1092_acpi_ids);

static struct i2c_driver hm1092_i2c_driver = {
	.driver = {
		.name = "hm1092",
		.pm = pm_ptr(&hm1092_pm_ops),
		.acpi_match_table = hm1092_acpi_ids,
	},
	.probe = hm1092_probe,
	.remove = hm1092_remove,
};
module_i2c_driver(hm1092_i2c_driver);

MODULE_AUTHOR("Jake Steinman <j@metarealtyinc.ca>");
MODULE_DESCRIPTION("Himax HM1092 monochrome IR image sensor driver");
MODULE_LICENSE("GPL");
