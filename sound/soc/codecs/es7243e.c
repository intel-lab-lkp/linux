// SPDX-License-Identifier: GPL-2.0-only
/**
 * es7243e.c - ASoC Everest Semiconductor ES7243E audio ADC driver
 *
 * Copyright (c) 2024, SaluteDevices. All Rights Reserved.
 *
 * Authors: Viktor Prutyanov <vvprutyanov@sberdevices.ru>
 *	    Igor Prusov <ivprusov@salutedevices.com>
 *
 * Based on ES7243E driver by David Yang <yangxiaohua@everest-semi.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define ES7243E_RESET			0x00
#define ES7243E_CLK1			0x01
#define  ES7243E_CLK1_ANA_ON		BIT(1)
#define  ES7243E_CLK1_ADC_ON		BIT(3)
#define ES7243E_CLK2			0x02
#define  ES7243E_CLK2_BCLK_INV		BIT(0)
#define ES7243E_ADC_OSR			0x03
#define ES7243E_MCLK_PRE		0x04
#define ES7243E_CF_DSP_DIV		0x05
#define ES7243E_BCLK_DIV		0x06
#define ES7243E_CLK7			0x07
#define  ES7243E_CLK7_LRCK_DIV_HI	GENMASK(3, 0)
#define ES7243E_LRCK_DIV		0x08
#define ES7243E_S1_SEL			0x09
#define ES7243E_S3_SEL			0x0A
#define ES7243E_SDP			0x0B
#define  ES7243E_SDP_MUTE		GENMASK(7, 6)
#define  ES7243E_SDP_LRP		BIT(5)
#define  ES7243E_SDP_WL			GENMASK(4, 2)
#define  ES7243E_SDP_FMT		GENMASK(1, 0)
#define ES7243E_TDM			0x0B
#define ES7243E_ADCCTL1			0x0D
#define  ES7243E_ADCCTL1_SCALE		GENMASK(2, 0)
#define ES7243E_ADC_VOL			0x0E
#define ES7243E_ADCCTL2			0x0F
#define ES7243E_AUTOMUTE1		0x10
#define ES7243E_AUTOMUTE2		0x11
#define ES7243E_AUTOMUTE3		0x12
#define ES7243E_AUTOMUTE4		0x13
#define ES7243E_ADC_HPF1		0x14
#define ES7243E_ADC_HPF2		0x15
#define ES7243E_PDN			0x16
#define  ES7243E_PDN_ANA		BIT(7)
#define  ES7243E_PDN_BIAS		BIT(6)
#define  ES7243E_PDN_MOD1_RST		BIT(5)
#define  ES7243E_PDN_MOD2_RST		BIT(4)
#define  ES7243E_PDN_MOD1		BIT(3)
#define  ES7243E_PDN_MOD2		BIT(2)
#define  ES7243E_PDN_PGA1		BIT(1)
#define  ES7243E_PDN_PGA2		BIT(0)
#define ES7243E_VMIDSEL			0x17
#define ES7243E_ADC_BIAS_0x18		0x18
#define ES7243E_PGA_BIAS		0x19
#define ES7243E_ADC_BIAS_0x1A		0x1A
#define ES7243E_ADC_MICBIAS		0x1B
#define ES7243E_ADC_VRPBIAS		0x1C
#define ES7243E_ADC_LP			0x1D
#define ES7243E_ADC_PGA_LP		0x1E
#define ES7243E_ADC_VMID		0x1F
#define ES7243E_PGA1			0x20
#define	 ES7243E_PGA1_EN		BIT(4)
#define ES7243E_PGA2			0x21
#define	 ES7243E_PGA2_EN		BIT(4)
#define ES7243E_DLL_PWN			0xF9
#define ES7243E_I2C_CONFIG		0xFC
#define ES7243E_FLAG			0xFA
#define ES7243E_CHIP_ID1		0xFD
#define ES7243E_CHIP_ID2		0xFE
#define ES7243E_CHIP_VER		0xFF

struct es7243e_priv {
	struct i2c_client *i2c;
	struct clk *sclk;
	struct clk *lrclk;
	unsigned int sysclk;
	const struct snd_pcm_hw_constraint_list *sysclk_constraints;
};

struct coeff_div {
	u32 mclk;		/* mclk frequency */
	u32 sr_rate;		/* sample rate */
	u8 osr;			/* ADC over sample rate */
	u8 mclk_pre;		/* mclk prediv/premult */
	u8 cf_dsp_div;		/* adclrck divider and daclrck divider */
	u8 scale;		/* ADC gain scale up */
	u8 lrckdiv;		/* lrck divider */
	u8 bclkdiv;		/* sclk divider */
};

