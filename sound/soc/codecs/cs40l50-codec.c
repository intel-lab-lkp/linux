// SPDX-License-Identifier: GPL-2.0
/*
 * CS40L50 Advanced Haptic Driver with waveform memory,
 * integrated DSP, and closed-loop algorithms
 *
 * Copyright 2024 Cirrus Logic, Inc.
 *
 * Author: James Ogletree <james.ogletree@cirrus.com>
 */

#include <linux/bitfield.h>
#include <linux/mfd/cs40l50.h>
#include <linux/pm_runtime.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define CS40L50_REFCLK_INPUT		0x2C04
#define CS40L50_ASP_CONTROL2		0x4808
#define CS40L50_ASP_DATA_CONTROL5	0x4840

/* PLL Config */
#define CS40L50_PLL_CLK_CFG_32768	0x00
#define CS40L50_PLL_CLK_CFG_1536000	0x1B
#define CS40L50_PLL_CLK_CFG_3072000	0x21
#define CS40L50_PLL_CLK_CFG_6144000	0x28
#define CS40L50_PLL_CLK_CFG_9600000	0x30
#define CS40L50_PLL_CLK_CFG_12288000	0x33
#define CS40L50_PLL_CLK_FRQ_32768	32768
#define CS40L50_PLL_CLK_FRQ_1536000	1536000
#define CS40L50_PLL_CLK_FRQ_3072000	3072000
#define CS40L50_PLL_CLK_FRQ_6144000	6144000
#define CS40L50_PLL_CLK_FRQ_9600000	9600000
#define CS40L50_PLL_CLK_FRQ_12288000	12288000
#define CS40L50_PLL_REFCLK_BCLK		0x0
#define CS40L50_PLL_REFCLK_MCLK		0x5
#define CS40L50_PLL_REFCLK_LOOP_MASK	BIT(11)
#define CS40L50_PLL_REFCLK_OPEN_LOOP	1
#define CS40L50_PLL_REFCLK_CLOSED_LOOP	0
#define CS40L50_PLL_REFCLK_LOOP_SHIFT	11
#define CS40L50_PLL_REFCLK_FREQ_MASK	GENMASK(10, 5)
#define CS40L50_PLL_REFCLK_FREQ_SHIFT	5
#define CS40L50_PLL_REFCLK_SEL_MASK	GENMASK(2, 0)

/* ASP Config */
#define CS40L50_ASP_RX_WIDTH_SHIFT	24
#define CS40L50_ASP_RX_WIDTH_MASK	GENMASK(31, 24)
#define CS40L50_ASP_RX_WL_MASK		GENMASK(5, 0)
#define CS40L50_ASP_FSYNC_INV_MASK	BIT(2)
#define CS40L50_ASP_BCLK_INV_MASK	BIT(6)
#define CS40L50_ASP_FMT_MASK		GENMASK(10, 8)
#define CS40L50_ASP_FMT_I2S		0x2
#define CS40L50_FORMATS			(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE)

struct cs40l50_pll_sysclk_config {
	unsigned int freq;
	unsigned int clk_cfg;
};

struct cs40l50_codec {
	struct device *dev;
	struct regmap *regmap;
	unsigned int sysclk_rate;
	unsigned int daifmt;
};

static const struct cs40l50_pll_sysclk_config cs40l50_pll_sysclk[] = {
	{CS40L50_PLL_CLK_FRQ_32768, CS40L50_PLL_CLK_CFG_32768},
	{CS40L50_PLL_CLK_FRQ_1536000, CS40L50_PLL_CLK_CFG_1536000},
	{CS40L50_PLL_CLK_FRQ_3072000, CS40L50_PLL_CLK_CFG_3072000},
	{CS40L50_PLL_CLK_FRQ_6144000, CS40L50_PLL_CLK_CFG_6144000},
	{CS40L50_PLL_CLK_FRQ_9600000, CS40L50_PLL_CLK_CFG_9600000},
	{CS40L50_PLL_CLK_FRQ_12288000, CS40L50_PLL_CLK_CFG_12288000},
};

