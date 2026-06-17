// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   ALSA driver for Reloop Jockey 3 devices
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>
#include <sound/pcm.h>
#include <linux/mutex.h>
#include <linux/cleanup.h>
#include "ploytec_proto.h"

#define RELOOP_VENDOR_ID         0x200c
#define RELOOP_JOCKEY3_ME_PID    0x1009
#define RELOOP_JOCKEY3_REMIX_PID 0x1037

enum { JOCKEY3_ME, JOCKEY3_REMIX };

#define JOCKEY3_N_URBS 8

/* Chip flags */
#define JOCKEY3_FLAG_DISCONNECTED 0

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

#define CARD_NAME "Reloop Jockey 3"

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for " CARD_NAME " soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for " CARD_NAME " soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable " CARD_NAME " soundcard.");

struct jockey3_chip {
	/* Core ALSA and USB handles (Mostly read-only after probe) */
	struct snd_card *card;
	struct usb_device *dev;
	struct usb_interface *intf0;
	struct usb_interface *intf1;
	struct snd_pcm *pcm;
	struct snd_rawmidi *rmidi;
	unsigned char *xfer_buf;
	struct mutex rate_mutex; // serializes sample rate changes and active stream tracking
	unsigned long flags;
	unsigned int current_rate;
	int active_streams;

	/* MIDI Path */
	struct snd_rawmidi_substream *midi_in_substream;
	struct snd_rawmidi_substream *midi_out_substream;
	struct urb *midi_in_urb;
	unsigned char *midi_in_buf;
	spinlock_t midi_lock; // protects MIDI substreams in completion handlers and rate-limiting
	unsigned int midi_out_acc;
	int midi_expected_data;
	u8 midi_last_status;
	u8 midi_queued_byte;
	bool midi_has_queued_byte;

	/* Playback Path */
	struct snd_pcm_substream *playback_substream;
	struct usb_anchor playback_anchor;
	struct urb *playback_urbs[JOCKEY3_N_URBS];
	unsigned char *playback_bufs[JOCKEY3_N_URBS];
	spinlock_t playback_lock; // protects playback stream state and buffer offsets
	unsigned int dma_off;
	unsigned int period_off;
	bool stream_running;

	/* Capture Path */
	struct snd_pcm_substream *capture_substream;
	struct usb_anchor capture_anchor;
	struct urb *capture_urbs[JOCKEY3_N_URBS];
	unsigned char *capture_bufs[JOCKEY3_N_URBS];
	spinlock_t capture_lock; // protects capture stream state and buffer offsets
	unsigned int capture_dma_off;
	unsigned int capture_period_off;
	bool capture_running;
};

static bool jockey3_process_out_packet(struct jockey3_chip *chip, u8 *urb_buf)
{
	struct snd_pcm_substream *substream = chip->playback_substream;
	struct snd_pcm_runtime *runtime;
	unsigned int pcm_buffer_size;
	unsigned int alsa_frame_size;
	int f;

	if (unlikely(!substream || !substream->runtime))
		return false;

	runtime = substream->runtime;
	if (unlikely(!runtime->dma_area))
		return false;

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(substream);
	alsa_frame_size = runtime->channels * 3;

	for (f = 0; f < PLOYTEC_PLAYBACK_FRAMES; f++) {
		ploytec_encode_s24_3le(urb_buf + f * PLOYTEC_PLAYBACK_FRAME_SIZE,
				       runtime->dma_area + chip->dma_off);
		chip->dma_off += alsa_frame_size;
		if (chip->dma_off >= pcm_buffer_size)
			chip->dma_off -= pcm_buffer_size;
		chip->period_off += alsa_frame_size;
	}

	if (chip->period_off >= runtime->period_size * alsa_frame_size) {
		chip->period_off %= runtime->period_size * alsa_frame_size;
		return true;
	}

	return false;
}

