// SPDX-License-Identifier: GPL-2.0
/*
 * camss-csid.c
 *
 * Qualcomm MSM Camera Subsystem - CSID (CSI Decoder) Module
 *
 * Copyright (c) 2011-2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2015-2018 Linaro Ltd.
 */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/media-entity.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-subdev.h>

#include "camss-csid.h"
#include "camss-csid-gen1.h"
#include "camss.h"

/* offset of CSID registers in VFE region for VFE 480 */
#define VFE_480_CSID_OFFSET 0x1200
#define VFE_480_LITE_CSID_OFFSET 0x200

#define CSID_HW_VERSION		0x0
#define		HW_VERSION_STEPPING	0
#define		HW_VERSION_REVISION	16
#define		HW_VERSION_GENERATION	28

#define LANE_CFG_BITWIDTH 4

#define MSM_CSID_NAME "msm_csid"

const char * const csid_testgen_modes[] = {
	"Disabled",
	"Incrementing",
	"Alternating 0x55/0xAA",
	"All Zeros 0x00",
	"All Ones 0xFF",
	"Pseudo-random Data",
	"User Specified",
	"Complex pattern",
	"Color box",
	"Color bars",
	NULL
};

static const struct csid_format_info formats_4_1[] = {
	{
		MEDIA_BUS_FMT_UYVY8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_VYUY8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_YUYV8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_YVYU8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_SBGGR8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SBGGR10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SBGGR12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_Y10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
};

static const struct csid_format_info formats_4_7[] = {
	{
		MEDIA_BUS_FMT_UYVY8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_VYUY8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_YUYV8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_YVYU8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_SBGGR8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SBGGR10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SBGGR12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SBGGR14_1X14,
		MIPI_CSI2_DT_RAW14,
		DECODE_FORMAT_UNCOMPRESSED_14_BIT,
		14,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG14_1X14,
		MIPI_CSI2_DT_RAW14,
		DECODE_FORMAT_UNCOMPRESSED_14_BIT,
		14,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG14_1X14,
		MIPI_CSI2_DT_RAW14,
		DECODE_FORMAT_UNCOMPRESSED_14_BIT,
		14,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB14_1X14,
		MIPI_CSI2_DT_RAW14,
		DECODE_FORMAT_UNCOMPRESSED_14_BIT,
		14,
		1,
	},
	{
		MEDIA_BUS_FMT_Y10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
};

static const struct csid_format_info formats_gen2[] = {
	{
		MEDIA_BUS_FMT_UYVY8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_VYUY8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_YUYV8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_YVYU8_1X16,
		MIPI_CSI2_DT_YUV422_8B,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		2,
	},
	{
		MEDIA_BUS_FMT_SBGGR8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_SBGGR10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_Y8_1X8,
		MIPI_CSI2_DT_RAW8,
		DECODE_FORMAT_UNCOMPRESSED_8_BIT,
		8,
		1,
	},
	{
		MEDIA_BUS_FMT_Y10_1X10,
		MIPI_CSI2_DT_RAW10,
		DECODE_FORMAT_UNCOMPRESSED_10_BIT,
		10,
		1,
	},
	{
		MEDIA_BUS_FMT_SBGGR12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB12_1X12,
		MIPI_CSI2_DT_RAW12,
		DECODE_FORMAT_UNCOMPRESSED_12_BIT,
		12,
		1,
	},
	{
		MEDIA_BUS_FMT_SBGGR14_1X14,
		MIPI_CSI2_DT_RAW14,
		DECODE_FORMAT_UNCOMPRESSED_14_BIT,
		14,
		1,
	},
	{
		MEDIA_BUS_FMT_SGBRG14_1X14,
		MIPI_CSI2_DT_RAW14,
		DECODE_FORMAT_UNCOMPRESSED_14_BIT,
		14,
		1,
	},
	{
		MEDIA_BUS_FMT_SGRBG14_1X14,
		MIPI_CSI2_DT_RAW14,
		DECODE_FORMAT_UNCOMPRESSED_14_BIT,
		14,
		1,
	},
	{
		MEDIA_BUS_FMT_SRGGB14_1X14,
		MIPI_CSI2_DT_RAW14,
		DECODE_FORMAT_UNCOMPRESSED_14_BIT,
		14,
		1,
	},
};

const struct csid_formats csid_formats_4_1 = {
	.nformats = ARRAY_SIZE(formats_4_1),
	.formats = formats_4_1
};

const struct csid_formats csid_formats_4_7 = {
	.nformats = ARRAY_SIZE(formats_4_7),
	.formats = formats_4_7
};

const struct csid_formats csid_formats_gen2 = {
	.nformats = ARRAY_SIZE(formats_gen2),
	.formats = formats_gen2
};

u32 csid_find_code(u32 *codes, unsigned int ncodes,
		   unsigned int match_format_idx, u32 match_code)
{
	int i;

	if (!match_code && (match_format_idx >= ncodes))
		return 0;

	for (i = 0; i < ncodes; i++)
		if (match_code) {
			if (codes[i] == match_code)
				return match_code;
		} else {
			if (i == match_format_idx)
				return codes[i];
		}

	return codes[0];
}

const struct csid_format_info *csid_get_fmt_entry(const struct csid_format_info *formats,
						  unsigned int nformats,
						  u32 code)
{
	unsigned int i;

	for (i = 0; i < nformats; i++)
		if (code == formats[i].code)
			return &formats[i];

	WARN(1, "Unknown format\n");

	return &formats[0];
}

/*
 * csid_set_clock_rates - Calculate and set clock rates on CSID module
 * @csiphy: CSID device
 */
static int csid_set_clock_rates(struct csid_device *csid)
{
	struct device *dev = csid->camss->dev;
	const struct csid_format_info *fmt;
	s64 link_freq;
	int i, j;
	int ret;

	fmt = csid_get_fmt_entry(csid->res->formats->formats, csid->res->formats->nformats,
				 csid->fmt[MSM_CSIPHY_PAD_SINK].code);
	link_freq = camss_get_link_freq(&csid->subdev.entity, fmt->bpp,
					csid->phy.lane_cnt);
	if (link_freq < 0)
		link_freq = 0;

	for (i = 0; i < csid->nclocks; i++) {
		struct camss_clock *clock = &csid->clock[i];

		if (!strcmp(clock->name, "csi0") ||
		    !strcmp(clock->name, "csi1") ||
		    !strcmp(clock->name, "csi2") ||
		    !strcmp(clock->name, "csi3")) {
			u64 min_rate = link_freq / 4;
			long rate;

			camss_add_clock_margin(&min_rate);

			for (j = 0; j < clock->nfreqs; j++)
				if (min_rate < clock->freq[j])
					break;

			if (j == clock->nfreqs) {
				dev_err(dev,
					"Pixel clock is too high for CSID\n");
				return -EINVAL;
			}

			/* if sensor pixel clock is not available */
			/* set highest possible CSID clock rate */
			if (min_rate == 0)
				j = clock->nfreqs - 1;

			rate = clk_round_rate(clock->clk, clock->freq[j]);
			if (rate < 0) {
				dev_err(dev, "clk round rate failed: %ld\n",
					rate);
				return -EINVAL;
			}

			ret = clk_set_rate(clock->clk, rate);
			if (ret < 0) {
				dev_err(dev, "clk set rate failed: %d\n", ret);
				return ret;
			}
		} else if (clock->nfreqs) {
			clk_set_rate(clock->clk, clock->freq[0]);
		}
	}

	return 0;
}

/*
 * csid_hw_version - CSID hardware version query
 * @csid: CSID device
 *
 * Return HW version or error
 */
u32 csid_hw_version(struct csid_device *csid)
{
	u32 hw_version;
	u32 hw_gen;
	u32 hw_rev;
	u32 hw_step;

	hw_version = readl_relaxed(csid->base + CSID_HW_VERSION);
	hw_gen = (hw_version >> HW_VERSION_GENERATION) & 0xF;
	hw_rev = (hw_version >> HW_VERSION_REVISION) & 0xFFF;
	hw_step = (hw_version >> HW_VERSION_STEPPING) & 0xFFFF;
	dev_dbg(csid->camss->dev, "CSID:%d HW Version = %u.%u.%u\n",
		csid->id, hw_gen, hw_rev, hw_step);

	return hw_version;
}

/*
 * csid_src_pad_code - Pick an output/src format based on the input/sink format
 * @csid: CSID device
 * @sink_code: The sink format of the input
 * @match_format_idx: Request preferred index, as defined by subdevice csid
 *                    format. Set @match_code to 0 if used.
 * @match_code: Request preferred code, set @match_format_idx to 0 if used
 *
 * Return 0 on failure or src format code otherwise
 */
u32 csid_src_pad_code(struct csid_device *csid, u32 sink_code,
		      unsigned int match_format_idx, u32 match_code)
{
	if (csid->camss->res->version == CAMSS_8x16) {
		if (match_format_idx > 0)
			return 0;

		return sink_code;
	}

	switch (sink_code) {
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	{
		u32 src_code[] = {
			MEDIA_BUS_FMT_SBGGR10_1X10,
			MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_LE,
		};

		return csid_find_code(src_code, ARRAY_SIZE(src_code),
				      match_format_idx, match_code);
	}
	case MEDIA_BUS_FMT_Y10_1X10:
	{
		u32 src_code[] = {
			MEDIA_BUS_FMT_Y10_1X10,
			MEDIA_BUS_FMT_Y10_2X8_PADHI_LE,
		};

		return csid_find_code(src_code, ARRAY_SIZE(src_code),
				      match_format_idx, match_code);
	}
	default:
		if (match_format_idx > 0)
			return 0;

		return sink_code;
	}
}

/*
 * csid_set_power - Power on/off CSID module
 * @sd: CSID V4L2 subdevice
 * @on: Requested power state
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_set_power(struct v4l2_subdev *sd, int on)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	struct camss *camss = csid->camss;
	struct device *dev = camss->dev;
	int ret = 0;

	if (on) {
		/*
		 * From SDM845 onwards, the VFE needs to be powered on before
		 * switching on the CSID. Do so unconditionally, as there is no
		 * drawback in following the same powering order on older SoCs.
		 */
		ret = csid->res->parent_dev_ops->get(camss, csid->id);
		if (ret < 0)
			return ret;

		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			return ret;

		ret = regulator_bulk_enable(csid->num_supplies,
					    csid->supplies);
		if (ret < 0) {
			pm_runtime_put_sync(dev);
			return ret;
		}

		ret = csid_set_clock_rates(csid);
		if (ret < 0) {
			regulator_bulk_disable(csid->num_supplies,
					       csid->supplies);
			pm_runtime_put_sync(dev);
			return ret;
		}

		ret = camss_enable_clocks(csid->nclocks, csid->clock, dev);
		if (ret < 0) {
			regulator_bulk_disable(csid->num_supplies,
					       csid->supplies);
			pm_runtime_put_sync(dev);
			return ret;
		}

		csid->phy.need_vc_update = true;

		enable_irq(csid->irq);

		ret = csid->res->hw_ops->reset(csid);
		if (ret < 0) {
			disable_irq(csid->irq);
			camss_disable_clocks(csid->nclocks, csid->clock);
			regulator_bulk_disable(csid->num_supplies,
					       csid->supplies);
			pm_runtime_put_sync(dev);
			return ret;
		}

		csid->res->hw_ops->hw_version(csid);
	} else {
		disable_irq(csid->irq);
		camss_disable_clocks(csid->nclocks, csid->clock);
		regulator_bulk_disable(csid->num_supplies,
				       csid->supplies);
		pm_runtime_put_sync(dev);
		csid->res->parent_dev_ops->put(camss, csid->id);
	}

	return ret;
}

