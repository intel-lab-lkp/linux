// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2026 BayLibre, SAS.
// Author: Valerio Setti <vsetti@baylibre.com>

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include "gx-formatter.h"

/* I2SIN_CTRL register and bits */
#define AUDIN_I2SIN_CTRL			0x0
#define  AUDIN_I2SIN_CTRL_I2SIN_DIR		BIT(0)
#define  AUDIN_I2SIN_CTRL_I2SIN_CLK_SEL		BIT(1)
#define  AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SEL	BIT(2)
#define  AUDIN_I2SIN_CTRL_I2SIN_POS_SYNC	BIT(3)
#define  AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SKEW_MASK	GENMASK(6, 4)
#define  AUDIN_I2SIN_CTRL_I2SIN_LRCLK_INV	BIT(7)
#define  AUDIN_I2SIN_CTRL_I2SIN_SIZE_MASK	GENMASK(9, 8)
#define  AUDIN_I2SIN_CTRL_I2SIN_CHAN_EN_MASK	GENMASK(13, 10)
#define  AUDIN_I2SIN_CTRL_I2SIN_EN		BIT(15)

static struct snd_soc_dai *
audin_decoder_i2s_get_be(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_path *p;
	struct snd_soc_dai *be;

	snd_soc_dapm_widget_for_each_source_path(w, p) {
		if (!p->connect)
			continue;

		if (p->source->id == snd_soc_dapm_dai_out)
			return (struct snd_soc_dai *)p->source->priv;

		be = audin_decoder_i2s_get_be(p->source);
		if (be)
			return be;
	}

	return NULL;
}

static struct gx_stream *
audin_decoder_i2s_get_stream(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dai *be = audin_decoder_i2s_get_be(w);

	if (!be)
		return NULL;

	return snd_soc_dai_dma_data_get_capture(be);
}

static void audin_decoder_i2s_enable(struct regmap *map)
{
	regmap_update_bits(map, AUDIN_I2SIN_CTRL,
			   AUDIN_I2SIN_CTRL_I2SIN_EN,
			   AUDIN_I2SIN_CTRL_I2SIN_EN);
}

static void audin_decoder_i2s_disable(struct regmap *map)
{
	regmap_update_bits(map, AUDIN_I2SIN_CTRL,
			   AUDIN_I2SIN_CTRL_I2SIN_EN, 0);
}

static int audin_decoder_i2s_prepare(struct regmap *map,
				   const struct gx_formatter_hw *quirks,
				   struct gx_stream *ts)
{
	unsigned int val;
	int ret;

	if (ts->width != 16)
		return -EINVAL;

	if (ts->channels != 2)
		return -EINVAL;

	/*
	 * I2S decoder always outputs 24bits to the FIFO according to the
	 * manual. The only thing we can do is mask some bits as follows:
	 * - 0: 16 bit
	 * - 1: 18 bits (not exposed as supported format)
	 * - 2: 20 bits (not exposed as supported format)
	 * - 3: 24 bits
	 *
	 * At the moment only 16 bit format is supported, but we force 24 bit
	 * anyway here to ease the future support of 24 bit format. Extra bits
	 * will be filtered out at FIFO stage.
	 * Note: data is left-justified, so in case of 16 bits samples, this
	 *       means that the LSB is to be discarded at FIFO level and the
	 *       relevant part is in bits [23:8].
	 */
	val = FIELD_PREP(AUDIN_I2SIN_CTRL_I2SIN_SIZE_MASK, 3);
	ret = regmap_update_bits(map, AUDIN_I2SIN_CTRL,
				 AUDIN_I2SIN_CTRL_I2SIN_SIZE_MASK, val);
	if (ret)
		return ret;