static bool jockey3_process_in_packet(struct jockey3_chip *chip, const u8 *urb_buf)
{
	struct snd_pcm_substream *substream = chip->capture_substream;
	struct snd_pcm_runtime *runtime;
	unsigned int pcm_buffer_size;
	unsigned int alsa_frame_size;
	int f;

	if (unlikely(!substream || !substream->runtime))
		return false;

	runtime = substream->runtime;
	if (unlikely(!runtime->dma_area))
		return false;

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(substream);
	alsa_frame_size = runtime->channels * 3; // 6 * 3 = 18 bytes

	for (f = 0; f < PLOYTEC_CAPTURE_FRAMES; f++) {
		ploytec_decode_s24_3le(runtime->dma_area + chip->capture_dma_off,
				       urb_buf + f * PLOYTEC_CAPTURE_FRAME_SIZE);
		chip->capture_dma_off += alsa_frame_size;
		if (chip->capture_dma_off >= pcm_buffer_size)
			chip->capture_dma_off -= pcm_buffer_size;
		chip->capture_period_off += alsa_frame_size;
	}

	if (chip->capture_period_off >= runtime->period_size * alsa_frame_size) {
		chip->capture_period_off %= runtime->period_size * alsa_frame_size;
		return true;
	}

	return false;
}

static void jockey3_capture_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	struct snd_pcm_substream *substream = NULL;
	bool period_elapsed = false;
	int ret;

	if (urb->status) {
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN)
			return;

		/* Fatal error: stop resubmitting to prevent interrupt storm */
		dev_err(&chip->intf0->dev, "Capture URB fatal error: %d\n",
			urb->status);
		set_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags);
		return;
	}

	if (unlikely(test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags)))
		return;

	scoped_guard(spinlock_irqsave, &chip->capture_lock) {
		if (chip->capture_running && chip->capture_substream) {
			period_elapsed = jockey3_process_in_packet(chip, urb->transfer_buffer);
			substream = chip->capture_substream;
		}
	}

	if (period_elapsed && substream)
		snd_pcm_period_elapsed(substream);

	usb_anchor_urb(urb, &chip->capture_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		usb_unanchor_urb(urb);
		if (ret != -ENODEV && ret != -EPERM)
			dev_err(&chip->intf0->dev, "Failed to resubmit capture URB: %d\n", ret);
	}
}

static u8 jockey3_get_next_midi_out_byte(struct jockey3_chip *chip)
{
	struct snd_rawmidi_substream *substream = chip->midi_out_substream;
	u8 byte = PLOYTEC_MIDI_IDLE_BYTE;
	u8 b;

	/*
	 * Rate limit MIDI to ~3125 bytes/sec. Sending at higher rates causes buffer
	 * overflows and message truncation in the device.
	 */
	chip->midi_out_acc += 3125;
	if (chip->midi_out_acc < (chip->current_rate / 10))
		return PLOYTEC_MIDI_IDLE_BYTE;

	chip->midi_out_acc -= (chip->current_rate / 10);

	if (chip->midi_has_queued_byte) {
		byte = chip->midi_queued_byte;
		chip->midi_has_queued_byte = false;
		dev_dbg(&chip->intf0->dev, "MIDI OUT: 0x%02x\n", byte);
		return byte;
	}

	if (!substream)
		return PLOYTEC_MIDI_IDLE_BYTE;

	if (snd_rawmidi_transmit(substream, &b, 1) != 1)
		return PLOYTEC_MIDI_IDLE_BYTE;

	/*
	 * Running Status Expansion:
	 * The Ploytec firmware's internal MIDI parser does not
	 * support Running Status (omitting the status byte when
	 * it hasn't changed). It expects every message to be
	 * complete (e.g., [Status, Data, Data]).
	 * Here we track the message state and re-inject the last
	 * status byte if a data byte is received when a message
	 * was already complete.
	 */
	if (b >= 0x80) { // Status byte
		if (b < 0xf8) { // Not a real-time message
			chip->midi_last_status = b;
			/* Determine expected data bytes based on MIDI opcode */
			if ((b & 0xf0) == 0xc0 || (b & 0xf0) == 0xd0)
				chip->midi_expected_data = 1; // PC, Channel Pressure
			else if (b < 0xf0)
				chip->midi_expected_data = 2; // Note On/Off, CC, etc.
			else
				chip->midi_expected_data = 0; // System messages
		}
		byte = b;
		dev_dbg(&chip->intf0->dev, "MIDI OUT: 0x%02x\n", byte);
	} else { // Data byte
		if (chip->midi_expected_data > 0) {
			byte = b;
			chip->midi_expected_data--;
			dev_dbg(&chip->intf0->dev, "MIDI OUT: 0x%02x\n", byte);
		} else if (chip->midi_last_status >= 0x80) {
			/* Message is complete but we got a data byte -> expand Running Status */
			byte = chip->midi_last_status;
			chip->midi_queued_byte = b;
			chip->midi_has_queued_byte = true;

			/* Set expectation for the remainder of the expanded message */
			if ((byte & 0xf0) == 0xc0 || (byte & 0xf0) == 0xd0)
				chip->midi_expected_data = 0; // 1 total, 1 already queued
			else
				chip->midi_expected_data = 1; // 2 total, 1 already queued

			dev_dbg(&chip->intf0->dev, "MIDI OUT: 0x%02x (Running Status)\n", byte);
		}
	}

	return byte;
}