/*
 * csid_set_stream - Enable/disable streaming on CSID module
 * @sd: CSID V4L2 subdevice
 * @enable: Requested streaming state
 *
 * Main configuration of CSID module is also done here.
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	int ret;

	if (enable) {
		if (csid->testgen.nmodes != CSID_PAYLOAD_MODE_DISABLED) {
			ret = v4l2_ctrl_handler_setup(&csid->ctrls);
			if (ret < 0) {
				dev_err(csid->camss->dev,
					"could not sync v4l2 controls: %d\n", ret);
				return ret;
			}
		}

		if (!csid->testgen.enabled &&
		    !media_pad_remote_pad_first(&csid->pads[MSM_CSID_PAD_SINK]))
			return -ENOLINK;
	}

	if (csid->phy.need_vc_update) {
		csid->res->hw_ops->configure_stream(csid, enable);
		csid->phy.need_vc_update = false;
	}

	return 0;
}

/*
 * __csid_get_format - Get pointer to format structure
 * @csid: CSID device
 * @sd_state: V4L2 subdev state
 * @pad: pad from which format is requested
 * @which: TRY or ACTIVE format
 *
 * Return pointer to TRY or ACTIVE format structure
 */
static struct v4l2_mbus_framefmt *
__csid_get_format(struct csid_device *csid,
		  struct v4l2_subdev_state *sd_state,
		  unsigned int pad,
		  enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_state_get_format(sd_state, pad);

	return &csid->fmt[pad];
}

