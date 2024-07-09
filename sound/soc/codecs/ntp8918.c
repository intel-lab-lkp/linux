// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the NTP8918 Audio Amplifier
 *
 * Copyright (c) 2024, SaluteDevices. All Rights Reserved.
 *
 * Author: Igor Prusov <ivprusov@salutedevices.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/reset.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>

#include <sound/initval.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-component.h>
#include <sound/tlv.h>

#include "ntpfw.h"

#define NTP8918_RATES   (SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | \
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000)

#define NTP8918_FORMATS     (SNDRV_PCM_FMTBIT_S16_LE | \
			     SNDRV_PCM_FMTBIT_S20_3LE | \
			     SNDRV_PCM_FMTBIT_S24_LE | \
			     SNDRV_PCM_FMTBIT_S32_LE)

#define NTP8918_INPUT_FMT			0x0
#define  NTP8918_INPUT_FMT_MASTER_MODE		BIT(0)
#define  NTP8918_INPUT_FMT_GSA_MODE		BIT(1)
#define NTP8918_GSA_FMT				0x1
#define  NTP8918_GSA_BS_MASK			GENMASK(3, 2)
#define  NTP8918_GSA_BS(x)			((x) << 2)
#define  NTP8918_GSA_RIGHT_J			BIT(0)
#define  NTP8918_GSA_LSB			BIT(1)
#define NTP8918_MASTER_VOL			0x0C
#define NTP8918_CHNL_A_VOL			0x17
#define NTP8918_CHNL_B_VOL			0x18
#define NTP8918_SOFT_MUTE			0x33
#define REG_MAX					NTP8918_SOFT_MUTE

#define NTP8918_FW_NAME		"eq_8918.bin"
#define NTP8918_FW_MAGIC	0x38393138	/* "8918" */

struct ntp8918_priv {
	struct i2c_client *i2c;
	struct reset_control *reset;
	unsigned int format;
};

static const DECLARE_TLV_DB_SCALE(ntp8918_master_vol_scale, -12550, 50, 0);

static const struct snd_kcontrol_new ntp8918_vol_control[] = {
	SOC_SINGLE_RANGE_TLV("Playback Volume", NTP8918_MASTER_VOL, 0,
			     0x04, 0xff, 0, ntp8918_master_vol_scale),
	SOC_DOUBLE("Playback Switch", NTP8918_SOFT_MUTE, 1, 0, 1, 1),
};

static void ntp8918_reset_gpio(struct ntp8918_priv *ntp8918, bool active)
{
	if (active) {
		/*
		 * According to NTP8918 datasheet, 6.2 Timing Sequence 1:
		 * Deassert for T2 >= 1ms...
		 */
		reset_control_deassert(ntp8918->reset);
		fsleep(1000);

		/* ...Assert for T3 >= 0.1us... */
		reset_control_assert(ntp8918->reset);
		fsleep(1);

		/* ...Deassert, and wait for T4 >= 0.5ms before sound on sequence. */
		reset_control_deassert(ntp8918->reset);
		fsleep(500);
	} else {
		reset_control_assert(ntp8918->reset);
	}
}

static const struct reg_sequence ntp8918_sound_off[] = {
	{ NTP8918_MASTER_VOL, 0 },
};

static const struct reg_sequence ntp8918_sound_on[] = {
	{ NTP8918_MASTER_VOL, 0b11 },
};

static int ntp8918_load_firmware(struct ntp8918_priv *ntp8918)
{
	int ret;

	ret = ntpfw_load(ntp8918->i2c, NTP8918_FW_NAME, NTP8918_FW_MAGIC);
	if (ret == -ENOENT) {
		dev_warn_once(&ntp8918->i2c->dev, "Could not find firmware %s\n",
			      NTP8918_FW_NAME);
		return 0;
	}

	return ret;
}

static int ntp8918_snd_suspend(struct snd_soc_component *component)
{
	struct ntp8918_priv *ntp8918 = snd_soc_component_get_drvdata(component);

	regcache_cache_only(component->regmap, true);

	regmap_multi_reg_write_bypassed(component->regmap,
					ntp8918_sound_off,
					ARRAY_SIZE(ntp8918_sound_off));

	/*
	 * According to NTP8918 datasheet, 6.2 Timing Sequence 1:
	 * wait after sound off for T6 >= 0.5ms
	 */
	fsleep(500);
	ntp8918_reset_gpio(ntp8918, false);

	regcache_mark_dirty(component->regmap);

	return 0;
}

static int ntp8918_snd_resume(struct snd_soc_component *component)
{
	struct ntp8918_priv *ntp8918 = snd_soc_component_get_drvdata(component);
	int ret;

	ntp8918_reset_gpio(ntp8918, true);

	regmap_multi_reg_write_bypassed(component->regmap,
					ntp8918_sound_on,
					ARRAY_SIZE(ntp8918_sound_on));

	ret = ntp8918_load_firmware(ntp8918);
	if (ret) {
		dev_err(&ntp8918->i2c->dev, "Failed to load firmware\n");
		return ret;
	}

	regcache_cache_only(component->regmap, false);
	snd_soc_component_cache_sync(component);

	return 0;
}

static int ntp8918_probe(struct snd_soc_component *component)
{
	int ret;
	struct ntp8918_priv *ntp8918 = snd_soc_component_get_drvdata(component);
	struct device *dev = component->dev;

	ret = snd_soc_add_component_controls(component, ntp8918_vol_control,
					     ARRAY_SIZE(ntp8918_vol_control));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add controls\n");

	ret = ntp8918_load_firmware(ntp8918);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to load firmware\n");

	return 0;
}