static void jockey3_playback_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	unsigned char *buf = (unsigned char *)urb->transfer_buffer;
	struct snd_pcm_substream *substream = NULL;
	bool period_elapsed = false;
	int i, ret;

	if (urb->status) {
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN)
			return;

		/* Fatal error: stop resubmitting to prevent interrupt storm */
		dev_err(&chip->intf0->dev, "Playback URB fatal error: %d\n", urb->status);
		set_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags);
		return;
	}

	if (unlikely(test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags)))
		return;

	scoped_guard(spinlock_irqsave, &chip->playback_lock) {
		if (chip->stream_running && chip->playback_substream) {
			period_elapsed = jockey3_process_out_packet(chip, buf);
			substream = chip->playback_substream;
		} else {
			ploytec_prepare_out_packet(buf);
		}
	}

	if (period_elapsed && substream)
		snd_pcm_period_elapsed(substream);

	scoped_guard(spinlock_irqsave, &chip->midi_lock) {
		buf[PLOYTEC_MIDI_OUT_OFFSET] = jockey3_get_next_midi_out_byte(chip);
	}

	/* Ploytec Sync byte and gap padding */
	buf[PLOYTEC_SYNC_BYTE_OFFSET] = PLOYTEC_SYNC_BYTE_VALUE;
	for (i = PLOYTEC_SYNC_BYTE_OFFSET + 1; i < PLOYTEC_PKT_SIZE; i++)
		buf[i] = 0x00;

	usb_anchor_urb(urb, &chip->playback_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		usb_unanchor_urb(urb);
		if (ret != -ENODEV && ret != -EPERM)
			dev_err(&chip->intf0->dev, "Failed to resubmit playback URB: %d\n", ret);
	}
}

static void jockey3_midi_in_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	unsigned char *buf = (unsigned char *)urb->transfer_buffer;
	int i, ret;

	if (urb->status) {
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN)
			return;

		/* Fatal error: stop resubmitting to prevent interrupt storm */
		dev_err(&chip->intf0->dev, "MIDI IN URB fatal error: %d\n", urb->status);
		set_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags);
		return;
	}

	if (unlikely(test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags)))
		return;

	scoped_guard(spinlock_irqsave, &chip->midi_lock) {
		if (chip->midi_in_substream) {
			for (i = 0; i < urb->actual_length; i++) {
				if (buf[i] != PLOYTEC_MIDI_IDLE_BYTE && buf[i] != 0xF9) {
					dev_dbg(&chip->intf0->dev, "MIDI IN: 0x%02x\n",
						buf[i]);
					snd_rawmidi_receive(chip->midi_in_substream,
							    &buf[i], 1);
				}
			}
		}
	}

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0 && ret != -ENODEV && ret != -EPERM)
		dev_err(&chip->intf0->dev, "Failed to resubmit MIDI IN URB: %d\n", ret);
}