/*
 * csid_try_format - Handle try format by pad subdev method
 * @csid: CSID device
 * @sd_state: V4L2 subdev state
 * @pad: pad on which format is requested
 * @fmt: pointer to v4l2 format structure
 * @which: wanted subdev format
 */
static void csid_try_format(struct csid_device *csid,
			    struct v4l2_subdev_state *sd_state,
			    unsigned int pad,
			    struct v4l2_mbus_framefmt *fmt,
			    enum v4l2_subdev_format_whence which)
{
	unsigned int i;

	switch (pad) {
	case MSM_CSID_PAD_SINK:
		/* Set format on sink pad */

		for (i = 0; i < csid->res->formats->nformats; i++)
			if (fmt->code == csid->res->formats->formats[i].code)
				break;

		/* If not found, use UYVY as default */
		if (i >= csid->res->formats->nformats)
			fmt->code = MEDIA_BUS_FMT_UYVY8_1X16;

		fmt->width = clamp_t(u32, fmt->width, 1, 8191);
		fmt->height = clamp_t(u32, fmt->height, 1, 8191);

		fmt->field = V4L2_FIELD_NONE;
		fmt->colorspace = V4L2_COLORSPACE_SRGB;

		break;

	default:
		if (csid->testgen.nmodes == CSID_PAYLOAD_MODE_DISABLED ||
		    csid->testgen_mode->cur.val == 0) {
			/* Test generator is disabled, */
			/* keep pad formats in sync */
			u32 code = fmt->code;

			*fmt = *__csid_get_format(csid, sd_state,
						      MSM_CSID_PAD_SINK, which);
			fmt->code = csid->res->hw_ops->src_pad_code(csid, fmt->code, 0, code);
		} else {
			/* Test generator is enabled, set format on source */
			/* pad to allow test generator usage */

			for (i = 0; i < csid->res->formats->nformats; i++)
				if (csid->res->formats->formats[i].code == fmt->code)
					break;

			/* If not found, use UYVY as default */
			if (i >= csid->res->formats->nformats)
				fmt->code = MEDIA_BUS_FMT_UYVY8_1X16;

			fmt->width = clamp_t(u32, fmt->width, 1, 8191);
			fmt->height = clamp_t(u32, fmt->height, 1, 8191);

			fmt->field = V4L2_FIELD_NONE;
		}
		break;
	}

	fmt->colorspace = V4L2_COLORSPACE_SRGB;
}

/*
 * csid_enum_mbus_code - Handle pixel format enumeration
 * @sd: CSID V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @code: pointer to v4l2_subdev_mbus_code_enum structure
 * return -EINVAL or zero on success
 */
static int csid_enum_mbus_code(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);

	if (code->pad == MSM_CSID_PAD_SINK) {
		if (code->index >= csid->res->formats->nformats)
			return -EINVAL;

		code->code = csid->res->formats->formats[code->index].code;
	} else {
		if (csid->testgen.nmodes == CSID_PAYLOAD_MODE_DISABLED ||
		    csid->testgen_mode->cur.val == 0) {
			struct v4l2_mbus_framefmt *sink_fmt;

			sink_fmt = __csid_get_format(csid, sd_state,
						     MSM_CSID_PAD_SINK,
						     code->which);

			code->code = csid->res->hw_ops->src_pad_code(csid, sink_fmt->code,
								     code->index, 0);
			if (!code->code)
				return -EINVAL;
		} else {
			if (code->index >= csid->res->formats->nformats)
				return -EINVAL;

			code->code = csid->res->formats->formats[code->index].code;
		}
	}

	return 0;
}

/*
 * csid_enum_frame_size - Handle frame size enumeration
 * @sd: CSID V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fse: pointer to v4l2_subdev_frame_size_enum structure
 * return -EINVAL or zero on success
 */
static int csid_enum_frame_size(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_frame_size_enum *fse)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt format;

	if (fse->index != 0)
		return -EINVAL;

	format.code = fse->code;
	format.width = 1;
	format.height = 1;
	csid_try_format(csid, sd_state, fse->pad, &format, fse->which);
	fse->min_width = format.width;
	fse->min_height = format.height;

	if (format.code != fse->code)
		return -EINVAL;

	format.code = fse->code;
	format.width = -1;
	format.height = -1;
	csid_try_format(csid, sd_state, fse->pad, &format, fse->which);
	fse->max_width = format.width;
	fse->max_height = format.height;

	return 0;
}

/*
 * csid_get_format - Handle get format by pads subdev method
 * @sd: CSID V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fmt: pointer to v4l2 subdev format structure
 *
 * Return -EINVAL or zero on success
 */
