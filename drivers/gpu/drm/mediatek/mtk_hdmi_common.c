// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Copyright (c) 2022 BayLibre, SAS
 * Copyright (c) 2024 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <drm/drm_modes.h>
#include <linux/device.h>
#include <linux/hdmi.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <sound/hdmi-codec.h>

#include "mtk_hdmi_common.h"

struct hdmi_acr_n {
	unsigned int clock;
	unsigned int n[3];
};

/* Recommended N values from HDMI specification, tables 7-1 to 7-3 */
static const struct hdmi_acr_n hdmi_rec_n_table[] = {
	/* Clock, N: 32kHz 44.1kHz 48kHz */
	{ 25175,  {  4576,  7007,  6864 } },
	{ 74176,  { 11648, 17836, 11648 } },
	{ 148352, { 11648,  8918,  5824 } },
	{ 296703, {  5824,  4459,  5824 } },
	{ 297000, {  3072,  4704,  5120 } },
	{ 0,      {  4096,  6272,  6144 } }, /* all other TMDS clocks */
};

/**
 * hdmi_recommended_n() - Return N value recommended by HDMI specification
 * @freq: audio sample rate in Hz
 * @clock: rounded TMDS clock in kHz
 */
static int hdmi_recommended_n(unsigned int freq, unsigned int clock)
{
	const struct hdmi_acr_n *recommended;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hdmi_rec_n_table) - 1; i++) {
		if (clock == hdmi_rec_n_table[i].clock)
			break;
	}

	if (i == ARRAY_SIZE(hdmi_rec_n_table))
		return -EINVAL;

	recommended = hdmi_rec_n_table + i;

	switch (freq) {
	case 32000:
		return recommended->n[0];
	case 44100:
		return recommended->n[1];
	case 48000:
		return recommended->n[2];
	case 88200:
		return recommended->n[1] * 2;
	case 96000:
		return recommended->n[2] * 2;
	case 176400:
		return recommended->n[1] * 4;
	case 192000:
		return recommended->n[2] * 4;
	default:
		return (128 * freq) / 1000;
	}
}

static unsigned int hdmi_mode_clock_to_hz(unsigned int clock)
{
	switch (clock) {
	case 25175:
		return 25174825; /* 25.2/1.001 MHz */
	case 74176:
		return 74175824; /* 74.25/1.001 MHz */
	case 148352:
		return 148351648; /* 148.5/1.001 MHz */
	case 296703:
		return 296703297; /* 297/1.001 MHz */
	default:
		return clock * 1000;
	}
}

static unsigned int hdmi_expected_cts(unsigned int audio_sample_rate,
				      unsigned int tmds_clock, unsigned int n)
{
	return DIV_ROUND_CLOSEST_ULL((u64)hdmi_mode_clock_to_hz(tmds_clock) * n,
				     128 * audio_sample_rate);
}

int mtk_hdmi_get_ncts(unsigned int sample_rate, unsigned int clock,
		      unsigned int *n, unsigned int *cts)
{
	int rec_n = hdmi_recommended_n(sample_rate, clock);

	if (rec_n < 0)
		return rec_n;

	*cts = hdmi_expected_cts(sample_rate, clock, rec_n);
	*n = rec_n;

	return 0;
}

static int mtk_hdmi_setup_spd_infoframe(struct mtk_hdmi *hdmi, u8 *buffer, size_t bufsz,
					const char *vendor, const char *product)
{
	struct hdmi_spd_infoframe frame;
	ssize_t err;

	err = hdmi_spd_infoframe_init(&frame, vendor, product);
	if (err < 0) {
		dev_err(hdmi->dev, "Failed to initialize SPD infoframe: %zd\n", err);
		return err;
	}

	err = hdmi_spd_infoframe_pack(&frame, buffer, bufsz);
	if (err < 0) {
		dev_err(hdmi->dev, "Failed to pack SDP infoframe: %zd\n", err);
		return err;
	}

	return 0;
}

static int mtk_hdmi_get_all_clk(struct mtk_hdmi *hdmi, struct device_node *np,
				const char * const *mtk_hdmi_clk_names, size_t num_clocks)
{
	int i;

	for (i = 0; i < num_clocks; i++) {
		hdmi->clk[i] = of_clk_get_by_name(np, mtk_hdmi_clk_names[i]);

		if (IS_ERR(hdmi->clk[i]))
			return PTR_ERR(hdmi->clk[i]);
	}

	return 0;
}

