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
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>
#include <sound/pcm.h>
#include <linux/mutex.h>
#include <linux/cleanup.h>
#include "ploytec_codec.h"

#define RELOOP_VENDOR_ID         0x200c
#define RELOOP_JOCKEY3_ME_PID    0x1009
#define RELOOP_JOCKEY3_REMIX_PID 0x1037

enum { JOCKEY3_ME, JOCKEY3_REMIX };

#define JOCKEY3_N_URBS 8

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;
static int debug;

#define CARD_NAME "Reloop Jockey 3"

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for " CARD_NAME " soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for " CARD_NAME " soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable " CARD_NAME " soundcard.");
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable debug messages for " CARD_NAME " soundcard.");

#define J3_DEBUG
#ifdef J3_DEBUG
#define j3_dbg(dev, fmt, ...) \
	do { \
		if (debug) \
			dev_info(dev, fmt, ##__VA_ARGS__); \
	} while (0)
#else
#define j3_dbg(dev, fmt, ...) \
	do { } while (0)
#endif

struct jockey3_chip {
	struct snd_card *card;
	struct usb_device *dev;
	struct usb_interface *intf0;
	struct usb_interface *intf1;
	unsigned char *xfer_buf;
	unsigned int current_rate;
	unsigned int midi_out_acc;
	struct mutex rate_mutex; // serializes sample rate changes and active stream tracking
	int active_streams;

	struct urb *midi_in_urb;
	unsigned char *midi_in_buf;
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_substream *midi_in_substream;
	struct snd_rawmidi_substream *midi_out_substream;
	spinlock_t midi_lock; // protects MIDI substreams in completion handlers and rate-limiting

	struct snd_pcm *pcm;
	struct snd_pcm_substream *playback_substream;
	struct urb *playback_urbs[JOCKEY3_N_URBS];
	unsigned char *playback_bufs[JOCKEY3_N_URBS];
	spinlock_t playback_lock; // protects playback stream state and buffer offsets
	unsigned int dma_off;
	unsigned int period_off;
	bool stream_running;

	struct snd_pcm_substream *capture_substream;
	struct urb *capture_urbs[JOCKEY3_N_URBS];
	unsigned char *capture_bufs[JOCKEY3_N_URBS];
	spinlock_t capture_lock; // protects capture stream state and buffer offsets
	unsigned int capture_dma_off;
	unsigned int capture_period_off;
	bool capture_running;
};

static void jockey3_process_out_packet(struct jockey3_chip *chip, u8 *urb_buf)
{
	struct snd_pcm_substream *substream = chip->playback_substream;
	struct snd_pcm_runtime *runtime;
	unsigned int pcm_buffer_size;
	unsigned int alsa_frame_size;
	int f;

	if (unlikely(!substream || !substream->runtime))
		return;

	runtime = substream->runtime;
	if (unlikely(!runtime->dma_area))
		return;

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
		snd_pcm_period_elapsed(substream);
	}
}

static void jockey3_process_in_packet(struct jockey3_chip *chip, const u8 *urb_buf)
{
	struct snd_pcm_substream *substream = chip->capture_substream;
	struct snd_pcm_runtime *runtime;
	unsigned int pcm_buffer_size;
	unsigned int alsa_frame_size;
	int f;

	if (unlikely(!substream || !substream->runtime))
		return;

	runtime = substream->runtime;
	if (unlikely(!runtime->dma_area))
		return;

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
		snd_pcm_period_elapsed(substream);
	}
}

static void jockey3_capture_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	int ret;

	if (urb->status) {
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN || urb->status == -EPROTO)
			return;
		dev_err(&chip->intf0->dev, "Capture URB error: %d\n",
			urb->status);
	} else {
		scoped_guard(spinlock_irqsave, &chip->capture_lock) {
			if (chip->capture_running && chip->capture_substream)
				jockey3_process_in_packet(chip, urb->transfer_buffer);
		}
	}

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0 && ret != -ENODEV && ret != -EPERM)
		dev_err(&chip->intf0->dev, "Failed to resubmit capture URB: %d\n", ret);
}