static int csid_get_format(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *format;

	format = __csid_get_format(csid, sd_state, fmt->pad, fmt->which);
	if (format == NULL)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

/*
 * csid_set_format - Handle set format by pads subdev method
 * @sd: CSID V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fmt: pointer to v4l2 subdev format structure
 *
 * Return -EINVAL or zero on success
 */
static int csid_set_format(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *format;
	int i;

	format = __csid_get_format(csid, sd_state, fmt->pad, fmt->which);
	if (format == NULL)
		return -EINVAL;

	csid_try_format(csid, sd_state, fmt->pad, &fmt->format, fmt->which);
	*format = fmt->format;

	/* Propagate the format from sink to source pads */
	if (fmt->pad == MSM_CSID_PAD_SINK) {
		for (i = MSM_CSID_PAD_FIRST_SRC; i < MSM_CSID_PADS_NUM; ++i) {
			format = __csid_get_format(csid, sd_state, i, fmt->which);

			*format = fmt->format;
			csid_try_format(csid, sd_state, i, format, fmt->which);
		}
	}

	return 0;
}

/*
 * csid_init_formats - Initialize formats on all pads
 * @sd: CSID V4L2 subdevice
 * @fh: V4L2 subdev file handle
 *
 * Initialize all pad formats with default values.
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_init_formats(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_subdev_format format = {
		.pad = MSM_CSID_PAD_SINK,
		.which = fh ? V4L2_SUBDEV_FORMAT_TRY :
			      V4L2_SUBDEV_FORMAT_ACTIVE,
		.format = {
			.code = MEDIA_BUS_FMT_UYVY8_1X16,
			.width = 1920,
			.height = 1080
		}
	};

	return csid_set_format(sd, fh ? fh->state : NULL, &format);
}

/*
 * csid_set_test_pattern - Set test generator's pattern mode
 * @csid: CSID device
 * @value: desired test pattern mode
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_set_test_pattern(struct csid_device *csid, s32 value)
{
	struct csid_testgen_config *tg = &csid->testgen;

	/* If CSID is linked to CSIPHY, do not allow to enable test generator */
	if (value && media_pad_remote_pad_first(&csid->pads[MSM_CSID_PAD_SINK]))
		return -EBUSY;

	tg->enabled = !!value;

	return csid->res->hw_ops->configure_testgen_pattern(csid, value);
}

/*
 * csid_s_ctrl - Handle set control subdev method
 * @ctrl: pointer to v4l2 control structure
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct csid_device *csid = container_of(ctrl->handler,
						struct csid_device, ctrls);
	int ret = -EINVAL;

	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		ret = csid_set_test_pattern(csid, ctrl->val);
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops csid_ctrl_ops = {
	.s_ctrl = csid_s_ctrl,
};

/*
 * msm_csid_subdev_init - Initialize CSID device structure and resources
 * @csid: CSID device
 * @res: CSID module resources table
 * @id: CSID module id
 *
 * Return 0 on success or a negative error code otherwise
 */
int msm_csid_subdev_init(struct camss *camss, struct csid_device *csid,
			 const struct camss_subdev_resources *res, u8 id)
{
	struct device *dev = camss->dev;
	struct platform_device *pdev = to_platform_device(dev);
	int i, j;
	int ret;

	csid->camss = camss;
	csid->id = id;
	csid->res = &res->csid;

	if (dev_WARN_ONCE(dev, !csid->res->parent_dev_ops,
			  "Error: CSID depends on VFE/IFE device ops!\n")) {
		return -EINVAL;
	}

	csid->res->hw_ops->subdev_init(csid);

	/* Memory */

	if (camss->res->version == CAMSS_8250) {
		/* for titan 480, CSID registers are inside the VFE region,
		 * between the VFE "top" and "bus" registers. this requires
		 * VFE to be initialized before CSID
		 */
		if (id >= 2) /* VFE/CSID lite */
			csid->base = csid->res->parent_dev_ops->get_base_address(camss, id)
				+ VFE_480_LITE_CSID_OFFSET;
		else
			csid->base = csid->res->parent_dev_ops->get_base_address(camss, id)
				 + VFE_480_CSID_OFFSET;
	} else {
		csid->base = devm_platform_ioremap_resource_byname(pdev, res->reg[0]);
		if (IS_ERR(csid->base))
			return PTR_ERR(csid->base);
	}

	/* Interrupt */

	ret = platform_get_irq_byname(pdev, res->interrupt[0]);
	if (ret < 0)
		return ret;

	csid->irq = ret;
	snprintf(csid->irq_name, sizeof(csid->irq_name), "%s_%s%d",
		 dev_name(dev), MSM_CSID_NAME, csid->id);
	ret = devm_request_irq(dev, csid->irq, csid->res->hw_ops->isr,
			       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
			       csid->irq_name, csid);
	if (ret < 0) {
		dev_err(dev, "request_irq failed: %d\n", ret);
		return ret;
	}

	/* Clocks */

	csid->nclocks = 0;
	while (res->clock[csid->nclocks])
		csid->nclocks++;

	csid->clock = devm_kcalloc(dev, csid->nclocks, sizeof(*csid->clock),
				    GFP_KERNEL);
	if (!csid->clock)
		return -ENOMEM;

	for (i = 0; i < csid->nclocks; i++) {
		struct camss_clock *clock = &csid->clock[i];

		clock->clk = devm_clk_get(dev, res->clock[i]);
		if (IS_ERR(clock->clk))
			return PTR_ERR(clock->clk);

		clock->name = res->clock[i];

		clock->nfreqs = 0;
		while (res->clock_rate[i][clock->nfreqs])
			clock->nfreqs++;

		if (!clock->nfreqs) {
			clock->freq = NULL;
			continue;
		}

		clock->freq = devm_kcalloc(dev,
					   clock->nfreqs,
					   sizeof(*clock->freq),
					   GFP_KERNEL);
		if (!clock->freq)
			return -ENOMEM;

		for (j = 0; j < clock->nfreqs; j++)
			clock->freq[j] = res->clock_rate[i][j];
	}

	/* Regulator */
	for (i = 0; i < ARRAY_SIZE(res->regulators); i++) {
		if (res->regulators[i].supply)
			csid->num_supplies++;
	}

	ret = devm_regulator_bulk_get_const(camss->dev, csid->num_supplies,
					    res->regulators, &csid->supplies);
	if (ret)
		return ret;

	init_completion(&csid->reset_complete);

	return 0;
}

/*
 * msm_csid_get_csid_id - Get CSID HW module id
 * @entity: Pointer to CSID media entity structure
 * @id: Return CSID HW module id here
 */
void msm_csid_get_csid_id(struct media_entity *entity, u8 *id)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct csid_device *csid = v4l2_get_subdevdata(sd);

	*id = csid->id;
}

/*
 * csid_get_lane_assign - Calculate lane assign by csiphy/tpg lane num
 * @lane_cfg: CSI2 lane configuration
 * @num_lanes: lane num
 *
 * Return lane assign
 */