bool mtk_hdmi_bridge_mode_fixup(struct drm_bridge *bridge,
				const struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	return true;
}

void mtk_hdmi_bridge_mode_set(struct drm_bridge *bridge,
			      const struct drm_display_mode *mode,
			      const struct drm_display_mode *adjusted_mode)
{
	struct mtk_hdmi *hdmi = hdmi_ctx_from_bridge(bridge);

	drm_mode_copy(&hdmi->mode, adjusted_mode);
}

static int mtk_hdmi_setup_avi_infoframe(struct mtk_hdmi *hdmi, u8 *buffer, size_t bufsz,
					struct drm_display_mode *mode)
{
	struct hdmi_avi_infoframe frame;
	ssize_t err;

	err = drm_hdmi_avi_infoframe_from_display_mode(&frame, hdmi->curr_conn, mode);
	if (err < 0) {
		dev_err(hdmi->dev,
			"Failed to get AVI infoframe from mode: %zd\n", err);
		return err;
	}

	err = hdmi_avi_infoframe_pack(&frame, buffer, bufsz);
	if (err < 0) {
		dev_err(hdmi->dev, "Failed to pack AVI infoframe: %zd\n", err);
		return err;
	}

	return 0;
}

void mtk_hdmi_send_infoframe(struct mtk_hdmi *hdmi, u8 *buffer_spd, size_t bufsz_spd,
			     u8 *buffer_avi, size_t bufsz_avi, struct drm_display_mode *mode)
{
	mtk_hdmi_setup_avi_infoframe(hdmi, buffer_avi, bufsz_avi, mode);
	mtk_hdmi_setup_spd_infoframe(hdmi, buffer_spd, bufsz_spd, "mediatek", "On-chip HDMI");
}

static int mtk_hdmi_get_cec_dev(struct mtk_hdmi *hdmi, struct device *dev, struct device_node *np)
{
	struct platform_device *cec_pdev;
	struct device_node *cec_np;
	int ret;

	/* The CEC module handles HDMI hotplug detection */
	cec_np = of_get_compatible_child(np->parent, "mediatek,mt8173-cec");
	if (!cec_np)
		return dev_err_probe(dev, -ENOTSUPP, "Failed to find CEC node\n");

	cec_pdev = of_find_device_by_node(cec_np);
	if (!cec_pdev) {
		dev_err(hdmi->dev, "Waiting for CEC device %pOF\n", cec_np);
		of_node_put(cec_np);
		return -EPROBE_DEFER;
	}
	of_node_put(cec_np);

	/*
	 * The mediatek,syscon-hdmi property contains a phandle link to the
	 * MMSYS_CONFIG device and the register offset of the HDMI_SYS_CFG
	 * registers it contains.
	 */
	hdmi->sys_regmap = syscon_regmap_lookup_by_phandle(np, "mediatek,syscon-hdmi");
	if (IS_ERR(hdmi->sys_regmap))
		return PTR_ERR(hdmi->sys_regmap);

	ret = of_property_read_u32_index(np, "mediatek,syscon-hdmi", 1, &hdmi->sys_offset);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get system configuration registers\n");

	hdmi->cec_dev = &cec_pdev->dev;
	return 0;
}