static void jockey3_stop_urbs(struct jockey3_chip *chip)
{
	dev_dbg(&chip->intf0->dev, "Stopping all URBs\n");
	usb_kill_urb(chip->midi_in_urb);
	usb_kill_anchored_urbs(&chip->playback_anchor);
	usb_kill_anchored_urbs(&chip->capture_anchor);
}

static void jockey3_start_urbs(struct jockey3_chip *chip)
{
	int i, ret;

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return;

	dev_dbg(&chip->intf0->dev, "Starting all URBs\n");
	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		usb_anchor_urb(chip->playback_urbs[i], &chip->playback_anchor);
		ret = usb_submit_urb(chip->playback_urbs[i], GFP_KERNEL);
		if (ret < 0) {
			usb_unanchor_urb(chip->playback_urbs[i]);
			dev_err(&chip->intf0->dev, "Failed to submit playback URB %d: %d\n",
				i, ret);
		}

		usb_anchor_urb(chip->capture_urbs[i], &chip->capture_anchor);
		ret = usb_submit_urb(chip->capture_urbs[i], GFP_KERNEL);
		if (ret < 0) {
			usb_unanchor_urb(chip->capture_urbs[i]);
			dev_err(&chip->intf0->dev, "Failed to submit capture URB %d: %d\n",
				i, ret);
		}
	}
	ret = usb_submit_urb(chip->midi_in_urb, GFP_KERNEL);
	if (ret < 0)
		dev_err(&chip->intf0->dev, "Failed to submit MIDI IN URB: %d\n", ret);
}

static int jockey3_set_rate(struct jockey3_chip *chip, unsigned int rate)
{
	int ret;

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return -ENODEV;

	dev_dbg(&chip->intf0->dev, "Setting rate to %u Hz\n", rate);
	ret = ploytec_set_rate(chip->dev, chip->xfer_buf, rate);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Failed to set rate: %d\n", ret);
		return ret;
	}
	dev_dbg(&chip->intf0->dev, "Rate set OK\n");
	return 0;
}

static int jockey3_pcm_open(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	dev_dbg(&chip->intf0->dev, "PCM open stream %d\n", substream->stream);

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return -ENODEV;

	runtime->hw.info =
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S24_3LE;
	runtime->hw.rates =
		SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 |
		SNDRV_PCM_RATE_96000;
	runtime->hw.rate_min = 44100;
	runtime->hw.rate_max = 96000;
	runtime->hw.buffer_bytes_max = 1024 * 1024;
	runtime->hw.period_bytes_min = 64;
	runtime->hw.period_bytes_max = 512 * 1024;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = 1024;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		runtime->hw.channels_min = 4;
		runtime->hw.channels_max = 4;
		chip->playback_substream = substream;
	} else {
		runtime->hw.channels_min = 6;
		runtime->hw.channels_max = 6;
		chip->capture_substream = substream;
	}

	scoped_guard(mutex, &chip->rate_mutex) {
		if (chip->active_streams > 0) {
			/* Force the new stream to match the existing hardware rate */
			ret = snd_pcm_hw_constraint_single(runtime,
							   SNDRV_PCM_HW_PARAM_RATE,
							   chip->current_rate);
			if (ret < 0)
				return ret;
		}
		chip->active_streams++;
		dev_dbg(&chip->intf0->dev, "active_streams incremented to %d\n",
			chip->active_streams);
	}

	return 0;
}

