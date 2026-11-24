// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2023, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soundwire/sdw.h>
#include <linux/spinlock.h>
#include <sound/pcm.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include "common.h"
#include "qdsp6/q6afe.h"
#include "qdsp6/q6dsp-common.h"
#include "sdw.h"

#define X1E80100_WSA_MAX_CHANNELS	6
#define X1E80100_WSA_PORTS		2

struct x1e80100_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack[8];
	bool jack_setup;
	/* Protects the WSA channel map state below */
	spinlock_t wsa_chmap_lock;
	/* Userspace-selected SNDRV_CHMAP_* positions, 0 means unset */
	unsigned int wsa_chmap[X1E80100_WSA_PORTS][X1E80100_WSA_MAX_CHANNELS];
	/* Channel count of the last prepared stream */
	unsigned int wsa_channels[X1E80100_WSA_PORTS];
	bool wsa_prepared[X1E80100_WSA_PORTS];
};

/* Default WSA RX channel maps, indexed by channel count */
static const unsigned int
x1e80100_wsa_default_chmap[X1E80100_WSA_MAX_CHANNELS + 1][X1E80100_WSA_MAX_CHANNELS] = {
	[1] = { SNDRV_CHMAP_MONO },
	[2] = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR },
	[3] = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_FC },
	[4] = { SNDRV_CHMAP_FL, SNDRV_CHMAP_RL, SNDRV_CHMAP_FR, SNDRV_CHMAP_RR },
	[6] = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_RL,
		SNDRV_CHMAP_RR, SNDRV_CHMAP_FC, SNDRV_CHMAP_LFE },
};

static int x1e80100_wsa_port_index(unsigned int dai_id)
{
	switch (dai_id) {
	case WSA_CODEC_DMA_RX_0:
		return 0;
	case WSA_CODEC_DMA_RX_1:
		return 1;
	default:
		return -EINVAL;
	}
}

static struct x1e80100_snd_data *
x1e80100_wsa_chmap_kcontrol_data(struct snd_kcontrol *kcontrol, int *idx)
{
	struct snd_pcm_chmap *info = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd = info->pcm->private_data;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	*idx = x1e80100_wsa_port_index(cpu_dai->id);
	if (*idx < 0)
		return NULL;

	return snd_soc_card_get_drvdata(rtd->card);
}

static int x1e80100_wsa_chmap_ctl_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct x1e80100_snd_data *data;
	const unsigned int *map;
	int i, idx;

	data = x1e80100_wsa_chmap_kcontrol_data(kcontrol, &idx);
	if (!data)
		return -EINVAL;

	guard(spinlock)(&data->wsa_chmap_lock);

	/* Report the default map until userspace selects one */
	map = data->wsa_chmap[idx];
	if (!map[0])
		map = x1e80100_wsa_default_chmap[data->wsa_channels[idx]];

	for (i = 0; i < X1E80100_WSA_MAX_CHANNELS; i++)
		ucontrol->value.integer.value[i] = map[i];

	return 0;
}

static int x1e80100_wsa_chmap_ctl_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	unsigned int map[X1E80100_WSA_MAX_CHANNELS];
	struct x1e80100_snd_data *data;
	bool changed = false;
	int i, idx;

	data = x1e80100_wsa_chmap_kcontrol_data(kcontrol, &idx);
	if (!data)
		return -EINVAL;

	for (i = 0; i < X1E80100_WSA_MAX_CHANNELS; i++) {
		map[i] = ucontrol->value.integer.value[i];

		if (map[i] && qcom_snd_chmap_to_q6(map[i]) < 0)
			return -EINVAL;

		/* Unset positions are allowed only as a suffix of the map */
		if (map[i] && i && !map[i - 1])
			return -EINVAL;
	}

	guard(spinlock)(&data->wsa_chmap_lock);

	/* The map is applied on prepare; refuse updating prepared streams */
	if (data->wsa_prepared[idx])
		return -EBUSY;

	for (i = 0; i < X1E80100_WSA_MAX_CHANNELS; i++) {
		if (data->wsa_chmap[idx][i] != map[i]) {
			data->wsa_chmap[idx][i] = map[i];
			changed = true;
		}
	}

	return changed ? 1 : 0;
}

static int x1e80100_add_wsa_chmap_ctls(struct snd_soc_pcm_runtime *rtd,
				       const char *name)
{
	struct snd_pcm_chmap *info;
	unsigned int i;
	int ret;

	ret = snd_pcm_add_named_chmap_ctls(rtd->pcm, SNDRV_PCM_STREAM_PLAYBACK,
					   name, snd_pcm_std_chmaps,
					   X1E80100_WSA_MAX_CHANNELS, 0, &info);
	if (ret)
		return ret;

	/*
	 * The core registers a query-only control. Make it writable so that
	 * userspace (UCM) can select the WSA channel map. The card is not yet
	 * registered at this point, so the control is not visible to
	 * userspace while being modified.
	 */
	for (i = 0; i < info->kctl->count; i++)
		info->kctl->vd[i].access |= SNDRV_CTL_ELEM_ACCESS_WRITE;
	info->kctl->get = x1e80100_wsa_chmap_ctl_get;
	info->kctl->put = x1e80100_wsa_chmap_ctl_put;

	return 0;
}

