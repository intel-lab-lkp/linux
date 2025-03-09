/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2018 Baylibre SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#ifndef _MESON_CODEC_GLUE_H
#define _MESON_CODEC_GLUE_H

#include <sound/soc.h>

struct meson_codec_glue_input {
	struct snd_soc_pcm_stream params;
	unsigned int fmt;
};

/*
 * Playback stream
 */

/* Input helpers */
struct meson_codec_glue_input *
meson_codec_glue_input_get_data(struct snd_soc_dai *dai);
int meson_codec_glue_input_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai);
int meson_codec_glue_input_set_fmt(struct snd_soc_dai *dai,
				   unsigned int fmt);
int meson_codec_glue_input_dai_probe(struct snd_soc_dai *dai);
int meson_codec_glue_input_dai_remove(struct snd_soc_dai *dai);

/* Output helpers */
int meson_codec_glue_output_startup(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai);

/*
 * Capture stream
 */

/* Output helpers */
struct meson_codec_glue_input *
meson_codec_glue_capture_output_get_data(struct snd_soc_dai *dai);
int meson_codec_glue_capture_output_hw_params(struct snd_pcm_substream *substream,
					      struct snd_pcm_hw_params *params,
					      struct snd_soc_dai *dai);
int meson_codec_glue_capture_output_set_fmt(struct snd_soc_dai *dai,
					    unsigned int fmt);
int meson_codec_glue_capture_output_dai_probe(struct snd_soc_dai *dai);
int meson_codec_glue_capture_output_dai_remove(struct snd_soc_dai *dai);

/* Input helpers */
int meson_codec_glue_capture_input_startup(struct snd_pcm_substream *substream,
					   struct snd_soc_dai *dai);

#endif /* _MESON_CODEC_GLUE_H */
