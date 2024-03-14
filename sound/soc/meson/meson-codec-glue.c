// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2019 BayLibre, SAS.
// Author: Jerome Brunet <jbrunet@baylibre.com>

#include <linux/module.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include "meson-codec-glue.h"

static struct snd_soc_dapm_widget *
meson_codec_glue_get_data_widget(struct snd_soc_dapm_widget *w, bool playback)
{
	struct snd_soc_dapm_path *p;
	struct snd_soc_dapm_widget *node;
	enum snd_soc_dapm_type id = playback ? snd_soc_dapm_dai_in
					     : snd_soc_dapm_dai_out;
	enum snd_soc_dapm_direction dir = playback ? SND_SOC_DAPM_DIR_IN
						   : SND_SOC_DAPM_DIR_OUT;
	enum snd_soc_dapm_direction rdir = playback ? SND_SOC_DAPM_DIR_OUT
						    : SND_SOC_DAPM_DIR_IN;

	snd_soc_dapm_widget_for_each_path(w, rdir, p) {
		if (!p->connect)
			continue;

		/* Check that we still are in the same component */
		if (snd_soc_dapm_to_component(w->dapm) !=
		    snd_soc_dapm_to_component(p->node[dir]->dapm))
			continue;

		if (p->node[dir]->id == id)
			return p->node[dir];

		node = meson_codec_glue_get_data_widget(p->node[dir], playback);
		if (node)
			return node;
	}

	return NULL;
}

static void meson_codec_glue_set_data(struct snd_soc_dai *dai,
				      struct meson_codec_glue_input *data,
				      bool playback)
{
	int stream = playback ? SNDRV_PCM_STREAM_PLAYBACK
			      : SNDRV_PCM_STREAM_CAPTURE;

	snd_soc_dai_dma_data_set(dai, stream, data);
}

static struct meson_codec_glue_input *
meson_codec_glue_get_data(struct snd_soc_dai *dai, bool playback)
{
	int stream = playback ? SNDRV_PCM_STREAM_PLAYBACK
			      : SNDRV_PCM_STREAM_CAPTURE;

	return snd_soc_dai_dma_data_get(dai, stream);
}

struct meson_codec_glue_input *
meson_codec_glue_input_get_data(struct snd_soc_dai *dai)
{
	return meson_codec_glue_get_data(dai, true);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_input_get_data);

struct meson_codec_glue_input *
meson_codec_glue_capture_output_get_data(struct snd_soc_dai *dai)
{
	return meson_codec_glue_get_data(dai, false);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_capture_output_get_data);

static struct meson_codec_glue_input *
meson_codec_glue_data(struct snd_soc_dapm_widget *w, bool playback)
{
	struct snd_soc_dapm_widget *node =
		meson_codec_glue_get_data_widget(w, playback);
	struct snd_soc_dai *dai;

	if (WARN_ON(!node))
		return NULL;

	dai = node->priv;

	return meson_codec_glue_get_data(dai, playback);
}

static int meson_codec_glue_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params,
				      struct snd_soc_dai *dai,
				      bool playback)
{
	struct meson_codec_glue_input *data =
		meson_codec_glue_get_data(dai, playback);
	struct snd_soc_pcm_stream *stream = playback ? &dai->driver->playback
						     : &dai->driver->capture;

	data->params.rates = snd_pcm_rate_to_rate_bit(params_rate(params));
	data->params.rate_min = params_rate(params);
	data->params.rate_max = params_rate(params);
	data->params.formats = 1ULL << (__force int) params_format(params);
	data->params.channels_min = params_channels(params);
	data->params.channels_max = params_channels(params);
	data->params.sig_bits = stream->sig_bits;

	return 0;
}

int meson_codec_glue_input_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	return meson_codec_glue_hw_params(substream, params, dai, true);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_input_hw_params);

int meson_codec_glue_capture_output_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	return meson_codec_glue_hw_params(substream, params, dai, false);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_capture_output_hw_params);

static int meson_codec_glue_set_fmt(struct snd_soc_dai *dai,
				    unsigned int fmt,
				    bool playback)
{
	struct meson_codec_glue_input *data =
		meson_codec_glue_get_data(dai, playback);

	/* Save the source stream format for the downstream link */
	data->fmt = fmt;
	return 0;
}

int meson_codec_glue_input_set_fmt(struct snd_soc_dai *dai,
				   unsigned int fmt)
{
	return meson_codec_glue_set_fmt(dai, fmt, true);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_input_set_fmt);

int meson_codec_glue_capture_output_set_fmt(struct snd_soc_dai *dai,
				    unsigned int fmt)
{
	return meson_codec_glue_set_fmt(dai, fmt, false);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_capture_output_set_fmt);

static int meson_codec_glue_startup(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai,
				    bool playback)
{
	int stream = playback ? SNDRV_PCM_STREAM_CAPTURE
			      : SNDRV_PCM_STREAM_PLAYBACK;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dapm_widget *w = snd_soc_dai_get_widget(dai, stream);
	struct meson_codec_glue_input *data = meson_codec_glue_data(w, playback);

	if (!data)
		return -ENODEV;

	if (WARN_ON(!rtd->dai_link->c2c_params)) {
		dev_warn(dai->dev, "codec2codec link expected\n");
		return -EINVAL;
	}

	/* Replace link params with the input params */
	rtd->dai_link->c2c_params = &data->params;
	rtd->dai_link->num_c2c_params = 1;

	return snd_soc_runtime_set_dai_fmt(rtd, data->fmt);
}

int meson_codec_glue_output_startup(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	return meson_codec_glue_startup(substream, dai, true);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_output_startup);

int meson_codec_glue_capture_input_startup(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	return meson_codec_glue_startup(substream, dai, false);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_capture_input_startup);

static int meson_codec_glue_dai_probe(struct snd_soc_dai *dai, bool playback)
{
	struct meson_codec_glue_input *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	meson_codec_glue_set_data(dai, data, playback);
	return 0;
}

int meson_codec_glue_input_dai_probe(struct snd_soc_dai *dai)
{
	return meson_codec_glue_dai_probe(dai, true);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_input_dai_probe);

int meson_codec_glue_capture_output_dai_probe(struct snd_soc_dai *dai)
{
	return meson_codec_glue_dai_probe(dai, false);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_capture_output_dai_probe);

static int meson_codec_glue_dai_remove(struct snd_soc_dai *dai, bool playback)
{
	struct meson_codec_glue_input *data =
		meson_codec_glue_get_data(dai, playback);

	kfree(data);
	return 0;
}

int meson_codec_glue_input_dai_remove(struct snd_soc_dai *dai)
{
	return meson_codec_glue_dai_remove(dai, true);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_input_dai_remove);

int meson_codec_glue_capture_output_dai_remove(struct snd_soc_dai *dai)
{
	return meson_codec_glue_dai_remove(dai, false);
}
EXPORT_SYMBOL_GPL(meson_codec_glue_capture_output_dai_remove);

MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_DESCRIPTION("Amlogic Codec Glue Helpers");
MODULE_LICENSE("GPL v2");

