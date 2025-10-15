// SPDX-License-Identifier: GPL-2.0-only
/*
 * rt5575.c  --  ALC5575 ALSA SoC audio component driver
 *
 * Copyright 2022 Realtek Semiconductor Corp.
 * Author: Oder Chiou <oder_chiou@realtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/firmware.h>
#include <linux/property.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "rt5575.h"
#include "rt5575-spi.h"

static bool rt5575_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case RT5575_ID:
	case RT5575_ID_1:
	case RT5575_MIXL_VOL:
	case RT5575_MIXR_VOL:
	case RT5575_PROMPT_VOL:
	case RT5575_SPK01_VOL:
	case RT5575_SPK23_VOL:
	case RT5575_MIC1_VOL:
	case RT5575_MIC2_VOL:
	case RT5575_WNC_CTRL:
	case RT5575_MODE_CTRL:
	case RT5575_I2S_RATE_CTRL:
	case RT5575_SLEEP_CTRL:
	case RT5575_ALG_BYPASS_CTRL:
	case RT5575_PINMUX_CTRL_2:
	case RT5575_GPIO_CTRL_1:
	case RT5575_DSP_BUS_CTRL:
	case RT5575_SW_INT:
	case RT5575_DSP_BOOT_ERR:
	case RT5575_DSP_READY:
	case RT5575_DSP_CMD_ADDR:
	case RT5575_EFUSE_DATA_2:
	case RT5575_EFUSE_DATA_3:
		return true;
	default:
		return false;
	}
}

static const DECLARE_TLV_DB_SCALE(ob_tlv, -9525, 75, 0);

#if IS_ENABLED(CONFIG_SND_SOC_RT5575_SPI)
static int rt5575_spi_fw_run_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int rt5575_spi_fw_run_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct rt5575_priv *rt5575 = snd_soc_component_get_drvdata(component);
	const struct firmware *firmware;
	int i, ret;

	if (!ucontrol->value.bytes.data[0])
		return 0;

	regmap_write(rt5575->dsp_regmap, 0xfafafafa, 0x00000004);
	regmap_write(rt5575->dsp_regmap, 0x18008064, 0x00000000);
	regmap_write(rt5575->dsp_regmap, 0x18008068, 0x0002ffff);

	ret = request_firmware(&firmware, "realtek/rt5575/0x70000000.dat",
		component->dev);
	if (ret == 0) {
		rt5575_spi_burst_write(0x5f400000, firmware->data,
			firmware->size);
		release_firmware(firmware);
	}

	ret = request_firmware(&firmware, "realtek/rt5575/0x70200000.dat",
		component->dev);
	if (ret == 0) {
		rt5575_spi_burst_write(0x5f600000, firmware->data,
			firmware->size);
		release_firmware(firmware);
	}

	ret = request_firmware(&firmware, "realtek/rt5575/0x703fe000.dat",
		component->dev);
	if (ret == 0) {
		rt5575_spi_burst_write(0x5f7fe000, firmware->data,
			firmware->size);
		release_firmware(firmware);
	}

	ret = request_firmware(&firmware, "realtek/rt5575/0x703ff000.dat",
		component->dev);
	if (ret == 0) {
		rt5575_spi_burst_write(0x5f7ff000, firmware->data,
			firmware->size);
		release_firmware(firmware);
	}

	regmap_write(rt5575->dsp_regmap, 0x18000000, 0x00000000);

	regmap_update_bits(rt5575->regmap, RT5575_SW_INT, 1, 1);

	ret = 1;
	for (i = 0; i < 100 && ret; i++) {
		regmap_read(rt5575->regmap, RT5575_SW_INT, &ret);
		msleep(100);
	}

	if (ret) {
		dev_err(component->dev, "Firmware failure\n");
		return -EINVAL;
	}

	return 0;
}
#endif

static const struct snd_kcontrol_new rt5575_snd_controls[] = {
	SOC_DOUBLE("Speaker01 Playback Switch", RT5575_SPK01_VOL, 31, 15, 1, 1),
	SOC_DOUBLE_TLV("Speaker01 Playback Volume", RT5575_SPK01_VOL, 17, 1,
		167, 0, ob_tlv),
	SOC_DOUBLE("Speaker23 Playback Switch", RT5575_SPK23_VOL, 31, 15, 1, 1),
	SOC_DOUBLE_TLV("Speaker23 Playback Volume", RT5575_SPK23_VOL, 17, 1,
		167, 0, ob_tlv),
	SOC_DOUBLE("Mic1 Capture Switch", RT5575_MIC1_VOL, 31, 15, 1, 1),
	SOC_DOUBLE_TLV("Mic1 Capture Volume", RT5575_MIC1_VOL, 17, 1, 167,
		0, ob_tlv),
	SOC_DOUBLE("Mic2 Capture Switch", RT5575_MIC2_VOL, 31, 15, 1, 1),
	SOC_DOUBLE_TLV("Mic2 Capture Volume", RT5575_MIC2_VOL, 17, 1, 167,
		0, ob_tlv),
	SOC_DOUBLE_R("Mix Playback Switch", RT5575_MIXL_VOL, RT5575_MIXR_VOL,
		31, 1, 1),
	SOC_DOUBLE_R_TLV("Mix Playback Volume", RT5575_MIXL_VOL,
		RT5575_MIXR_VOL, 1, 127, 0, ob_tlv),
	SOC_DOUBLE("Prompt Playback Switch", RT5575_PROMPT_VOL, 31, 15, 1, 1),
	SOC_DOUBLE_TLV("Prompt Playback Volume", RT5575_PROMPT_VOL, 17, 1, 167,
		0, ob_tlv),
#if IS_ENABLED(CONFIG_SND_SOC_RT5575_SPI)
	SND_SOC_BYTES_EXT("SPI FW run", 1, rt5575_spi_fw_run_get,
		rt5575_spi_fw_run_put),
#endif
};

static const struct snd_soc_dapm_widget rt5575_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("AIF1RX", "AIF1 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF1TX", "AIF1 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIF2RX", "AIF2 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF2TX", "AIF2 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIF3RX", "AIF3 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF3TX", "AIF3 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIF4RX", "AIF4 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF4TX", "AIF4 Capture", 0, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_INPUT("INPUT"),
	SND_SOC_DAPM_OUTPUT("OUTPUT"),
};

static const struct snd_soc_dapm_route rt5575_dapm_routes[] = {
	{ "AIF1TX", NULL, "INPUT" },
	{ "AIF2TX", NULL, "INPUT" },
	{ "AIF3TX", NULL, "INPUT" },
	{ "AIF4TX", NULL, "INPUT" },
	{ "OUTPUT", NULL, "AIF1RX" },
	{ "OUTPUT", NULL, "AIF2RX" },
	{ "OUTPUT", NULL, "AIF3RX" },
	{ "OUTPUT", NULL, "AIF4RX" },
};

static long long rt5575_getuuid(struct rt5575_priv *rt5575)
{
	int efuse_uuid_low, efuse_uuid_high;

	regmap_write(rt5575->regmap, RT5575_EFUSE_PID, 0xa0000000);
	regmap_read(rt5575->regmap, RT5575_EFUSE_DATA_2, &efuse_uuid_low);
	regmap_read(rt5575->regmap, RT5575_EFUSE_DATA_3, &efuse_uuid_high);
	regmap_write(rt5575->regmap, RT5575_EFUSE_PID, 0);

	return ((long long)efuse_uuid_high << 32) | (long long)efuse_uuid_low;
}

static int rt5575_probe(struct snd_soc_component *component)
{
	struct rt5575_priv *rt5575 = snd_soc_component_get_drvdata(component);

	rt5575->component = component;

	dev_info(component->dev, "UUID: %llx\n", rt5575_getuuid(rt5575));

	return 0;
}

#define RT5575_STEREO_RATES SNDRV_PCM_RATE_8000_192000
#define RT5575_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S8 | \
			SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver rt5575_dai[] = {
	{
		.name = "rt5575-aif1",
		.id = RT5575_AIF1,
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 8,
			.rates = RT5575_STEREO_RATES,
			.formats = RT5575_FORMATS,
		},
		.capture = {
			.stream_name = "AIF1 Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = RT5575_STEREO_RATES,
			.formats = RT5575_FORMATS,
		},
	},
	{
		.name = "rt5575-aif2",
		.id = RT5575_AIF2,
		.playback = {
			.stream_name = "AIF2 Playback",
			.channels_min = 1,
			.channels_max = 8,
			.rates = RT5575_STEREO_RATES,
			.formats = RT5575_FORMATS,
		},
		.capture = {
			.stream_name = "AIF2 Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = RT5575_STEREO_RATES,
			.formats = RT5575_FORMATS,
		},
	},
	{
		.name = "rt5575-aif3",
		.id = RT5575_AIF3,
		.playback = {
			.stream_name = "AIF3 Playback",
			.channels_min = 1,
			.channels_max = 8,
			.rates = RT5575_STEREO_RATES,
			.formats = RT5575_FORMATS,
		},
		.capture = {
			.stream_name = "AIF3 Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = RT5575_STEREO_RATES,
			.formats = RT5575_FORMATS,
		},
	},
	{
		.name = "rt5575-aif4",
		.id = RT5575_AIF4,
		.playback = {
			.stream_name = "AIF4 Playback",
			.channels_min = 1,
			.channels_max = 8,
			.rates = RT5575_STEREO_RATES,
			.formats = RT5575_FORMATS,
		},
		.capture = {
			.stream_name = "AIF4 Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = RT5575_STEREO_RATES,
			.formats = RT5575_FORMATS,
		},
	},
};

const struct snd_soc_component_driver rt5575_soc_component_dev = {
	.probe = rt5575_probe,
	.controls = rt5575_snd_controls,
	.num_controls = ARRAY_SIZE(rt5575_snd_controls),
	.dapm_widgets = rt5575_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(rt5575_dapm_widgets),
	.dapm_routes = rt5575_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(rt5575_dapm_routes),
	.use_pmdown_time = 1,
	.endianness = 1,
};

static const struct regmap_config rt5575_dsp_regmap = {
	.name = "dsp",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 2,
};

static int rt5575_i2c_read(void *context, unsigned int reg, unsigned int *val)
{
	struct i2c_client *client = context;
	struct rt5575_priv *rt5575 = i2c_get_clientdata(client);

	regmap_read(rt5575->dsp_regmap, reg | RT5575_DSP_MAPPING, val);

	return 0;
}

static int rt5575_i2c_write(void *context, unsigned int reg, unsigned int val)
{
	struct i2c_client *client = context;
	struct rt5575_priv *rt5575 = i2c_get_clientdata(client);

	regmap_write(rt5575->dsp_regmap, reg | RT5575_DSP_MAPPING, val);

	return 0;
}

static const struct regmap_config rt5575_regmap = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xfffc,
	.readable_reg = rt5575_readable_register,
	.reg_read = rt5575_i2c_read,
	.reg_write = rt5575_i2c_write,
	.use_single_read = true,
	.use_single_write = true,
};

static const struct i2c_device_id rt5575_i2c_id[] = {
	{ "rt5575", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rt5575_i2c_id);

static int rt5575_i2c_probe(struct i2c_client *i2c)
{
	struct rt5575_priv *rt5575;
	int ret, val;

	rt5575 = devm_kzalloc(&i2c->dev, sizeof(struct rt5575_priv),
				GFP_KERNEL);
	if (rt5575 == NULL)
		return -ENOMEM;

	i2c_set_clientdata(i2c, rt5575);

	rt5575->i2c = i2c;

	rt5575->dsp_regmap = devm_regmap_init_i2c(i2c, &rt5575_dsp_regmap);
	if (IS_ERR(rt5575->dsp_regmap)) {
		ret = PTR_ERR(rt5575->dsp_regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	rt5575->regmap = devm_regmap_init(&i2c->dev, NULL, i2c, &rt5575_regmap);
	if (IS_ERR(rt5575->regmap)) {
		ret = PTR_ERR(rt5575->regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	regmap_read(rt5575->regmap, RT5575_ID, &val);
	if (val != RT5575_DEVICE_ID) {
		dev_err(&i2c->dev,
			"Device with ID register %08x is not rt5575\n", val);
		return -ENODEV;
	}

	regmap_read(rt5575->regmap, RT5575_ID_1, &val);
	if (!val) {
		dev_err(&i2c->dev, "This is not formal version\n");
		return -ENODEV;
	}

	return devm_snd_soc_register_component(&i2c->dev, &rt5575_soc_component_dev,
				      rt5575_dai, ARRAY_SIZE(rt5575_dai));
}

static const struct of_device_id rt5575_of_match[] = {
	{.compatible = "realtek,rt5575"},
	{},
};
MODULE_DEVICE_TABLE(of, rt5575_of_match);

static struct i2c_driver rt5575_i2c_driver = {
	.driver = {
		.name = "rt5575",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(rt5575_of_match),
	},
	.probe = rt5575_i2c_probe,
	.id_table = rt5575_i2c_id,
};
module_i2c_driver(rt5575_i2c_driver);

MODULE_DESCRIPTION("ASoC ALC5575 driver");
MODULE_AUTHOR("Oder Chiou <oder_chiou@realtek.com>");
MODULE_LICENSE("GPL");
