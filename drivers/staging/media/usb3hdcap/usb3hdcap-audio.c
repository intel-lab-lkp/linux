// SPDX-License-Identifier: GPL-2.0
/*
 * USB3HDCAP audio support - CS53L21 init + ALSA capture device
 *
 * Audio is embedded in the isochronous video stream as 48kHz stereo
 * 16-bit LE PCM. Each video line carries 8, 12, or 16 bytes of audio data.
 */

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "usb3hdcap.h"

/* ALSA buffer sizing */
#define HDCAP_AUDIO_BUFSZ	(64 * 1024)

static const struct snd_pcm_hardware snd_hdcap_hw = {
	.info = SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.period_bytes_min = 1024,
	.period_bytes_max = 65536,
	.periods_min = 2,
	.periods_max = 128,
	.buffer_bytes_max = HDCAP_AUDIO_BUFSZ,
};

/* ------------------------------------------------------------------ */
/* PCM ops                                                            */
/* ------------------------------------------------------------------ */

static int snd_hdcap_pcm_open(struct snd_pcm_substream *substream)
{
	struct usb3hdcap *hdcap = snd_pcm_substream_chip(substream);

	hdcap->snd_substream = substream;
	substream->runtime->hw = snd_hdcap_hw;

	return 0;
}

static int snd_hdcap_pcm_close(struct snd_pcm_substream *substream)
{
	struct usb3hdcap *hdcap = snd_pcm_substream_chip(substream);

	atomic_set(&hdcap->snd_stream, 0);
	hdcap->snd_substream = NULL;

	return 0;
}

static int snd_hdcap_prepare(struct snd_pcm_substream *substream)
{
	struct usb3hdcap *hdcap = snd_pcm_substream_chip(substream);

	hdcap->snd_buffer_pos = 0;
	hdcap->snd_period_pos = 0;

	return 0;
}

static int snd_hdcap_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct usb3hdcap *hdcap = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		atomic_set(&hdcap->snd_stream, 1);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		atomic_set(&hdcap->snd_stream, 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t snd_hdcap_pointer(
	struct snd_pcm_substream *substream)
{
	struct usb3hdcap *hdcap = snd_pcm_substream_chip(substream);

	return hdcap->snd_buffer_pos;
}

static const struct snd_pcm_ops snd_hdcap_pcm_ops = {
	.open = snd_hdcap_pcm_open,
	.close = snd_hdcap_pcm_close,
	.prepare = snd_hdcap_prepare,
	.trigger = snd_hdcap_trigger,
	.pointer = snd_hdcap_pointer,
};

/* ------------------------------------------------------------------ */
/* Audio data feed (called from ISO URB handler in usb3hdcap-core.c)  */
/* ------------------------------------------------------------------ */

void usb3hdcap_audio_data(struct usb3hdcap *hdcap, const u8 *data, int len)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	size_t frame_bytes, nframes;
	size_t buffer_pos, period_pos;
	int period_elapsed = 0;
	unsigned long flags;

	if (!atomic_read(&hdcap->snd_stream))
		return;

	substream = hdcap->snd_substream;
	if (!substream)
		return;

	runtime = substream->runtime;
	if (!runtime || !runtime->dma_area)
		return;

	frame_bytes = runtime->frame_bits >> 3;
	nframes = len / frame_bytes;
	if (!nframes)
		return;

	snd_pcm_stream_lock_irqsave(substream, flags);

	buffer_pos = hdcap->snd_buffer_pos;
	period_pos = hdcap->snd_period_pos;

	/* Copy into ALSA ring buffer, handling wrap */
	if (buffer_pos + nframes >= runtime->buffer_size) {
		size_t cnt = (runtime->buffer_size - buffer_pos) * frame_bytes;

		memcpy(runtime->dma_area + buffer_pos * frame_bytes,
			data, cnt);
		memcpy(runtime->dma_area, data + cnt,
			nframes * frame_bytes - cnt);
	} else {
		memcpy(runtime->dma_area + buffer_pos * frame_bytes,
			data, nframes * frame_bytes);
	}

	buffer_pos += nframes;
	period_pos += nframes;

	if (buffer_pos >= runtime->buffer_size)
		buffer_pos -= runtime->buffer_size;

	if (period_pos >= runtime->period_size) {
		period_pos -= runtime->period_size;
		period_elapsed = 1;
	}

	hdcap->snd_buffer_pos = buffer_pos;
	hdcap->snd_period_pos = period_pos;

	snd_pcm_stream_unlock_irqrestore(substream, flags);

	if (period_elapsed)
		snd_pcm_period_elapsed(substream);
}

