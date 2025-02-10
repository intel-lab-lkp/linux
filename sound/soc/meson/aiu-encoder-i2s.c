// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2020 BayLibre, SAS.
// Author: Jerome Brunet <jbrunet@baylibre.com>

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include "aiu.h"

#define AIU_I2S_SOURCE_DESC_MODE_8CH	BIT(0)
#define AIU_I2S_SOURCE_DESC_MODE_24BIT	BIT(5)
#define AIU_I2S_SOURCE_DESC_MODE_32BIT	BIT(9)
#define AIU_I2S_SOURCE_DESC_MODE_SPLIT	BIT(11)
#define AIU_RST_SOFT_I2S_FAST		BIT(0)

#define AIU_I2S_DAC_CFG_MSB_FIRST	BIT(2)
#define AIU_CLK_CTRL_AOCLK_INVERT	BIT(6)
#define AIU_CLK_CTRL_LRCLK_INVERT	BIT(7)
#define AIU_CLK_CTRL_LRCLK_SKEW		GENMASK(9, 8)
#define AIU_CLK_CTRL_MORE_HDMI_AMCLK	BIT(6)

static int aiu_encoder_i2s_setup_desc(struct snd_soc_component *component,
				      struct snd_pcm_hw_params *params)
{
	/* Always operate in split (classic interleaved) mode */
	unsigned int desc = AIU_I2S_SOURCE_DESC_MODE_SPLIT;

	/* Reset required to update the pipeline */
	snd_soc_component_write(component, AIU_RST_SOFT, AIU_RST_SOFT_I2S_FAST);
	snd_soc_component_read(component, AIU_I2S_SYNC);

	switch (params_physical_width(params)) {
	case 16: /* Nothing to do */
		break;

	case 32:
		desc |= (AIU_I2S_SOURCE_DESC_MODE_24BIT |
			 AIU_I2S_SOURCE_DESC_MODE_32BIT);
		break;

	default:
		return -EINVAL;
	}

	switch (params_channels(params)) {
	case 2: /* Nothing to do */
		break;
	case 8:
		desc |= AIU_I2S_SOURCE_DESC_MODE_8CH;
		break;
	default:
		return -EINVAL;
	}

	snd_soc_component_update_bits(component, AIU_I2S_SOURCE_DESC,
				      AIU_I2S_SOURCE_DESC_MODE_8CH |
				      AIU_I2S_SOURCE_DESC_MODE_24BIT |
				      AIU_I2S_SOURCE_DESC_MODE_32BIT |
				      AIU_I2S_SOURCE_DESC_MODE_SPLIT,
				      desc);

	return 0;
}

static int aiu_encoder_i2s_set_legacy_div(struct snd_soc_component *component,
					  struct snd_pcm_hw_params *params,
					  unsigned long mclk_rate,
					  unsigned long aoclk_rate)
{
	struct aiu *aiu = snd_soc_component_get_drvdata(component);
	unsigned long bs = mclk_rate / aoclk_rate;
	int ret;

	switch (bs) {
	case 1:
	case 2:
	case 4:
	case 8:
		/* These are the only valid legacy dividers */
		break;

	default:
		dev_err(component->dev, "Unsupported i2s divider: %lu\n", bs);
		return -EINVAL;
	}

	/* Use AOCLK_BASIC divider, i.e. set AOCLK_MORE to the same rate as
	 * its parent so that it acts as a passthrough.
	 */
	ret = clk_set_rate(aiu->i2s_extra.clks[AOCLK_BASIC_DIV].clk,
			   aoclk_rate);
	if (ret) {
		dev_err(component->dev, "failed to set AOCLK_BASIC_DIV\n");
		return ret;
	}

	ret = clk_set_rate(aiu->i2s_extra.clks[AOCLK_MORE_DIV].clk, aoclk_rate);
	if (ret) {
		dev_err(component->dev, "failed to set AOCLK_MORE_DIV\n");
		return ret;
	}

	return 0;
}