static const struct regmap_config es7243e_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ES7243E_CHIP_VER,
	.cache_type = REGCACHE_MAPLE,
};

static const struct coeff_div coeff_div[] = {
	/* mclk     lrck   osr   pre   div   scale lrdiv bclkdiv */
	{ 24576000, 8000,  0x20, 0x50, 0x00, 0x00, 0x0b, 0x2f },
	{ 24576000, 12000, 0x20, 0x30, 0x00, 0x00, 0x07, 0x1f },
	{ 24576000, 16000, 0x20, 0x20, 0x00, 0x00, 0x05, 0x17 },
	{ 24576000, 24000, 0x20, 0x10, 0x00, 0x00, 0x03, 0x0f },
	{ 24576000, 32000, 0x20, 0x21, 0x00, 0x00, 0x02, 0x0b },
	{ 24576000, 48000, 0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 12288000, 8000,  0x20, 0x20, 0x00, 0x00, 0x05, 0x17 },
	{ 12288000, 12000, 0x20, 0x10, 0x00, 0x00, 0x03, 0x0f },
	{ 12288000, 16000, 0x20, 0x21, 0x00, 0x00, 0x02, 0x0b },
	{ 12288000, 24000, 0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 12288000, 32000, 0x20, 0x22, 0x00, 0x00, 0x01, 0x05 },
	{ 12288000, 48000, 0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 6144000,  8000,  0x20, 0x21, 0x00, 0x00, 0x02, 0x0b },
	{ 6144000,  12000, 0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 6144000,  16000, 0x20, 0x22, 0x00, 0x00, 0x01, 0x05 },
	{ 6144000,  24000, 0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 6144000,  32000, 0x20, 0x23, 0x00, 0x00, 0x00, 0x02 },
	{ 6144000,  48000, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 3072000,  8000,  0x20, 0x22, 0x00, 0x00, 0x01, 0x05 },
	{ 3072000,  12000, 0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 3072000,  16000, 0x20, 0x23, 0x00, 0x00, 0x00, 0x02 },
	{ 3072000,  24000, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 3072000,  32000, 0x10, 0x03, 0x20, 0x04, 0x00, 0x02 },
	{ 3072000,  48000, 0x20, 0x03, 0x00, 0x00, 0x00, 0x00 },
	{ 1536000,  8000,  0x20, 0x23, 0x00, 0x00, 0x00, 0x02 },
	{ 1536000,  12000, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 1536000,  16000, 0x10, 0x03, 0x20, 0x04, 0x00, 0x00 },
	{ 1536000,  24000, 0x20, 0x03, 0x00, 0x00, 0x00, 0x00 },
	{ 32768000, 8000,  0x20, 0x70, 0x00, 0x00, 0x0f, 0x3f },
	{ 32768000, 16000, 0x20, 0x30, 0x00, 0x00, 0x07, 0x1f },
	{ 32768000, 32000, 0x20, 0x10, 0x00, 0x00, 0x03, 0x0f },
	{ 16384000, 8000,  0x20, 0x30, 0x00, 0x00, 0x07, 0x1f },
	{ 16384000, 16000, 0x20, 0x10, 0x00, 0x00, 0x03, 0x0f },
	{ 16384000, 32000, 0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 8192000,  8000,  0x20, 0x10, 0x00, 0x00, 0x03, 0x0f },
	{ 8192000,  16000, 0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 8192000,  32000, 0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 4096000,  8000,  0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 4096000,  16000, 0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 4096000,  32000, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 2048000,  8000,  0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 2048000,  16000, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 2048000,  32000, 0x20, 0x03, 0x00, 0x00, 0x00, 0x00 },
	{ 1024000,  8000,  0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 1024000,  16000, 0x20, 0x03, 0x00, 0x00, 0x00, 0x00 },
	{ 22579200, 11025, 0x20, 0x30, 0x00, 0x00, 0x07, 0x1f },
	{ 22579200, 22050, 0x20, 0x10, 0x00, 0x00, 0x03, 0x0f },
	{ 22579200, 44100, 0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 11289600, 11025, 0x20, 0x10, 0x00, 0x00, 0x03, 0x0f },
	{ 11289600, 22050, 0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 11289600, 44100, 0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 56448000, 11025, 0x20, 0x00, 0x00, 0x00, 0x01, 0x07 },
	{ 56448000, 22050, 0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 56448000, 44100, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 28224000, 11025, 0x20, 0x01, 0x00, 0x00, 0x00, 0x03 },
	{ 28224000, 22050, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 28224000, 44100, 0x20, 0x03, 0x00, 0x00, 0x00, 0x00 },
	{ 14112000, 11025, 0x20, 0x02, 0x00, 0x00, 0x00, 0x01 },
	{ 14112000, 22050, 0x20, 0x03, 0x00, 0x00, 0x00, 0x00 },
};

