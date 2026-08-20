// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Nicholas Johnson */
#include "quantum.h"

/* Sample-rate control */

static int quantum_srate_info(struct snd_kcontrol *k,
			      struct snd_ctl_elem_info *uinfo)
{
	struct quantum_chip *chip = k->private_data;
	static const char * const rates[] = {
		"44100", "48000", "88200", "96000", "176400", "192000"
	};

	if (quantum_device_gone(chip))
		return -ENODEV;

	return snd_ctl_enum_info(uinfo, 1, 6, rates);
}

static int quantum_srate_get(struct snd_kcontrol *k,
			     struct snd_ctl_elem_value *u)
{
	struct quantum_chip *chip = k->private_data;

	if (quantum_device_gone(chip))
		return -ENODEV;

	u->value.enumerated.item[0] =
		(chip->sample_rate == 44100) ? 0 :
		(chip->sample_rate == 48000) ? 1 :
		(chip->sample_rate == 88200) ? 2 :
		(chip->sample_rate == 96000) ? 3 :
		(chip->sample_rate == 176400) ? 4 : 5;

	return 0;
}

static int quantum_srate_put(struct snd_kcontrol *k,
			     struct snd_ctl_elem_value *u)
{
	struct quantum_chip *chip = k->private_data;
	unsigned int rate;
	unsigned int item = u->value.enumerated.item[0];
	int err;

	static const unsigned int map[] = {
		44100, 48000, 88200, 96000, 176400, 192000
	};

	if (quantum_device_gone(chip))
		return -ENODEV;

	if (item >= ARRAY_SIZE(map))
		return -EINVAL;
	rate = map[item];
	if (READ_ONCE(chip->sample_rate) == rate)
		return 0;

	mutex_lock(&chip->dma_mutex);
	if (quantum_device_gone(chip)) {
		err = -ENODEV;
		goto out;
	}
	if (chip->pcm_configured || READ_ONCE(chip->stream_active)) {
		err = -EBUSY;
		goto out;
	}
	err = quantum_set_sample_rate(chip, rate);
out:
	mutex_unlock(&chip->dma_mutex);
	return err < 0 ? err : 1;
}

/* Clock-source control */

static int quantum_clock_info(struct snd_kcontrol *k,
			      struct snd_ctl_elem_info *uinfo)
{
	struct quantum_chip *chip = k->private_data;
	static const char * const src[] = {
		"Internal", "S/PDIF", "WordClock", "ADAT1", "ADAT2"
	};

	if (quantum_device_gone(chip))
		return -ENODEV;

	return snd_ctl_enum_info(uinfo, 1, 5, src);
}

static int quantum_clock_get(struct snd_kcontrol *k,
			     struct snd_ctl_elem_value *u)
{
	struct quantum_chip *chip = k->private_data;

	if (quantum_device_gone(chip))
		return -ENODEV;

	u->value.enumerated.item[0] = chip->clock_source;
	return 0;
}

static int quantum_clock_put(struct snd_kcontrol *k,
			     struct snd_ctl_elem_value *u)
{
	struct quantum_chip *chip = k->private_data;
	unsigned int source = u->value.enumerated.item[0];
	int err;

	if (quantum_device_gone(chip))
		return -ENODEV;

	if (source > CLK_SOURCE_EXTERNAL_ADAT2)
		return -EINVAL;
	if (READ_ONCE(chip->clock_source) == source)
		return 0;

	mutex_lock(&chip->dma_mutex);
	if (quantum_device_gone(chip)) {
		err = -ENODEV;
		goto out;
	}
	if (chip->pcm_configured || READ_ONCE(chip->stream_active)) {
		err = -EBUSY;
		goto out;
	}
	err = quantum_set_clock_source(chip, source);
out:
	mutex_unlock(&chip->dma_mutex);
	return err < 0 ? err : 1;
}

static int quantum_xrun_info(struct snd_kcontrol *k,
			     struct snd_ctl_elem_info *uinfo)
{
	struct quantum_chip *chip = k->private_data;

	if (quantum_device_gone(chip))
		return -ENODEV;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER64;
	uinfo->count = 1;
	uinfo->value.integer64.min = 0;
	uinfo->value.integer64.max = S64_MAX;
	return 0;
}

static int quantum_xrun_get(struct snd_kcontrol *k,
			    struct snd_ctl_elem_value *u)
{
	struct quantum_chip *chip = k->private_data;
	u64 count;

	if (quantum_device_gone(chip))
		return -ENODEV;

	count = k->private_value ? chip->capture_xruns :
		chip->playback_xruns;
	u->value.integer64.value[0] = count;
	return 0;
}

/* Control registration */

int quantum_mixer_new(struct quantum_chip *chip)
{
	static const struct snd_kcontrol_new ctrls[] = {
		{
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Quantum Sample Rate",
			.info = quantum_srate_info,
			.get = quantum_srate_get,
			.put = quantum_srate_put,
		},
		{
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Quantum Clock Source",
			.info = quantum_clock_info,
			.get = quantum_clock_get,
			.put = quantum_clock_put,
		},
		{
			.iface = SNDRV_CTL_ELEM_IFACE_CARD,
			.name = "Quantum Playback XRUN Count",
			.access = SNDRV_CTL_ELEM_ACCESS_READ |
				  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
			.info = quantum_xrun_info,
			.get = quantum_xrun_get,
		},
		{
			.iface = SNDRV_CTL_ELEM_IFACE_CARD,
			.name = "Quantum Capture XRUN Count",
			.access = SNDRV_CTL_ELEM_ACCESS_READ |
				  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
			.info = quantum_xrun_info,
			.get = quantum_xrun_get,
			.private_value = 1,
		},
	};

	int i;

	if (quantum_device_gone(chip))
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(ctrls); i++) {
		int err = snd_ctl_add(chip->card,
			snd_ctl_new1(&ctrls[i], chip));
		if (err < 0)
			return err;
	}

	return 0;
}