static int cs40l50_get_clk_config(unsigned int freq, unsigned int *clk_cfg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs40l50_pll_sysclk); i++) {
		if (cs40l50_pll_sysclk[i].freq == freq) {
			*clk_cfg = cs40l50_pll_sysclk[i].clk_cfg;
			return 0;
		}
	}

	return -EINVAL;
}

static int cs40l50_swap_ext_clk(struct cs40l50_codec *codec, unsigned int clk_src)
{
	unsigned int clk_cfg;
	int ret;

	switch (clk_src) {
	case CS40L50_PLL_REFCLK_BCLK:
		ret = cs40l50_get_clk_config(codec->sysclk_rate, &clk_cfg);
		if (ret)
			return ret;
		break;
	case CS40L50_PLL_REFCLK_MCLK:
		clk_cfg = CS40L50_PLL_CLK_CFG_32768;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(codec->regmap, CS40L50_REFCLK_INPUT,
				 CS40L50_PLL_REFCLK_LOOP_MASK,
				 CS40L50_PLL_REFCLK_OPEN_LOOP <<
				 CS40L50_PLL_REFCLK_LOOP_SHIFT);
	if (ret)
		return ret;

	ret = regmap_update_bits(codec->regmap, CS40L50_REFCLK_INPUT,
				 CS40L50_PLL_REFCLK_FREQ_MASK |
				 CS40L50_PLL_REFCLK_SEL_MASK,
				 (clk_cfg << CS40L50_PLL_REFCLK_FREQ_SHIFT) | clk_src);
	if (ret)
		return ret;

	return regmap_update_bits(codec->regmap, CS40L50_REFCLK_INPUT,
				  CS40L50_PLL_REFCLK_LOOP_MASK,
				  CS40L50_PLL_REFCLK_CLOSED_LOOP <<
				  CS40L50_PLL_REFCLK_LOOP_SHIFT);
}

static int cs40l50_clk_en(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *kcontrol,
			  int event)
{
	struct snd_soc_component *comp = snd_soc_dapm_to_component(w->dapm);
	struct cs40l50_codec *codec = snd_soc_component_get_drvdata(comp);
	int ret;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = cs40l50_dsp_write(codec->dev, codec->regmap, CS40L50_STOP_PLAYBACK);
		if (ret)
			return ret;

		ret = cs40l50_dsp_write(codec->dev, codec->regmap, CS40L50_START_I2S);
		if (ret)
			return ret;

		ret = cs40l50_swap_ext_clk(codec, CS40L50_PLL_REFCLK_BCLK);
		if (ret)
			return ret;
		break;
	case SND_SOC_DAPM_PRE_PMD:
		ret = cs40l50_swap_ext_clk(codec, CS40L50_PLL_REFCLK_MCLK);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dapm_widget cs40l50_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY_S("ASP PLL", 0, SND_SOC_NOPM, 0, 0, cs40l50_clk_en,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_AIF_IN("ASPRX1", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("ASPRX2", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("OUT"),
};

static const struct snd_soc_dapm_route cs40l50_dapm_routes[] = {
	{ "ASP Playback", NULL, "ASP PLL" },
	{ "ASPRX1", NULL, "ASP Playback" },
	{ "ASPRX2", NULL, "ASP Playback" },

	{ "OUT", NULL, "ASPRX1" },
	{ "OUT", NULL, "ASPRX2" },
};

static int cs40l50_component_set_sysclk(struct snd_soc_component *component,
		int clk_id, int source, unsigned int freq, int dir)
{
	struct cs40l50_codec *codec = snd_soc_component_get_drvdata(component);

	codec->sysclk_rate = freq;

	return 0;
}

static int cs40l50_set_dai_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct cs40l50_codec *codec = snd_soc_component_get_drvdata(codec_dai->component);

	if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS)
		return -EINVAL;

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		codec->daifmt = 0;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		codec->daifmt = CS40L50_ASP_FSYNC_INV_MASK;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		codec->daifmt = CS40L50_ASP_BCLK_INV_MASK;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		codec->daifmt = CS40L50_ASP_FSYNC_INV_MASK | CS40L50_ASP_BCLK_INV_MASK;
		break;
	default:
		dev_err(codec->dev, "Invalid clock invert\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		codec->daifmt |= FIELD_PREP(CS40L50_ASP_FMT_MASK, CS40L50_ASP_FMT_I2S);
		break;
	default:
		dev_err(codec->dev, "Unsupported DAI format\n");
		return -EINVAL;
	}

	return 0;
}

static int cs40l50_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct cs40l50_codec *codec = snd_soc_component_get_drvdata(dai->component);
	unsigned int asp_rx_wl = params_width(params);
	int ret;

	ret = regmap_update_bits(codec->regmap, CS40L50_ASP_DATA_CONTROL5,
				 CS40L50_ASP_RX_WL_MASK, asp_rx_wl);
	if (ret)
		return ret;

	codec->daifmt |= (asp_rx_wl << CS40L50_ASP_RX_WIDTH_SHIFT);

	return regmap_update_bits(codec->regmap, CS40L50_ASP_CONTROL2,
				  CS40L50_ASP_FSYNC_INV_MASK |
				  CS40L50_ASP_BCLK_INV_MASK |
				  CS40L50_ASP_FMT_MASK |
				  CS40L50_ASP_RX_WIDTH_MASK, codec->daifmt);
}

static const struct snd_soc_dai_ops cs40l50_dai_ops = {
	.set_fmt = cs40l50_set_dai_fmt,
	.hw_params = cs40l50_hw_params,
};

static struct snd_soc_dai_driver cs40l50_dai[] = {
	{
		.name = "cs40l50-pcm",
		.id = 0,
		.playback = {
			.stream_name = "ASP Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_48000,
			.formats = CS40L50_FORMATS,
		},
		.ops = &cs40l50_dai_ops,
	},
};

static int cs40l50_codec_probe(struct snd_soc_component *component)
{
	struct cs40l50_codec *codec = snd_soc_component_get_drvdata(component);

	codec->sysclk_rate = CS40L50_PLL_CLK_FRQ_1536000;

	return 0;
}

static const struct snd_soc_component_driver soc_codec_dev_cs40l50 = {
	.probe = cs40l50_codec_probe,
	.set_sysclk = cs40l50_component_set_sysclk,
	.dapm_widgets = cs40l50_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cs40l50_dapm_widgets),
	.dapm_routes = cs40l50_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(cs40l50_dapm_routes),
};

static int cs40l50_codec_driver_probe(struct platform_device *pdev)
{
	struct cs40l50 *cs40l50 = dev_get_drvdata(pdev->dev.parent);
	struct cs40l50_codec *codec;

	codec = devm_kzalloc(&pdev->dev, sizeof(*codec), GFP_KERNEL);
	if (!codec)
		return -ENOMEM;

	codec->regmap = cs40l50->regmap;
	codec->dev = &pdev->dev;

	return devm_snd_soc_register_component(&pdev->dev, &soc_codec_dev_cs40l50,
					       cs40l50_dai, ARRAY_SIZE(cs40l50_dai));
}

static struct platform_driver cs40l50_codec_driver = {
	.probe = cs40l50_codec_driver_probe,
	.driver = {
		.name = "cs40l50-codec",
	},
};
module_platform_driver(cs40l50_codec_driver);

MODULE_DESCRIPTION("ASoC CS40L50 driver");
MODULE_AUTHOR("James Ogletree <james.ogletree@cirrus.com>");
MODULE_LICENSE("GPL");