static const struct coeff_div *get_coeff(int mclk, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(coeff_div); i++)
		if (coeff_div[i].sr_rate == rate && coeff_div[i].mclk == mclk)
			return &coeff_div[i];

	return NULL;
}

static const unsigned int rates_12288[] = {
	8000, 12000, 16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000,
};

static const struct snd_pcm_hw_constraint_list constraints_12288 = {
	.count = ARRAY_SIZE(rates_12288),
	.list = rates_12288,
};

static const unsigned int rates_8192[] = {
	8000, 16000, 32000, 64000, 128000,
};

static const struct snd_pcm_hw_constraint_list constraints_8192 = {
	.count = ARRAY_SIZE(rates_8192),
	.list = rates_8192,
};

static const unsigned int rates_112896[] = {
	8000, 11025, 22050, 44100,
};

static const struct snd_pcm_hw_constraint_list constraints_112896 = {
	.count = ARRAY_SIZE(rates_112896),
	.list = rates_112896,
};

static int es7243e_set_dai_sysclk(struct snd_soc_dai *dai,
				  int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_component *component = dai->component;
	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(component);
	struct es7243e_priv *es7243e = snd_soc_component_get_drvdata(component);

	switch (freq) {
	case 11289600:
	case 22579200:
		es7243e->sysclk_constraints = &constraints_112896;
		es7243e->sysclk = freq;
		break;
	case 12288000:
	case 24576000:
		es7243e->sysclk_constraints = &constraints_12288;
		es7243e->sysclk = freq;
		break;
	case 4096000:
	case 8192000:
		es7243e->sysclk_constraints = &constraints_8192;
		es7243e->sysclk = freq;
		break;
	default:
		return -EINVAL;
	}

	return snd_soc_dapm_sync(dapm);
}