static int mtk_hdmi_dt_parse_pdata(struct mtk_hdmi *hdmi, struct platform_device *pdev,
				   const char * const *clk_names, size_t num_clocks)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *remote, *i2c_np;
	int ret;

	ret = mtk_hdmi_get_all_clk(hdmi, np, clk_names, num_clocks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get clocks\n");

	hdmi->irq = platform_get_irq(pdev, 0);
	if (!hdmi->irq)
		return hdmi->irq;

	hdmi->regs = device_node_to_regmap(dev->of_node);
	if (IS_ERR(hdmi->regs))
		return PTR_ERR(hdmi->regs);

	/* Populate HDMI sub-devices if present */
	ret = devm_of_platform_populate(&pdev->dev);
	if (ret)
		return ret;

	remote = of_graph_get_remote_node(np, 1, 0);
	if (!remote)
		return dev_err_probe(dev, -EINVAL, "Cannot find HDMI input port\n");

	if (!of_device_is_compatible(remote, "hdmi-connector")) {
		hdmi->next_bridge = of_drm_find_bridge(remote);
		if (!hdmi->next_bridge) {
			dev_err(dev, "Waiting for external bridge\n");
			of_node_put(remote);
			return -EPROBE_DEFER;
		}
	}

	i2c_np = of_parse_phandle(remote, "ddc-i2c-bus", 0);
	if (!i2c_np) {
		of_node_put(pdev->dev.of_node);
		return dev_err_probe(dev, -EINVAL, "No ddc-i2c-bus in connector\n");
	}

	hdmi->ddc_adpt = of_find_i2c_adapter_by_node(i2c_np);
	of_node_put(i2c_np);
	if (!hdmi->ddc_adpt) {
		dev_err(dev, "Failed to get ddc i2c adapter by node");
		return -EPROBE_DEFER;
	}

	ret = mtk_hdmi_get_cec_dev(hdmi, dev, np);
	if (ret == -ENOTSUPP)
		dev_info(dev, "CEC support unavailable: node not found\n");
	else if (ret)
		return ret;

	return 0;
}

static void mtk_hdmi_unregister_audio_driver(void *data)
{
	platform_device_unregister(data);
}

static int mtk_hdmi_register_audio_driver(struct device *dev)
{
	struct mtk_hdmi *hdmi = dev_get_drvdata(dev);
	struct hdmi_codec_pdata codec_data = {
		.i2s = 1,
		.max_i2s_channels = 2,
		.data = hdmi,
	};
	int ret;

	if (hdmi->conf->ver_conf->mtk_hdmi_audio_init)
		hdmi->conf->ver_conf->mtk_hdmi_audio_init(hdmi, &codec_data);

	hdmi->audio_pdev = platform_device_register_data(dev,
							 HDMI_CODEC_DRV_NAME,
							 PLATFORM_DEVID_AUTO,
							 &codec_data,
							 sizeof(codec_data));
	if (IS_ERR(hdmi->audio_pdev))
		return PTR_ERR(hdmi->audio_pdev);

	ret = devm_add_action_or_reset(dev, mtk_hdmi_unregister_audio_driver,
				       hdmi->audio_pdev);
	if (ret) {
		platform_device_unregister(hdmi->audio_pdev);
		return ret;
	}

	return 0;
}

int mtk_hdmi_common_probe(struct platform_device *pdev, struct mtk_hdmi *hdmi)
{
	const struct mtk_hdmi_ver_conf *ver_conf;
	struct device *dev = &pdev->dev;
	int ret;

	hdmi->dev = dev;
	hdmi->conf = of_device_get_match_data(dev);
	ver_conf = hdmi->conf->ver_conf;

	hdmi->clk = devm_kcalloc(dev, ver_conf->num_clocks, sizeof(*hdmi->clk), GFP_KERNEL);
	if (!hdmi->clk)
		return -ENOMEM;

	hdmi->phy = devm_phy_get(dev, "hdmi");
	if (IS_ERR(hdmi->phy))
		return dev_err_probe(dev, PTR_ERR(hdmi->phy), "Failed to get HDMI PHY\n");

	ret = mtk_hdmi_dt_parse_pdata(hdmi, pdev, ver_conf->mtk_hdmi_clock_names,
				      ver_conf->num_clocks);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, hdmi);

	ret = mtk_hdmi_register_audio_driver(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register HDMI Audio driver\n");

	hdmi->bridge.funcs = ver_conf->bridge_funcs;
	hdmi->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID | DRM_BRIDGE_OP_HPD;
	hdmi->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	hdmi->bridge.of_node = pdev->dev.of_node;
	hdmi->bridge.ddc = hdmi->ddc_adpt;

	ret = devm_drm_bridge_add(dev, &hdmi->bridge);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add bridge\n");

	return 0;
}

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_AUTHOR("Can Zeng <can.zeng@mediatek.com>");
MODULE_AUTHOR("Jie Qiu <jie.qiu@mediatek.com>");
MODULE_DESCRIPTION("MediaTek HDMI Common Library");
MODULE_LICENSE("GPL");