static u32 csid_get_lane_assign(struct csiphy_lanes_cfg *lane_cfg, int num_lanes)
{
	u32 lane_assign = 0;
	int pos;
	int i;

	for (i = 0; i < num_lanes; i++) {
		pos = lane_cfg ? lane_cfg->data[i].pos : i;
		lane_assign |= pos << (i * LANE_CFG_BITWIDTH);
	}

	return lane_assign;
}

/*
 * csid_link_setup - Setup CSID connections
 * @entity: Pointer to media entity structure
 * @local: Pointer to local pad
 * @remote: Pointer to remote pad
 * @flags: Link flags
 *
 * Return 0 on success
 */
static int csid_link_setup(struct media_entity *entity,
			   const struct media_pad *local,
			   const struct media_pad *remote, u32 flags)
{
	if (flags & MEDIA_LNK_FL_ENABLED)
		if (media_pad_remote_pad_first(local))
			return -EBUSY;

	if ((local->flags & MEDIA_PAD_FL_SINK) &&
	    (flags & MEDIA_LNK_FL_ENABLED)) {
		struct v4l2_subdev *sd;
		struct tpg_device *tpg;
		struct csid_device *csid;
		struct csiphy_device *csiphy;
		struct csiphy_lanes_cfg *lane_cfg;

		sd = media_entity_to_v4l2_subdev(entity);
		csid = v4l2_get_subdevdata(sd);

		/* If test generator is enabled */
		/* do not allow a link from CSIPHY to CSID */
		if (csid->testgen.nmodes != CSID_PAYLOAD_MODE_DISABLED &&
		    csid->testgen_mode->cur.val != 0)
			return -EBUSY;

		sd = media_entity_to_v4l2_subdev(remote->entity);
		if (sd->grp_id == TPG_GRP_ID) {
			tpg = v4l2_get_subdevdata(sd);

			csid->phy.lane_cnt = tpg->res->lane_cnt;
			csid->phy.csiphy_id = tpg->id;
			csid->phy.lane_assign = csid_get_lane_assign(NULL, csid->phy.lane_cnt);
			csid->tpg_linked = true;
		} else {
			csiphy = v4l2_get_subdevdata(sd);

			/* If a sensor is not linked to CSIPHY */
			/* do no allow a link from CSIPHY to CSID */
			if (!csiphy->cfg.csi2)
				return -EPERM;

			csid->phy.csiphy_id = csiphy->id;

			lane_cfg = &csiphy->cfg.csi2->lane_cfg;
			csid->phy.lane_cnt = lane_cfg->num_data;
			csid->phy.lane_assign = csid_get_lane_assign(lane_cfg, lane_cfg->num_data);
			csid->tpg_linked = false;
		}
	}
	/* Decide which virtual channels to enable based on which source pads are enabled */
	if (local->flags & MEDIA_PAD_FL_SOURCE) {
		struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
		struct csid_device *csid = v4l2_get_subdevdata(sd);
		struct device *dev = csid->camss->dev;

		if (flags & MEDIA_LNK_FL_ENABLED)
			csid->phy.en_vc |= BIT(local->index - 1);
		else
			csid->phy.en_vc &= ~BIT(local->index - 1);

		csid->phy.need_vc_update = true;

		dev_dbg(dev, "%s: Enabled CSID virtual channels mask 0x%x\n",
			__func__, csid->phy.en_vc);
	}

	return 0;
}

