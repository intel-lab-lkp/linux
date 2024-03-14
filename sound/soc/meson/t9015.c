// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2020 BayLibre, SAS.
// Author: Jerome Brunet <jbrunet@baylibre.com>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define BLOCK_EN	0x00
#define  LORN_EN	0
#define  LORP_EN	1
#define  LOLN_EN	2
#define  LOLP_EN	3
#define  DACR_EN	4
#define  DACL_EN	5
#define  ADCR_EN	6
#define  ADCL_EN	7
#define  PGAR_ZCD_EN	8
#define  PGAL_ZCD_EN	9
#define  PGAR_EN	10
#define  PGAL_EN	11
#define  ADCR_INV	16
#define  ADCL_INV	17
#define  ADCR_SRC	18
#define  ADCL_SRC	19
#define  DACR_INV	20
#define  DACL_INV	21
#define  DACR_SRC	22
#define  DACL_SRC	23
#define  ADC_DEM_EN	26
#define  ADC_FILTER_MODE 28
#define  ADC_FILTER_EN	29
#define  REFP_BUF_EN	BIT(12)
#define  BIAS_CURRENT_EN BIT(13)
#define  VMID_GEN_FAST	BIT(14)
#define  VMID_GEN_EN	BIT(15)
#define  I2S_MODE	BIT(30)
#define VOL_CTRL0	0x04
#define  PGAR_VC	0
#define  PGAL_VC	8
#define  ADCR_VC	16
#define  ADCL_VC	24
#define  GAIN_H		31
#define  GAIN_L		23
#define VOL_CTRL1	0x08
#define  DAC_MONO	8
#define  RAMP_RATE	10
#define  VC_RAMP_MODE	12
#define  MUTE_MODE	13
#define  UNMUTE_MODE	14
#define  DAC_SOFT_MUTE	15
#define  DACR_VC	16
#define  DACL_VC	24
#define LINEOUT_CFG	0x0c
#define  LORN_POL	0
#define  LORP_POL	4
#define  LOLN_POL	8
#define  LOLP_POL	12
#define POWER_CFG	0x10
#define LINEIN_CFG	0x14
#define  MICBIAS_LEVEL	0
#define  MICBIAS_EN	3
#define  PGAR_CTVMN	8
#define  PGAR_CTVMP	9
#define  PGAL_CTVMN	10
#define  PGAL_CTVMP	11
#define  PGAR_CTVIN	12
#define  PGAR_CTVIP	13
#define  PGAL_CTVIN	14
#define  PGAL_CTVIP	15

#define PGAR_MASK	(BIT(PGAR_CTVMP) | BIT(PGAR_CTVMN) | \
			 BIT(PGAR_CTVIP) | BIT(PGAR_CTVIN))
#define PGAR_DIFF	(BIT(PGAR_CTVIP) | BIT(PGAR_CTVIN))
#define PGAR_POSITIVE	(BIT(PGAR_CTVIP) | BIT(PGAR_CTVMN))
#define PGAR_NEGATIVE	(BIT(PGAR_CTVIN) | BIT(PGAR_CTVMP))
#define PGAL_MASK	(BIT(PGAL_CTVMP) | BIT(PGAL_CTVMN) | \
			 BIT(PGAL_CTVIP) | BIT(PGAL_CTVIN))
#define PGAL_DIFF	(BIT(PGAL_CTVIP) | BIT(PGAL_CTVIN))
#define PGAL_POSITIVE	(BIT(PGAL_CTVIP) | BIT(PGAL_CTVMN))
#define PGAL_NEGATIVE	(BIT(PGAL_CTVIN) | BIT(PGAL_CTVMP))

struct t9015 {
	struct regulator *avdd;
};

struct t9015_match_data {
	const struct snd_soc_component_driver *component_drv;
	struct snd_soc_dai_driver *dai_drv;
	unsigned int max_register;
};

static int t9015_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	unsigned int val;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		val = I2S_MODE;
		break;

	case SND_SOC_DAIFMT_CBS_CFS:
		val = 0;
		break;

	default:
		return -EINVAL;
	}

	snd_soc_component_update_bits(component, BLOCK_EN, I2S_MODE, val);

	if (((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S) &&
	    ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_LEFT_J))
		return -EINVAL;

	return 0;
}

static const struct snd_soc_dai_ops t9015_dai_ops = {
	.set_fmt = t9015_dai_set_fmt,
};

