// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <linux/input-event-codes.h>
#include "qdsp6/q6afe.h"
#include "common.h"
#include "sdw.h"

struct sc8280xp_mi2s_codec_config {
	unsigned int cpu_dai_id;
	unsigned int dai_fmt;
	unsigned int sysclk_rate;
};

struct sc8280xp_be_hw_params {
	unsigned int rate;
	snd_pcm_format_t format;
	unsigned int channels_min;
	unsigned int channels_max;
};

struct sc8280xp_be_hw_params_config {
	unsigned int cpu_dai_id;
	struct sc8280xp_be_hw_params hw_params;
};

struct sc8280xp_sndcard_data {
	const char *driver_name;
	struct sc8280xp_be_hw_params default_be_hw_params;
	const struct sc8280xp_mi2s_codec_config *mi2s_codec_configs;
	int num_mi2s_codec_configs;
	const struct sc8280xp_be_hw_params_config *be_hw_params_configs;
	int num_be_hw_params_configs;
	const unsigned int *headset_jack_dais;
	int num_headset_jack_dais;
	struct snd_soc_jack_pin *headset_jack_pins;
	unsigned int num_headset_jack_pins;
};

struct sc8280xp_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack[8];
	const struct sc8280xp_sndcard_data *card_data;
	bool jack_setup;
};

static const struct sc8280xp_mi2s_codec_config *
sc8280xp_snd_get_mi2s_codec_config(const struct sc8280xp_sndcard_data *card_data,
				   unsigned int cpu_dai_id)
{
	int i;

	for (i = 0; i < card_data->num_mi2s_codec_configs; i++) {
		if (card_data->mi2s_codec_configs[i].cpu_dai_id == cpu_dai_id)
			return &card_data->mi2s_codec_configs[i];
	}

	return NULL;
}

static const struct sc8280xp_be_hw_params *
sc8280xp_snd_get_be_hw_params(const struct sc8280xp_sndcard_data *card_data,
			      unsigned int cpu_dai_id)
{
	int i;

	for (i = 0; i < card_data->num_be_hw_params_configs; i++) {
		if (card_data->be_hw_params_configs[i].cpu_dai_id == cpu_dai_id)
			return &card_data->be_hw_params_configs[i].hw_params;
	}

	return NULL;
}

static bool sc8280xp_snd_is_headset_jack_dai(const struct sc8280xp_sndcard_data *card_data,
					     unsigned int cpu_dai_id)
{
	int i;

	for (i = 0; i < card_data->num_headset_jack_dais; i++) {
		if (card_data->headset_jack_dais[i] == cpu_dai_id)
			return true;
	}

	return false;
}

static int sc8280xp_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	const struct sc8280xp_sndcard_data *card_data = data->card_data;
	const struct sc8280xp_mi2s_codec_config *mi2s_config;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_jack *dp_jack  = NULL;
	int dp_pcm_id = 0;
	int i, ret;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX...QUATERNARY_MI2S_TX:
	case QUINARY_MI2S_RX...QUINARY_MI2S_TX:
		snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);

		mi2s_config = sc8280xp_snd_get_mi2s_codec_config(card_data,
								 cpu_dai->id);
		if (mi2s_config) {
			for_each_rtd_codec_dais(rtd, i, codec_dai) {
				if (mi2s_config->dai_fmt) {
					ret = snd_soc_dai_set_fmt(codec_dai,
								  mi2s_config->dai_fmt);
					if (ret && ret != -ENOTSUPP)
						return ret;
				}

				if (mi2s_config->sysclk_rate) {
					ret = snd_soc_dai_set_sysclk(codec_dai, 0,
								     mi2s_config->sysclk_rate,
								     SND_SOC_CLOCK_IN);
					if (ret && ret != -ENOTSUPP)
						return dev_err_probe(card->dev, ret,
								     "%s: failed to set sysclk for %s\n",
								     rtd->dai_link->name,
								     codec_dai->name);
				}
			}
		}
		break;
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		/*
		 * Set limit of -3 dB on Digital Volume and 0 dB on PA Volume
		 * to reduce the risk of speaker damage until we have active
		 * speaker protection in place.
		 */
		snd_soc_limit_volume(card, "WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "SpkrLeft PA Volume", 17);
		snd_soc_limit_volume(card, "SpkrRight PA Volume", 17);
		break;
	case DISPLAY_PORT_RX_0:
		/* DISPLAY_PORT dai ids are not contiguous */
		dp_pcm_id = 0;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		dp_pcm_id = cpu_dai->id - DISPLAY_PORT_RX_1 + 1;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	default:
		break;
	}

	if (dp_jack)
		return qcom_snd_dp_jack_setup(rtd, dp_jack, dp_pcm_id);

	if (sc8280xp_snd_is_headset_jack_dai(card_data, cpu_dai->id))
		return qcom_snd_headset_jack_setup(rtd, &data->jack,
						   &data->jack_setup,
						   card_data->headset_jack_pins,
						   card_data->num_headset_jack_pins);

	if (card_data->headset_jack_dais)
		return 0;

	return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
}