static const struct v4l2_subdev_core_ops csid_core_ops = {
	.s_power = csid_set_power,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops csid_video_ops = {
	.s_stream = csid_set_stream,
};

static const struct v4l2_subdev_pad_ops csid_pad_ops = {
	.enum_mbus_code = csid_enum_mbus_code,
	.enum_frame_size = csid_enum_frame_size,
	.get_fmt = csid_get_format,
	.set_fmt = csid_set_format,
};

static const struct v4l2_subdev_ops csid_v4l2_ops = {
	.core = &csid_core_ops,
	.video = &csid_video_ops,
	.pad = &csid_pad_ops,
};

/*
 * csid_get_stream_csi2_desc - Discover the virtual channel and data type
 *			       used by a given sink stream, from an
 *			       already-fetched frame descriptor
 * @frame_desc: Frame descriptor fetched via .get_frame_desc from the remote
 *		subdev linked on the sink pad
 * @sink_stream: Sink-side stream number to look up
 * @desc_csi2: Returns the discovered virtual channel/data type on success
 *
 * A frame descriptor with a single entry means the remote only exposes one
 * stream (e.g. a single-VC sensor), which feeds every CSID source pad, so
 * that entry is used regardless of @sink_stream.
 *
 * Return true if a matching entry was found, false otherwise
 */
static bool csid_get_stream_csi2_desc(struct v4l2_mbus_frame_desc *frame_desc,
				      u32 sink_stream,
				      struct v4l2_mbus_frame_desc_entry_csi2 *desc_csi2)
{
	unsigned int i;

	if (frame_desc->type != V4L2_MBUS_FRAME_DESC_TYPE_CSI2 || !frame_desc->num_entries)
		return false;

	if (frame_desc->num_entries == 1) {
		*desc_csi2 = frame_desc->entry[0].bus.csi2;
		return true;
	}

	for (i = 0; i < frame_desc->num_entries; i++) {
		if (frame_desc->entry[i].stream == sink_stream) {
			*desc_csi2 = frame_desc->entry[i].bus.csi2;
			return true;
		}
	}

	return false;
}

/*
 * csid_get_stream_vc_dt - Discover the virtual channel and data type to
 *			   program for a given sink pad/stream, falling back
 *			   to @format_dt when no frame descriptor is available
 * @csid: CSID device
 * @state: V4L2 subdevice state
 * @remote_pad: Remote pad linked on the CSID sink pad, or NULL if unlinked
 * @pad: Source pad number the caller is enabling a stream on
 * @format_dt: Data type derived from the sink format, used as a fallback
 *	       and sanity-checked against the discovered data type
 *
 * Return the discovered virtual channel/data type, or {0, @format_dt} if
 * not discovered
 */
static struct v4l2_mbus_frame_desc_entry_csi2
csid_get_stream_vc_dt(struct csid_device *csid, struct v4l2_subdev_state *state,
		      struct media_pad *remote_pad, u32 pad, u8 format_dt)
{
	struct v4l2_mbus_frame_desc_entry_csi2 desc_csi2 = { .dt = format_dt };
	struct v4l2_mbus_frame_desc fd = { };
	u32 sink_stream;

	if (!remote_pad ||
	    v4l2_subdev_call(media_entity_to_v4l2_subdev(remote_pad->entity),
			     pad, get_frame_desc, remote_pad->index, &fd))
		return desc_csi2;

	if (v4l2_subdev_routing_find_opposite_end(&state->routing, pad, 0, NULL, &sink_stream))
		return desc_csi2;

	if (!csid_get_stream_csi2_desc(&fd, sink_stream, &desc_csi2)) {
		dev_warn(csid->camss->dev,
			 "Failed to find CSI2 descriptor for sink stream %u, using vc=%u dt=%u\n",
			 sink_stream, desc_csi2.vc, desc_csi2.dt);
		return desc_csi2;
	}

	if (desc_csi2.dt != format_dt)
		dev_warn(csid->camss->dev,
			 "Sink stream %u frame desc dt=%u differs from format dt=%u, using dt=%u\n",
			 sink_stream, desc_csi2.dt, format_dt, desc_csi2.dt);

	return desc_csi2;
}

/*
 * csid_pad_enable_streams - Enable one or more streams on a source pad
 * @sd: CSID V4L2 subdevice
 * @state: V4L2 subdevice state
 * @pad: Pad number
 * @streams_mask: Bitmask of v4l2 streams to enable
 *
 * The v4l2 core only calls this on a source pad (v4l2_subdev_enable_streams()
 * rejects sink pads with -EOPNOTSUPP before reaching the driver), so @pad is
 * not checked here. Each source pad only ever carries stream 0.
 *
 * The shared sink stream(s) are propagated upstream only once, on the
 * transition from no active sink streams to at least one, so that a second
 * consumer of the same shared sink stream never triggers a second, redundant
 * propagation to the sensor. The Rx front-end is likewise only configured
 * once, on that same transition.
 *
 * Return 0 on success, -ENOLINK if there is no remote sink link and the test
 * generator is disabled, or another negative error code otherwise
 */
static int csid_pad_enable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   u32 pad, u64 streams_mask)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	const struct csid_hw_ops *hw_ops = csid->res->hw_ops;
	struct media_pad *remote_pad = media_pad_remote_pad_first(&csid->pads[MSM_CSID_PAD_SINK]);
	unsigned int hw_port = pad - MSM_CSID_PAD_FIRST_SRC;
	const struct csid_format_info *format;
	struct v4l2_mbus_frame_desc_entry_csi2 desc_csi2;
	u64 sink_streams, propagate_mask;
	int ret;

	if (!csid->testgen.enabled && !remote_pad)
		return -ENOLINK;

	sink_streams = v4l2_subdev_state_xlate_streams(state, pad, MSM_CSID_PAD_SINK,
						       &streams_mask);

	if (!csid->enabled_streams[MSM_CSID_PAD_SINK]) {
		if (csid->testgen.nmodes != CSID_PAYLOAD_MODE_DISABLED) {
			/*
			 * sd->state_lock is aliased to csid->ctrls.lock, and is
			 * already held here by the v4l2_subdev_enable_streams()
			 * caller, so use the lock-free variant to avoid
			 * self-deadlocking on the same mutex.
			 */
			ret = __v4l2_ctrl_handler_setup(&csid->ctrls);
			if (ret < 0) {
				dev_err(csid->camss->dev,
					"could not sync v4l2 controls: %d\n", ret);
				return ret;
			}
		}

		hw_ops->configure_rx(csid);
	}

	/* Sink streams already active elsewhere don't need re-propagating. */
	propagate_mask = sink_streams & ~csid->enabled_streams[MSM_CSID_PAD_SINK];
	csid->enabled_streams[MSM_CSID_PAD_SINK] |= sink_streams;
	csid->enabled_streams[pad] |= streams_mask;

	format = csid_get_fmt_entry(csid->res->formats->formats,
				    csid->res->formats->nformats,
				    csid->fmt[pad].code);
	desc_csi2 = csid_get_stream_vc_dt(csid, state, remote_pad, pad, format->data_type);

	hw_ops->enable_stream(csid, hw_port, desc_csi2.vc, desc_csi2.dt);

	if (propagate_mask && remote_pad) {
		ret = v4l2_subdev_enable_streams(media_entity_to_v4l2_subdev(remote_pad->entity),
						 remote_pad->index, propagate_mask);
		if (ret) {
			csid->enabled_streams[MSM_CSID_PAD_SINK] &= ~propagate_mask;
			csid->enabled_streams[pad] &= ~streams_mask;

			hw_ops->disable_stream(csid, hw_port);

			return ret;
		}
	}

	return 0;
}

/*
 * csid_sink_streams_in_use - Compute the subset of sink streams still
 *			      referenced by a source pad other than @pad
 * @csid: CSID device
 * @state: V4L2 subdevice state
 * @pad: Source pad to exclude from the check
 * @sink_streams: Candidate sink streams to check
 *
 * Return the subset of @sink_streams still referenced by some other source
 * pad
 */
static u64 csid_sink_streams_in_use(struct csid_device *csid, struct v4l2_subdev_state *state,
				    u32 pad, u64 sink_streams)
{
	u64 in_use = 0;
	unsigned int i;

	for (i = MSM_CSID_PAD_FIRST_SRC; i < MSM_CSID_PADS_NUM; i++) {
		u64 other_streams = csid->enabled_streams[i];
		u64 other_sink_streams;

		if (i == pad)
			continue;

		other_sink_streams = v4l2_subdev_state_xlate_streams(state, i, MSM_CSID_PAD_SINK,
								     &other_streams);
		in_use |= sink_streams & other_sink_streams;
	}

	return in_use;
}