static int jockey3_pcm_close(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);

	dev_dbg(&chip->intf0->dev, "PCM close stream %d\n", substream->stream);

	guard(mutex)(&chip->rate_mutex);
	chip->active_streams--;
	dev_dbg(&chip->intf0->dev, "active_streams decremented to %d\n", chip->active_streams);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		guard(spinlock_irqsave)(&chip->playback_lock);
		chip->playback_substream = NULL;
		chip->stream_running = false;
	} else {
		guard(spinlock_irqsave)(&chip->capture_lock);
		chip->capture_substream = NULL;
		chip->capture_running = false;
	}
	return 0;
}

static int jockey3_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);

	dev_dbg(&chip->intf0->dev, "PCM prepare stream %d\n", substream->stream);

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return -ENODEV;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		guard(spinlock_irqsave)(&chip->playback_lock);
		chip->dma_off = 0;
		chip->period_off = 0;
	} else {
		guard(spinlock_irqsave)(&chip->capture_lock);
		chip->capture_dma_off = 0;
		chip->capture_period_off = 0;
	}
	return 0;
}

static int jockey3_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);

	dev_dbg(&chip->intf0->dev, "PCM trigger stream %d, cmd %d\n", substream->stream, cmd);

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return -ENODEV;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		guard(spinlock_irqsave)(&chip->playback_lock);
		if (cmd == SNDRV_PCM_TRIGGER_START)
			chip->stream_running = true;
		else if (cmd == SNDRV_PCM_TRIGGER_STOP)
			chip->stream_running = false;
	} else {
		guard(spinlock_irqsave)(&chip->capture_lock);
		if (cmd == SNDRV_PCM_TRIGGER_START)
			chip->capture_running = true;
		else if (cmd == SNDRV_PCM_TRIGGER_STOP)
			chip->capture_running = false;
	}

	return 0;
}

static snd_pcm_uframes_t jockey3_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int dma_off;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		scoped_guard(spinlock_irqsave, &chip->playback_lock) {
			dma_off = chip->dma_off;
		}
	} else {
		scoped_guard(spinlock_irqsave, &chip->capture_lock) {
			dma_off = chip->capture_dma_off;
		}
	}
	return bytes_to_frames(substream->runtime, dma_off);
}

static int jockey3_handshake_step(struct jockey3_chip *chip)
{
	int ret;

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return -ENODEV;

	ret = ploytec_handshake_step(chip->dev, chip->xfer_buf);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Ploytec handshake failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int jockey3_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int rate = params_rate(hw_params);
	int ret = 0;

	dev_dbg(&chip->intf0->dev, "PCM hw_params rate %u, active_streams %d\n",
		rate, chip->active_streams);

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return -ENODEV;

	scoped_guard(mutex, &chip->rate_mutex) {
		if (chip->current_rate == rate) {
			dev_dbg(&chip->intf0->dev, "Rate already set to %u, skipping change\n",
				rate);
			return 0;
		}

		/*
		 * If multiple streams are active, the ALSA core should have
		 * enforced the constraint from jockey3_pcm_open. We still
		 * sanity check here to be safe.
		 */
		if (chip->active_streams > 1) {
			dev_err(&chip->intf0->dev, "Cannot change rate while other stream is active\n");
			return -EBUSY;
		}

		jockey3_stop_urbs(chip);
		msleep(50);

		ret = jockey3_set_rate(chip, rate);
		if (ret != 0) {
			dev_err(&chip->intf0->dev, "Rate change to %u failed: %d\n", rate, ret);
			jockey3_start_urbs(chip);
			return ret;
		}
		chip->current_rate = rate;
	}

	dev_dbg(&chip->intf0->dev, "Rate changed to %u successfully, resetting device\n",
		rate);
	/*
	 * Ploytec firmware re-synchronization:
	 * Ploytec firmware require a full USB reset to re-synchronize the internal
	 * engine after a sample rate change. Without this, the Capture EP (0x86)
	 * often stalls or stops transmitting data, leading to EIO errors in ALSA.
	 *
	 * TODO: This behavior is currently kept as-is to match observed traces.
	 * There is an opportunity to improve or replace this once we have a
	 * better understanding of the Ploytec firmware interaction through
	 * further protocol analysis or reverse engineering.
	 *
	 * pre_reset/post_reset callbacks handle the URB lifecycle.
	 * We call this outside the rate_mutex to allow pre/post_reset to acquire it.
	 */
	usb_reset_device(chip->dev);

	return 0;
}