static int aiu_encoder_i2s_set_more_div(struct snd_soc_component *component,
					struct snd_pcm_hw_params *params,
					unsigned long mclk_rate,
					unsigned long aoclk_rate)
{
	struct aiu *aiu = snd_soc_component_get_drvdata(component);
	unsigned long bs = mclk_rate / aoclk_rate;
	int ret;

	/*
	 * NOTE: this HW is odd.
	 * In most configuration, the i2s divider is 'mclk / blck'.
	 * However, in 16 bits - 8ch mode, this factor needs to be
	 * increased by 50% to get the correct output rate.
	 * No idea why !
	 */
	if (params_width(params) == 16 && params_channels(params) == 8) {
		if (bs % 2) {
			dev_err(component->dev,
				"Cannot increase i2s divider by 50%%\n");
			return -EINVAL;
		}
		bs += bs / 2;
		aoclk_rate = mclk_rate / bs;
	}

	/* Use AOCLK_MORE divider, i.e. set AOCLK_BASIC to the same rate as
	 * its parent so that it acts as a passthough.
	 */
	ret = clk_set_rate(aiu->i2s_extra.clks[AOCLK_BASIC_DIV].clk, mclk_rate);
	if (ret) {
		dev_err(component->dev, "failed to set AOCLK_BASIC_DIV\n");
		return ret;
	}

	ret = clk_set_rate(aiu->i2s_extra.clks[AOCLK_MORE_DIV].clk, aoclk_rate);
	if (ret) {
		dev_err(component->dev, "failed to set AOCLK_MORE_DIV\n");
		return ret;
	}

	return 0;
}

static int aiu_encoder_i2s_set_clocks(struct snd_soc_component *component,
				      struct snd_pcm_hw_params *params,
				      struct snd_soc_dai *dai)
{
	struct aiu *aiu = snd_soc_component_get_drvdata(component);
	unsigned int srate = params_rate(params);
	unsigned int fs;
	unsigned long mclk_rate, aoclk_rate;
	int ret;

	mclk_rate = clk_get_rate(aiu->i2s.clks[MCLK].clk);

	/* Get the oversampling factor */
	fs = DIV_ROUND_CLOSEST(mclk_rate, srate);

	if (fs % 64)
		return -EINVAL;

	/* Send data MSB first */
	snd_soc_component_update_bits(component, AIU_I2S_DAC_CFG,
				      AIU_I2S_DAC_CFG_MSB_FIRST,
				      AIU_I2S_DAC_CFG_MSB_FIRST);

	/* aoclk rate is 64 times the sample rate */
	aoclk_rate = srate * 64;

	if (aiu->platform->has_clk_ctrl_more_i2s_div)
		ret = aiu_encoder_i2s_set_more_div(component, params,
						   mclk_rate, aoclk_rate);
	else
		ret = aiu_encoder_i2s_set_legacy_div(component, params,
						     mclk_rate, aoclk_rate);

	if (ret)
		return ret;

	/* lrclk rate is equal to the sample rate */
	ret = clk_set_rate(aiu->i2s_extra.clks[LRCLK_DIV].clk, srate);
	if (ret) {
		dev_err(dai->dev, "failed to set LRCLK_DIV\n");
		return ret;
	}

	/* Make sure amclk is used for HDMI i2s as well */
	snd_soc_component_update_bits(component, AIU_CLK_CTRL_MORE,
				      AIU_CLK_CTRL_MORE_HDMI_AMCLK,
				      AIU_CLK_CTRL_MORE_HDMI_AMCLK);

	return 0;
}

static int aiu_encoder_i2s_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aiu *aiu = snd_soc_component_get_drvdata(dai->component);
	int ret;

	/* Disable the clock while changing the settings */
	// clk_disable_unprepare(aiu->i2s_extra.clks[AOCLK_DIV_GATE].clk);

	ret = aiu_encoder_i2s_setup_desc(component, params);
	if (ret) {
		dev_err(dai->dev, "setting i2s desc failed\n");
		return ret;
	}

	ret = aiu_encoder_i2s_set_clocks(component, params, dai);
	if (ret) {
		dev_err(dai->dev, "setting i2s clocks failed\n");
		return ret;
	}

	ret = clk_prepare_enable(aiu->i2s_extra.clks[AOCLK_DIV_GATE].clk);
	if (ret) {
		dev_err(dai->dev, "failed to enable AOCLK_DIV_GATE\n");
		return ret;
	}

	return 0;
}