/*
 * csid_pad_disable_streams - Disable one or more streams on a source pad
 * @sd: CSID V4L2 subdevice
 * @state: V4L2 subdevice state
 * @pad: Pad number
 * @streams_mask: Bitmask of v4l2 streams to disable
 *
 * The v4l2 core only calls this on a source pad (v4l2_subdev_disable_streams()
 * rejects sink pads with -EOPNOTSUPP before reaching the driver), so @pad is
 * not checked here. Each source pad only ever carries stream 0.
 *
 * A sink stream is only disabled, and propagated upstream to disable it there
 * too, once no source pad references it any more.
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_pad_disable_streams(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    u32 pad, u64 streams_mask)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	const struct csid_hw_ops *hw_ops = csid->res->hw_ops;
	struct media_pad *remote_pad = media_pad_remote_pad_first(&csid->pads[MSM_CSID_PAD_SINK]);
	unsigned int hw_port = pad - MSM_CSID_PAD_FIRST_SRC;
	u64 sink_streams, disable_sink_streams;
	int ret = 0;

	sink_streams = v4l2_subdev_state_xlate_streams(state, pad, MSM_CSID_PAD_SINK,
						       &streams_mask);

	/* Keep a sink stream active as long as any other source pad still uses it. */
	disable_sink_streams = sink_streams &
				~csid_sink_streams_in_use(csid, state, pad, sink_streams);

	if (disable_sink_streams && remote_pad) {
		ret = v4l2_subdev_disable_streams(media_entity_to_v4l2_subdev(remote_pad->entity),
						  remote_pad->index, disable_sink_streams);
		if (ret)
			dev_err(csid->camss->dev,
				"Failed to disable stream on remote pad: %d\n", ret);
	}

	hw_ops->disable_stream(csid, hw_port);

	csid->enabled_streams[pad] &= ~streams_mask;
	csid->enabled_streams[MSM_CSID_PAD_SINK] &= ~disable_sink_streams;

	return ret;
}

static const struct v4l2_mbus_framefmt csid_default_format = {
	.code = MEDIA_BUS_FMT_UYVY8_1X16,
	.width = 1920,
	.height = 1080,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_SRGB,
};

/*
 * csid_set_routing - Handle setting of routing table
 * @sd: CSID V4L2 subdevice
 * @state: V4L2 subdevice state
 * @which: TRY or ACTIVE routing
 * @routing: Routing table to set
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_set_routing(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    enum v4l2_subdev_format_whence which,
			    struct v4l2_subdev_krouting *routing)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	unsigned int i;
	int ret;

	if (which == V4L2_SUBDEV_FORMAT_ACTIVE && csid->enabled_streams[MSM_CSID_PAD_SINK])
		return -EBUSY;

	for (i = 0; i < routing->num_routes; i++)
		if (routing->routes[i].source_stream != 0)
			return -EINVAL;

	ret = v4l2_subdev_routing_validate(sd, routing,
					   V4L2_SUBDEV_ROUTING_NO_SOURCE_STREAM_MIX |
					   V4L2_SUBDEV_ROUTING_NO_SOURCE_MULTIPLEXING |
					   V4L2_SUBDEV_ROUTING_NO_N_TO_1);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(sd, state, routing, &csid_default_format);
}

/*
 * __csid_get_stream_format - Get pointer to per-stream format structure
 * @csid: CSID device
 * @sd_state: V4L2 subdev state
 * @pad: pad from which format is requested
 * @stream: stream from which format is requested
 * @which: TRY or ACTIVE format
 *
 * Same as __csid_get_format(), but honors @stream for TRY-state lookups.
 * For ACTIVE state, csid->fmt[] is indexed by pad + stream. @stream is
 * always 0 and @pad selects the RDI channel (0-3).
 *
 * Return pointer to TRY or ACTIVE format structure
 */
static struct v4l2_mbus_framefmt *
__csid_get_stream_format(struct csid_device *csid,
			 struct v4l2_subdev_state *sd_state,
			 unsigned int pad, u32 stream,
			 enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_state_get_format(sd_state, pad, stream);

	if (pad == MSM_CSID_PAD_SINK)
		return &csid->fmt[MSM_CSID_PAD_SINK];

	return &csid->fmt[pad + stream];
}

/*
 * csid_streams_get_format - Handle get format by pads subdev method
 * @sd: CSID V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fmt: pointer to v4l2 subdev format structure
 *
 * Return -EINVAL or zero on success
 */
static int csid_streams_get_format(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *fmt)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *format;

	format = __csid_get_stream_format(csid, sd_state, fmt->pad, fmt->stream, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

/*
 * csid_streams_set_format - Handle set format by pads subdev method
 * @sd: CSID V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fmt: pointer to v4l2 subdev format structure
 *
 * Return -EINVAL or zero on success
 */
static int csid_streams_set_format(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *fmt)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_route *route;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE && csid->enabled_streams[MSM_CSID_PAD_SINK])
		return -EBUSY;

	format = __csid_get_stream_format(csid, sd_state, fmt->pad, fmt->stream, fmt->which);
	if (!format)
		return -EINVAL;

	csid_try_format(csid, sd_state, fmt->pad, &fmt->format, fmt->which);
	*format = fmt->format;

	/* Propagate the format from the sink stream to every source stream it feeds */
	for_each_active_route(&sd_state->routing, route) {
		struct v4l2_mbus_framefmt *src_format;

		if (route->sink_pad != fmt->pad || route->sink_stream != fmt->stream)
			continue;

		src_format = __csid_get_stream_format(csid, sd_state, route->source_pad,
						      route->source_stream, fmt->which);
		if (!src_format)
			continue;

		*src_format = fmt->format;
		csid_try_format(csid, sd_state, route->source_pad, src_format, fmt->which);
	}

	return 0;
}

static const struct v4l2_subdev_pad_ops csid_streams_pad_ops = {
	.enum_mbus_code = csid_enum_mbus_code,
	.enum_frame_size = csid_enum_frame_size,
	.get_fmt = csid_streams_get_format,
	.set_fmt = csid_streams_set_format,
	.set_routing = csid_set_routing,
	.enable_streams = csid_pad_enable_streams,
	.disable_streams = csid_pad_disable_streams,
};

static const struct v4l2_subdev_video_ops csid_streams_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_ops csid_streams_v4l2_ops = {
	.core = &csid_core_ops,
	.pad = &csid_streams_pad_ops,
	.video = &csid_streams_video_ops,
};

