// SPDX-License-Identifier: GPL-2.0
//
// CS40L26 Boosted Haptic Driver with integrated DSP and
// waveform memory with advanced closed loop algorithms and
// LRA protection
//
// Copyright 2025 Cirrus Logic Inc.
//
// Author: Fred Treven <ftreven@opensource.cirrus.com>

#include <linux/bitfield.h>
#include <linux/mfd/cs40l26.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define CS40L26_MONITOR_FILT 0x4008
#define CS40L26_ASP_ENABLES1 0x4800
#define CS40L26_ASP_CONTROL2 0x4808
#define CS40L26_ASP_FRAME_CONTROL5 0x4820
#define CS40L26_ASP_DATA_CONTROL5 0x4840
#define CS40L26_DACPCM1_INPUT 0x4C00
#define CS40L26_ASPTX1_INPUT 0x4C20

#define CS40L26_PLL_CLK_SEL_BCLK 0x0
#define CS40L26_PLL_CLK_SEL_MCLK 0x5

#define CS40L26_PLL_CLK_FREQ_MASK GENMASK(31, 0)

#define CS40L26_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE)
#define CS40L26_RATES (SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000)

#define CS40L26_ASP_RX_WIDTH_MASK GENMASK(31, 24)
#define CS40L26_ASP_FMT_MASK GENMASK(10, 8)
#define CS40L26_ASP_BCLK_INV_MASK BIT(6)
#define CS40L26_ASP_FSYNC_INV_MASK BIT(2)
#define CS40L26_ASP_FSYNC_INV_SHIFT 2

#define CS40L26_ASP_FMT_TDM1_DSPA 0x0
#define CS40L26_ASP_FMT_I2S 0x2

#define CS40L26_PLL_REFCLK_BCLK 0x0
#define CS40L26_PLL_REFCLK_FSYNC 0x1
#define CS40L26_PLL_REFCLK_MCLK 0x5

#define CS40L26_PLL_REFCLK_SEL_MASK GENMASK(2, 0)
#define CS40L26_PLL_REFCLK_FREQ_MASK GENMASK(10, 5)
#define CS40L26_PLL_REFCLK_FREQ_SHIFT 5
#define CS40L26_PLL_REFCLK_LOOP_MASK BIT(11)

#define CS40L26_ASP_RX_WL_MASK GENMASK(5, 0)

#define CS40L26_DATA_SRC_DSP1TX1 0x32

#define CS40L26_DATA_SRC_MASK GENMASK(6, 0)

#define CS40L26_ASP_TX1_EN_MASK BIT(0)
#define CS40L26_ASP_TX2_EN_MASK BIT(1)
#define CS40L26_ASP_RX1_EN_MASK BIT(16)
#define CS40L26_ASP_RX2_EN_MASK BIT(17)
#define CS40L26_ASP_ENABLE_MASK                                                        \
	(CS40L26_ASP_TX1_EN_MASK | CS40L26_ASP_TX2_EN_MASK | CS40L26_ASP_RX1_EN_MASK | \
	 CS40L26_ASP_RX2_EN_MASK)

#define CS40L26_ASP_RX1_SLOT_MASK GENMASK(5, 0)
#define CS40L26_ASP_RX2_SLOT_MASK GENMASK(13, 8)

#define CS40L26_VIMON_DUAL_RATE_MASK BIT(16)

struct cs40l26_pll_sysclk_config {
	u32 freq;
	u8 cfg;
};

struct cs40l26_codec {
	struct cs40l26 *core;
	struct device *dev;
	struct regmap *regmap;
	unsigned int rate;
	u32 daifmt;
	int tdm_width;
	int tdm_slot[2];
	u32 refclk_input;
};

static const struct cs40l26_pll_sysclk_config cs40l26_pll_sysclk[] = {
	{ 32768, 0x00 },
	{ 1536000, 0x1B },
	{ 3072000, 0x21 },
	{ 6144000, 0x28 },
	{ 9600000, 0x30 },
	{ 12288000, 0x33 },
};

static int cs40l26_get_clk_config(struct cs40l26_codec *codec, u32 freq, u8 *clk_cfg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs40l26_pll_sysclk); i++) {
		if (cs40l26_pll_sysclk[i].freq == freq) {
			*clk_cfg = cs40l26_pll_sysclk[i].cfg;
			return 0;
		}
	}

	dev_err(codec->dev, "Invalid clock frequency: %u Hz\n", freq);

	return -EINVAL;
}