static int sc8280xp_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				       struct snd_pcm_hw_params *params)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	const struct sc8280xp_sndcard_data *card_data = data->card_data;
	const struct sc8280xp_be_hw_params *be_hw_params;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	bool use_default = false;

	be_hw_params = sc8280xp_snd_get_be_hw_params(card_data, cpu_dai->id);
	if (!be_hw_params) {
		be_hw_params = &card_data->default_be_hw_params;
		use_default = true;
	}

	rate->min = be_hw_params->rate;
	rate->max = be_hw_params->rate;
	snd_mask_set_format(fmt, be_hw_params->format);
	channels->min = be_hw_params->channels_min;
	channels->max = be_hw_params->channels_max;

	if (use_default) {
		switch (cpu_dai->id) {
		case TX_CODEC_DMA_TX_0:
		case TX_CODEC_DMA_TX_1:
		case TX_CODEC_DMA_TX_2:
		case TX_CODEC_DMA_TX_3:
			channels->min = 1;
			break;
		default:
			break;
		}
	}

	return 0;
}

static int sc8280xp_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sc8280xp_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops sc8280xp_be_ops = {
	.startup = qcom_snd_sdw_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.hw_free = sc8280xp_snd_hw_free,
	.prepare = sc8280xp_snd_prepare,
};

static void sc8280xp_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sc8280xp_snd_init;
			link->be_hw_params_fixup = sc8280xp_be_hw_params_fixup;
			link->ops = &sc8280xp_be_ops;
		}
	}
}

static int sc8280xp_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sc8280xp_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;
	card->owner = THIS_MODULE;

	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->card_data = of_device_get_match_data(dev);
	if (!data->card_data)
		return -EINVAL;

	card->dev = dev;
	card->driver_name = data->card_data->driver_name;

	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	sc8280xp_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

#define SC8280XP_SND_DATA(_driver_name)				\
	.driver_name = _driver_name,				\
	.default_be_hw_params = {				\
		.rate = 48000,					\
		.format = SNDRV_PCM_FORMAT_S16_LE,		\
		.channels_min = 2,				\
		.channels_max = 2,				\
	}

static const struct sc8280xp_sndcard_data kaanapali_data = {
	SC8280XP_SND_DATA("kaanapali"),
};

static const struct sc8280xp_sndcard_data qcm6490_data = {
	SC8280XP_SND_DATA("qcm6490"),
};

static const struct sc8280xp_sndcard_data qcs615_data = {
	SC8280XP_SND_DATA("qcs615"),
};

static const struct sc8280xp_sndcard_data qcs6490_data = {
	SC8280XP_SND_DATA("qcs6490"),
};

static const struct sc8280xp_sndcard_data qcs8300_data = {
	SC8280XP_SND_DATA("qcs8300"),
};

static const struct sc8280xp_mi2s_codec_config qcs6490_rubikpi3_mi2s_codec_configs[] = {
	{
		.cpu_dai_id = PRIMARY_MI2S_RX,
		.dai_fmt = SND_SOC_DAIFMT_BC_FC |
			   SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_I2S,
		.sysclk_rate = 19200000,
	},
	{
		.cpu_dai_id = PRIMARY_MI2S_TX,
		.dai_fmt = SND_SOC_DAIFMT_BC_FC |
			   SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_I2S,
		.sysclk_rate = 19200000,
	},
};

static const unsigned int qcs6490_rubikpi3_headset_jack_dais[] = {
	PRIMARY_MI2S_RX,
};

