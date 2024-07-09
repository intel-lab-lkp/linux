// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the NTP8835/NTP8835C Audio Amplifiers
 *
 * Copyright (c) 2024, SaluteDevices. All Rights Reserved.
 *
 * Author: Igor Prusov <ivprusov@salutedevices.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/bits.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/reset.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>

#include <sound/initval.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-component.h>
#include <sound/tlv.h>

#include "ntpfw.h"

#define NTP8835_FORMATS     (SNDRV_PCM_FMTBIT_S16_LE | \
			     SNDRV_PCM_FMTBIT_S20_3LE | \
			     SNDRV_PCM_FMTBIT_S24_LE | \
			     SNDRV_PCM_FMTBIT_S32_LE)

#define NTP8835_INPUT_FMT			0x0
#define  NTP8835_INPUT_FMT_MASTER_MODE		BIT(0)
#define  NTP8835_INPUT_FMT_GSA_MODE		BIT(1)
#define NTP8835_GSA_FMT				0x1
#define  NTP8835_GSA_BS_MASK			GENMASK(3, 2)
#define  NTP8835_GSA_BS(x)			((x) << 2)
#define  NTP8835_GSA_RIGHT_J			BIT(0)
#define  NTP8835_GSA_LSB			BIT(1)
#define NTP8835_SOFT_MUTE			0x26
#define  NTP8835_SOFT_MUTE_SM1			BIT(0)
#define  NTP8835_SOFT_MUTE_SM2			BIT(1)
#define  NTP8835_SOFT_MUTE_SM3			BIT(2)
#define NTP8835_PWM_SWITCH			0x27
#define  NTP8835_PWM_SWITCH_POF1		BIT(0)
#define  NTP8835_PWM_SWITCH_POF2		BIT(1)
#define  NTP8835_PWM_SWITCH_POF3		BIT(2)
#define NTP8835_PWM_MASK_CTRL0			0x28
#define  NTP8835_PWM_MASK_CTRL0_OUT_LOW		BIT(1)
#define  NTP8835_PWM_MASK_CTRL0_FPMLD		BIT(2)
#define NTP8835_MASTER_VOL			0x2e
#define NTP8835_CHNL_A_VOL			0x2f
#define NTP8835_CHNL_B_VOL			0x30
#define NTP8835_CHNL_C_VOL			0x31
#define REG_MAX					NTP8835_CHNL_C_VOL

#define NTP8835_FW_NAME				"eq_8835.bin"
#define NTP8835_FW_MAGIC			0x38383335	/* "8835" */

struct ntp8835_priv {
	struct i2c_client *i2c;
	struct reset_control *reset;
	unsigned int format;
};

static const DECLARE_TLV_DB_RANGE(ntp8835_vol_scale,
	0, 1, TLV_DB_SCALE_ITEM(-15000, 0, 0),
	2, 6, TLV_DB_SCALE_ITEM(-15000, 1000, 0),
	7, 0xff, TLV_DB_SCALE_ITEM(-10000, 50, 0),
);

static int ntp8835_mute_info(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;

	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	uinfo->value.integer.step = 1;

	return 0;
}

static int ntp8835_mute_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	unsigned int val;

	val = snd_soc_component_read(component, NTP8835_SOFT_MUTE);

	ucontrol->value.integer.value[0] = val ? 0 : 1;
	return 0;
}

static int ntp8835_mute_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	unsigned int val;

	val = ucontrol->value.integer.value[0] ? 0 : 7;

	snd_soc_component_write(component, NTP8835_SOFT_MUTE, val);

	return 0;
}

static const struct snd_kcontrol_new ntp8835_vol_control[] = {
	SOC_SINGLE_TLV("Playback Volume", NTP8835_MASTER_VOL, 0,
		       0xff, 0, ntp8835_vol_scale),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Playback Switch",
		.info = ntp8835_mute_info,
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.get = ntp8835_mute_get,
		.put = ntp8835_mute_put,
	},
};

static void ntp8835_reset_gpio(struct ntp8835_priv *ntp8835, bool active)
{
	if (active) {
		/*
		 * According to NTP8835 datasheet, 6.2 Timing Sequence (recommended):
		 * Deassert for T2 >= 1ms...
		 */
		reset_control_deassert(ntp8835->reset);
		fsleep(1000);

		/* ...Assert for T3 >= 0.1us... */
		reset_control_assert(ntp8835->reset);
		fsleep(1);

		/* ...Deassert, and wait for T4 >= 0.5ms before sound on sequence. */
		reset_control_deassert(ntp8835->reset);
		fsleep(500);
	} else {
		reset_control_assert(ntp8835->reset);
	}
}