static int x1e80100_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_jack *dp_jack = NULL;
	int dp_pcm_id = 0;

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		/*
		 * Set limit of -3 dB on Digital Volume and 0 dB on PA Volume
		 * to reduce the risk of speaker damage until we have active
		 * speaker protection in place.
		 */
		snd_soc_limit_volume(card, "WSA WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA2 WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA2 WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "SpkrLeft PA Volume", 6);
		snd_soc_limit_volume(card, "SpkrRight PA Volume", 6);
		snd_soc_limit_volume(card, "WooferLeft PA Volume", 6);
		snd_soc_limit_volume(card, "TweeterLeft PA Volume", 6);
		snd_soc_limit_volume(card, "WooferRight PA Volume", 6);
		snd_soc_limit_volume(card, "TweeterRight PA Volume", 6);
		break;
	case DISPLAY_PORT_RX_0:
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

	return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
}

static void x1e80100_snd_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case DISPLAY_PORT_RX_0:
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		return;
	default:
		break;
	}

	qcom_snd_headset_jack_cleanup(rtd);
	data->jack_setup = false;
}

static int x1e80100_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
							  SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = 48000;
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

	return 0;
}

static int x1e80100_snd_build_wsa_slots(struct x1e80100_snd_data *data,
					unsigned int dai_id,
					unsigned int channels,
					unsigned int *rx_slot)
{
	unsigned int map[X1E80100_WSA_MAX_CHANNELS];
	unsigned int i, set = 0;
	int idx;

	idx = x1e80100_wsa_port_index(dai_id);
	if (idx < 0)
		return idx;

	if (channels > X1E80100_WSA_MAX_CHANNELS ||
	    !x1e80100_wsa_default_chmap[channels][0])
		return -EINVAL;

	scoped_guard(spinlock, &data->wsa_chmap_lock) {
		memcpy(map, data->wsa_chmap[idx], sizeof(map));
		data->wsa_channels[idx] = channels;
		data->wsa_prepared[idx] = true;
	}

	for (i = 0; i < X1E80100_WSA_MAX_CHANNELS; i++)
		if (map[i])
			set++;

	/* Use the map only when it describes exactly this channel count */
	if (set != channels)
		memcpy(map, x1e80100_wsa_default_chmap[channels], sizeof(map));

	for (i = 0; i < channels; i++) {
		int q6 = qcom_snd_chmap_to_q6(map[i]);

		if (q6 < 0)
			return q6;
		rx_slot[i] = q6;
	}

	return 0;
}

static int x1e80100_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	unsigned int channels = substream->runtime->channels;
	unsigned int rx_slot[X1E80100_WSA_MAX_CHANNELS];
	int ret;

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		ret = x1e80100_snd_build_wsa_slots(data, cpu_dai->id,
						   channels, rx_slot);
		if (ret)
			return ret;

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
						  channels, rx_slot);
		if (ret)
			return ret;
		break;
	default:
		break;
	}

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int x1e80100_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct x1e80100_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int idx = x1e80100_wsa_port_index(cpu_dai->id);

	if (idx >= 0) {
		guard(spinlock)(&data->wsa_chmap_lock);
		data->wsa_prepared[idx] = false;
	}

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops x1e80100_be_ops = {
	.startup = qcom_snd_sdw_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.hw_free = x1e80100_snd_hw_free,
	.prepare = x1e80100_snd_prepare,
};

/*
 * The WSA channel map controls are added here rather than in the DAI link
 * init or startup callbacks: the backend PCMs are created only after all
 * links are initialized, and adding controls from startup would take
 * controls_rwsem under dapm_mutex, deadlocking against DAPM mixer writes.
 */
static int x1e80100_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	int ret;

	for_each_card_rtds(card, rtd) {
		struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
		const char *name;

		switch (cpu_dai->id) {
		case WSA_CODEC_DMA_RX_0:
			name = "WSA Playback Channel Map";
			break;
		case WSA_CODEC_DMA_RX_1:
			name = "WSA2 Playback Channel Map";
			break;
		default:
			continue;
		}

		ret = x1e80100_add_wsa_chmap_ctls(rtd, name);
		if (ret)
			return ret;
	}

	return 0;
}

static void x1e80100_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = x1e80100_snd_init;
			link->exit = x1e80100_snd_exit;
			link->be_hw_params_fixup = x1e80100_be_hw_params_fixup;
			link->ops = &x1e80100_be_ops;
		}
	}
}

static int x1e80100_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct x1e80100_snd_data *data;
	struct device *dev = &pdev->dev;
	int i, ret;

	card = devm_kzalloc_obj(dev, *card);
	if (!card)
		return -ENOMEM;
	/* Allocate the private data */
	data = devm_kzalloc_obj(dev, *data);
	if (!data)
		return -ENOMEM;

	spin_lock_init(&data->wsa_chmap_lock);
	for (i = 0; i < X1E80100_WSA_PORTS; i++)
		data->wsa_channels[i] = 2;

	card->owner = THIS_MODULE;
	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);
	card->late_probe = x1e80100_late_probe;
	x1e80100_add_be_ops(card);

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_x1e80100_dt_match[] = {
	{ .compatible = "qcom,x1e80100-sndcard", .data = "x1e80100" },
	{ .compatible = "qcom,glymur-sndcard", .data = "glymur" },
	{}
};
MODULE_DEVICE_TABLE(of, snd_x1e80100_dt_match);

static struct platform_driver snd_x1e80100_driver = {
	.probe  = x1e80100_platform_probe,
	.driver = {
		.name = "snd-x1e80100",
		.of_match_table = snd_x1e80100_dt_match,
	},
};
module_platform_driver(snd_x1e80100_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_AUTHOR("Krzysztof Kozlowski <krzysztof.kozlowski@linaro.org>");
MODULE_DESCRIPTION("Qualcomm X1E80100 ASoC Machine Driver");
MODULE_LICENSE("GPL");