static struct snd_soc_jack_pin qcs6490_rubikpi3_headset_jack_pins[] = {
	{
		.pin = "Mic Jack",
		.mask = SND_JACK_HEADPHONE,
	},
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static const struct sc8280xp_be_hw_params_config qcs6490_rubikpi3_be_hw_params_configs[] = {
	{
		.cpu_dai_id = PRIMARY_MI2S_TX,
		.hw_params = {
			.rate = 48000,
			.format = SNDRV_PCM_FORMAT_S16_LE,
			.channels_min = 2,
			.channels_max = 2,
		},
	},
	{
		.cpu_dai_id = QUATERNARY_MI2S_RX,
		.hw_params = {
			.rate = 48000,
			.format = SNDRV_PCM_FORMAT_S16_LE,
			.channels_min = 2,
			.channels_max = 2,
		},
	},
	{
		.cpu_dai_id = TERTIARY_MI2S_RX,
		.hw_params = {
			.rate = 48000,
			.format = SNDRV_PCM_FORMAT_S32_LE,
			.channels_min = 2,
			.channels_max = 2,
		},
	},
	{
		.cpu_dai_id = TERTIARY_MI2S_TX,
		.hw_params = {
			.rate = 48000,
			.format = SNDRV_PCM_FORMAT_S32_LE,
			.channels_min = 1,
			.channels_max = 2,
		},
	},
};

static const struct sc8280xp_sndcard_data qcs6490_rubikpi3_data = {
	SC8280XP_SND_DATA("qcs6490"),
	.mi2s_codec_configs = qcs6490_rubikpi3_mi2s_codec_configs,
	.num_mi2s_codec_configs = ARRAY_SIZE(qcs6490_rubikpi3_mi2s_codec_configs),
	.be_hw_params_configs = qcs6490_rubikpi3_be_hw_params_configs,
	.num_be_hw_params_configs = ARRAY_SIZE(qcs6490_rubikpi3_be_hw_params_configs),
	.headset_jack_dais = qcs6490_rubikpi3_headset_jack_dais,
	.num_headset_jack_dais = ARRAY_SIZE(qcs6490_rubikpi3_headset_jack_dais),
	.headset_jack_pins = qcs6490_rubikpi3_headset_jack_pins,
	.num_headset_jack_pins = ARRAY_SIZE(qcs6490_rubikpi3_headset_jack_pins),
};

static const struct sc8280xp_sndcard_data sa8775p_data = {
	SC8280XP_SND_DATA("sa8775p"),
};

static const struct sc8280xp_sndcard_data sc8280xp_data = {
	SC8280XP_SND_DATA("sc8280xp"),
};

static const struct sc8280xp_sndcard_data sm8450_data = {
	SC8280XP_SND_DATA("sm8450"),
};

static const struct sc8280xp_sndcard_data sm8550_data = {
	SC8280XP_SND_DATA("sm8550"),
};

static const struct sc8280xp_sndcard_data sm8650_data = {
	SC8280XP_SND_DATA("sm8650"),
};

static const struct sc8280xp_sndcard_data sm8750_data = {
	SC8280XP_SND_DATA("sm8750"),
};

static const struct of_device_id snd_sc8280xp_dt_match[] = {
	{ .compatible = "thundercomm,qcs6490-rubikpi3-sndcard", .data = &qcs6490_rubikpi3_data },
	{ .compatible = "qcom,kaanapali-sndcard", .data = &kaanapali_data },
	{ .compatible = "qcom,qcm6490-idp-sndcard", .data = &qcm6490_data },
	{ .compatible = "qcom,qcs615-sndcard", .data = &qcs615_data },
	{ .compatible = "qcom,qcs6490-rb3gen2-sndcard", .data = &qcs6490_data },
	{ .compatible = "qcom,qcs8275-sndcard", .data = &qcs8300_data },
	{ .compatible = "qcom,qcs9075-sndcard", .data = &sa8775p_data },
	{ .compatible = "qcom,qcs9100-sndcard", .data = &sa8775p_data },
	{ .compatible = "qcom,sc8280xp-sndcard", .data = &sc8280xp_data },
	{ .compatible = "qcom,sm8450-sndcard", .data = &sm8450_data },
	{ .compatible = "qcom,sm8550-sndcard", .data = &sm8550_data },
	{ .compatible = "qcom,sm8650-sndcard", .data = &sm8650_data },
	{ .compatible = "qcom,sm8750-sndcard", .data = &sm8750_data },
	{}
};

MODULE_DEVICE_TABLE(of, snd_sc8280xp_dt_match);

static struct platform_driver snd_sc8280xp_driver = {
	.probe  = sc8280xp_platform_probe,
	.driver = {
		.name = "snd-sc8280xp",
		.of_match_table = snd_sc8280xp_dt_match,
	},
};
module_platform_driver(snd_sc8280xp_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SC8280XP ASoC Machine Driver");
MODULE_LICENSE("GPL");