static void jockey3_playback_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	unsigned char *buf = (unsigned char *)urb->transfer_buffer;
	int i, ret;

	if (urb->status) {
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN || urb->status == -EPROTO)
			return;
		dev_err(&chip->intf0->dev, "Playback URB error: %d\n", urb->status);
	} else {
		guard(spinlock_irqsave)(&chip->playback_lock);
		if (chip->stream_running && chip->playback_substream)
			jockey3_process_out_packet(chip, buf);
		else
			memset(buf, 0, PLOYTEC_PKT_SIZE);

		guard(spinlock)(&chip->midi_lock);

		/*
		 * Rate limit MIDI to ~3125 bytes/sec (standard MIDI baud rate).
		 * The Ploytec firmware has a small MIDI buffer; sending at the
		 * PCM packet rate (~4410-4800 pkts/sec) causes buffer overflows
		 * and message truncation in the device.
		 */
		chip->midi_out_acc += 3125;
		if (chip->midi_out_acc >= (chip->current_rate / 10)) {
			chip->midi_out_acc -= (chip->current_rate / 10);
			if (chip->midi_out_substream) {
				u8 byte;

				if (snd_rawmidi_transmit(chip->midi_out_substream, &byte, 1) == 1)
					buf[480] = byte;
				else
					buf[480] = PLOYTEC_MIDI_IDLE_BYTE;
			} else {
				buf[480] = PLOYTEC_MIDI_IDLE_BYTE;
			}
		} else {
			buf[480] = PLOYTEC_MIDI_IDLE_BYTE;
		}

		/* Ploytec Sync byte and gap padding */
		buf[481] = 0xFF;
		for (i = 482; i < PLOYTEC_PKT_SIZE; i++)
			buf[i] = PLOYTEC_MIDI_IDLE_BYTE;
	}

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0 && ret != -ENODEV && ret != -EPERM)
		dev_err(&chip->intf0->dev, "Failed to resubmit playback URB: %d\n", ret);
}

static void jockey3_midi_in_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	unsigned char *buf = (unsigned char *)urb->transfer_buffer;
	int i, ret;

	if (urb->status) {
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN || urb->status == -EPROTO)
			return;
		dev_err(&chip->intf0->dev, "MIDI IN URB error: %d\n", urb->status);
	} else {
		scoped_guard(spinlock_irqsave, &chip->midi_lock) {
			if (chip->midi_in_substream) {
				for (i = 0; i < urb->actual_length; i++) {
					if (buf[i] != PLOYTEC_MIDI_IDLE_BYTE && buf[i] != 0xF9) {
						j3_dbg(&chip->intf0->dev, "MIDI IN: 0x%02x\n",
						       buf[i]);
						snd_rawmidi_receive(chip->midi_in_substream,
								    &buf[i], 1);
					}
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
	int i;

	j3_dbg(&chip->intf0->dev, "Stopping all URBs\n");
	usb_kill_urb(chip->midi_in_urb);
	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		usb_kill_urb(chip->playback_urbs[i]);
		usb_kill_urb(chip->capture_urbs[i]);
	}
}

static void jockey3_start_urbs(struct jockey3_chip *chip)
{
	int i, ret;

	j3_dbg(&chip->intf0->dev, "Starting all URBs\n");
	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		ret = usb_submit_urb(chip->playback_urbs[i], GFP_KERNEL);
		if (ret < 0)
			dev_err(&chip->intf0->dev, "Failed to submit playback URB %d: %d\n",
				i, ret);
		ret = usb_submit_urb(chip->capture_urbs[i], GFP_KERNEL);
		if (ret < 0)
			dev_err(&chip->intf0->dev, "Failed to submit capture URB %d: %d\n",
				i, ret);
	}
	ret = usb_submit_urb(chip->midi_in_urb, GFP_KERNEL);
	if (ret < 0)
		dev_err(&chip->intf0->dev, "Failed to submit MIDI IN URB: %d\n", ret);
}

static int jockey3_set_rate(struct jockey3_chip *chip, unsigned int rate)
{
	int ret;

	chip->xfer_buf[0] = rate & 0xFF;
	chip->xfer_buf[1] = (rate >> 8) & 0xFF;
	chip->xfer_buf[2] = (rate >> 16) & 0xFF;

	j3_dbg(&chip->intf0->dev, "Setting rate to %u Hz\n", rate);
	ret = usb_control_msg_send(chip->dev, 0, 0x01, 0x22, 0x0100, 0x0086,
				   chip->xfer_buf, 3, 2000, GFP_KERNEL);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Failed to set rate on EP 0x86: %d\n", ret);
		return ret;
	}
	j3_dbg(&chip->intf0->dev, "Rate set on EP 0x86 OK\n");
	msleep(50);
	ret = usb_control_msg_send(chip->dev, 0, 0x01, 0x22, 0x0100, 0x0005,
				   chip->xfer_buf, 3, 2000, GFP_KERNEL);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Failed to set rate on EP 0x05: %d\n", ret);
		return ret;
	}
	j3_dbg(&chip->intf0->dev, "Rate set on EP 0x05 OK\n");
	msleep(50);
	return 0;
}