static int es7243e_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	unsigned int sdpfmt = 0;
	unsigned int clk2 = 0;
	bool is_dsp = false;
	bool invert_lrck = false;
	int ret;

	/* Set protocol of the serial data port */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		sdpfmt |= FIELD_PREP(ES7243E_SDP_FMT, 0);
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		sdpfmt |= FIELD_PREP(ES7243E_SDP_FMT, 1);
		break;
	case SND_SOC_DAIFMT_DSP_A:
		sdpfmt |= FIELD_PREP(ES7243E_SDP_FMT, 3);
		is_dsp = true;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		sdpfmt |= FIELD_PREP(ES7243E_SDP_FMT, 3);
		is_dsp = true;
		break;
	default:
		return -EINVAL;
	}

	/* Set LR and BCLK polarity */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		clk2 |= FIELD_PREP(ES7243E_CLK2_BCLK_INV, 0);
		sdpfmt |= FIELD_PREP(ES7243E_SDP_LRP, 0);
		break;
	case SND_SOC_DAIFMT_IB_IF:
		clk2 |= FIELD_PREP(ES7243E_CLK2_BCLK_INV, 1);
		sdpfmt |= FIELD_PREP(ES7243E_SDP_LRP, 1);
		invert_lrck = true;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		clk2 |= FIELD_PREP(ES7243E_CLK2_BCLK_INV, 1);
		sdpfmt |= FIELD_PREP(ES7243E_SDP_LRP, 0);
		break;
	case SND_SOC_DAIFMT_NB_IF:
		clk2 |= FIELD_PREP(ES7243E_CLK2_BCLK_INV, 0);
		sdpfmt |= FIELD_PREP(ES7243E_SDP_LRP, 1);
		invert_lrck = true;
		break;
	default:
		return -EINVAL;
	}

	/* Inverted LRCK is not available in DSP mode. */
	if (is_dsp && invert_lrck)
		return -EINVAL;

	ret = snd_soc_component_update_bits(component, ES7243E_CLK2,
					    ES7243E_CLK2_BCLK_INV, clk2);
	if (ret < 0)
		return ret;

	return snd_soc_component_update_bits(component, ES7243E_SDP,
					     ES7243E_SDP_LRP, sdpfmt);
}

static int es7243e_pcm_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct es7243e_priv *es7243e = snd_soc_component_get_drvdata(dai->component);

	if (!es7243e->sysclk_constraints)
		return 0;

	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_RATE,
					  es7243e->sysclk_constraints);
}

static int es7243e_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct es7243e_priv *es7243e = dev_get_drvdata(dai->component->dev);
	const struct coeff_div *cd;
	unsigned int val;
	int width;
	int ret;

	cd = get_coeff(es7243e->sysclk, params_rate(params));
	if (!cd) {
		dev_err(dai->component->dev,
			"Can't set sample rate = %uHz, F_mclk = %uHz\n",
			params_rate(params), es7243e->sysclk);

		return -EINVAL;
	}

	ret = snd_soc_component_write(component, ES7243E_ADC_OSR, cd->osr);
	if (ret < 0)
		return ret;

	ret = snd_soc_component_write(component, ES7243E_MCLK_PRE,  cd->mclk_pre);
	if (ret < 0)
		return ret;

	ret = snd_soc_component_write(component, ES7243E_CF_DSP_DIV, cd->cf_dsp_div);
	if (ret < 0)
		return ret;

	val = FIELD_PREP(ES7243E_ADCCTL1_SCALE, cd->scale);
	ret = snd_soc_component_update_bits(component, ES7243E_ADCCTL1,
					    ES7243E_ADCCTL1_SCALE, val);
	if (ret < 0)
		return ret;

	val = FIELD_PREP(ES7243E_CLK7_LRCK_DIV_HI, cd->lrckdiv);
	ret = snd_soc_component_update_bits(component, ES7243E_CLK7,
					    ES7243E_CLK7_LRCK_DIV_HI, val);
	if (ret < 0)
		return ret;

	ret = snd_soc_component_write(component, ES7243E_BCLK_DIV, cd->bclkdiv);
	if (ret < 0)
		return ret;

	switch ((params_width(params))) {
	case 24:
		width = 0;
		break;
	case 20:
		width = 1;
		break;
	case 18:
		width = 2;
		break;
	case 16:
		width = 3;
		break;
	case 32:
		width = 4;
		break;
	default:
		return -ENOENT;
	}

	val = FIELD_PREP(ES7243E_SDP_WL, width);
	ret = snd_soc_component_update_bits(component, ES7243E_SDP,
					    ES7243E_SDP_WL, val);
	if (ret < 0)
		return ret;

	/*
	 * Delay timings are not documented in datasheet and come from vendor's
	 * driver implementation.
	 */
	fsleep(50 * USEC_PER_MSEC);
	val = FIELD_PREP(ES7243E_SDP_MUTE, 0);
	return snd_soc_component_update_bits(component, ES7243E_SDP,
					     ES7243E_SDP_MUTE, val);
}

