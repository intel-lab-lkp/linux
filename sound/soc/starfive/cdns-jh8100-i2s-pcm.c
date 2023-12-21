// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence Multi-Channel I2S controller PCM driver
 *
 * Copyright (c) 2022-2023 StarFive Technology Co., Ltd.
 */

#include <linux/io.h>
#include <linux/rcupdate.h>
#include <sound/pcm_params.h>
#include "cdns-jh8100-i2s.h"

#define PERIOD_BYTES_MIN	4096
#define BUFFER_BYTES_MAX	(3 * 2 * 8 * PERIOD_BYTES_MIN)
#define PERIODS_MIN		2

static unsigned int cdns_jh8100_i2s_pcm_tx(struct cdns_jh8100_i2s_dev *dev,
					   struct snd_pcm_runtime *runtime,
					   unsigned int tx_ptr, bool *period_elapsed,
					   snd_pcm_format_t format)
{
	unsigned int period_pos = tx_ptr % runtime->period_size;
	const u16 (*p16)[2] = (void *)runtime->dma_area;
	const u32 (*p32)[2] = (void *)runtime->dma_area;
	u32 data[2];
	int i;

	for (i = 0; i < CDNS_JH8100_I2S_FIFO_DEPTH; i++) {
		if (format == SNDRV_PCM_FORMAT_S16_LE) {
			data[0] = p16[tx_ptr][0];
			data[1] = p16[tx_ptr][1];
		} else if (format == SNDRV_PCM_FORMAT_S32_LE) {
			data[0] = p32[tx_ptr][0];
			data[1] = p32[tx_ptr][1];
		}

		iowrite32(data[0], dev->base + CDNS_JH8100_FIFO_MEM);
		iowrite32(data[1], dev->base + CDNS_JH8100_FIFO_MEM);
		period_pos++;
		if (++tx_ptr >= runtime->buffer_size)
			tx_ptr = 0;
	}

	*period_elapsed = period_pos >= runtime->period_size;
	return tx_ptr;
}

static unsigned int cdns_jh8100_i2s_pcm_rx(struct cdns_jh8100_i2s_dev *dev,
					   struct snd_pcm_runtime *runtime,
					   unsigned int rx_ptr, bool *period_elapsed,
					   snd_pcm_format_t format)
{
	unsigned int period_pos = rx_ptr % runtime->period_size;
	u16 (*p16)[2] = (void *)runtime->dma_area;
	u32 (*p32)[2] = (void *)runtime->dma_area;
	u32 data[2];
	int i;

	for (i = 0; i < CDNS_JH8100_I2S_FIFO_DEPTH; i++) {
		data[0] = ioread32(dev->base + CDNS_JH8100_FIFO_MEM);
		data[1] = ioread32(dev->base + CDNS_JH8100_FIFO_MEM);
		if (format == SNDRV_PCM_FORMAT_S16_LE) {
			p16[rx_ptr][0] = data[0];
			p16[rx_ptr][1] = data[1];
		} else if (format == SNDRV_PCM_FORMAT_S32_LE) {
			p32[rx_ptr][0] = data[0];
			p32[rx_ptr][1] = data[1];
		}

		period_pos++;
		if (++rx_ptr >= runtime->buffer_size)
			rx_ptr = 0;
	}

	*period_elapsed = period_pos >= runtime->period_size;
	return rx_ptr;
}

static const struct snd_pcm_hardware cdns_jh8100_i2s_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_RESUME,
	.rates = SNDRV_PCM_RATE_8000 |
		SNDRV_PCM_RATE_11025 |
		SNDRV_PCM_RATE_16000 |
		SNDRV_PCM_RATE_22050 |
		SNDRV_PCM_RATE_32000 |
		SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000,
	.rate_min = 8000,
	.rate_max = 48000,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		SNDRV_PCM_FMTBIT_S32_LE,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = BUFFER_BYTES_MAX,
	.period_bytes_min = PERIOD_BYTES_MIN,
	.period_bytes_max = BUFFER_BYTES_MAX / PERIODS_MIN,
	.periods_min = PERIODS_MIN,
	.periods_max = BUFFER_BYTES_MAX / PERIOD_BYTES_MIN,
	.fifo_size = 16,
};

