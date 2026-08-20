// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Nicholas Johnson */
#include "quantum.h"

static const unsigned int quantum_channel_counts[] = { 26 };
static const struct snd_pcm_hw_constraint_list quantum_channel_list = {
	.count = ARRAY_SIZE(quantum_channel_counts),
	.list = quantum_channel_counts,
};

static const unsigned int quantum_rates[] = {
	44100, 48000, 88200, 96000, 176400, 192000,
};

static const struct snd_pcm_hw_constraint_list quantum_rate_list = {
	.count = ARRAY_SIZE(quantum_rates),
	.list = quantum_rates,
};

/*
 * The hardware's real DMA service granularity doubles/quadruples above
 * 48kHz and 96kHz (see quantum_hw_quantum()), so the set of period sizes
 * that are actually usable differs per rate. Publish the exact per-rate
 * table instead of one rate-independent list, so a properly-querying
 * client only ever sees period sizes that will actually work.
 */
static const unsigned int quantum_period_sizes_1x[] = { 32, 64, 128, 256, 512 };
static const unsigned int quantum_period_sizes_2x[] = { 64, 128, 256, 512 };
static const unsigned int quantum_period_sizes_4x[] = { 128, 256, 512 };

static int quantum_pcm_hw_rule_period_size(struct snd_pcm_hw_params *params,
					   struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *p = hw_param_interval(params,
						   SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	struct snd_interval *r = hw_param_interval(params,
						   SNDRV_PCM_HW_PARAM_RATE);
	const unsigned int *list = quantum_period_sizes_1x;
	unsigned int count = ARRAY_SIZE(quantum_period_sizes_1x);

	if (r->min > 96000) {
		list = quantum_period_sizes_4x;
		count = ARRAY_SIZE(quantum_period_sizes_4x);
	} else if (r->min > 48000) {
		list = quantum_period_sizes_2x;
		count = ARRAY_SIZE(quantum_period_sizes_2x);
	}

	return snd_interval_list(p, count, list, 0);
}

static int quantum_pcm_open(struct snd_pcm_substream *ss)
{
	struct quantum_chip *chip = ss->pcm->private_data;
	struct snd_pcm_runtime *runtime = ss->runtime;
	u32 channel_cfg;
	u32 channels;
	int err;

	if (quantum_device_gone(chip))
		return -ENODEV;

	if (chip->tci_fatal_error)
		return -EIO;

	if (!chip->tci_ready)
		return -EAGAIN;

	channel_cfg = quantum_read32(chip, QUANTUM_REG_CHANNEL_COUNTS);
	channels = ss->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		   ((channel_cfg >> 8) & 0xff) :
		   (channel_cfg & 0xff);

	if (!channels)
		return -EIO;

	if (channels != 26)
		return -EIO;

	runtime->hw.info = SNDRV_PCM_INFO_MMAP |
			   SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED |
			   SNDRV_PCM_INFO_BLOCK_TRANSFER;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.rates = SNDRV_PCM_RATE_44100 |
			    SNDRV_PCM_RATE_48000 |
			    SNDRV_PCM_RATE_88200 |
			    SNDRV_PCM_RATE_96000 |
			    SNDRV_PCM_RATE_176400 |
			    SNDRV_PCM_RATE_192000;
	runtime->hw.rate_min = 44100;
	runtime->hw.rate_max = 192000;
	runtime->hw.channels_min = channels;
	runtime->hw.channels_max = channels;
	runtime->hw.period_bytes_min = 32 * channels * sizeof(s32);
	runtime->hw.period_bytes_max = 512 * channels * sizeof(s32);
	runtime->hw.periods_min = 1;
	runtime->hw.periods_max = 1024;
	runtime->hw.buffer_bytes_max = 65536 * channels * sizeof(s32);

	err = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &quantum_rate_list);
	if (err < 0)
		return err;

	err = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
					 &quantum_channel_list);
	if (err < 0)
		return err;

	err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  quantum_pcm_hw_rule_period_size, NULL,
				  SNDRV_PCM_HW_PARAM_RATE, -1);
	if (err < 0)
		return err;

	err = snd_pcm_hw_constraint_integer(runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (err < 0)
		return err;

	err = snd_pcm_hw_constraint_minmax(runtime,
					   SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
					   64, 65536);
	if (err < 0)
		return err;

	return 0;
}