static const struct snd_pcm_ops jockey3_pcm_ops = {
	.open = jockey3_pcm_open,
	.close = jockey3_pcm_close,
	.hw_params = jockey3_pcm_hw_params,
	.prepare = jockey3_pcm_prepare,
	.trigger = jockey3_pcm_trigger,
	.pointer = jockey3_pcm_pointer,
};

static int jockey3_midi_in_open(struct snd_rawmidi_substream *substream)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return -ENODEV;
	return 0;
}

static int jockey3_midi_in_close(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static void jockey3_midi_in_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	guard(spinlock_irqsave)(&chip->midi_lock);
	chip->midi_in_substream = up ? substream : NULL;
}

static int jockey3_midi_out_open(struct snd_rawmidi_substream *substream)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	if (test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags))
		return -ENODEV;
	return 0;
}

static int jockey3_midi_out_close(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static void jockey3_midi_out_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	guard(spinlock_irqsave)(&chip->midi_lock);
	chip->midi_out_substream = up ? substream : NULL;
}

static const struct snd_rawmidi_ops jockey3_midi_in_ops = {
	.open = jockey3_midi_in_open,
	.close = jockey3_midi_in_close,
	.trigger = jockey3_midi_in_trigger
};

static const struct snd_rawmidi_ops jockey3_midi_out_ops = {
	.open = jockey3_midi_out_open,
	.close = jockey3_midi_out_close,
	.trigger = jockey3_midi_out_trigger
};

static int jockey3_handshake(struct jockey3_chip *chip)
{
	int ret;

	ret = jockey3_handshake_step(chip);
	if (ret < 0)
		return ret;

	chip->current_rate = 44100;
	ret = jockey3_set_rate(chip, 44100);
	if (ret < 0)
		return ret;
	msleep(20);

	dev_dbg(&chip->intf0->dev, "Handshake complete.\n");

	jockey3_start_urbs(chip);

	return 0;
}

static const struct usb_device_id jockey3_ids[] = {
	{ USB_DEVICE(RELOOP_VENDOR_ID, RELOOP_JOCKEY3_ME_PID), .driver_info = JOCKEY3_ME },
	{ USB_DEVICE(RELOOP_VENDOR_ID, RELOOP_JOCKEY3_REMIX_PID), .driver_info = JOCKEY3_REMIX },
	{}
};
MODULE_DEVICE_TABLE(usb, jockey3_ids);

static struct usb_driver jockey3_driver;

static void jockey3_release_intf1(void *data)
{
	struct usb_interface *intf1 = data;

	usb_driver_release_interface(&jockey3_driver, intf1);
}

static void jockey3_free_urb_action(void *data)
{
	usb_free_urb(data);
}

static void jockey3_kfree_action(void *data)
{
	kfree(data);
}

static void jockey3_stop_urbs_action(void *data)
{
	jockey3_stop_urbs(data);
}

static int jockey3_init_playback_urbs(struct jockey3_chip *chip)
{
	struct usb_device *dev = chip->dev;
	struct usb_interface *intf = chip->intf0;
	int i, ret;

	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		chip->playback_bufs[i] = kzalloc(PLOYTEC_PKT_SIZE, GFP_KERNEL);
		if (!chip->playback_bufs[i])
			return -ENOMEM;
		ret = devm_add_action_or_reset(&intf->dev, jockey3_kfree_action,
					       chip->playback_bufs[i]);
		if (ret)
			return ret;

		chip->playback_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!chip->playback_urbs[i])
			return -ENOMEM;
		ret = devm_add_action_or_reset(&intf->dev, jockey3_free_urb_action,
					       chip->playback_urbs[i]);
		if (ret)
			return ret;

		ploytec_prepare_out_packet(chip->playback_bufs[i]);

		usb_fill_bulk_urb(chip->playback_urbs[i], dev,
				  usb_sndbulkpipe(dev, PLOYTEC_EP_PCM_OUT),
				  chip->playback_bufs[i], PLOYTEC_PKT_SIZE,
				  jockey3_playback_callback, chip);
	}

	return 0;
}