static int jockey3_pcm_open(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	j3_dbg(&chip->intf0->dev, "PCM open stream %d\n", substream->stream);

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

	guard(mutex)(&chip->rate_mutex);
	chip->active_streams++;
	j3_dbg(&chip->intf0->dev, "active_streams incremented to %d\n", chip->active_streams);

	return 0;
}

static int jockey3_pcm_close(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);

	j3_dbg(&chip->intf0->dev, "PCM close stream %d\n", substream->stream);

	guard(mutex)(&chip->rate_mutex);
	chip->active_streams--;
	j3_dbg(&chip->intf0->dev, "active_streams decremented to %d\n", chip->active_streams);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		chip->playback_substream = NULL;
	else
		chip->capture_substream = NULL;
	return 0;
}

static int jockey3_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);

	j3_dbg(&chip->intf0->dev, "PCM prepare stream %d\n", substream->stream);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		chip->dma_off = 0;
		chip->period_off = 0;
	} else {
		chip->capture_dma_off = 0;
		chip->capture_period_off = 0;
	}
	return 0;
}

static int jockey3_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);

	j3_dbg(&chip->intf0->dev, "PCM trigger stream %d, cmd %d\n", substream->stream, cmd);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (cmd == SNDRV_PCM_TRIGGER_START)
			chip->stream_running = true;
		else if (cmd == SNDRV_PCM_TRIGGER_STOP)
			chip->stream_running = false;
	} else {
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

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return bytes_to_frames(substream->runtime, chip->dma_off);
	else
		return bytes_to_frames(substream->runtime, chip->capture_dma_off);
}