static int aiu_encoder_i2s_hw_free(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct aiu *aiu = snd_soc_component_get_drvdata(dai->component);

	clk_disable_unprepare(aiu->i2s_extra.clks[AOCLK_DIV_GATE].clk);
	
	return 0;
}

static int aiu_encoder_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	unsigned int inv = fmt & SND_SOC_DAIFMT_INV_MASK;
	unsigned int val = 0;
	unsigned int skew;

	/* Only CPU Master / Codec Slave supported ATM */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;

	if (inv == SND_SOC_DAIFMT_NB_IF ||
	    inv == SND_SOC_DAIFMT_IB_IF)
		val |= AIU_CLK_CTRL_LRCLK_INVERT;

	if (inv == SND_SOC_DAIFMT_IB_NF ||
	    inv == SND_SOC_DAIFMT_IB_IF)
		val |= AIU_CLK_CTRL_AOCLK_INVERT;

	/* Signal skew */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		/* Invert sample clock for i2s */
		val ^= AIU_CLK_CTRL_LRCLK_INVERT;
		skew = 1;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		skew = 0;
		break;
	default:
		return -EINVAL;
	}

	val |= FIELD_PREP(AIU_CLK_CTRL_LRCLK_SKEW, skew);
	snd_soc_component_update_bits(component, AIU_CLK_CTRL,
				      AIU_CLK_CTRL_LRCLK_INVERT |
				      AIU_CLK_CTRL_AOCLK_INVERT |
				      AIU_CLK_CTRL_LRCLK_SKEW,
				      val);

	return 0;
}

static int aiu_encoder_i2s_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				      unsigned int freq, int dir)
{
	struct aiu *aiu = snd_soc_component_get_drvdata(dai->component);
	int ret;

	if (WARN_ON(clk_id != 0))
		return -EINVAL;

	if (dir == SND_SOC_CLOCK_IN)
		return 0;

	ret = clk_set_rate(aiu->i2s.clks[MCLK].clk, freq);
	if (ret)
		dev_err(dai->dev, "Failed to set sysclk to %uHz", freq);

	return ret;
}

static const unsigned int hw_channels[] = {2, 8};
static const struct snd_pcm_hw_constraint_list hw_channel_constraints = {
	.list = hw_channels,
	.count = ARRAY_SIZE(hw_channels),
	.mask = 0,
};

static int aiu_encoder_i2s_startup(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct aiu *aiu = snd_soc_component_get_drvdata(dai->component);
	int ret;

	/* Make sure the encoder gets either 2 or 8 channels */
	ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_CHANNELS,
					 &hw_channel_constraints);
	if (ret) {
		dev_err(dai->dev, "adding channels constraints failed\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(aiu->i2s.clk_num, aiu->i2s.clks);
	if (ret)
		dev_err(dai->dev, "failed to enable i2s clocks\n");

	return ret;
}

static void aiu_encoder_i2s_shutdown(struct snd_pcm_substream *substream,
				     struct snd_soc_dai *dai)
{
	struct aiu *aiu = snd_soc_component_get_drvdata(dai->component);

	clk_bulk_disable_unprepare(aiu->i2s.clk_num, aiu->i2s.clks);
}

const struct snd_soc_dai_ops aiu_encoder_i2s_dai_ops = {
	.hw_params	= aiu_encoder_i2s_hw_params,
	.hw_free	= aiu_encoder_i2s_hw_free,
	.set_fmt	= aiu_encoder_i2s_set_fmt,
	.set_sysclk	= aiu_encoder_i2s_set_sysclk,
	.startup	= aiu_encoder_i2s_startup,
	.shutdown	= aiu_encoder_i2s_shutdown,
};

