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

int mtk_hdmi_audio_params(struct mtk_hdmi *hdmi,
			  struct hdmi_codec_daifmt *daifmt,
			  struct hdmi_codec_params *params)
{
	struct hdmi_audio_param aud_params = { 0 };
	unsigned int chan = params->cea.channels;

	dev_dbg(hdmi->dev, "%s: %u Hz, %d bit, %d channels\n", __func__,
		params->sample_rate, params->sample_width, chan);

	if (!hdmi->bridge.encoder)
		return -ENODEV;

	switch (chan) {
	case 2:
		aud_params.aud_input_chan_type = HDMI_AUD_CHAN_TYPE_2_0;
		break;
	case 4:
		aud_params.aud_input_chan_type = HDMI_AUD_CHAN_TYPE_4_0;
		break;
	case 6:
		aud_params.aud_input_chan_type = HDMI_AUD_CHAN_TYPE_5_1;
		break;
	case 8:
		aud_params.aud_input_chan_type = HDMI_AUD_CHAN_TYPE_7_1;
		break;
	default:
		dev_err(hdmi->dev, "channel[%d] not supported!\n", chan);
		return -EINVAL;
	}

	switch (params->sample_rate) {
	case 32000:
	case 44100:
	case 48000:
	case 88200:
	case 96000:
	case 176400:
	case 192000:
		break;
	default:
		dev_err(hdmi->dev, "rate[%d] not supported!\n",
			params->sample_rate);
		return -EINVAL;
	}

	switch (daifmt->fmt) {
	case HDMI_I2S:
		aud_params.aud_codec = HDMI_AUDIO_CODING_TYPE_PCM;
		aud_params.aud_sample_size = HDMI_AUDIO_SAMPLE_SIZE_16;
		aud_params.aud_input_type = HDMI_AUD_INPUT_I2S;
		aud_params.aud_i2s_fmt = HDMI_I2S_MODE_I2S_24BIT;
		aud_params.aud_mclk = HDMI_AUD_MCLK_128FS;
		break;
	case HDMI_SPDIF:
		aud_params.aud_codec = HDMI_AUDIO_CODING_TYPE_PCM;
		aud_params.aud_sample_size = HDMI_AUDIO_SAMPLE_SIZE_16;
		aud_params.aud_input_type = HDMI_AUD_INPUT_SPDIF;
		break;
	default:
		dev_err(hdmi->dev, "%s: Invalid DAI format %d\n", __func__,
			daifmt->fmt);
		return -EINVAL;
	}
	memcpy(&aud_params.codec_params, params, sizeof(aud_params.codec_params));
	memcpy(&hdmi->aud_param, &aud_params, sizeof(aud_params));

	dev_dbg(hdmi->dev, "codec:%d, input:%d, channel:%d, fs:%d\n",
		aud_params.aud_codec, aud_params.aud_input_type,
		aud_params.aud_input_chan_type, aud_params.codec_params.sample_rate);

	return 0;
}

int mtk_hdmi_audio_get_eld(struct device *dev, void *data, uint8_t *buf, size_t len)
{
	struct mtk_hdmi *hdmi = dev_get_drvdata(dev);

	if (hdmi->enabled)
		memcpy(buf, hdmi->curr_conn->eld, min(sizeof(hdmi->curr_conn->eld), len));
	else
		memset(buf, 0, len);

	return 0;
}

void mtk_hdmi_audio_set_plugged_cb(struct mtk_hdmi *hdmi, hdmi_codec_plugged_cb fn,
				   struct device *codec_dev)
{
	mutex_lock(&hdmi->update_plugged_status_lock);
	hdmi->plugged_cb = fn;
	hdmi->codec_dev = codec_dev;
	mutex_unlock(&hdmi->update_plugged_status_lock);
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
	struct hdmi_audio_param *aud_param = &hdmi->aud_param;
	struct hdmi_codec_pdata codec_data = {
		.i2s = 1,
		.max_i2s_channels = 2,
		.data = hdmi,
		.ops = hdmi->conf->ver_conf->codec_ops
	};
	int ret;

	aud_param->aud_codec = HDMI_AUDIO_CODING_TYPE_PCM;
	aud_param->aud_sample_size = HDMI_AUDIO_SAMPLE_SIZE_16;
	aud_param->aud_input_type = HDMI_AUD_INPUT_I2S;
	aud_param->aud_i2s_fmt = HDMI_I2S_MODE_I2S_24BIT;
	aud_param->aud_mclk = HDMI_AUD_MCLK_128FS;
	aud_param->aud_input_chan_type = HDMI_AUD_CHAN_TYPE_2_0;

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

struct mtk_hdmi *mtk_hdmi_common_probe(struct platform_device *pdev)
{
	const struct mtk_hdmi_ver_conf *ver_conf;
	struct device *dev = &pdev->dev;
	struct mtk_hdmi *hdmi;
	int ret;

	hdmi = devm_kzalloc(&pdev->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return ERR_PTR(-ENOMEM);

	hdmi->dev = dev;
	hdmi->conf = of_device_get_match_data(dev);
	ver_conf = hdmi->conf->ver_conf;

	hdmi->clk = devm_kcalloc(dev, ver_conf->num_clocks, sizeof(*hdmi->clk), GFP_KERNEL);
	if (!hdmi->clk)
		return ERR_PTR(-ENOMEM);

	hdmi->phy = devm_phy_get(dev, "hdmi");
	if (IS_ERR(hdmi->phy))
		return dev_err_cast_probe(dev, hdmi->phy, "Failed to get HDMI PHY\n");

	ret = mtk_hdmi_dt_parse_pdata(hdmi, pdev, ver_conf->mtk_hdmi_clock_names,
				      ver_conf->num_clocks);
	if (ret)
		return ERR_PTR(ret);

	platform_set_drvdata(pdev, hdmi);

	ret = mtk_hdmi_register_audio_driver(dev);
	if (ret)
		return dev_err_ptr_probe(dev, ret, "Cannot register HDMI Audio driver\n");

	hdmi->bridge.funcs = ver_conf->bridge_funcs;
	hdmi->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID | DRM_BRIDGE_OP_HPD;

	if (ver_conf->bridge_funcs->hdmi_write_infoframe &&
	    ver_conf->bridge_funcs->hdmi_clear_infoframe)
		hdmi->bridge.ops |= DRM_BRIDGE_OP_HDMI;

	hdmi->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	hdmi->bridge.of_node = pdev->dev.of_node;
	hdmi->bridge.ddc = hdmi->ddc_adpt;
	hdmi->bridge.vendor = "MediaTek";
	hdmi->bridge.product = "On-Chip HDMI";

	ret = devm_drm_bridge_add(dev, &hdmi->bridge);
	if (ret)
		return dev_err_ptr_probe(dev, ret, "Failed to add bridge\n");

	return hdmi;
}

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_AUTHOR("Can Zeng <can.zeng@mediatek.com>");
MODULE_AUTHOR("Jie Qiu <jie.qiu@mediatek.com>");
MODULE_DESCRIPTION("MediaTek HDMI Common Library");
MODULE_LICENSE("GPL");
