// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (C) 2025 Cirrus Logic, Inc. and
//                    Cirrus Logic International Semiconductor Ltd.

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "../common/soc-intel-quirks.h"
#include "sof_board_helpers.h"

#define CS35L56_LINK_ORDER	SOF_LINK_ORDER(SOF_LINK_AMP,		\
					       SOF_LINK_CODEC,		\
					       SOF_LINK_DMIC01,		\
					       SOF_LINK_DMIC16K,	\
					       SOF_LINK_IDISP_HDMI,	\
					       SOF_LINK_HDMI_IN,	\
					       SOF_LINK_NONE)

static const struct snd_soc_dapm_widget sof_widgets[] = {
	SND_SOC_DAPM_HP("Speaker", NULL),
};

static const struct snd_soc_dapm_route sof_map[] = {
	{"Speaker", NULL, "AMP1 SPK"},
	{"Speaker", NULL, "AMP2 SPK"},
	{"Speaker", NULL, "AMP3 SPK"},
	{"Speaker", NULL, "AMP4 SPK"},
};

/* sof audio machine driver for cs35l56 codec */
static struct snd_soc_card sof_audio_card_cs35l56 = {
	.name = "cs35l56", /* the sof- prefix is added by the core */
	.owner = THIS_MODULE,
	.dapm_widgets = sof_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sof_widgets),
	.dapm_routes = sof_map,
	.num_dapm_routes = ARRAY_SIZE(sof_map),
	.fully_routed = true,
};

static struct snd_soc_dai_link_component cs35l56_component_i2c[] = {
	{
		.name = "i2c-CSC355C:00",
		.dai_name = "cs35l56-asp1",
	},
	{
		.name = "i2c-CSC355C:01",
		.dai_name = "cs35l56-asp1",
	},
	{
		.name = "i2c-CSC355C:02",
		.dai_name = "cs35l56-asp1",
	},
	{
		.name = "i2c-CSC355C:03",
		.dai_name = "cs35l56-asp1",
	},
};

static struct snd_soc_dai_link_component cs35l56_component_spi[] = {
	{
		.name = "spi-CSC355C:00",
		.dai_name = "cs35l56-asp1",
	},
	{
		.name = "spi-CSC355C:01",
		.dai_name = "cs35l56-asp1",
	},
	{
		.name = "spi-CSC355C:02",
		.dai_name = "cs35l56-asp1",
	},
	{
		.name = "spi-CSC355C:03",
		.dai_name = "cs35l56-asp1",
	},
};

struct cs35l56_dlc_entry {
	struct snd_soc_dai_link_component *dlc;
	int n_dlc;
};

static const struct cs35l56_dlc_entry cs35l56_components[] = {
	{ cs35l56_component_i2c, ARRAY_SIZE(cs35l56_component_i2c) },
	{ cs35l56_component_spi, ARRAY_SIZE(cs35l56_component_spi) },
};

static const char * const sof_cs35l56_name_prefixes[] = {
	"AMP1", "AMP2", "AMP3", "AMP4",
};

static int sof_cs35l56_init(struct snd_soc_pcm_runtime *rtd)
{
	int i, ret;
	unsigned int rx_mask = 3;
	struct snd_soc_dai *codec_dai;

	/* SSP has 8 x 16-bit sample slots and FSYNC=48000, BCLK=6.144 MHz */
	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_set_tdm_slot(codec_dai, 0x3, rx_mask, 8, 16);
		if (ret < 0)
			return ret;

		ret = snd_soc_dai_set_sysclk(codec_dai, 0, 6144000, SND_SOC_CLOCK_IN);
		if (ret < 0)
			return ret;

		rx_mask <<= 2;
	}

	return 0;
}

static int sof_audio_card_cs35l56_add_name_prefixes(struct device *dev)
{
	struct snd_soc_codec_conf *confs;
	struct snd_soc_dai_link_component *codec;
	int num_codecs = sof_audio_card_cs35l56.dai_link->num_codecs;
	int conf_idx, i;

	confs = devm_kcalloc(dev, num_codecs, sizeof(*confs), GFP_KERNEL);
	if (!confs)
		return -ENOMEM;

	/* Assumes dailink 0 contains one codec entry per codec */
	conf_idx = 0;
	for (i = 0; i < num_codecs; ++i) {
		codec = snd_soc_link_to_codec(&sof_audio_card_cs35l56.dai_link[0], i);
		confs[conf_idx].dlc.name = codec->name;
		confs[conf_idx].name_prefix = sof_cs35l56_name_prefixes[conf_idx];
		conf_idx++;
	}

	sof_audio_card_cs35l56.codec_conf = confs;
	sof_audio_card_cs35l56.num_configs = conf_idx;

	return 0;
}