static int quantum_pcm_hw_params(struct snd_pcm_substream *ss,
				 struct snd_pcm_hw_params *params)
{
	struct quantum_chip *chip = ss->pcm->private_data;
	unsigned int rate = params_rate(params);
	unsigned int hw_quantum = quantum_hw_quantum(rate,
						     params_period_size(params));
	struct snd_pcm_substream *playback_substream;
	struct snd_pcm_substream *capture_substream;
	int err;
	int pages;

	if (quantum_device_gone(chip))
		return -ENODEV;

	if (chip->tci_fatal_error)
		return -EIO;

	if (!chip->tci_ready)
		return -EAGAIN;

	pages = snd_pcm_lib_malloc_pages(ss, params_buffer_bytes(params));
	if (pages < 0)
		return pages;

	mutex_lock(&chip->dma_mutex);

	if (quantum_device_gone(chip)) {
		err = -ENODEV;
		goto out_free_pages;
	}

	if (chip->pcm_configured &&
	    !(chip->pcm_configured & BIT(ss->stream)) &&
	    (chip->current_sample_rate != rate ||
	     chip->dma_quantum_frames != hw_quantum ||
	     chip->buffer_frames != params_buffer_size(params))) {
		err = -EBUSY;
		goto out_free_pages;
	}

	playback_substream = READ_ONCE(chip->playback_configured_substream);
	capture_substream = READ_ONCE(chip->capture_configured_substream);
	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		playback_substream = ss;
		WRITE_ONCE(chip->playback_configured_substream, ss);
	} else {
		capture_substream = ss;
		WRITE_ONCE(chip->capture_configured_substream, ss);
	}

	err = quantum_set_sample_rate(chip, rate);
	if (err < 0) {
		dev_err(&chip->pci->dev,
			"Set sample rate failed: %d\n", err);
		goto out_free_pages;
	}

	err = quantum_allocate_dma_resources(chip, rate,
					     params_period_size(params),
					     params_buffer_size(params),
					     playback_substream,
					     capture_substream);
	if (err) {
		dev_err(&chip->pci->dev,
			"DMA allocation failed: %d\n", err);
		goto out_free_pages;
	}

	chip->pcm_configured |= BIT(ss->stream);

	dev_dbg(&chip->pci->dev,
		"configured %u Hz, %u channels, period %lu, buffer %lu\n",
		rate, params_channels(params),
		(unsigned long)params_period_size(params),
		(unsigned long)params_buffer_size(params));

	mutex_unlock(&chip->dma_mutex);
	return 0;

out_free_pages:
	if (!(chip->pcm_configured & BIT(ss->stream))) {
		if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
			WRITE_ONCE(chip->playback_configured_substream, NULL);
		else
			WRITE_ONCE(chip->capture_configured_substream, NULL);
	}

	mutex_unlock(&chip->dma_mutex);
	snd_pcm_lib_free_pages(ss);
	return err;
}

static void quantum_pcm_forget_substream_locked(struct quantum_chip *chip,
						struct snd_pcm_substream *ss)
{
	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (READ_ONCE(chip->playback_substream) == ss) {
			chip->playback_attach_pending = false;
			WRITE_ONCE(chip->playback_substream, NULL);
			dev_info(&chip->pci->dev, "pcm forget: playback\n");
		}
		if (READ_ONCE(chip->playback_configured_substream) == ss)
			WRITE_ONCE(chip->playback_configured_substream, NULL);
	} else {
		if (READ_ONCE(chip->capture_substream) == ss) {
			chip->capture_attach_pending = false;
			WRITE_ONCE(chip->capture_substream, NULL);
			dev_info(&chip->pci->dev, "pcm forget: capture\n");
		}
		if (READ_ONCE(chip->capture_configured_substream) == ss)
			WRITE_ONCE(chip->capture_configured_substream, NULL);
	}
}