static struct snd_soc_dai_driver t9015_dai = {
	.name = "t9015-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = (SNDRV_PCM_FMTBIT_S8 |
			    SNDRV_PCM_FMTBIT_S16_LE |
			    SNDRV_PCM_FMTBIT_S20_LE |
			    SNDRV_PCM_FMTBIT_S24_LE),
	},
	.ops = &t9015_dai_ops,
};

static struct snd_soc_dai_driver a1_t9015_dai = {
	.name = "t9015-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = (SNDRV_PCM_FMTBIT_S8 |
			    SNDRV_PCM_FMTBIT_S16_LE |
			    SNDRV_PCM_FMTBIT_S20_LE |
			    SNDRV_PCM_FMTBIT_S24_LE),
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_96000,
		.formats = (SNDRV_PCM_FMTBIT_S8 |
			    SNDRV_PCM_FMTBIT_S16_LE |
			    SNDRV_PCM_FMTBIT_S20_LE |
			    SNDRV_PCM_FMTBIT_S24_LE),
	},
	.ops = &t9015_dai_ops,
};

static const DECLARE_TLV_DB_MINMAX_MUTE(dac_vol_tlv, -9525, 0);

static const char * const ramp_rate_txt[] = { "Fast", "Slow" };
static SOC_ENUM_SINGLE_DECL(ramp_rate_enum, VOL_CTRL1, RAMP_RATE,
			    ramp_rate_txt);

static const char * const dacr_in_txt[] = { "Right", "Left" };
static SOC_ENUM_SINGLE_DECL(dacr_in_enum, BLOCK_EN, DACR_SRC, dacr_in_txt);

static const char * const dacl_in_txt[] = { "Left", "Right" };
static SOC_ENUM_SINGLE_DECL(dacl_in_enum, BLOCK_EN, DACL_SRC, dacl_in_txt);

static const char * const mono_txt[] = { "Stereo", "Mono"};
static SOC_ENUM_SINGLE_DECL(mono_enum, VOL_CTRL1, DAC_MONO, mono_txt);

static const struct snd_kcontrol_new t9015_right_dac_mux =
	SOC_DAPM_ENUM("Right DAC Source", dacr_in_enum);
static const struct snd_kcontrol_new t9015_left_dac_mux =
	SOC_DAPM_ENUM("Left DAC Source", dacl_in_enum);

static const struct snd_kcontrol_new t9015_snd_controls[] = {
	/* Volume Controls */
	SOC_ENUM("Playback Channel Mode", mono_enum),
	SOC_SINGLE("Playback Switch", VOL_CTRL1, DAC_SOFT_MUTE, 1, 1),
	SOC_DOUBLE_TLV("Playback Volume", VOL_CTRL1, DACL_VC, DACR_VC,
		       0xff, 0, dac_vol_tlv),

	/* Ramp Controls */
	SOC_ENUM("Ramp Rate", ramp_rate_enum),
	SOC_SINGLE("Volume Ramp Switch", VOL_CTRL1, VC_RAMP_MODE, 1, 0),
	SOC_SINGLE("Mute Ramp Switch", VOL_CTRL1, MUTE_MODE, 1, 0),
	SOC_SINGLE("Unmute Ramp Switch", VOL_CTRL1, UNMUTE_MODE, 1, 0),
};

static const struct snd_soc_dapm_widget t9015_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("Right IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("Left IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX("Right DAC Sel", SND_SOC_NOPM, 0, 0,
			 &t9015_right_dac_mux),
	SND_SOC_DAPM_MUX("Left DAC Sel", SND_SOC_NOPM, 0, 0,
			 &t9015_left_dac_mux),
	SND_SOC_DAPM_DAC("Right DAC", NULL, BLOCK_EN, DACR_EN, 0),
	SND_SOC_DAPM_DAC("Left DAC",  NULL, BLOCK_EN, DACL_EN, 0),
	SND_SOC_DAPM_OUT_DRV("Right- Driver", BLOCK_EN, LORN_EN, 0,
			 NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Right+ Driver", BLOCK_EN, LORP_EN, 0,
			 NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Left- Driver",  BLOCK_EN, LOLN_EN, 0,
			 NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Left+ Driver",  BLOCK_EN, LOLP_EN, 0,
			 NULL, 0),
	SND_SOC_DAPM_OUTPUT("LORN"),
	SND_SOC_DAPM_OUTPUT("LORP"),
	SND_SOC_DAPM_OUTPUT("LOLN"),
	SND_SOC_DAPM_OUTPUT("LOLP"),
};

