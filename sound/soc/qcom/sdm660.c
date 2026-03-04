// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Richard Acayan. All rights reserved.
 */

#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-card.h>
#include <sound/soc-dai.h>
#include <sound/soc-dapm.h>
#include <sound/soc-jack.h>

#include "common.h"
#include "qdsp6/q6afe.h"

#define DEFAULT_SAMPLE_RATE_48K		48000
#define DEFAULT_INT_MCLK_RATE		9600000
#define MI2S_BCLK_RATE			1536000

struct sdm660_snd_data {
	struct snd_soc_jack jack;
	bool jack_setup;
	uint32_t int0_mi2s_clk_count;
	uint32_t int3_mi2s_clk_count;
};

static int snd_sdm660_startup(struct snd_pcm_substream *stream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(stream);
	struct sdm660_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu->id) {
	case INT0_MI2S_RX:
		data->int0_mi2s_clk_count++;
		if (data->int0_mi2s_clk_count == 1)
			snd_soc_dai_set_sysclk(cpu,
				Q6AFE_LPASS_CLK_ID_INT0_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);

		snd_soc_dai_set_fmt(cpu, SND_SOC_DAIFMT_CBP_CFP);

		break;
	case INT3_MI2S_TX:
		data->int3_mi2s_clk_count++;
		if (data->int3_mi2s_clk_count == 1)
			snd_soc_dai_set_sysclk(cpu,
				Q6AFE_LPASS_CLK_ID_INT3_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);

		snd_soc_dai_set_fmt(cpu, SND_SOC_DAIFMT_CBP_CFP);

		break;
	default:
		dev_err(rtd->dev, "%s: invalid dai id 0x%x\n", __func__,
			cpu->id);
		return -EINVAL;
	}

	return 0;
}

static void snd_sdm660_shutdown(struct snd_pcm_substream *stream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(stream);
	struct sdm660_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu->id) {
	case INT0_MI2S_RX:
		data->int0_mi2s_clk_count--;
		if (data->int0_mi2s_clk_count == 0)
			snd_soc_dai_set_sysclk(cpu,
				Q6AFE_LPASS_CLK_ID_INT0_MI2S_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);

		break;
	case INT3_MI2S_TX:
		data->int3_mi2s_clk_count--;
		if (data->int3_mi2s_clk_count == 0)
			snd_soc_dai_set_sysclk(cpu,
				Q6AFE_LPASS_CLK_ID_INT3_MI2S_IBIT,
				0, SNDRV_PCM_STREAM_PLAYBACK);

		break;
	default:
		dev_err(rtd->dev, "%s: invalid dai id 0x%x\n", __func__,
			cpu->id);
		break;
	}
}

static const struct snd_soc_ops sdm660_ops = {
	.startup = snd_sdm660_startup,
	.shutdown = snd_sdm660_shutdown,
};

static int sdm660_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = DEFAULT_SAMPLE_RATE_48K;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	channels->min = channels->max = 2;

	return 0;
}

static int sdm660_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct sdm660_snd_data *data = snd_soc_card_get_drvdata(card);

	return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
}

static void snd_sdm660_add_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->ops = &sdm660_ops;
			link->be_hw_params_fixup = sdm660_be_hw_params_fixup;
		}

		link->init = sdm660_dai_init;
	}
}

static int snd_sdm660_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sdm660_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(struct snd_soc_card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	data = devm_kzalloc(dev, sizeof(struct sdm660_snd_data), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->driver_name = "sdm660";
	card->dev = dev;
	card->owner = THIS_MODULE;

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	snd_soc_card_set_drvdata(card, data);

	snd_sdm660_add_ops(card);

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sdm660_device_id[] = {
	{ .compatible = "qcom,sdm660-sndcard", },
	{ }
};
MODULE_DEVICE_TABLE(of, snd_sdm660_device_id);

static struct platform_driver snd_sdm660_driver = {
	.probe = snd_sdm660_probe,
	.driver = {
		.name = "sdm660-sndcard",
		.of_match_table = snd_sdm660_device_id,
		.pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(snd_sdm660_driver);

MODULE_DESCRIPTION("sdm660 ASoC Machine Driver");
MODULE_LICENSE("GPL");