static int quantum_pcm_close(struct snd_pcm_substream *ss)
{
	struct quantum_chip *chip = ss->pcm->private_data;

	if (quantum_device_gone(chip))
		return 0;

	mutex_lock(&chip->dma_mutex);
	if (!quantum_device_gone(chip)) {
		dev_info(&chip->pci->dev,
			 "pcm close: %s state=%d active=%u configured=0x%x play=%u cap=%u\n",
			 ss->stream == SNDRV_PCM_STREAM_PLAYBACK ?
			 "playback" : "capture",
			 ss->runtime ? READ_ONCE(ss->runtime->state) : -1,
			 READ_ONCE(chip->stream_active), chip->pcm_configured,
			 !!READ_ONCE(chip->playback_substream),
			 !!READ_ONCE(chip->capture_substream));
		quantum_pcm_forget_substream_locked(chip, ss);
	}
	mutex_unlock(&chip->dma_mutex);

	return 0;
}

static int quantum_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct quantum_chip *chip = ss->pcm->private_data;
	unsigned int rate;
	unsigned int period;
	unsigned int buffer;
	bool restart;
	int err = 0;

	/* disconnect_sync() prevents final resource release until this returns. */
	if (quantum_device_gone(chip))
		return snd_pcm_lib_free_pages(ss);

	mutex_lock(&chip->dma_mutex);
	if (quantum_device_gone(chip)) {
		mutex_unlock(&chip->dma_mutex);
		return snd_pcm_lib_free_pages(ss);
	}

	rate = chip->current_sample_rate;
	period = chip->period_frames;
	buffer = chip->buffer_frames;

	/*
	 * This device has one shared audio DMA engine and one pair of global
	 * playback/capture page-table roots.  If one direction is freed while
	 * the opposite direction is still active, stop the engine, rebuild the
	 * global tables with dummy pages only for the freed direction, then
	 * restart the surviving stream.  Do not reject the later direction in
	 * hw_params(); full-duplex userspace commonly opens capture while
	 * playback is already running.
	 */
	restart = READ_ONCE(chip->stream_active) &&
		(ss->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		 READ_ONCE(chip->capture_substream) :
		 READ_ONCE(chip->playback_substream));

	dev_info(&chip->pci->dev,
		 "pcm hw_free: %s state=%d restart=%u active=%u configured=0x%x play=%u cap=%u\n",
		 ss->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		 "playback" : "capture",
		 ss->runtime ? READ_ONCE(ss->runtime->state) : -1,
		 restart, READ_ONCE(chip->stream_active), chip->pcm_configured,
		 !!READ_ONCE(chip->playback_substream),
		 !!READ_ONCE(chip->capture_substream));

	quantum_pcm_forget_substream_locked(chip, ss);
	chip->pcm_configured &= ~BIT(ss->stream);

	quantum_free_dma_resources(chip);
	if (chip->pcm_configured) {
		err = quantum_allocate_dma_resources(chip, rate, period, buffer,
						     READ_ONCE(chip->playback_configured_substream),
						     READ_ONCE(chip->capture_configured_substream));
		if (!err && restart)
			err = quantum_start_dma(chip);
	}

	mutex_unlock(&chip->dma_mutex);
	snd_pcm_lib_free_pages(ss);
	return err;
}

static void quantum_silence_playback_runtime(struct quantum_chip *chip,
					     struct snd_pcm_substream *ss)
{
	struct snd_pcm_runtime *runtime = ss->runtime;

	if (ss->stream != SNDRV_PCM_STREAM_PLAYBACK || !runtime)
		return;

	if (!runtime->dma_area || !runtime->dma_bytes)
		return;