static void cdns_jh8100_i2s_pcm_transfer(struct cdns_jh8100_i2s_dev *dev, bool push)
{
	struct snd_pcm_substream *substream;
	bool active, period_elapsed;

	rcu_read_lock();
	if (push)
		substream = rcu_dereference(dev->tx_substream);
	else
		substream = rcu_dereference(dev->rx_substream);

	active = substream && snd_pcm_running(substream);
	if (active) {
		unsigned int ptr;
		unsigned int new_ptr;

		if (push) {
			ptr = READ_ONCE(dev->tx_ptr);
			new_ptr = dev->tx_fn(dev, substream->runtime, ptr,
					&period_elapsed, dev->format);
			cmpxchg(&dev->tx_ptr, ptr, new_ptr);
		} else {
			ptr = READ_ONCE(dev->rx_ptr);
			new_ptr = dev->rx_fn(dev, substream->runtime, ptr,
					&period_elapsed, dev->format);
			cmpxchg(&dev->rx_ptr, ptr, new_ptr);
		}

		if (period_elapsed)
			snd_pcm_period_elapsed(substream);
	}
	rcu_read_unlock();
}

void cdns_jh8100_i2s_pcm_push_tx(struct cdns_jh8100_i2s_dev *dev)
{
	cdns_jh8100_i2s_pcm_transfer(dev, true);
}

void cdns_jh8100_i2s_pcm_pop_rx(struct cdns_jh8100_i2s_dev *dev)
{
	cdns_jh8100_i2s_pcm_transfer(dev, false);
}

static int cdns_jh8100_i2s_pcm_open(struct snd_soc_component *component,
				    struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct cdns_jh8100_i2s_dev *dev = snd_soc_dai_get_drvdata(snd_soc_rtd_to_cpu(rtd, 0));

	snd_soc_set_runtime_hwparams(substream, &cdns_jh8100_i2s_pcm_hardware);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	runtime->private_data = dev;

	return 0;
}

static int cdns_jh8100_i2s_pcm_close(struct snd_soc_component *component,
				     struct snd_pcm_substream *substream)
{
	synchronize_rcu();
	return 0;
}

static int cdns_jh8100_i2s_pcm_hw_params(struct snd_soc_component *component,
					 struct snd_pcm_substream *substream,
					 struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct cdns_jh8100_i2s_dev *dev = runtime->private_data;

	dev->format = params_format(hw_params);
	dev->tx_fn = cdns_jh8100_i2s_pcm_tx;
	dev->rx_fn = cdns_jh8100_i2s_pcm_rx;

	return 0;
}

static int cdns_jh8100_i2s_pcm_trigger(struct snd_soc_component *component,
				       struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct cdns_jh8100_i2s_dev *dev = runtime->private_data;
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			WRITE_ONCE(dev->tx_ptr, 0);
			rcu_assign_pointer(dev->tx_substream, substream);
		} else {
			WRITE_ONCE(dev->rx_ptr, 0);
			rcu_assign_pointer(dev->rx_substream, substream);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			rcu_assign_pointer(dev->tx_substream, NULL);
		else
			rcu_assign_pointer(dev->rx_substream, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static snd_pcm_uframes_t cdns_jh8100_i2s_pcm_pointer(struct snd_soc_component *component,
						     struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct cdns_jh8100_i2s_dev *dev = runtime->private_data;
	snd_pcm_uframes_t pos;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		pos = READ_ONCE(dev->tx_ptr);
	else
		pos = READ_ONCE(dev->rx_ptr);

	return pos < runtime->buffer_size ? pos : 0;
}

static int cdns_jh8100_i2s_pcm_new(struct snd_soc_component *component,
				   struct snd_soc_pcm_runtime *rtd)
{
	size_t size = cdns_jh8100_i2s_pcm_hardware.buffer_bytes_max;

	snd_pcm_set_managed_buffer_all(rtd->pcm,
				       SNDRV_DMA_TYPE_CONTINUOUS,
				       NULL, size, size);

	return 0;
}

static const struct snd_soc_component_driver cdns_jh8100_i2s_pcm_component = {
	.open		= cdns_jh8100_i2s_pcm_open,
	.close		= cdns_jh8100_i2s_pcm_close,
	.hw_params	= cdns_jh8100_i2s_pcm_hw_params,
	.trigger	= cdns_jh8100_i2s_pcm_trigger,
	.pointer	= cdns_jh8100_i2s_pcm_pointer,
	.pcm_construct	= cdns_jh8100_i2s_pcm_new,
};

int cdns_jh8100_i2s_pcm_register(struct platform_device *pdev)
{
	return devm_snd_soc_register_component(&pdev->dev,
					       &cdns_jh8100_i2s_pcm_component,
					       NULL, 0);
}