static int cs40l26_swap_ext_clk(struct cs40l26_codec *codec, u8 clk_src)
{
	u8 clk_cfg, clk_sel;
	int ret;

	switch (clk_src) {
	case CS40L26_PLL_REFCLK_BCLK:
		clk_sel = CS40L26_PLL_CLK_SEL_BCLK;
		ret = cs40l26_get_clk_config(codec, codec->rate, &clk_cfg);
		break;
	case CS40L26_PLL_REFCLK_MCLK:
		clk_sel = CS40L26_PLL_CLK_SEL_MCLK;
		ret = cs40l26_get_clk_config(codec, 32768, &clk_cfg);
		break;
	case CS40L26_PLL_REFCLK_FSYNC:
		ret = -EPERM;
		break;
	default:
		ret = -EINVAL;
	}

	if (ret) {
		dev_err(codec->dev, "Failed to get clock configuration\n");
		return ret;
	}

	ret = cs40l26_set_pll_loop(codec->core, CS40L26_PLL_OPEN);
	if (ret)
		return ret;

	ret = regmap_update_bits(codec->regmap, CS40L26_REFCLK_INPUT,
				 CS40L26_PLL_REFCLK_FREQ_MASK | CS40L26_PLL_REFCLK_SEL_MASK,
				 (clk_cfg << CS40L26_PLL_REFCLK_FREQ_SHIFT) | clk_sel);
	if (ret)
		return ret;

	return cs40l26_set_pll_loop(codec->core, CS40L26_PLL_CLOSED);
}

static int cs40l26_clk_en(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct cs40l26_codec *codec = snd_soc_component_get_drvdata(component);
	struct cs40l26 *cs40l26 = codec->core;
	int ret;

	guard(mutex)(&cs40l26->lock);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = cs40l26_dsp_write(cs40l26, CS40L26_STOP_PLAYBACK);
		if (ret)
			return ret;

		ret = regmap_read(codec->regmap, CS40L26_REFCLK_INPUT, &codec->refclk_input);
		if (ret)
			return ret;

		ret = cs40l26_dsp_write(cs40l26, CS40L26_START_I2S);
		if (ret)
			return ret;

		ret = cs40l26_swap_ext_clk(codec, CS40L26_PLL_REFCLK_BCLK);
		if (ret)
			return ret;
		break;
	case SND_SOC_DAPM_PRE_PMD:
		ret = cs40l26_swap_ext_clk(codec, CS40L26_PLL_REFCLK_MCLK);
		if (ret)
			return ret;

		/* Restore PLL Configuration */
		ret = cs40l26_set_pll_loop(cs40l26, (u32)FIELD_GET(CS40L26_PLL_REFCLK_LOOP_MASK,
								   codec->refclk_input));
		if (ret)
			return ret;
		break;
	default:
		dev_err(codec->dev, "Invalid event: %d\n", event);
		return -EINVAL;
	}

	return 0;
}

static int cs40l26_dsp_tx(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct cs40l26_codec *codec = snd_soc_component_get_drvdata(component);
	struct cs40l26 *cs40l26 = codec->core;
	int ret;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = cs40l26_fw_write(&cs40l26->dsp, "A2HEN", CS40L26_A2H_ALGO_ID, 1);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		ret = cs40l26_fw_write(&cs40l26->dsp, "A2HEN", CS40L26_A2H_ALGO_ID, 0);
		break;
	default:
		dev_err(codec->dev, "Invalid DSPTX event: %d\n", event);
		ret = -EINVAL;
	}

	return ret;
}

static int cs40l26_asp_rx(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event)
{
	struct cs40l26_codec *codec;
	struct cs40l26 *cs40l26;
	int ret;

	codec = snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	cs40l26 = codec->core;

	guard(mutex)(&cs40l26->lock);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = regmap_update_bits(codec->regmap, CS40L26_DACPCM1_INPUT,
					 CS40L26_DATA_SRC_MASK, CS40L26_DATA_SRC_DSP1TX1);
		if (ret)
			return ret;

		ret = regmap_update_bits(codec->regmap, CS40L26_ASPTX1_INPUT, CS40L26_DATA_SRC_MASK,
					 CS40L26_DATA_SRC_DSP1TX1);
		if (ret)
			return ret;

		ret = regmap_set_bits(codec->regmap, CS40L26_ASP_ENABLES1, CS40L26_ASP_ENABLE_MASK);
		if (ret)
			return ret;
		break;
	case SND_SOC_DAPM_PRE_PMD:
		ret = cs40l26_dsp_write(cs40l26, CS40L26_STOP_I2S);
		if (ret)
			return ret;

		ret = regmap_clear_bits(codec->regmap, CS40L26_ASP_ENABLES1,
					CS40L26_ASP_ENABLE_MASK);
		if (ret)
			return ret;
		break;
	default:
		dev_err(codec->dev, "Invalid ASPRX event: %d\n", event);
		return -EINVAL;
	}

	return 0;
}