	memset(runtime->dma_area, 0, runtime->dma_bytes);
	/* Ensure silence is visible before the hardware can fetch after START. */
	dma_wmb();

	chip->playback_period_accum = 0;
}

static int quantum_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct quantum_chip *chip = ss->pcm->private_data;

	if (quantum_device_gone(chip))
		return -ENODEV;

	if (chip->tci_fatal_error)
		return -EIO;

	if (!chip->tci_ready)
		return -EAGAIN;

	if (!READ_ONCE(chip->dma_resources_allocated))
		return -EINVAL;

	/*
	 * period_frames, buffer_frames, and the playback/capture period
	 * accumulators are also touched by the audio IRQ thread
	 * (quantum_process_audio() in quantum_main.c), which takes this same
	 * dma_mutex around its read/modify/write of these fields. Unlike that
	 * function, prepare() never calls snd_pcm_period_elapsed() (that only
	 * happens in the IRQ thread), so there is no reentrancy hazard in
	 * holding the lock for this whole function.
	 */
	mutex_lock(&chip->dma_mutex);

	if (READ_ONCE(ss->runtime->state) == SNDRV_PCM_STATE_XRUN) {
		if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			chip->playback_xruns++;
			quantum_silence_playback_runtime(chip, ss);
		} else {
			chip->capture_xruns++;
		}

		dev_warn_ratelimited(&chip->pci->dev,
				     "recovering %s XRUN (playback=%llu capture=%llu)\n",
			ss->stream == SNDRV_PCM_STREAM_PLAYBACK ?
			"playback" : "capture",
			(unsigned long long)chip->playback_xruns,
			(unsigned long long)chip->capture_xruns);
	} else if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK &&
		   READ_ONCE(chip->stream_active)) {
		/*
		 * Playback may be prepared while capture keeps the shared DMA engine
		 * running.  The hardware can fetch from the absolute ring slot before
		 * userspace has refilled it, so make stale contents deterministic.
		 */
		quantum_silence_playback_runtime(chip, ss);
	}

	chip->period_frames = ss->runtime->period_size;
	chip->buffer_frames = ss->runtime->buffer_size;

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		chip->playback_period_accum = 0;
	else
		chip->capture_period_accum = 0;

	mutex_unlock(&chip->dma_mutex);
	return 0;
}

static int quantum_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct quantum_chip *chip = ss->pcm->private_data;
	int err = 0;

	if (quantum_device_gone(chip))
		return -ENODEV;

	mutex_lock(&chip->dma_mutex);

	if (quantum_device_gone(chip)) {
		err = -ENODEV;
		goto out;
	}

	if (chip->tci_fatal_error) {
		err = -EIO;
		goto out;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/*
		 * If the opposite direction is already running, this substream is
		 * joining a shared hardware ring in the middle of a cycle.  Mark it
		 * pending so pointer() and period wakeups remain at zero until the
		 * ring naturally reaches frame zero.
		 */
		if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			chip->playback_attach_pending =
				READ_ONCE(chip->stream_active) &&
				READ_ONCE(chip->capture_substream) &&
				chip->last_dma_pos;
			chip->playback_period_accum = 0;
			WRITE_ONCE(chip->playback_substream, ss);
		} else {
			chip->capture_attach_pending =
				READ_ONCE(chip->stream_active) &&
				READ_ONCE(chip->playback_substream) &&
				chip->last_dma_pos;
			chip->capture_period_accum = 0;
			WRITE_ONCE(chip->capture_substream, ss);
		}

		if (!READ_ONCE(chip->stream_active)) {
			err = quantum_start_dma(chip);
			if (err) {
				if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
					WRITE_ONCE(chip->playback_substream, NULL);
				else
					WRITE_ONCE(chip->capture_substream, NULL);
			}
		}
		if (!err)
			dev_dbg(&chip->pci->dev,
				"pcm trigger: %s cmd=%d state=%d active=%u configured=0x%x play=%u cap=%u\n",
				ss->stream == SNDRV_PCM_STREAM_PLAYBACK ?
				"playback" : "capture", cmd,
				READ_ONCE(ss->runtime->state),
				READ_ONCE(chip->stream_active),
				chip->pcm_configured,
				!!READ_ONCE(chip->playback_substream),
				!!READ_ONCE(chip->capture_substream));
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			chip->playback_attach_pending = false;
			WRITE_ONCE(chip->playback_substream, NULL);
		} else {
			chip->capture_attach_pending = false;
			WRITE_ONCE(chip->capture_substream, NULL);
		}

		if (!READ_ONCE(chip->playback_substream) &&
		    !READ_ONCE(chip->capture_substream))
			quantum_stop_dma(chip);
		dev_dbg(&chip->pci->dev,
			"pcm trigger: %s cmd=%d state=%d active=%u configured=0x%x play=%u cap=%u\n",
			ss->stream == SNDRV_PCM_STREAM_PLAYBACK ?
			"playback" : "capture", cmd,
			READ_ONCE(ss->runtime->state),
			READ_ONCE(chip->stream_active),
			chip->pcm_configured,
			!!READ_ONCE(chip->playback_substream),
			!!READ_ONCE(chip->capture_substream));
		break;

	default:
		err = -EINVAL;
		break;
	}