	/*
	 * The manual claims that this platform supports up to 4 streams
	 * (8 channels), but the SOC only has 1 input pin (i.e. it only allows
	 * for 1 stream and 2 channels) so this is what we support here.
	 */
	val = FIELD_PREP(AUDIN_I2SIN_CTRL_I2SIN_CHAN_EN_MASK, 1);
	ret = regmap_update_bits(map, AUDIN_I2SIN_CTRL,
				 AUDIN_I2SIN_CTRL_I2SIN_CHAN_EN_MASK, val);
	if (ret)
		return ret;

	/*
	 * Use clocks from AIU and not from the pads since we only want to
	 * support master mode.
	 */
	val = AUDIN_I2SIN_CTRL_I2SIN_CLK_SEL |
	      AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SEL |
	      AUDIN_I2SIN_CTRL_I2SIN_DIR;
	ret = regmap_update_bits(map, AUDIN_I2SIN_CTRL, val, val);
	if (ret)
		return ret;

	switch (ts->iface->fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_IB_NF:
		val = AUDIN_I2SIN_CTRL_I2SIN_POS_SYNC;
		break;
	case SND_SOC_DAIFMT_NB_NF:
		val = 0;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(map, AUDIN_I2SIN_CTRL,
				 AUDIN_I2SIN_CTRL_I2SIN_POS_SYNC, val);
	if (ret)
		return ret;

	/*
	 * MSB data starts 1 clock cycle after LRCLK transition, as per I2S
	 * specs.
	 */
	val = FIELD_PREP(AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SKEW_MASK, 1);
	ret = regmap_update_bits(map, AUDIN_I2SIN_CTRL,
				 AUDIN_I2SIN_CTRL_I2SIN_LRCLK_INV |
				 AUDIN_I2SIN_CTRL_I2SIN_LRCLK_SKEW_MASK,
				 val);
	if (ret)
		return ret;

	return 0;
}

static const struct snd_soc_dapm_widget audin_decoder_i2s_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("IN",  NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_PGA_E("DEC", SND_SOC_NOPM, 0, 0, NULL, 0,
			   gx_formatter_event,
			   (SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD)),
	SND_SOC_DAPM_AIF_OUT("OUT", NULL, 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route audin_decoder_i2s_dapm_routes[] = {
	{ "DEC", NULL, "IN" },
	{ "OUT", NULL, "DEC" },
};

static const struct snd_soc_component_driver audin_decoder_i2s_component = {
	.dapm_widgets		= audin_decoder_i2s_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(audin_decoder_i2s_dapm_widgets),
	.dapm_routes		= audin_decoder_i2s_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(audin_decoder_i2s_dapm_routes),
};

static const struct regmap_config audin_decoder_i2s_regmap_cfg = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= 0x3,
};

static const struct gx_formatter_ops audin_decoder_i2s_ops = {
	.get_stream	= audin_decoder_i2s_get_stream,
	.prepare	= audin_decoder_i2s_prepare,
	.enable		= audin_decoder_i2s_enable,
	.disable	= audin_decoder_i2s_disable,
};

static const struct gx_formatter_driver audin_decoder_i2s_drv = {
	.component_drv	= &audin_decoder_i2s_component,
	.regmap_cfg	= &audin_decoder_i2s_regmap_cfg,
	.ops		= &audin_decoder_i2s_ops,
};

static const struct of_device_id audin_decoder_i2s_of_match[] = {
	{
		.compatible = "amlogic,meson-gxbb-audin-decoder-i2s",
		.data = &audin_decoder_i2s_drv
	},
	{}
};
MODULE_DEVICE_TABLE(of, audin_decoder_i2s_of_match);

static struct platform_driver audin_decoder_i2s_pdrv = {
	.probe = gx_formatter_probe,
	.driver = {
		.name = "meson-gx-audin-decoder-i2s",
		.of_match_table = audin_decoder_i2s_of_match,
	},
};
module_platform_driver(audin_decoder_i2s_pdrv);

MODULE_DESCRIPTION("Meson AUDIN Formatter Driver");
MODULE_AUTHOR("Valerio Setti <vsetti@baylibre.com>");
MODULE_LICENSE("GPL");
