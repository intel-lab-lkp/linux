// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2025 BayLibre, SAS.
// Author: Valerio Setti <vsetti@baylibre.com>

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include "audin.h"

static int audin_decoder_i2s_setup_desc(struct snd_pcm_hw_params *params,
					struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	int val;

	/* I2S decoder always outputs 24bits to the FIFO according to the
	 * manual. The only thing we can do is mask some bits as follows:
	 * - 0: 16 bit
	 * - 1: 18 bits (not exposed as supported format)
	 * - 2: 20 bits (not exposed as supported format)
	 * - 3: 24 bits
	 *
	 * We force 24 bit output here and filter unnecessary ones at the FIFO
	 * stage.
	 * Note: data is left-justified, so in case of 16 bits samples, this
	 *       means that the LSB is to be discarded at FIFO level and the
	 *       relevant part is in bits [23:8].
	 */
	switch (params_width(params)) {
	case 16:
	case 24:
		val = 3;
		break;
	default:
		dev_err(dai->dev, "Error: wrong sample width %d",
			params_physical_width(params));
		return -EINVAL;
	}
	val = FIELD_PREP(AUDIN_I2SIN_CTRL_I2SIN_SIZE_MASK, val);
	snd_soc_component_update_bits(component, AUDIN_I2SIN_CTRL,
				      AUDIN_I2SIN_CTRL_I2SIN_SIZE_MASK, val);

	/* The manual claims that this platform supports up to 4 streams
	 * (8 channels), but only 1 stream (2 channels) is supported ATM.
	 */
	val = FIELD_PREP(AUDIN_I2SIN_CTRL_I2SIN_CHAN_EN_MASK, 1);
	snd_soc_component_update_bits(component, AUDIN_I2SIN_CTRL,
				      AUDIN_I2SIN_CTRL_I2SIN_CHAN_EN_MASK, val);

	return 0;
}

static int audin_decoder_i2s_set_clocks(struct snd_soc_component *component,
					struct snd_pcm_hw_params *params,
					struct snd_soc_dai *dai)
{
	struct audin *audin = snd_soc_component_get_drvdata(component);
	unsigned int sample_rate = params_rate(params);
	unsigned long mclk;
	int ret;

	mclk = clk_get_rate(audin->bulk_clks[MCLK].clk);

	/* Set mclk to bclk ratio.
	 * We're going to use the new/finer clock divider (BCLK_MORE_DIV) for
	 * this, so let's keep the legacy one (BCLK_DIV) as passthrough.
	 */
	ret = clk_set_rate(audin->aoclk_basic_div, mclk);
	if (ret) {
		dev_err(dai->dev, "Failed to set aoclk_basic_div %d\n", ret);
		return ret;
	}

	/* We're going for a fixed bclk to lrclk ratio of 64. */
	ret = clk_set_rate(audin->aoclk_more_div, sample_rate * 64UL);
	if (ret) {
		dev_err(dai->dev, "Failed to set aoclk_more_div %d\n", ret);
		return ret;
	}

	ret = clk_set_rate(audin->lrclk_div, sample_rate);
	if (ret) {
		dev_err(dai->dev, "Failed to set lrclk_div %d\n", ret);
		return ret;
	}

	return 0;
}

static int audin_decoder_i2s_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params,
				       struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct audin *audin = snd_soc_component_get_drvdata(component);
	int ret;

	ret = audin_decoder_i2s_setup_desc(params, dai);
	if (ret) {
		dev_err(dai->dev, "setting i2s desc failed\n");
		return ret;
	}

	ret = audin_decoder_i2s_set_clocks(component, params, dai);
	if (ret) {
		dev_err(dai->dev, "setting i2s clocks failed\n");
		return ret;
	}

	ret = clk_prepare_enable(audin->aoclk_div_gate);
	if (ret)
		return ret;

	return 0;
}