out:
	mutex_unlock(&chip->dma_mutex);
	return err;
}

static int quantum_pcm_ack(struct snd_pcm_substream *ss)
{
	struct quantum_chip *chip = ss->pcm->private_data;

	if (quantum_device_gone(chip))
		return -ENODEV;

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		/* Ensure newly written playback frames are visible to DMA. */
		dma_wmb();

	return 0;
}

static snd_pcm_uframes_t
quantum_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct quantum_chip *chip = ss->pcm->private_data;
	struct snd_pcm_runtime *runtime = ss->runtime;
	u32 raw;
	u32 ring_pos;
	u64 absolute_pos;

	if (quantum_device_gone(chip))
		return SNDRV_PCM_POS_XRUN;

	if (!runtime || !runtime->buffer_size)
		return 0;

	if (!READ_ONCE(chip->stream_active) ||
	    !READ_ONCE(chip->dma_resources_allocated) ||
	    !chip->dma_ring_frames)
		return 0;

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (READ_ONCE(chip->playback_attach_pending))
			return 0;
	} else if (READ_ONCE(chip->capture_attach_pending)) {
		return 0;
	}

	raw = quantum_read32(chip, QUANTUM_REG_DMA_POSITION);
	if (raw == U32_MAX || quantum_device_gone(chip))
		return SNDRV_PCM_POS_XRUN;

	ring_pos = raw & 0x000fffff;
	if (ring_pos >= chip->dma_ring_frames)
		return 0;

	absolute_pos = ring_pos % runtime->buffer_size;
	return (snd_pcm_uframes_t)absolute_pos;
}

static const struct snd_pcm_ops quantum_ops = {
	.open = quantum_pcm_open,
	.close = quantum_pcm_close,
	.hw_params = quantum_pcm_hw_params,
	.hw_free = quantum_pcm_hw_free,
	.prepare = quantum_pcm_prepare,
	.trigger = quantum_pcm_trigger,
	.pointer = quantum_pcm_pointer,
	.ack = quantum_pcm_ack,
};

int snd_quantum_pcm_new(struct quantum_chip *chip)
{
	struct snd_pcm *pcm;
	int err;

	if (quantum_device_gone(chip))
		return -ENODEV;

	err = snd_pcm_new(chip->card, chip->model_id,
			  0, 1, 1, &pcm);
	if (err < 0)
		return err;

	pcm->private_data = chip;
	pcm->nonatomic = true;
	strscpy(pcm->name, chip->model_name,
		sizeof(pcm->name));
	chip->pcm = pcm;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&quantum_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
			&quantum_ops);

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV_SG,
					      &chip->pci->dev, 64 * 1024,
					      16 * 1024 * 1024);

	return 0;
}