static const struct reg_sequence ntp8835_sound_on[] = {
	{ NTP8835_PWM_MASK_CTRL0,	NTP8835_PWM_MASK_CTRL0_FPMLD },
	{ NTP8835_PWM_SWITCH,		0x00 },
	{ NTP8835_SOFT_MUTE,		0x00 },
};

static const struct reg_sequence ntp8835_sound_off[] = {
	{ NTP8835_SOFT_MUTE,		NTP8835_SOFT_MUTE_SM1 |
					NTP8835_SOFT_MUTE_SM2 |
					NTP8835_SOFT_MUTE_SM3 },

	{ NTP8835_PWM_SWITCH,		NTP8835_PWM_SWITCH_POF1 |
					NTP8835_PWM_SWITCH_POF2 |
					NTP8835_PWM_SWITCH_POF3 },

	{ NTP8835_PWM_MASK_CTRL0,	NTP8835_PWM_MASK_CTRL0_OUT_LOW |
					NTP8835_PWM_MASK_CTRL0_FPMLD },
};

static int ntp8835_load_firmware(struct ntp8835_priv *ntp8835)
{
	int ret;

	ret = ntpfw_load(ntp8835->i2c, NTP8835_FW_NAME, NTP8835_FW_MAGIC);
	if (ret == -ENOENT) {
		dev_warn_once(&ntp8835->i2c->dev,
			      "Could not find firmware %s\n", NTP8835_FW_NAME);
		return 0;
	}

	return ret;
}

static int ntp8835_snd_suspend(struct snd_soc_component *component)
{
	struct ntp8835_priv *ntp8835 = snd_soc_component_get_drvdata(component);

	regcache_cache_only(component->regmap, true);

	regmap_multi_reg_write_bypassed(component->regmap,
					ntp8835_sound_off,
					ARRAY_SIZE(ntp8835_sound_off));

	/*
	 * According to NTP8835 datasheet, 6.2 Timing Sequence (recommended):
	 * wait after sound off for T6 >= 0.5ms
	 */
	fsleep(500);
	ntp8835_reset_gpio(ntp8835, false);

	regcache_mark_dirty(component->regmap);

	return 0;
}

static int ntp8835_snd_resume(struct snd_soc_component *component)
{
	struct ntp8835_priv *ntp8835 = snd_soc_component_get_drvdata(component);
	int ret;

	ntp8835_reset_gpio(ntp8835, true);

	regmap_multi_reg_write_bypassed(component->regmap,
					ntp8835_sound_on,
					ARRAY_SIZE(ntp8835_sound_on));

	ret = ntp8835_load_firmware(ntp8835);
	if (ret) {
		dev_err(&ntp8835->i2c->dev, "Failed to load firmware\n");
		return ret;
	}

	regcache_cache_only(component->regmap, false);
	snd_soc_component_cache_sync(component);

	return 0;
}

static int ntp8835_probe(struct snd_soc_component *component)
{
	int ret;
	struct ntp8835_priv *ntp8835 = snd_soc_component_get_drvdata(component);
	struct device *dev = component->dev;

	ret = snd_soc_add_component_controls(component, ntp8835_vol_control,
					     ARRAY_SIZE(ntp8835_vol_control));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add controls\n");

	ret = ntp8835_load_firmware(ntp8835);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to load firmware\n");

	return 0;
}