static const struct snd_soc_dapm_route t9015_dapm_routes[] = {
	{ "Right IN", NULL, "Playback" },
	{ "Left IN",  NULL, "Playback" },
	{ "Right DAC Sel", "Right", "Right IN" },
	{ "Right DAC Sel", "Left",  "Left IN" },
	{ "Left DAC Sel",  "Right", "Right IN" },
	{ "Left DAC Sel",  "Left",  "Left IN" },
	{ "Right DAC", NULL, "Right DAC Sel" },
	{ "Left DAC",  NULL, "Left DAC Sel" },
	{ "Right- Driver", NULL, "Right DAC" },
	{ "Right+ Driver", NULL, "Right DAC" },
	{ "Left- Driver",  NULL, "Left DAC"  },
	{ "Left+ Driver",  NULL, "Left DAC"  },
	{ "LORN", NULL, "Right- Driver", },
	{ "LORP", NULL, "Right+ Driver", },
	{ "LOLN", NULL, "Left- Driver",  },
	{ "LOLP", NULL, "Left+ Driver",  },
};

static const char * const a1_right_driver_txt[] = { "None", "Right DAC",
	"Left DAC Inverted" };
static const unsigned int a1_right_driver_values[] = { 0, 2, 4 };

static const char * const a1_left_driver_txt[] = { "None", "Left DAC",
	"Right DAC Inverted" };
static const unsigned int a1_left_driver_values[] = { 0, 2, 4 };

static SOC_VALUE_ENUM_SINGLE_DECL(a1_right_driver, LINEOUT_CFG, 12, 0x7,
				  a1_right_driver_txt, a1_right_driver_values);
static SOC_VALUE_ENUM_SINGLE_DECL(a1_left_driver, LINEOUT_CFG, 4, 0x7,
				  a1_left_driver_txt, a1_left_driver_values);

static const struct snd_kcontrol_new a1_right_driver_mux =
	SOC_DAPM_ENUM("Right Driver+ Source", a1_right_driver);
static const struct snd_kcontrol_new a1_left_driver_mux =
	SOC_DAPM_ENUM("Left Driver+ Source", a1_left_driver);

static const DECLARE_TLV_DB_MINMAX_MUTE(a1_adc_vol_tlv, -29625, 0);
static const DECLARE_TLV_DB_MINMAX_MUTE(a1_adc_pga_vol_tlv, -1200, 0);

static const char * const a1_adc_right_txt[] = { "Right", "Left" };
static SOC_ENUM_SINGLE_DECL(a1_adc_right, BLOCK_EN, ADCR_SRC, a1_adc_right_txt);

static const char * const a1_adc_left_txt[] = { "Left", "Right" };
static SOC_ENUM_SINGLE_DECL(a1_adc_left, BLOCK_EN, ADCL_SRC, a1_adc_left_txt);

static const struct snd_kcontrol_new a1_adc_right_mux =
	SOC_DAPM_ENUM("ADC Right Source", a1_adc_right);
static const struct snd_kcontrol_new a1_adc_left_mux =
	SOC_DAPM_ENUM("ADC Left Source", a1_adc_left);

static const char * const a1_adc_filter_mode_txt[] = { "Voice", "HiFi"};
static SOC_ENUM_SINGLE_DECL(a1_adc_filter_mode, BLOCK_EN, ADC_FILTER_MODE,
			    a1_adc_filter_mode_txt);

static const char * const a1_adc_mic_bias_level_txt[] = { "2.0V", "2.1V",
	"2.3V", "2.5V", "2.8V" };
static const unsigned int a1_adc_mic_bias_level_values[] = { 0, 1, 2, 3, 7 };
static SOC_VALUE_ENUM_SINGLE_DECL(a1_adc_mic_bias_level,
				  LINEIN_CFG, MICBIAS_LEVEL, 0x7,
				  a1_adc_mic_bias_level_txt,
				  a1_adc_mic_bias_level_values);

static const char * const a1_adc_pga_txt[] = { "None", "Differential",
	"Positive", "Negative" };
static const unsigned int a1_adc_pga_right_values[] = { 0, PGAR_DIFF,
	PGAR_POSITIVE, PGAR_NEGATIVE };
static const unsigned int a1_adc_pga_left_values[] = { 0, PGAL_DIFF,
	PGAL_POSITIVE, PGAL_NEGATIVE };

static SOC_VALUE_ENUM_SINGLE_DECL(a1_adc_pga_right, LINEIN_CFG, 0, PGAR_MASK,
				  a1_adc_pga_txt, a1_adc_pga_right_values);