static int es7243e_mute(struct snd_soc_dai *dai, int mute, int direction)
{
	const int val = FIELD_PREP(ES7243E_SDP_MUTE, 3 * !!mute);

	return snd_soc_component_update_bits(dai->component, ES7243E_SDP,
					     ES7243E_SDP_MUTE, val);
}

static const DECLARE_TLV_DB_RANGE(pga_scale,
	0, 10, TLV_DB_SCALE_ITEM(0, 300, 0),
	11, 14, TLV_DB_SCALE_ITEM(3300, 150, 0),
);

static const DECLARE_TLV_DB_SCALE(vol_scale, -9550, 50, 0);

static const struct snd_kcontrol_new es7243e_snd_controls[] = {
	SOC_SINGLE_RANGE_TLV("PGA1 Volume", ES7243E_PGA1, 0, 0, 14, 0, pga_scale),
	SOC_SINGLE_RANGE_TLV("PGA2 Volume", ES7243E_PGA2, 0, 0, 14, 0, pga_scale),
	SOC_SINGLE_RANGE_TLV("ADC Volume", ES7243E_ADC_VOL, 0, 0, 255, 0, vol_scale),
};

static const struct snd_soc_dapm_widget es7243e_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("INPUT"),
	SND_SOC_DAPM_ADC("ADC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SDOUT", "I2S Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route es7243e_dapm_routes[] = {
	{ "ADC", NULL, "INPUT" },
	{ "SDOUT", NULL, "ADC" },
};

static const struct snd_soc_dai_ops es7243e_ops = {
	.startup = es7243e_pcm_startup,
	.hw_params = es7243e_pcm_hw_params,
	.set_fmt = es7243e_set_dai_fmt,
	.set_sysclk = es7243e_set_dai_sysclk,
	.mute_stream = es7243e_mute,
};

static struct snd_soc_dai_driver es7243e_dai = {
	.name = "es7243e-hifi",
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = (SNDRV_PCM_FMTBIT_S16_LE  |
			    SNDRV_PCM_FMTBIT_S20_3LE |
			    SNDRV_PCM_FMTBIT_S24_LE  |
			    SNDRV_PCM_FMTBIT_S32_LE),
		},
	.ops = &es7243e_ops,
};