/* ------------------------------------------------------------------ */
/* CS53L21 codec init                                                 */
/* ------------------------------------------------------------------ */

int usb3hdcap_cs53l21_init(struct usb3hdcap *hdcap)
{
	int ret;

	/* MIC power control: power down mic B, mic A, and bias */
	/* SPEED = "10 - Half-Speed Mode (HSM) - 12.5 to 25 kHz sample rates" */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_MIC_PWR_CTL, 0x4e);
	if (ret)
		return ret;

	/* interface: I2S, master */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_IFACE_CTL, 0x44);
	if (ret)
		return ret;

	/* ADC input select: default */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_ADC_IN_SEL, 0x00);
	if (ret)
		return ret;

	/* ALC/PGA: 0 dB */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_ALC_PGAA, 0x00);
	if (ret)
		return ret;
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_ALC_PGAB, 0x00);
	if (ret)
		return ret;

	/* ADC attenuators: 0 dB */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_ADCA_ATT, 0x00);
	if (ret)
		return ret;
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_ADCB_ATT, 0x00);
	if (ret)
		return ret;

	/* ALC enable and attack rate = fastest attack */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_ALC_EN_ATK, 0xc0);
	if (ret)
		return ret;

	/* interface: "SPE Processed ADC data to ADC serial port, SDOUT data." */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_IFACE_CTL, 0x46);
	if (ret)
		return ret;

	/* SPE control: SPE enable, "Soft Ramp" turned on */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_SPE_CTL, 0x42);
	if (ret)
		return ret;

	/* ADC mixer volume: +2.5 dB */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_ADCA_MIX_VOL, 0x05);
	if (ret)
		return ret;
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_ADCB_MIX_VOL, 0x05);
	if (ret)
		return ret;

	/* channel mixer: off */
	ret = u3hc_i2c_write(hdcap, ADDR_CS53L21, CS53L21_CH_MIXER, 0x00);
	if (ret)
		return ret;

	return 0;
}

/* ------------------------------------------------------------------ */
/* ALSA card init / teardown                                          */
/* ------------------------------------------------------------------ */

int usb3hdcap_audio_init(struct usb3hdcap *hdcap)
{
	int ret;
	struct snd_card *card;
	struct snd_pcm *pcm;

	atomic_set(&hdcap->snd_stream, 0);

	ret = snd_card_new(hdcap->dev, SNDRV_DEFAULT_IDX1, "usb3hdcap",
		THIS_MODULE, 0, &card);
	if (ret < 0)
		return ret;

	strscpy(card->driver, "usb3hdcap", sizeof(card->driver));
	strscpy(card->shortname, "USB3HDCAP Audio",
		sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		"USB3HDCAP Audio at bus %d device %d",
		hdcap->usb_dev->bus->busnum, hdcap->usb_dev->devnum);

	hdcap->snd = card;

	ret = snd_pcm_new(card, "USB3HDCAP Audio", 0, 0, 1, &pcm);
	if (ret < 0)
		goto err;

	strscpy(pcm->name, "USB3HDCAP Audio Input", sizeof(pcm->name));
	pcm->info_flags = 0;
	pcm->private_data = hdcap;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_hdcap_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
		NULL, HDCAP_AUDIO_BUFSZ, HDCAP_AUDIO_BUFSZ);

	ret = snd_card_register(card);
	if (ret)
		goto err;

	dev_info(hdcap->dev, "ALSA audio device registered\n");
	return 0;

err:
	hdcap->snd = NULL;
	snd_card_free(card);
	return ret;
}

void usb3hdcap_audio_free(struct usb3hdcap *hdcap)
{
	if (hdcap->snd && hdcap->usb_dev) {
		snd_card_free_when_closed(hdcap->snd);
		hdcap->snd = NULL;
	}
}