static int jockey3_handshake_step(struct jockey3_chip *chip)
{
	u8 status;
	int ret;

	ret = usb_set_interface(chip->dev, 0, 1);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Failed to set altsetting 1 on interface 0: %d\n", ret);
		return ret;
	}
	msleep(20);
	ret = usb_set_interface(chip->dev, 1, 1);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Failed to set altsetting 1 on interface 1: %d\n", ret);
		return ret;
	}
	msleep(20);

	ret = usb_control_msg_recv(chip->dev, 0, 0x56, 0xC0, 0, 0, chip->xfer_buf, 15, 2000,
				   GFP_KERNEL);
	if (ret < 0)
		j3_dbg(&chip->intf0->dev, "Handshake step 1 (0x56) failed: %d (ignoring)\n", ret);
	msleep(20);

	ret = usb_control_msg_recv(chip->dev, 0, 0x49, 0xC0, 0, 0, chip->xfer_buf, 1, 2000,
				   GFP_KERNEL);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Handshake step 2 (0x49 R) failed: %d\n", ret);
		return ret;
	}
	status = chip->xfer_buf[0];
	msleep(20);

	if (!(status & 0x20)) {
		ret = usb_control_msg_send(chip->dev, 0, 0x49, 0x40,
					   (uint16_t)(int16_t)(int8_t)(status | 0x20), 0,
					   NULL, 0, 2000, GFP_KERNEL);
		if (ret < 0) {
			dev_err(&chip->intf0->dev, "Handshake step 3 (0x49 W) failed: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int jockey3_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int rate = params_rate(hw_params);
	int ret = 0;

	j3_dbg(&chip->intf0->dev, "PCM hw_params rate %u, active_streams %d\n",
	       rate, chip->active_streams);

	guard(mutex)(&chip->rate_mutex);

	if (chip->current_rate == rate) {
		j3_dbg(&chip->intf0->dev, "Rate already set to %u, skipping change\n", rate);
		return 0;
	}

	if (chip->active_streams > 1) {
		dev_err(&chip->intf0->dev, "Cannot change rate while other stream is active\n");
		return -EBUSY;
	}

	jockey3_stop_urbs(chip);
	msleep(50);

	ret = jockey3_set_rate(chip, rate);
	if (ret == 0) {
		chip->current_rate = rate;
		j3_dbg(&chip->intf0->dev, "Rate changed to %u successfully, resetting device\n",
		       rate);
		/*
		 * Mandatory: Ploytec chipsets require a full USB reset to re-synchronize
		 * the internal engine after a sample rate change. Without this, the
		 * Capture EP (0x86) may stop transmitting data, leading to EIO.
		 * pre_reset/post_reset callbacks handle the URB lifecycle.
		 */
		usb_reset_device(chip->dev);
	} else {
		dev_err(&chip->intf0->dev, "Rate change to %u failed: %d\n", rate, ret);
		jockey3_start_urbs(chip);
	}

	return ret;
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

	j3_dbg(&chip->intf0->dev, "Handshake complete.\n");

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

static void jockey3_stop_urbs_action(void *data)
{
	jockey3_stop_urbs(data);
}

static int jockey3_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_interface *intf1;
	struct snd_card *card;
	struct jockey3_chip *chip;
	char *jockey3_type;
	int ret, i, j;
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
	spin_lock_init(&chip->midi_lock);
	spin_lock_init(&chip->playback_lock);
	spin_lock_init(&chip->capture_lock);
	mutex_init(&chip->rate_mutex);

	chip->xfer_buf = devm_kmalloc(&intf->dev, 64, GFP_KERNEL);
	if (!chip->xfer_buf)
		return -ENOMEM;

	chip->midi_in_buf = devm_kmalloc(&intf->dev, PLOYTEC_PKT_SIZE, GFP_KERNEL);
	if (!chip->midi_in_buf)
		return -ENOMEM;

	chip->midi_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!chip->midi_in_urb)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&intf->dev, jockey3_free_urb_action, chip->midi_in_urb);
	if (ret)
		return ret;

	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		chip->playback_bufs[i] = devm_kzalloc(&intf->dev, PLOYTEC_PKT_SIZE, GFP_KERNEL);
		if (!chip->playback_bufs[i])
			return -ENOMEM;

		chip->playback_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!chip->playback_urbs[i])
			return -ENOMEM;
		ret = devm_add_action_or_reset(&intf->dev, jockey3_free_urb_action,
					       chip->playback_urbs[i]);
		if (ret)
			return ret;

		/* Initial pattern: all MIDI positions are idle, sync byte at 481 */
		for (j = 480; j < PLOYTEC_PKT_SIZE; j++)
			chip->playback_bufs[i][j] = PLOYTEC_MIDI_IDLE_BYTE;
		chip->playback_bufs[i][481] = 0xFF;

		usb_fill_bulk_urb(chip->playback_urbs[i], dev,
				  usb_sndbulkpipe(dev, PLOYTEC_EP_PCM_OUT),
				  chip->playback_bufs[i], PLOYTEC_PKT_SIZE,
				  jockey3_playback_callback, chip);

		chip->capture_bufs[i] = devm_kzalloc(&intf->dev, PLOYTEC_PKT_SIZE, GFP_KERNEL);
		if (!chip->capture_bufs[i])
			return -ENOMEM;

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

	ret = snd_card_register(card);
	if (ret < 0)
		return ret;

	usb_set_intfdata(intf, chip);
	ret = jockey3_handshake(chip);
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

	if (chip && intf == chip->intf0)
		jockey3_stop_urbs(chip);
	return 0;
}

static int jockey3_post_reset(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	if (chip && intf == chip->intf0) {
		jockey3_handshake_step(chip);
		jockey3_start_urbs(chip);
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