static const struct reg_sequence init_sequence[] = {
	{ ES7243E_CLK2,			0x00 },
	{ ES7243E_SDP,			0x00 },
	{ ES7243E_TDM,			0x00 },

	/* Set MCLK/LRCK ratio to 256 */
	{ ES7243E_ADC_OSR,		0x20 },
	{ ES7243E_ADCCTL1,		0x00 },
	{ ES7243E_MCLK_PRE,		0x01 },
	{ ES7243E_CF_DSP_DIV,		0x00 },

	{ ES7243E_S1_SEL,		0xe0 },
	{ ES7243E_S3_SEL,		0xa0 },

	/* Set ADC volume to 0dB */
	{ ES7243E_ADC_VOL,		0xbf },
	/* Enable dynamic HPF (high-pass filter), disable auto level control */
	{ ES7243E_ADCCTL2,		0x80 },
	{ ES7243E_ADC_HPF1,		0x0c },
	{ ES7243E_ADC_HPF2,		0x0c },
	/* VDDA = 3.3V */
	{ ES7243E_ADC_BIAS_0x18,	0x26 },
	{ ES7243E_VMIDSEL,		0x02 },
	{ ES7243E_PGA_BIAS,		0x77 },
	{ ES7243E_ADC_BIAS_0x1A,	0xf4 },
	{ ES7243E_ADC_MICBIAS,		0x66 },
	{ ES7243E_ADC_VRPBIAS,		0x44 },
	{ ES7243E_ADC_LP,		0x3c },
	{ ES7243E_ADC_PGA_LP,		0x00 },
	{ ES7243E_ADC_VMID,		0x0c },
	/* Set slave mode, enable clocks, power up */
	{ ES7243E_RESET,		0x80 },
	{ ES7243E_CLK1,			0x3a },
	{ ES7243E_PDN,			0x00 },

	/* Set PGAs gain to 0dB */
	{ ES7243E_PGA1,			0x10 },
	{ ES7243E_PGA2,			0x10, 100 * USEC_PER_MSEC },

	/* Reset PGAs */
	{ ES7243E_PDN,			0x03, 100 * USEC_PER_MSEC  },
	{ ES7243E_PDN,			0x00 },
};

static int es7243e_probe(struct snd_soc_component *component)
{
	return regmap_multi_reg_write(component->regmap, init_sequence,
				      ARRAY_SIZE(init_sequence));
}

static int es7243e_suspend(struct snd_soc_component *component)
{
	struct es7243e_priv *es7243e = snd_soc_component_get_drvdata(component);
	int ret;
	unsigned int val, mask;

	val = FIELD_PREP(ES7243E_SDP_MUTE, 0);
	ret = snd_soc_component_update_bits(component, ES7243E_SDP,
					    ES7243E_SDP_MUTE, val);
	if (ret < 0)
		return ret;

	val = FIELD_PREP(ES7243E_PGA1_EN, 0);
	snd_soc_component_update_bits(component, ES7243E_PGA1,
				      ES7243E_PGA1_EN, val);
	if (ret < 0)
		return ret;

	val = FIELD_PREP(ES7243E_PGA2_EN, 0);
	snd_soc_component_update_bits(component, ES7243E_PGA2,
				      ES7243E_PGA2_EN, val);
	if (ret < 0)
		return ret;

	ret = snd_soc_component_write(component, ES7243E_PDN, 0xff);
	if (ret < 0)
		return ret;

	mask = ES7243E_CLK1_ANA_ON | ES7243E_CLK1_ADC_ON;
	ret = snd_soc_component_update_bits(component, ES7243E_CLK1, mask, 0);
	if (ret < 0)
		return ret;

	clk_disable_unprepare(es7243e->lrclk);
	clk_disable_unprepare(es7243e->sclk);

	return 0;
}