static int cs40l26_component_set_sysclk(struct snd_soc_component *component, int clk_id, int source,
					unsigned int freq, int dir)
{
	struct cs40l26_codec *codec = snd_soc_component_get_drvdata(component);
	u8 clk_cfg;
	int ret;

	ret = cs40l26_get_clk_config(codec, (u32)(CS40L26_PLL_CLK_FREQ_MASK & freq), &clk_cfg);
	if (ret)
		return ret;

	if (clk_id) {
		dev_err(codec->dev, "Invalid input clock (ID: %d)\n", clk_id);
		return -EINVAL;
	}

	codec->rate = CS40L26_PLL_CLK_FREQ_MASK & freq;

	return 0;
}

static int cs40l26_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct cs40l26_codec *codec = snd_soc_component_get_drvdata(codec_dai->component);

	if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBC_CFC) {
		dev_err(codec->dev, "Device can not be master\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		codec->daifmt = 0;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		codec->daifmt = CS40L26_ASP_FSYNC_INV_MASK;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		codec->daifmt = CS40L26_ASP_BCLK_INV_MASK;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		codec->daifmt = CS40L26_ASP_FSYNC_INV_MASK | CS40L26_ASP_BCLK_INV_MASK;
		break;
	default:
		dev_err(codec->dev, "Invalid clock inversion\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
		codec->daifmt |= FIELD_PREP(CS40L26_ASP_FMT_MASK, CS40L26_ASP_FMT_TDM1_DSPA);
		break;
	case SND_SOC_DAIFMT_I2S:
		codec->daifmt |= FIELD_PREP(CS40L26_ASP_FMT_MASK, CS40L26_ASP_FMT_I2S);
		break;
	default:
		dev_err(codec->dev, "Invalid DAI format: 0x%X\n", fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	return 0;
}

static int cs40l26_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct cs40l26_codec *codec = snd_soc_component_get_drvdata(dai->component);
	u32 asp_rx_wl, asp_rx_width;
	int ret;

	ret = pm_runtime_resume_and_get(codec->core->dev);
	if (ret)
		return ret;

	switch (params_rate(params)) {
	case 48000:
		ret = regmap_clear_bits(codec->regmap, CS40L26_MONITOR_FILT,
					CS40L26_VIMON_DUAL_RATE_MASK);
		break;
	case 96000:
		ret = regmap_set_bits(codec->regmap, CS40L26_MONITOR_FILT,
				      CS40L26_VIMON_DUAL_RATE_MASK);
		break;
	default:
		dev_err(codec->dev, "Unsupported sample rate: %d Hz\n", params_rate(params));
		ret = -EINVAL;
	}

	if (ret)
		goto pm_exit;

	asp_rx_wl = params_width(params);

	ret = regmap_update_bits(codec->regmap, CS40L26_ASP_DATA_CONTROL5, CS40L26_ASP_RX_WL_MASK,
				 asp_rx_wl);
	if (ret)
		goto pm_exit;


	asp_rx_width = codec->tdm_width ? codec->tdm_width : asp_rx_wl;

	codec->daifmt |= FIELD_PREP(CS40L26_ASP_RX_WIDTH_MASK, asp_rx_width);

	ret = regmap_update_bits(codec->regmap, CS40L26_ASP_CONTROL2,
				 CS40L26_ASP_FSYNC_INV_MASK | CS40L26_ASP_BCLK_INV_MASK |
				 CS40L26_ASP_FMT_MASK | CS40L26_ASP_RX_WIDTH_MASK, codec->daifmt);
	if (ret)
		goto pm_exit;

	ret = regmap_update_bits(codec->regmap, CS40L26_ASP_FRAME_CONTROL5,
				 CS40L26_ASP_RX1_SLOT_MASK | CS40L26_ASP_RX2_SLOT_MASK,
				 codec->tdm_slot[0] |
					 FIELD_PREP(CS40L26_ASP_RX2_SLOT_MASK, codec->tdm_slot[1]));
	if (ret)
		goto pm_exit;

	dev_dbg(codec->dev, "ASP: %d bits in %d bit slots, slot #s: %d, %d\n", asp_rx_wl,
		asp_rx_width, codec->tdm_slot[0], codec->tdm_slot[1]);

pm_exit:
	cs40l26_pm_exit(codec->core->dev);

	return ret;
}

static int cs40l26_set_tdm_slot(struct snd_soc_dai *dai, unsigned int tx_mask, unsigned int rx_mask,
				int slots, int slot_width)
{
	struct cs40l26_codec *codec = snd_soc_component_get_drvdata(dai->component);

	codec->tdm_width = slot_width;

	/*
	 * Reset slots if TDM is being disabled, and catch the case in which both RX1 and RX2
	 * would be set to slot 0 which would cause the hardware to flag an error
	 */
	if (!slots || rx_mask == 0x1)
		rx_mask = 0x3;

	codec->tdm_slot[0] = ffs(rx_mask) - 1;
	rx_mask &= ~BIT(codec->tdm_slot[0]);
	codec->tdm_slot[1] = ffs(rx_mask) - 1;

	return 0;
}

static const struct snd_soc_dai_ops cs40l26_dai_ops = {
	.set_fmt = cs40l26_set_dai_fmt,
	.set_tdm_slot = cs40l26_set_tdm_slot,
	.hw_params = cs40l26_pcm_hw_params,
};

static struct snd_soc_dai_driver cs40l26_dai[] = {
	{
		.name = "cs40l26-pcm",
		.id = 0,
		.playback = {
			.stream_name = "ASP Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = CS40L26_RATES,
			.formats = CS40L26_FORMATS,
		},
		.ops = &cs40l26_dai_ops,
		.symmetric_rate = 1,
	},
};

static const char *const cs40l26_out_mux_texts[] = { "Off", "ASP", "DSP" };
static SOC_ENUM_SINGLE_VIRT_DECL(cs40l26_out_mux_enum, cs40l26_out_mux_texts);
static const struct snd_kcontrol_new cs40l26_out_mux =
	SOC_DAPM_ENUM("Haptics Source", cs40l26_out_mux_enum);

static const struct snd_soc_dapm_widget cs40l26_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY_S("ASP PLL", 0, SND_SOC_NOPM, 0, 0, cs40l26_clk_en,
			      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_AIF_IN("ASPRX1", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("ASPRX2", NULL, 0, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_PGA_E("ASP", SND_SOC_NOPM, 0, 0, NULL, 0, cs40l26_asp_rx,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("DSP", SND_SOC_NOPM, 0, 0, NULL, 0, cs40l26_dsp_tx,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_MUX("Haptics Source", SND_SOC_NOPM, 0, 0, &cs40l26_out_mux),
	SND_SOC_DAPM_OUTPUT("OUT"),
};

static const struct snd_soc_dapm_route cs40l26_dapm_routes[] = {
	{ "ASP Playback", NULL, "ASP PLL" },
	{ "ASPRX1", NULL, "ASP Playback" },
	{ "ASPRX2", NULL, "ASP Playback" },

	{ "ASP", NULL, "ASPRX1" },
	{ "ASP", NULL, "ASPRX2" },
	{ "DSP", NULL, "ASP" },

	{ "Haptics Source", "ASP", "ASP" },
	{ "Haptics Source", "DSP", "DSP" },
	{ "OUT", NULL, "Haptics Source" },
};

static int cs40l26_codec_probe(struct snd_soc_component *component)
{
	struct cs40l26_codec *codec = snd_soc_component_get_drvdata(component);

	/* Default audio SCLK frequency */
	codec->rate = 1536000;

	codec->tdm_slot[0] = 0;
	codec->tdm_slot[1] = 1;

	return 0;
}

static const struct snd_soc_component_driver soc_codec_dev_cs40l26 = {
	.probe = cs40l26_codec_probe,
	.set_sysclk = cs40l26_component_set_sysclk,
	.dapm_widgets = cs40l26_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cs40l26_dapm_widgets),
	.dapm_routes = cs40l26_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(cs40l26_dapm_routes),
};

static int cs40l26_platform_probe(struct platform_device *pdev)
{
	struct cs40l26 *cs40l26 = dev_get_drvdata(pdev->dev.parent);
	struct cs40l26_codec *codec;

	codec = devm_kzalloc(&pdev->dev, sizeof(struct cs40l26_codec), GFP_KERNEL);
	if (!codec)
		return -ENOMEM;

	codec->core = cs40l26;
	codec->regmap = cs40l26->regmap;
	codec->dev = &pdev->dev;

	platform_set_drvdata(pdev, codec);

	return snd_soc_register_component(&pdev->dev, &soc_codec_dev_cs40l26, cs40l26_dai,
					  ARRAY_SIZE(cs40l26_dai));
}

static const struct platform_device_id cs40l26_id[] = {
	{ "cs40l26-codec", },
	{}
};
MODULE_DEVICE_TABLE(platform, cs40l26_id);

static struct platform_driver cs40l26_codec_driver = {
	.probe = cs40l26_platform_probe,
	.id_table = cs40l26_id,
	.driver = {
		.name = "cs40l26-codec",
	},
};
module_platform_driver(cs40l26_codec_driver);

MODULE_DESCRIPTION("ASoC CS40L26 driver");
MODULE_AUTHOR("Fred Treven ftreven@opensource.cirrus.com");
MODULE_LICENSE("GPL");