static const struct snd_soc_dapm_widget ntp8918_dapm_widgets[] = {
	SND_SOC_DAPM_DAC("AIFIN", "Playback", SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_OUTPUT("OUT1"),
	SND_SOC_DAPM_OUTPUT("OUT2"),
};

static const struct snd_soc_dapm_route ntp8918_dapm_routes[] = {
	{ "OUT1", NULL, "AIFIN" },
	{ "OUT2", NULL, "AIFIN" },
};

static const struct snd_soc_component_driver soc_component_ntp8918 = {
	.probe = ntp8918_probe,
	.suspend = ntp8918_snd_suspend,
	.resume = ntp8918_snd_resume,
	.dapm_widgets = ntp8918_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(ntp8918_dapm_widgets),
	.dapm_routes = ntp8918_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(ntp8918_dapm_routes),
};

static int ntp8918_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ntp8918_priv *ntp8918 = snd_soc_component_get_drvdata(component);
	unsigned int input_fmt = 0;
	unsigned int gsa_fmt = 0;
	unsigned int gsa_fmt_mask;
	int ret;

	switch (ntp8918->format) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		input_fmt |= NTP8918_INPUT_FMT_GSA_MODE;
		gsa_fmt |= NTP8918_GSA_RIGHT_J;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		input_fmt |= NTP8918_INPUT_FMT_GSA_MODE;
		break;
	}

	ret = snd_soc_component_update_bits(component, NTP8918_INPUT_FMT,
					    NTP8918_INPUT_FMT_MASTER_MODE |
					    NTP8918_INPUT_FMT_GSA_MODE,
					    input_fmt);

	if (!(input_fmt & NTP8918_INPUT_FMT_GSA_MODE) || ret < 0)
		return ret;

	switch (params_width(params)) {
	case 24:
		gsa_fmt |= NTP8918_GSA_BS(0);
		break;
	case 20:
		gsa_fmt |= NTP8918_GSA_BS(1);
		break;
	case 18:
		gsa_fmt |= NTP8918_GSA_BS(2);
		break;
	case 16:
		gsa_fmt |= NTP8918_GSA_BS(3);
		break;
	default:
		return -EINVAL;
	}

	gsa_fmt_mask = NTP8918_GSA_BS_MASK |
		       NTP8918_GSA_RIGHT_J |
		       NTP8918_GSA_LSB;
	return snd_soc_component_update_bits(component, NTP8918_GSA_FMT,
					     gsa_fmt_mask, gsa_fmt);
}

static int ntp8918_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct ntp8918_priv *ntp8918 = snd_soc_component_get_drvdata(component);

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
		ntp8918->format = fmt & SND_SOC_DAIFMT_FORMAT_MASK;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

struct snd_soc_dai_ops ntp8918_dai_ops = {
	.hw_params = ntp8918_hw_params,
	.set_fmt = ntp8918_set_fmt,
};

static struct snd_soc_dai_driver ntp8918_dai = {
	.name = "ntp8918-amplifier",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = NTP8918_RATES,
		.formats = NTP8918_FORMATS,
	},
	.ops = &ntp8918_dai_ops,
};

static struct regmap_config ntp8918_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_MAX,
	.cache_type = REGCACHE_MAPLE,
};

static int ntp8918_i2c_probe(struct i2c_client *i2c)
{
	struct ntp8918_priv *ntp8918;
	int ret;
	struct regmap *regmap;

	ntp8918 = devm_kzalloc(&i2c->dev, sizeof(struct ntp8918_priv), GFP_KERNEL);
	if (!ntp8918)
		return -ENOMEM;

	ntp8918->i2c = i2c;

	ntp8918->reset = devm_reset_control_get_shared(&i2c->dev, NULL);
	if (IS_ERR(ntp8918->reset))
		return dev_err_probe(&i2c->dev, PTR_ERR(ntp8918->reset), "Failed to get reset\n");

	dev_set_drvdata(&i2c->dev, ntp8918);

	ntp8918_reset_gpio(ntp8918, true);

	regmap = devm_regmap_init_i2c(i2c, &ntp8918_regmap);
	if (IS_ERR(regmap))
		return dev_err_probe(&i2c->dev, PTR_ERR(regmap),
				     "Failed to allocate regmap\n");

	ret = devm_snd_soc_register_component(&i2c->dev, &soc_component_ntp8918,
					      &ntp8918_dai, 1);
	if (ret)
		return dev_err_probe(&i2c->dev, ret,
				     "Failed to register component\n");

	return 0;
}

static const struct i2c_device_id ntp8918_i2c_id[] = {
	{ "ntp8918", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, ntp8918_i2c_id);

static const struct of_device_id ntp8918_of_match[] = {
	{.compatible = "neofidelity,ntp8918"},
	{}
};
MODULE_DEVICE_TABLE(of, ntp8918_of_match);

static struct i2c_driver ntp8918_i2c_driver = {
	.probe = ntp8918_i2c_probe,
	.id_table = ntp8918_i2c_id,
	.driver = {
		.name = "NTP8918",
		.of_match_table = ntp8918_of_match,
	},
};
module_i2c_driver(ntp8918_i2c_driver);

MODULE_AUTHOR("Igor Prusov <ivprusov@salutedevices.com>");
MODULE_DESCRIPTION("NTP8918 Audio Amplifier Driver");
MODULE_LICENSE("GPL");