static int sof_audio_card_cs35l56_find_codecs(struct device *dev,
					      const struct cs35l56_dlc_entry *dlce)
{
	struct snd_soc_dai_link_component *dlc = dlce->dlc;
	int n;

	for (n = 0; n < dlce->n_dlc; ++n) {
		dev_info(dev, "Looking for (%s) on (%s)\n", dlc[n].dai_name, dlc[n].name);
		if (!snd_soc_find_dai_with_mutex(&dlc[n]))
			return -ENODEV;
	}

	return n;
}

static int sof_card_dai_links_create(struct device *dev, struct snd_soc_card *card,
				     struct sof_card_private *ctx)
{
	struct snd_soc_dai_link_component *dlc = NULL;
	int i, num_codecs, ret;

	for (i = 0; i < ARRAY_SIZE(cs35l56_components); ++i) {
		num_codecs = sof_audio_card_cs35l56_find_codecs(dev, &cs35l56_components[i]);
		if (num_codecs > 0) {
			dlc = cs35l56_components[i].dlc;
			break;
		}
	}
	if (!dlc) {
		dev_warn(dev, "Couldn't find amp\n");
		return -EPROBE_DEFER;
	}

	ret = sof_intel_board_set_dai_link(dev, card, ctx);
	if (ret)
		return ret;

	if (!ctx->amp_link) {
		dev_err(dev, "Amp link not available");
		return -EINVAL;
	}

	ctx->amp_link->codecs = dlc;
	ctx->amp_link->num_codecs = num_codecs;
	ctx->amp_link->init = sof_cs35l56_init;

	return 0;
}

static int sof_audio_probe(struct platform_device *pdev)
{
	struct snd_soc_acpi_mach *mach = pdev->dev.platform_data;
	unsigned long sof_cs35l56_quirk = (unsigned long)pdev->id_entry->driver_data;
	struct sof_card_private *ctx;
	int ret;

	dev_dbg(&pdev->dev, "sof_cs35l56_quirk = %lx\n", sof_cs35l56_quirk);

	ctx = sof_intel_board_get_ctx(&pdev->dev, sof_cs35l56_quirk);
	if (!ctx)
		return -ENOMEM;

	ctx->link_order_overwrite = CS35L56_LINK_ORDER;

	ret = sof_card_dai_links_create(&pdev->dev, &sof_audio_card_cs35l56, ctx);
	if (ret)
		return ret;

	ret = sof_audio_card_cs35l56_add_name_prefixes(&pdev->dev);
	if (ret)
		return ret;

	sof_audio_card_cs35l56.dev = &pdev->dev;

	/* set platform name for each dailink */
	ret = snd_soc_fixup_dai_links_platform_name(&sof_audio_card_cs35l56,
						    mach->mach_params.platform);
	if (ret)
		return ret;

	snd_soc_card_set_drvdata(&sof_audio_card_cs35l56, ctx);

	return devm_snd_soc_register_card(&pdev->dev, &sof_audio_card_cs35l56);
}

static const struct platform_device_id board_ids[] = {
	{
		.name = "tgl_cs35l56_ssp2_def",
		.driver_data = (kernel_ulong_t)(SOF_SSP_PORT_AMP(2)),
	},
	{ }
};
MODULE_DEVICE_TABLE(platform, board_ids);

static struct platform_driver sof_audio = {
	.probe = sof_audio_probe,
	.driver = {
		.name = "sof_cs35l56",
		.pm = &snd_soc_pm_ops,
	},
	.id_table = board_ids,
};
module_platform_driver(sof_audio)

/* Module information */
MODULE_DESCRIPTION("SOF Audio Machine driver for CS35L56");
MODULE_AUTHOR("Richard Fitzgerald <rf@opensource.cirrus.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SND_SOC_INTEL_SOF_BOARD_HELPERS");