static int audin_decoder_i2s_hw_free(struct snd_pcm_substream *substream,
				     struct snd_soc_dai *dai)
{
	struct audin *audin = snd_soc_component_get_drvdata(dai->component);

	clk_disable_unprepare(audin->aoclk_div_gate);

	return 0;
}

static int audin_decoder_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	unsigned int val = 0;

	/* Only CPU Master / Codec Slave supported ATM */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;

	/* Use clocks from AIU and not from the pads since we only want to
	 * support master mode.
	 */
	val = AUDIN_I2SIN_CTRL_I2SIN_CLK_SEL |
	      AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SEL |
	      AUDIN_I2SIN_CTRL_I2SIN_DIR;
	snd_soc_component_update_bits(component, AUDIN_I2SIN_CTRL, val, val);

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_IB_NF:
		val = AUDIN_I2SIN_CTRL_I2SIN_POS_SYNC;
		break;
	case SND_SOC_DAIFMT_NB_NF:
		val = 0;
		break;
	default:
		dev_err(dai->dev, "Error: unsupported format %x", fmt);
		return -EINVAL;
	}
	snd_soc_component_update_bits(component, AUDIN_I2SIN_CTRL,
				      AUDIN_I2SIN_CTRL_I2SIN_POS_SYNC, val);

	/* MSB data starts 1 clock cycle after LRCLK transition, as per I2S
	 * specs.
	 */
	val = FIELD_PREP(AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SKEW_MASK, 1);
	snd_soc_component_update_bits(component, AUDIN_I2SIN_CTRL,
				      AUDIN_I2SIN_CTRL_I2SIN_LRCLK_INV |
				      AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SKEW_MASK,
				      val);

	return 0;
}

static int audin_decoder_i2s_trigger(struct snd_pcm_substream *substream,
				     int cmd, struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		snd_soc_component_update_bits(component, AUDIN_I2SIN_CTRL,
					      AUDIN_I2SIN_CTRL_I2SIN_EN,
					      AUDIN_I2SIN_CTRL_I2SIN_EN);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_STOP:
		snd_soc_component_update_bits(component, AUDIN_I2SIN_CTRL,
					      AUDIN_I2SIN_CTRL_I2SIN_EN, 0);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int audin_decoder_i2s_set_sysclk(struct snd_soc_dai *dai, int clk_id,
					unsigned int freq, int dir)
{
	struct audin *audin = snd_soc_component_get_drvdata(dai->component);
	int ret;

	if (WARN_ON(clk_id != 0))
		return -EINVAL;

	if (dir == SND_SOC_CLOCK_IN)
		return 0;

	ret = clk_set_rate(audin->bulk_clks[MCLK].clk, freq);
	if (ret)
		dev_err(dai->dev, "Failed to set sysclk %d", ret);

	return ret;
}

static int audin_decoder_i2s_startup(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct audin *audin = snd_soc_component_get_drvdata(dai->component);
	int ret;

	ret = clk_bulk_prepare_enable(audin->bulk_clks_num, audin->bulk_clks);
	if (ret)
		dev_err(dai->dev, "Failed to enable bulk clocks %d\n", ret);

	return ret;
}

static void audin_decoder_i2s_shutdown(struct snd_pcm_substream *substream,
				     struct snd_soc_dai *dai)
{
	struct audin *audin = snd_soc_component_get_drvdata(dai->component);

	clk_bulk_disable_unprepare(audin->bulk_clks_num, audin->bulk_clks);
}

const struct snd_soc_dai_ops audin_decoder_i2s_dai_ops = {
	.hw_params	= audin_decoder_i2s_hw_params,
	.hw_free	= audin_decoder_i2s_hw_free,
	.set_fmt	= audin_decoder_i2s_set_fmt,
	.set_sysclk	= audin_decoder_i2s_set_sysclk,
	.startup	= audin_decoder_i2s_startup,
	.shutdown	= audin_decoder_i2s_shutdown,
	.trigger	= audin_decoder_i2s_trigger,
};