static int jockey3_init_capture_urbs(struct jockey3_chip *chip)
{
	struct usb_device *dev = chip->dev;
	struct usb_interface *intf = chip->intf0;
	int i, ret;

	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		chip->capture_bufs[i] = kzalloc(PLOYTEC_PKT_SIZE, GFP_KERNEL);
		if (!chip->capture_bufs[i])
			return -ENOMEM;
		ret = devm_add_action_or_reset(&intf->dev, jockey3_kfree_action,
					       chip->capture_bufs[i]);
		if (ret)
			return ret;

		chip->capture_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!chip->capture_urbs[i])
			return -ENOMEM;
		ret = devm_add_action_or_reset(&intf->dev, jockey3_free_urb_action,
					       chip->capture_urbs[i]);
		if (ret)
			return ret;

		usb_fill_bulk_urb(chip->capture_urbs[i], dev,
				  usb_rcvbulkpipe(dev, PLOYTEC_EP_PCM_IN),
				  chip->capture_bufs[i], PLOYTEC_PKT_SIZE,
				  jockey3_capture_callback, chip);
	}

	return 0;
}

static int jockey3_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_interface *intf1;
	struct snd_card *card;
	struct jockey3_chip *chip;
	char *jockey3_type;
	int ret;
	static int dev_idx;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	intf1 = usb_ifnum_to_if(dev, 1);
	if (!intf1)
		return -ENODEV;

	while (dev_idx < SNDRV_CARDS && !enable[dev_idx])
		dev_idx++;

	if (dev_idx >= SNDRV_CARDS)
		return -ENODEV;

	ret = snd_devm_card_new(&intf->dev, index[dev_idx], id[dev_idx], THIS_MODULE,
				sizeof(struct jockey3_chip), &card);
	if (ret < 0)
		return ret;

	chip = card->private_data;
	chip->card = card;
	chip->dev = dev;
	chip->intf0 = intf;
	chip->intf1 = intf1;
	chip->midi_out_acc = 0;
	chip->flags = 0;
	spin_lock_init(&chip->midi_lock);
	spin_lock_init(&chip->playback_lock);
	spin_lock_init(&chip->capture_lock);
	mutex_init(&chip->rate_mutex);

	init_usb_anchor(&chip->playback_anchor);
	init_usb_anchor(&chip->capture_anchor);

	chip->xfer_buf = kmalloc(64, GFP_KERNEL);
	if (!chip->xfer_buf)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&intf->dev, jockey3_kfree_action, chip->xfer_buf);
	if (ret)
		return ret;

	chip->midi_in_buf = kmalloc(PLOYTEC_PKT_SIZE, GFP_KERNEL);
	if (!chip->midi_in_buf)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&intf->dev, jockey3_kfree_action, chip->midi_in_buf);
	if (ret)
		return ret;

	chip->midi_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!chip->midi_in_urb)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&intf->dev, jockey3_free_urb_action, chip->midi_in_urb);
	if (ret)
		return ret;

	ret = jockey3_init_playback_urbs(chip);
	if (ret < 0)
		return ret;

	ret = jockey3_init_capture_urbs(chip);
	if (ret < 0)
		return ret;

	usb_fill_bulk_urb(chip->midi_in_urb, dev,
			  usb_rcvbulkpipe(dev, PLOYTEC_EP_MIDI_IN),
			  chip->midi_in_buf, PLOYTEC_PKT_SIZE,
			  jockey3_midi_in_callback, chip);

	/* Stop all URBs on disconnect */
	ret = devm_add_action(&intf->dev, jockey3_stop_urbs_action, chip);
	if (ret)
		return ret;

	ret = snd_pcm_new(card, CARD_NAME " Audio", 0, 1, 1, &chip->pcm);
	if (ret < 0)
		return ret;

	strscpy(chip->pcm->name, CARD_NAME " Audio", sizeof(chip->pcm->name));
	chip->pcm->private_data = chip;
	snd_pcm_set_ops(chip->pcm, SNDRV_PCM_STREAM_PLAYBACK, &jockey3_pcm_ops);
	snd_pcm_set_ops(chip->pcm, SNDRV_PCM_STREAM_CAPTURE, &jockey3_pcm_ops);
	snd_pcm_set_managed_buffer_all(chip->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	ret = snd_rawmidi_new(card, CARD_NAME " MIDI", 0, 1, 1, &chip->rmidi);
	if (ret < 0)
		return ret;

	chip->rmidi->private_data = chip;
	strscpy(chip->rmidi->name, CARD_NAME " MIDI", sizeof(chip->rmidi->name));
	snd_rawmidi_set_ops(chip->rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &jockey3_midi_in_ops);
	snd_rawmidi_set_ops(chip->rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &jockey3_midi_out_ops);
	chip->rmidi->info_flags = SNDRV_RAWMIDI_INFO_INPUT |
				  SNDRV_RAWMIDI_INFO_OUTPUT |
				  SNDRV_RAWMIDI_INFO_DUPLEX;

	strscpy(card->driver, "snd-reloop-jockey3", sizeof(card->driver));
	strscpy(card->shortname, CARD_NAME, sizeof(card->shortname));

	switch (usb_id->driver_info) {
	case JOCKEY3_ME:
		jockey3_type = "Master Edition";
		break;
	case JOCKEY3_REMIX:
		jockey3_type = "Remix";
		break;
	default:
		jockey3_type = "Unknown";
	}
	snprintf(card->longname, sizeof(card->longname),
		 "%s %s at USB %s", CARD_NAME, jockey3_type, dev_name(&dev->dev));

	if (card->id[0] == '\0')
		snd_card_set_id(card, "RJ3");

	ret = usb_driver_claim_interface(&jockey3_driver, intf1, chip);
	if (ret < 0)
		return ret;
	ret = devm_add_action_or_reset(&intf->dev, jockey3_release_intf1, intf1);
	if (ret)
		return ret;

	usb_set_intfdata(intf, chip);
	ret = jockey3_handshake(chip);
	if (ret < 0)
		return ret;

	ret = snd_card_register(card);
	if (ret < 0)
		return ret;

	dev_idx++;
	return 0;
}