/*
 * csid_init_state - Initialize the routing table for the streams API subdev
 * @sd: CSID V4L2 subdevice
 * @state: V4L2 subdev state
 *
 * source_stream is always 0: each source pad MSM_CSID_PAD_FIRST_SRC + i
 * links to its own independent downstream subdev, and a link's sink side is
 * validated against the implicit stream 0 exposed by any subdev without
 * V4L2_SUBDEV_FL_STREAMS (see v4l2_link_validate_get_streams()) — every
 * downstream VFE line is such a subdev.
 *
 * All source pads route from sink_stream 0 by default, fanning the single
 * incoming stream out to every port; a multi-VC source is supported by
 * remapping each route's sink_stream via .set_routing, leaving
 * source_pad/source_stream untouched.
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	struct csid_device *csid = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_route routes[MSM_CSID_MAX_SRC_STREAMS];
	struct v4l2_subdev_krouting routing = { };
	unsigned int num_routes;
	int i, ret;

	/* The full IFE has only 3 rdi's and pix output is not functional */
	if (csid_is_lite(csid))
		num_routes = MSM_CSID_MAX_SRC_STREAMS;
	else
		num_routes = MSM_CSID_MAX_SRC_STREAMS - 1;

	for (i = 0; i < num_routes; i++) {
		routes[i].sink_pad = MSM_CSID_PAD_SINK;
		routes[i].sink_stream = 0;
		routes[i].source_pad = MSM_CSID_PAD_FIRST_SRC + i;
		routes[i].source_stream = 0;
		routes[i].flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;
	}

	routing.num_routes = num_routes;
	routing.routes = routes;
	ret = v4l2_subdev_set_routing_with_fmt(sd, state, &routing, &csid_default_format);
	if (ret)
		dev_err(csid->camss->dev, "Failed to set routing: %d\n", ret);

	return ret;
}

static const struct v4l2_subdev_internal_ops csid_v4l2_internal_ops = {
	.open = csid_init_formats,
};

static const struct v4l2_subdev_internal_ops csid_streams_internal_ops = {
	.init_state = csid_init_state,
};

static const struct media_entity_operations csid_media_ops = {
	.link_setup = csid_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

/*
 * msm_csid_register_entity - Register subdev node for CSID module
 * @csid: CSID device
 * @v4l2_dev: V4L2 device
 *
 * Return 0 on success or a negative error code otherwise
 */
int msm_csid_register_entity(struct csid_device *csid,
			     struct v4l2_device *v4l2_dev)
{
	struct v4l2_subdev *sd = &csid->subdev;
	struct media_pad *pads = csid->pads;
	struct device *dev = csid->camss->dev;
	bool streams_api = csid->res->streams_enable;
	unsigned int num_pads = csid_is_lite(csid) ? MSM_CSID_PADS_NUM : MSM_CSID_PADS_NUM - 1;
	int i;
	int ret;

	v4l2_subdev_init(sd, streams_api ? &csid_streams_v4l2_ops : &csid_v4l2_ops);
	sd->internal_ops = streams_api ? &csid_streams_internal_ops
					: &csid_v4l2_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS;
	if (streams_api)
		sd->flags |= V4L2_SUBDEV_FL_STREAMS;
	snprintf(sd->name, ARRAY_SIZE(sd->name), "%s%d",
		 MSM_CSID_NAME, csid->id);
	v4l2_set_subdevdata(sd, csid);

	if (csid->testgen.nmodes != CSID_PAYLOAD_MODE_DISABLED) {
		ret = v4l2_ctrl_handler_init(&csid->ctrls, 1);
		if (ret < 0) {
			dev_err(dev, "Failed to init ctrl handler: %d\n", ret);
			return ret;
		}

		csid->testgen_mode =
			v4l2_ctrl_new_std_menu_items(&csid->ctrls,
						     &csid_ctrl_ops, V4L2_CID_TEST_PATTERN,
						     csid->testgen.nmodes, 0, 0,
						     csid->testgen.modes);

		if (csid->ctrls.error) {
			dev_err(dev, "Failed to init ctrl: %d\n", csid->ctrls.error);
			ret = csid->ctrls.error;
			goto free_ctrl;
		}

		csid->subdev.ctrl_handler = &csid->ctrls;
	}

	ret = csid_init_formats(sd, NULL);
	if (ret < 0) {
		dev_err(dev, "Failed to init format: %d\n", ret);
		goto free_ctrl;
	}

	pads[MSM_CSID_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	for (i = MSM_CSID_PAD_FIRST_SRC; i < num_pads; ++i)
		pads[i].flags = MEDIA_PAD_FL_SOURCE;

	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	sd->entity.ops = &csid_media_ops;
	ret = media_entity_pads_init(&sd->entity, num_pads, pads);
	if (ret < 0) {
		dev_err(dev, "Failed to init media entity: %d\n", ret);
		goto free_ctrl;
	}

	if (streams_api) {
		if (csid->testgen.nmodes != CSID_PAYLOAD_MODE_DISABLED)
			sd->state_lock = csid->ctrls.lock;

		ret = v4l2_subdev_init_finalize(sd);
		if (ret) {
			dev_err(dev, "Failed to finalize subdev: %d\n", ret);
			goto media_cleanup;
		}
	}

	ret = v4l2_device_register_subdev(v4l2_dev, sd);
	if (ret < 0) {
		dev_err(dev, "Failed to register subdev: %d\n", ret);
		goto media_cleanup;
	}

	return 0;

media_cleanup:
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
free_ctrl:
	if (csid->testgen.nmodes != CSID_PAYLOAD_MODE_DISABLED)
		v4l2_ctrl_handler_free(&csid->ctrls);

	return ret;
}

/*
 * msm_csid_unregister_entity - Unregister CSID module subdev node
 * @csid: CSID device
 */
void msm_csid_unregister_entity(struct csid_device *csid)
{
	v4l2_device_unregister_subdev(&csid->subdev);
	v4l2_subdev_cleanup(&csid->subdev);
	media_entity_cleanup(&csid->subdev.entity);
	if (csid->testgen.nmodes != CSID_PAYLOAD_MODE_DISABLED)
		v4l2_ctrl_handler_free(&csid->ctrls);
}

inline bool csid_is_lite(struct csid_device *csid)
{
	return csid->camss->res->csid_res[csid->id].csid.is_lite;
}