static SOC_VALUE_ENUM_SINGLE_DECL(a1_adc_pga_left, LINEIN_CFG, 0, PGAL_MASK,
				  a1_adc_pga_txt, a1_adc_pga_left_values);

static const struct snd_kcontrol_new a1_adc_pga_right_mux =
	SOC_DAPM_ENUM("ADC PGA Right Source", a1_adc_pga_right);
static const struct snd_kcontrol_new a1_adc_pga_left_mux =
	SOC_DAPM_ENUM("ADC PGA Left Source", a1_adc_pga_left);

static const struct snd_kcontrol_new a1_t9015_snd_controls[] = {
	/* Volume Controls */
	SOC_ENUM("Playback Channel Mode", mono_enum),
	SOC_SINGLE("Playback Switch", VOL_CTRL1, DAC_SOFT_MUTE, 1, 1),
	SOC_DOUBLE_TLV("Playback Volume", VOL_CTRL1, DACL_VC, DACR_VC,
		       0xff, 0, dac_vol_tlv),

	/* Ramp Controls */
	SOC_ENUM("Ramp Rate", ramp_rate_enum),
	SOC_SINGLE("Volume Ramp Switch", VOL_CTRL1, VC_RAMP_MODE, 1, 0),
	SOC_SINGLE("Mute Ramp Switch", VOL_CTRL1, MUTE_MODE, 1, 0),
	SOC_SINGLE("Unmute Ramp Switch", VOL_CTRL1, UNMUTE_MODE, 1, 0),

	/* ADC Controls */
	SOC_DOUBLE_TLV("ADC Volume", VOL_CTRL0, ADCL_VC, ADCR_VC,
		       0x7f, 0, a1_adc_vol_tlv),
	SOC_SINGLE("ADC Filter Switch", BLOCK_EN, ADC_FILTER_EN, 1, 0),
	SOC_ENUM("ADC Filter Mode", a1_adc_filter_mode),
	SOC_SINGLE("ADC Mic Bias Switch", LINEIN_CFG, MICBIAS_EN, 1, 0),
	SOC_ENUM("ADC Mic Bias Level", a1_adc_mic_bias_level),
	SOC_SINGLE("ADC DEM Switch", BLOCK_EN, ADC_DEM_EN, 1, 0),
	SOC_DOUBLE_TLV("ADC PGA Volume", VOL_CTRL0, PGAR_VC, PGAL_VC,
		       0x1f, 0, a1_adc_pga_vol_tlv),
	SOC_DOUBLE("ADC PGA Zero Cross-detection Switch", BLOCK_EN,
		   PGAL_ZCD_EN, PGAR_ZCD_EN, 1, 0),
};