static void jockey3_disconnect(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	if (chip && intf == chip->intf0) {
		chip->stream_running = false;
		chip->capture_running = false;
		set_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags);
		/*
		 * Card cleanup, URB stopping/freeing, and interface release
		 * are all handled automatically by devres.
		 */
	}
	usb_set_intfdata(intf, NULL);
}

static int jockey3_pre_reset(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	if (chip && intf == chip->intf0) {
		mutex_lock(&chip->rate_mutex);
		jockey3_stop_urbs(chip);
	}
	return 0;
}

static int jockey3_post_reset(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	if (chip && intf == chip->intf0) {
		jockey3_handshake_step(chip);
		jockey3_start_urbs(chip);
		mutex_unlock(&chip->rate_mutex);
	}
	return 0;
}

static struct usb_driver jockey3_driver = {
	.name = "snd-reloop-jockey3",
	.probe = jockey3_probe,
	.disconnect = jockey3_disconnect,
	.pre_reset = jockey3_pre_reset,
	.post_reset = jockey3_post_reset,
	.id_table = jockey3_ids
};

module_usb_driver(jockey3_driver);

MODULE_AUTHOR("Frank van de Pol");
MODULE_DESCRIPTION(CARD_NAME " ALSA Driver");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: snd-pcm snd-rawmidi");