static const struct snd_soc_dapm_widget ntp8835_dapm_widgets[] = {
	SND_SOC_DAPM_DAC("AIFIN", "Playback", SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_OUTPUT("OUT1"),
	SND_SOC_DAPM_OUTPUT("OUT2"),
	SND_SOC_DAPM_OUTPUT("OUT3"),
};

static const struct snd_soc_dapm_route ntp8835_dapm_routes[] = {
	{ "OUT1", NULL, "AIFIN" },
	{ "OUT2", NULL, "AIFIN" },
	{ "OUT3", NULL, "AIFIN" },
};

static const struct snd_soc_component_driver soc_component_ntp8835 = {
	.probe = ntp8835_probe,
	.suspend = ntp8835_snd_suspend,
	.resume = ntp8835_snd_resume,
	.dapm_widgets = ntp8835_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(ntp8835_dapm_widgets),
	.dapm_routes = ntp8835_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(ntp8835_dapm_routes),
};

static int ntp8835_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ntp8835_priv *ntp8835 = snd_soc_component_get_drvdata(component);
	unsigned int input_fmt = 0;
	unsigned int gsa_fmt = 0;
	unsigned int gsa_fmt_mask;
	int ret;

	switch (ntp8835->format) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		input_fmt |= NTP8835_INPUT_FMT_GSA_MODE;
		gsa_fmt |= NTP8835_GSA_RIGHT_J;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		input_fmt |= NTP8835_INPUT_FMT_GSA_MODE;
		break;
	}

	ret = snd_soc_component_update_bits(component, NTP8835_INPUT_FMT,
					    NTP8835_INPUT_FMT_MASTER_MODE |
					    NTP8835_INPUT_FMT_GSA_MODE,
					    input_fmt);

	if (!(input_fmt & NTP8835_INPUT_FMT_GSA_MODE) || ret < 0)
		return ret;

	switch (params_width(params)) {
	case 24:
		gsa_fmt |= NTP8835_GSA_BS(0);
		break;
	case 20:
		gsa_fmt |= NTP8835_GSA_BS(1);
		break;
	case 18:
		gsa_fmt |= NTP8835_GSA_BS(2);
		break;
	case 16:
		gsa_fmt |= NTP8835_GSA_BS(3);
		break;
	default:
		return -EINVAL;
	}

	gsa_fmt_mask = NTP8835_GSA_BS_MASK |
		       NTP8835_GSA_RIGHT_J |
		       NTP8835_GSA_LSB;
	return snd_soc_component_update_bits(component, NTP8835_GSA_FMT,
					     gsa_fmt_mask, gsa_fmt);
}

static int ntp8835_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct ntp8835_priv *ntp8835 = snd_soc_component_get_drvdata(component);

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
		ntp8835->format = fmt & SND_SOC_DAIFMT_FORMAT_MASK;
		break;
	default:
		return -EINVAL;
	}
	return 0;
};

static const struct snd_soc_dai_ops ntp8835_dai_ops = {
	.hw_params = ntp8835_hw_params,
	.set_fmt = ntp8835_set_fmt,
};

static struct snd_soc_dai_driver ntp8835_dai = {
	.name = "ntp8835-amplifier",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 3,
		.rates = SNDRV_PCM_RATE_8000_192000,
		.formats = NTP8835_FORMATS,
	},
	.ops = &ntp8835_dai_ops,
};

static struct regmap_config ntp8835_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_MAX,
	.cache_type = REGCACHE_MAPLE,
};

static int ntp8835_i2c_probe(struct i2c_client *i2c)
{
	struct ntp8835_priv *ntp8835;
	struct regmap *regmap;
	int ret;

	ntp8835 = devm_kzalloc(&i2c->dev, sizeof(struct ntp8835_priv), GFP_KERNEL);
	if (!ntp8835)
		return -ENOMEM;

	ntp8835->i2c = i2c;

	ntp8835->reset = devm_reset_control_get_shared(&i2c->dev, NULL);
	if (IS_ERR(ntp8835->reset))
		return dev_err_probe(&i2c->dev, PTR_ERR(ntp8835->reset),
				     "Failed to get reset\n");

	ret = reset_control_deassert(ntp8835->reset);
	if (ret)
		return dev_err_probe(&i2c->dev, PTR_ERR(ntp8835->reset),
				     "Failed to deassert reset\n");

	dev_set_drvdata(&i2c->dev, ntp8835);

	ntp8835_reset_gpio(ntp8835, true);

	regmap = devm_regmap_init_i2c(i2c, &ntp8835_regmap);
	if (IS_ERR(regmap))
		return dev_err_probe(&i2c->dev, PTR_ERR(regmap),
				     "Failed to allocate regmap\n");

	ret = devm_snd_soc_register_component(&i2c->dev, &soc_component_ntp8835,
					      &ntp8835_dai, 1);
	if (ret)
		return dev_err_probe(&i2c->dev, ret,
				     "Failed to register component\n");

	return 0;
}

static const struct i2c_device_id ntp8835_i2c_id[] = {
	{ "ntp8835", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, ntp8835_i2c_id);

static const struct of_device_id ntp8835_of_match[] = {
	{.compatible = "neofidelity,ntp8835",},
	{.compatible = "neofidelity,ntp8835c",},
	{}
};
MODULE_DEVICE_TABLE(of, ntp8835_of_match);

static struct i2c_driver ntp8835_i2c_driver = {
	.probe = ntp8835_i2c_probe,
	.id_table = ntp8835_i2c_id,
	.driver = {
		.name = "NTP8835",
		.of_match_table = ntp8835_of_match,
	},
};
module_i2c_driver(ntp8835_i2c_driver);

MODULE_AUTHOR("Igor Prusov <ivprusov@salutedevices.com>");
MODULE_DESCRIPTION("NTP8835 Audio Amplifier Driver");
MODULE_LICENSE("GPL");