static const struct snd_soc_dapm_widget a1_t9015_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("Right IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("Left IN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX("Right DAC Sel", SND_SOC_NOPM, 0, 0,
			 &t9015_right_dac_mux),
	SND_SOC_DAPM_MUX("Left DAC Sel", SND_SOC_NOPM, 0, 0,
			 &t9015_left_dac_mux),
	SND_SOC_DAPM_DAC("Right DAC", NULL, BLOCK_EN, DACR_EN, 0),
	SND_SOC_DAPM_DAC("Left DAC",  NULL, BLOCK_EN, DACL_EN, 0),
	SND_SOC_DAPM_MUX("Right+ Driver Sel", SND_SOC_NOPM, 0, 0,
			 &a1_right_driver_mux),
	SND_SOC_DAPM_MUX("Left+ Driver Sel", SND_SOC_NOPM, 0, 0,
			 &a1_left_driver_mux),
	SND_SOC_DAPM_OUT_DRV("Right+ Driver", BLOCK_EN, LORP_EN, 0, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("Left+ Driver",  BLOCK_EN, LOLP_EN, 0, NULL, 0),
	SND_SOC_DAPM_OUTPUT("LORP"),
	SND_SOC_DAPM_OUTPUT("LOLP"),

	SND_SOC_DAPM_INPUT("ADC IN Right"),
	SND_SOC_DAPM_INPUT("ADC IN Left"),
	SND_SOC_DAPM_MUX("ADC PGA Right Sel", SND_SOC_NOPM, 0, 0,
			 &a1_adc_pga_right_mux),
	SND_SOC_DAPM_MUX("ADC PGA Left Sel", SND_SOC_NOPM, 0, 0,
			 &a1_adc_pga_left_mux),
	SND_SOC_DAPM_PGA("ADC PGA Right", BLOCK_EN, PGAR_EN, 0, NULL, 0),
	SND_SOC_DAPM_PGA("ADC PGA Left", BLOCK_EN, PGAL_EN, 0, NULL, 0),
	SND_SOC_DAPM_ADC("ADC Right", NULL, BLOCK_EN, ADCR_EN, 0),
	SND_SOC_DAPM_ADC("ADC Left", NULL, BLOCK_EN, ADCL_EN, 0),
	SND_SOC_DAPM_MUX("ADC Right Sel", SND_SOC_NOPM, 0, 0, &a1_adc_right_mux),
	SND_SOC_DAPM_MUX("ADC Left Sel", SND_SOC_NOPM, 0, 0, &a1_adc_left_mux),
	SND_SOC_DAPM_AIF_OUT("ADC OUT Right", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("ADC OUT Left", NULL, 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route a1_t9015_dapm_routes[] = {
	{ "Right IN", NULL, "Playback" },
	{ "Left IN", NULL, "Playback" },
	{ "Right DAC Sel", "Right", "Right IN" },
	{ "Right DAC Sel", "Left", "Left IN" },
	{ "Left DAC Sel", "Right", "Right IN" },
	{ "Left DAC Sel", "Left", "Left IN" },
	{ "Right DAC", NULL, "Right DAC Sel" },
	{ "Left DAC", NULL, "Left DAC Sel" },
	{ "Right+ Driver Sel", "Right DAC", "Right DAC" },
	{ "Right+ Driver Sel", "Left DAC Inverted", "Right DAC" },
	{ "Left+ Driver Sel", "Left DAC", "Left DAC" },
	{ "Left+ Driver Sel", "Right DAC Inverted", "Left DAC" },
	{ "Right+ Driver", NULL, "Right+ Driver Sel" },
	{ "Left+ Driver", NULL, "Left+ Driver Sel" },
	{ "LORP", NULL, "Right+ Driver", },
	{ "LOLP", NULL, "Left+ Driver", },

	{ "ADC PGA Right Sel", "Differential", "ADC IN Right" },
	{ "ADC PGA Right Sel", "Positive", "ADC IN Right" },
	{ "ADC PGA Right Sel", "Negative", "ADC IN Right" },
	{ "ADC PGA Left Sel", "Differential", "ADC IN Left" },
	{ "ADC PGA Left Sel", "Positive", "ADC IN Left" },
	{ "ADC PGA Left Sel", "Negative", "ADC IN Left" },
	{ "ADC PGA Right", NULL, "ADC PGA Right Sel" },
	{ "ADC PGA Left", NULL, "ADC PGA Left Sel" },
	{ "ADC Right", NULL, "ADC PGA Right" },
	{ "ADC Left", NULL, "ADC PGA Left" },
	{ "ADC Right Sel", "Right", "ADC Right" },
	{ "ADC Right Sel", "Left", "ADC Left" },
	{ "ADC Left Sel", "Right", "ADC Right" },
	{ "ADC Left Sel", "Left", "ADC Left" },
	{ "ADC OUT Right", NULL, "ADC Right Sel" },
	{ "ADC OUT Left", NULL, "ADC Left Sel" },
	{ "Capture", NULL, "ADC OUT Right" },
	{ "Capture", NULL, "ADC OUT Left" },
};

static int t9015_set_bias_level(struct snd_soc_component *component,
				enum snd_soc_bias_level level)
{
	struct t9015 *priv = snd_soc_component_get_drvdata(component);
	enum snd_soc_bias_level now =
		snd_soc_component_get_bias_level(component);
	int ret;

	switch (level) {
	case SND_SOC_BIAS_ON:
		snd_soc_component_update_bits(component, BLOCK_EN,
					      BIAS_CURRENT_EN,
					      BIAS_CURRENT_EN);
		break;
	case SND_SOC_BIAS_PREPARE:
		snd_soc_component_update_bits(component, BLOCK_EN,
					      BIAS_CURRENT_EN,
					      0);
		break;
	case SND_SOC_BIAS_STANDBY:
		ret = regulator_enable(priv->avdd);
		if (ret) {
			dev_err(component->dev, "AVDD enable failed\n");
			return ret;
		}

		if (now == SND_SOC_BIAS_OFF) {
			snd_soc_component_update_bits(component, BLOCK_EN,
				VMID_GEN_EN | VMID_GEN_FAST | REFP_BUF_EN,
				VMID_GEN_EN | VMID_GEN_FAST | REFP_BUF_EN);

			mdelay(200);
			snd_soc_component_update_bits(component, BLOCK_EN,
						      VMID_GEN_FAST,
						      0);
		}

		break;
	case SND_SOC_BIAS_OFF:
		snd_soc_component_update_bits(component, BLOCK_EN,
			VMID_GEN_EN | VMID_GEN_FAST | REFP_BUF_EN,
			0);

		regulator_disable(priv->avdd);
		break;
	}

	return 0;
}

static int t9015_component_probe(struct snd_soc_component *component)
{
	/*
	 * Initialize output polarity:
	 * ATM the output polarity is fixed but in the future it might useful
	 * to add DT property to set this depending on the platform needs
	 */
	snd_soc_component_write(component, LINEOUT_CFG, 0x1111);

	return 0;
}

static int a1_t9015_component_probe(struct snd_soc_component *component)
{
	/*
	 * This configuration was stealed from original Amlogic's driver to
	 * reproduce the behavior of the driver more accurately. However, it is
	 * not known for certain what it actually affects.
	 */
	snd_soc_component_write(component, POWER_CFG, 0x00010000);

	return 0;
}

static const struct snd_soc_component_driver t9015_codec_driver = {
	.probe			= t9015_component_probe,
	.set_bias_level		= t9015_set_bias_level,
	.controls		= t9015_snd_controls,
	.num_controls		= ARRAY_SIZE(t9015_snd_controls),
	.dapm_widgets		= t9015_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(t9015_dapm_widgets),
	.dapm_routes		= t9015_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(t9015_dapm_routes),
	.suspend_bias_off	= 1,
	.endianness		= 1,
};

static const struct snd_soc_component_driver a1_t9015_codec_driver = {
	.probe			= a1_t9015_component_probe,
	.set_bias_level		= t9015_set_bias_level,
	.controls		= a1_t9015_snd_controls,
	.num_controls		= ARRAY_SIZE(a1_t9015_snd_controls),
	.dapm_widgets		= a1_t9015_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(a1_t9015_dapm_widgets),
	.dapm_routes		= a1_t9015_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(a1_t9015_dapm_routes),
	.suspend_bias_off	= 1,
	.endianness		= 1,
};

static int t9015_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct t9015_match_data *data;
	struct t9015 *priv;
	void __iomem *regs;
	struct regmap_config config = {
		.reg_bits = 32,
		.reg_stride = 4,
		.val_bits = 32,
	};
	struct regmap *regmap;
	struct clk *pclk;
	int ret;

	data = device_get_match_data(dev);
	if (!data)
		dev_err_probe(dev, -ENODEV, "failed to match device\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	platform_set_drvdata(pdev, priv);

	pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(pclk))
		return dev_err_probe(dev, PTR_ERR(pclk), "failed to get core clock\n");

	priv->avdd = devm_regulator_get(dev, "AVDD");
	if (IS_ERR(priv->avdd))
		return dev_err_probe(dev, PTR_ERR(priv->avdd), "failed to AVDD\n");

	ret = device_reset(dev);
	if (ret) {
		dev_err(dev, "reset failed\n");
		return ret;
	}

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs)) {
		dev_err(dev, "register map failed\n");
		return PTR_ERR(regs);
	}

	config.max_register = data->max_register;
	regmap = devm_regmap_init_mmio(dev, regs, &config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "regmap init failed\n");
		return PTR_ERR(regmap);
	}

	return devm_snd_soc_register_component(dev, data->component_drv,
					       data->dai_drv, 1);
}

static const struct t9015_match_data t9015_match_data = {
	.component_drv = &t9015_codec_driver,
	.dai_drv = &t9015_dai,
	.max_register = POWER_CFG,
};

static const struct t9015_match_data a1_t9015_match_data = {
	.component_drv = &a1_t9015_codec_driver,
	.dai_drv = &a1_t9015_dai,
	.max_register = LINEIN_CFG,
};

static const struct of_device_id t9015_ids[] __maybe_unused = {
	{
		.compatible = "amlogic,t9015",
		.data = &t9015_match_data,
	},
	{
		.compatible = "amlogic,t9015-a1",
		.data = &a1_t9015_match_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, t9015_ids);

static struct platform_driver t9015_driver = {
	.driver = {
		.name = "t9015-codec",
		.of_match_table = of_match_ptr(t9015_ids),
	},
	.probe = t9015_probe,
};

module_platform_driver(t9015_driver);

MODULE_DESCRIPTION("ASoC Amlogic T9015 codec driver");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL");