static int es7243e_resume(struct snd_soc_component *component)
{
	struct es7243e_priv *es7243e = snd_soc_component_get_drvdata(component);
	int ret;
	unsigned int val;

	ret = clk_prepare_enable(es7243e->sclk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(es7243e->lrclk);
	if (ret)
		return ret;

	val = ES7243E_CLK1_ANA_ON | ES7243E_CLK1_ADC_ON;
	ret = snd_soc_component_update_bits(component, ES7243E_CLK1, val, val);
	if (ret < 0)
		return ret;

	ret = snd_soc_component_write(component, ES7243E_PDN, 0);
	if (ret < 0)
		return ret;

	val = FIELD_PREP(ES7243E_PGA1_EN, 1);
	snd_soc_component_update_bits(component, ES7243E_PGA1,
				      ES7243E_PGA1_EN, val);
	if (ret < 0)
		return ret;

	val = FIELD_PREP(ES7243E_PGA2_EN, 1);
	snd_soc_component_update_bits(component, ES7243E_PGA2,
				      ES7243E_PGA2_EN, val);
	if (ret < 0)
		return ret;

	val = FIELD_PREP(ES7243E_SDP_MUTE, 3);
	ret = snd_soc_component_update_bits(component, ES7243E_SDP,
					    ES7243E_SDP_MUTE, val);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct snd_soc_component_driver soc_component_dev_es7243e = {
	.probe = es7243e_probe,
	.controls = es7243e_snd_controls,
	.suspend = es7243e_suspend,
	.resume = es7243e_resume,
	.num_controls = ARRAY_SIZE(es7243e_snd_controls),
	.dapm_widgets = es7243e_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(es7243e_dapm_widgets),
	.dapm_routes = es7243e_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(es7243e_dapm_routes),
};

static int es7243e_hw_init(struct device *dev)
{
	struct regmap *regmap = dev_get_regmap(dev, NULL);
	u8 ids[2];
	u16 chip_id;
	int ret;

	ret = regmap_bulk_read(regmap, ES7243E_CHIP_ID1, ids, ARRAY_SIZE(ids));
	if (ret < 0)
		goto fail;

	chip_id = (ids[0] << 8) | ids[1];

	if (chip_id != 0x7A43) {
		return dev_err_probe(dev, -ENODEV,
				     "wrong CHIP ID: 0x%04x, expected 0x7A43\n",
				     chip_id);
	}

	return 0;

fail:
	return dev_err_probe(dev, ret, "failed to read chip ID\n");
}

static int es7243e_i2c_probe(struct i2c_client *i2c)
{
	struct es7243e_priv *es7243e;
	struct regmap *regmap;
	int ret;

	es7243e = devm_kzalloc(&i2c->dev, sizeof(*es7243e), GFP_KERNEL);
	if (!es7243e)
		return -ENOMEM;

	es7243e->i2c = i2c;
	dev_set_drvdata(&i2c->dev, es7243e);

	regmap = devm_regmap_init_i2c(i2c, &es7243e_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&i2c->dev, "failed to init regmap\n");
		return PTR_ERR(regmap);
	}

	es7243e->sclk = devm_clk_get_enabled(&i2c->dev, "sclk");
	if (IS_ERR(es7243e->sclk))
		return dev_err_probe(&i2c->dev, PTR_ERR(es7243e->sclk),
				     "failed to get sclk clock\n");

	es7243e->lrclk = devm_clk_get_enabled(&i2c->dev, "lrclk");
	if (IS_ERR(es7243e->lrclk))
		return dev_err_probe(&i2c->dev, PTR_ERR(es7243e->lrclk),
				     "failed to get lrclk clock\n");

	if (es7243e_hw_init(&i2c->dev))
		return -ENODEV;

	ret = devm_snd_soc_register_component(&i2c->dev,
					      &soc_component_dev_es7243e,
					      &es7243e_dai, 1);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "failed to register component\n");

	return ret;
}

static const struct i2c_device_id es7243e_i2c_id[] = {
	{ "es7243e", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, es7243e_i2c_id);

static const struct of_device_id es7243e_dt_ids[] = {
	{ .compatible = "everest,es7243e", },
	{ }
};
MODULE_DEVICE_TABLE(of, es7243e_dt_ids);

static struct i2c_driver es7243e_i2c_driver = {
	.probe = es7243e_i2c_probe,
	.id_table = es7243e_i2c_id,
	.driver = {
		.name = "es7243e-codec",
		.of_match_table = es7243e_dt_ids,
	},
};

module_i2c_driver(es7243e_i2c_driver);
MODULE_DESCRIPTION("ASoC ES7243E audio ADC driver");
MODULE_AUTHOR("Igor Prusov <ivprusov@salutedevices.com>");
MODULE_LICENSE("GPL");
