// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro / Pro FS - proprietary-mode USB audio driver
 *
 * Core driver: USB vendor requests + cold init, interrupt-URB PCM
 * streaming, mixer-state persistence across re-probes, and the card
 * lifecycle (probe/disconnect/module entry).
 *
 * See babyfacepro.h for the shared device state and register map,
 * and babyfacepro-ctl.c for the ALSA control surface.
 */
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "babyfacepro.h"

/* -- sample-rate / alt classes -------------------- */

/* rate = family_base << (alt - 1): three rate families (32k / 44.1k /
 * 48k), three interface speeds.  Measured on hardware, all nine rates,
 * through the family register alone (2026-09-16).
 *
 * 64 kHz is the 32 kHz family at x2, not a 64 kHz base at x1.  Both clock
 * 64 kHz at pitch 0, but a 64 kHz base on alt 1 needs 64000 x 56 B =
 * 3584 kB/s, which is exactly alt 1's 448-byte packet ceiling: any
 * positive varispeed then runs off the end of the bus, and the device
 * delivers the mirrored rate instead (asked +5 %, got -5 %).  At x2 the
 * frame is 40 B, so the same 64 kHz costs 2560 kB/s against a 5120 kB/s
 * ceiling.  Same reasoning puts 128 kHz on alt 3 rather than alt 2.
 */
static const struct bf_rate bf_rates[] = {
	{  32000, BF_ALT_1, 56,  8 },
	{  44100, BF_ALT_1, 56,  8 },
	{  48000, BF_ALT_1, 56,  8 },
	{  64000, BF_ALT_2, 40, 16 },
	{  88200, BF_ALT_2, 40, 16 },
	{  96000, BF_ALT_2, 40, 16 },
	{ 128000, BF_ALT_3, 32, 32 },
	{ 176400, BF_ALT_3, 32, 32 },
	{ 192000, BF_ALT_3, 32, 32 },
};

static const unsigned int bf_rate_list[ARRAY_SIZE(bf_rates)] = {
	32000, 44100, 48000, 64000, 88200,
	96000, 128000, 176400, 192000,
};

const struct snd_pcm_hw_constraint_list bf_rates_constraint = {
	.count = ARRAY_SIZE(bf_rate_list),
	.list = bf_rate_list,
	.mask = 0,
};

const struct bf_rate *bf_rate_lookup(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bf_rates); i++)
		if (bf_rates[i].rate == rate)
			return &bf_rates[i];
	return NULL;
}

unsigned int bf_rate_family(const struct bf_rate *r)
{
	switch (r->rate >> (r->alt - 1)) {
	case 32000:
		return 0;
	case 44100:
		return 1;
	default:
		return 2;
	}
}

/* -- vendor requests ----------------------- */

/* Timeout for every vendor control transfer (ms). */
#define BF_CTL_TIMEOUT		1000

int bf_vendor_write(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx)
{
	return usb_control_msg_send(chip->dev, 0, req,
				    USB_DIR_OUT | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE,
				    val, idx, NULL, 0, BF_CTL_TIMEOUT, GFP_KERNEL);
}

int bf_vendor_read(struct snd_usb_babyface *chip, u8 req, u16 idx, u8 *buf)
{
	return usb_control_msg_recv(chip->dev, 0, req,
				    USB_DIR_IN | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE,
				    0, idx, buf, 4, BF_CTL_TIMEOUT, GFP_KERNEL);
}

/* Composes and sends the BF_REG_KEEPALIVE_SETTINGS word from every
 * currently-tracked flag together (PROTOCOL.md: "keepalive 0x10 0x05CF
 * wVal = host settings-state register" - a single shared word, not
 * independent per-setting writes).  Only clock source is tracked so
 * far; the single call site this replaces (which used to hardcode
 * 0x0001, i.e. "always Internal") is why this exists as its own
 * function rather than an inline write at each call site - the next
 * flag added to this word (EQ for Record / Optical-Out SPDIF, this
 * driver's own upstream follow-up list) just OR's in here too, instead
 * of every caller needing to remember every other bit.
 */
int bf_settings_write(struct snd_usb_babyface *chip)
{
	u16 w = chip->clock_optical ? BF_SETTINGS_CLOCK_OPTICAL :
				       BF_SETTINGS_CLOCK_INTERNAL;

	return bf_vendor_write(chip, BF_REQ_KEEPALIVE, w,
			       BF_REG_KEEPALIVE_SETTINGS);
}

/* Set the sample rate: the family register.  The firmware derives its
 * own DDS word from it; SET_INTERFACE(5, alt) (done in hw_params) is the
 * x1/x2/x4 speed.  This is exactly what the RME Windows driver sends on
 * a rate change (USBPcap, 2026-09-14, all fourteen changes): the family,
 * then the settings word, nothing else.  Measured on this driver's
 * target hardware for all nine rates with no quad in play (2026-09-16),
 * and to -1 ppm at 44.1 kHz against an NTP-disciplined clock.
 *
 * An earlier version of this branch had this register and measured it as
 * dead.  It was not: bf_cold_init()'s 0x0021 -> 0x05ff write is this
 * same register (0x05ff = 0x05cf | 0x0030) hardcoded to the 48 kHz
 * family, and it followed the write.  See bf_cold_init().
 */
int bf_clock_write(struct snd_usb_babyface *chip)
{
	const struct bf_rate *r = bf_rate_lookup(chip->rate);
	int ret;

	if (!r)
		return -EINVAL;
	ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE,
			      bf_rate_family(r) << 4, BF_REG_RATE_FAMILY);
	if (ret < 0)
		return ret;
	return bf_settings_write(chip);
}

/* Varispeed: the 0x1B DDS quad.  It is a pitch RATIO applied on top of
 * the family rate, not a clock, so it carries no base rate and is the
 * same four words at every sample rate (measured 2026-09-16: a 48 kHz
 * quad over the 44.1 kHz family gives 44.1 kHz; the same quad at +5 %
 * gives 46305).  It is sticky: the device keeps the last ratio across
 * family writes, so pitch 0 must send the x1.0 quad rather than nothing.
 *
 * Only bank 2 is read on the Babyface Pro (one bank at a time swapped
 * between two quads: only bank 2 moved the clock).  It is the ratio's
 * period as a 24-bit word, 1.0 = 2^14 x 25 MHz / 48000 = 8533333.33, and
 * the firmware's constant is exact: sent exact it measures -1 ppm, sent
 * as RME's own Q16-truncated word (0x8234d3) it measures +14 ppm.  So
 * bank 2 is computed exactly.  Banks 0, 1 and 3 are sent as the RME
 * host software computes them - bank 0 the x1.0 period 12800000 scaled
 * by the pitch, banks 1 and 3 that value times fixed Q16 constants
 * (0xb9c2, 0xa3d7), truncated - so that a unit which reads a different
 * bank (the FS is not measured) sees exactly what Windows gives it.
 * Together this reproduces the cold-plug capture's quad at pitch 0 in
 * banks 0, 1 and 3, and improves on it in bank 2.
 *
 * Every quad must be followed by the settings keepalive or it does not
 * apply.  pitch is in 0.1 % steps, -50..50.
 */
int bf_pitch_write(struct snd_usb_babyface *chip, int pitch)
{
	const u64 num0 = 12800000ULL * 1000;		/* bank 0: 12800000 / (1 + p) */
	const u64 num2 = 409600000000000ULL;		/* bank 2: 4.096e11 * 1000 */
	u32 den0 = 1000 + pitch;
	u32 den2 = 48000 * den0;
	u64 b0 = div_u64(num0 + den0 / 2, den0);
	u64 b[4];
	int k, ret;

	b[0] = b0;
	b[1] = (b0 * 47554) >> 16;
	b[2] = div_u64(num2 + den2 / 2, den2);
	b[3] = (b0 * 41943) >> 16;

	for (k = 0; k < 4; k++) {
		ret = bf_vendor_write(chip, BF_REQ_DDS, (b[k] >> 8) & 0xffff,
				      ((b[k] & 0xff) << 8) | k);
		if (ret < 0)
			return ret;
	}
	return bf_settings_write(chip);
}

/* The cold-start init (cap_coldplug.pcap), verbatim from the user-space
 * reference (protocol::streaming_init).  Run at probe, where the
 * device state is unknown; a session start does not need it (see
 * babyface_stream_start()).
 */
int bf_cold_init(struct snd_usb_babyface *chip)
{
	const struct bf_rate *r;
	int ret, i;

	for (i = 0; i <= 0x3d; i++) {
		if (i == 0x1e || i == 0x1f)
			continue;
		ret = bf_vendor_write(chip, BF_REQ_REG_CLEAR, 0x0000, i);
		if (ret < 0)
			return ret;
	}
	/* The varispeed quad, where the coldplug capture sends its x1.0
	 * quad.  At pitch 0 banks 0, 1 and 3 match the capture; bank 2 is
	 * the exact ratio (see bf_pitch_write).
	 */
	ret = bf_pitch_write(chip, chip->pitch);
	if (ret < 0)
		return ret;
	/* 0x1C status - the hardware-validated reference (protocol::
	 * streaming_init) sends it as an OUT write; Windows reads it.
	 * Both are tolerated; match the validated path.
	 */
	ret = bf_vendor_write(chip, BF_REQ_STATUS_2, 0x0000, 0x0000);
	if (ret < 0)
		return ret;
	/* The settings word and the family register in one write (0x05ff =
	 * 0x05cf | 0x0030).  The coldplug capture hardcodes 0x0021 here:
	 * family 48 kHz, clock internal.  Sent for the rate actually in use,
	 * this IS the rate write; hardcoded, it was the thing that reset every
	 * earlier family write to 48 kHz.
	 */
	r = bf_rate_lookup(chip->rate);
	if (!r)
		return -EINVAL;
	ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE,
			      (bf_rate_family(r) << 4) |
			      (chip->clock_optical ? BF_SETTINGS_CLOCK_OPTICAL :
						     BF_SETTINGS_CLOCK_INTERNAL),
			      BF_REG_KEEPALIVE_INIT);
	if (ret < 0)
		return ret;
	/* 0x17 wIdx=0x0000 does NOT touch the preamp state (0x003F). */
	ret = bf_vendor_write(chip, BF_REQ_PREAMP, 0x000c, 0x0000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_PREAMP_COMMIT, 0x0000, 0x0000);
	if (ret < 0)
		return ret;
	for (i = 0; i < 2; i++) {
		ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0000, 0x3000);
		if (ret < 0)
			return ret;
	}
	for (i = 0; i < 3; i++) {
		ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0800, 0x0800);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* -- mixer-state persistence across interface re-probes --------
 * A userspace client can claim the proprietary interface via usbfs
 * (USBDEVFS_DISCONNECT_CLAIM - seen with PipeWire grabbing the
 * device when a stream targets the sink, and with the TuxMix
 * user-space daemon's libusb).  That detaches us and the card
 * disappears for the duration; on release the interface re-probes.
 * The device keeps its registers across the detach, but our cold
 * init clears them - so save the mixer state at disconnect and
 * restore it at the next probe.
 */

static LIST_HEAD(bf_saved_list);
static DEFINE_MUTEX(bf_saved_mutex);

/* Re-apply the whole cached mixer state to the device, whose registers
 * the cold init cleared.  Caller holds chip->mutex.
 */
int babyface_restore_state(struct snd_usb_babyface *chip)
{
	int out, src, mic, ret;

	/* Preamp state + commit. */
	ret = bf_preamp_state_write(chip);
	if (ret < 0)
		return ret;

	/* The four input gains. */
	for (mic = 0; mic < 4; mic++) {
		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      (u16)bf_gain_raw(mic, chip->gain[mic]),
				      BF_REG_GAIN + mic);
		if (ret < 0)
			return ret;
	}

	/* Masters (8-bit = the real volume) + mutes. */
	ret = bf_apply_masters(chip);
	if (ret < 0)
		return ret;

	/* Crosspoints (canonical out -> register block; AN1/2 also needs
	 * the low map, see bf_xpoint_write's own comment).
	 */
	for (out = 0; out < 6; out++) {
		unsigned int blk = bf_xpoint_block[out];

		for (src = 0; src < 14; src++) {
			ret = bf_xpoint_write(chip, out, src,
					      chip->xpoint[out][src][0],
					      chip->xpoint[out][src][1]);
			if (ret < 0)
				return ret;
		}
		ret = bf_crosspoint_clear_cross(chip, blk);
		if (ret < 0)
			return ret;
	}

	/* The rate (family register), then the varispeed ratio, which is
	 * sticky in the device and so must be re-sent even at pitch 0 in
	 * case something else wrote it while we were detached.
	 */
	ret = bf_clock_write(chip);
	if (ret < 0)
		return ret;
	return bf_pitch_write(chip, chip->pitch);
}

/* Re-apply the state that sits on top of the masters and crosspoints
 * (the routing flags, DIM, phase invert, stereo split, trim) after a
 * state restore.  The write patterns mirror the corresponding _put()
 * handlers.  Caller holds chip->mutex.
 */
int bf_state_apply_flags(struct snd_usb_babyface *chip)
{
	int out, ret, on_out = -1;
	u16 l, r;

	/* Loopback: the full 30-channel map from the cached state (the
	 * single-active invariant keeps at most one pair at 0x0001).
	 */
	for (out = 0; out < 6; out++) {
		if (chip->loopback[out]) {
			on_out = out;
			break;
		}
	}
	ret = bf_loopback_write_map(chip, on_out, on_out >= 0);
	if (ret < 0)
		return ret;

	ret = bf_vendor_write(chip, BF_REQ_PREAMP,
			      (chip->linked ? 0x0400 : 0x0000) |
			      (chip->an12 ? 0x1000 : 0x0000), 0x1000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_PREAMP_COMMIT, 0x0000, 0x0000);
	if (ret < 0)
		return ret;

	l = (u16)(((0x2000 * (100 + chip->width) / 2) + 50) / 100);
	r = 0x2000 - l;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l, 0x0000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r, 0x001a);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r, 0x0001);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l, 0x001b);
	if (ret < 0)
		return ret;

	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, chip->fx_send, 0x0138);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, chip->fx_send, 0x0153);
	if (ret < 0)
		return ret;

	if (chip->ms_proc) {
		/* Same ON pattern as bf_ms_put (cap_ms2.pcap): mute the AN2
		 * (side) crosspoints, both maps.
		 */
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x0035);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x004f);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x0001);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x001b);
		if (ret < 0)
			return ret;
	}

	/* Re-apply an engaged DIM (the fixed -20 dB Phones pair + flag).  The
	 * level DIM releases back to is not persisted: it always tracks the
	 * Phones master while DIM is engaged, so rebuild it here - otherwise a
	 * DIM released after a re-probe would drive Phones to
	 * silence (dim_saved still zero on the fresh chip).
	 */
	if (chip->dim) {
		chip->dim_saved[0] = chip->master[1][0];
		chip->dim_saved[1] = chip->master[1][1];
		ret = bf_vendor_write(chip, BF_REQ_GAIN, 0xcb,
				      BF_REG_MASTER_8 + 2 * 1);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_GAIN, 0xcb,
				      BF_REG_MASTER_8 + 2 * 1 + 1);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0333,
				      (BF_REG_MASTER_16 + 2 * 1) |
				      bf_flag_cycle[chip->flag_cnt]);
		if (ret < 0)
			return ret;
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0333,
				      (BF_REG_MASTER_16 + 2 * 1 + 1) |
				      bf_flag_cycle[chip->flag_cnt]);
		if (ret < 0)
			return ret;
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_PREAMP, 0x2000, 0x2000);
		if (ret < 0)
			return ret;
	}

	/* Re-apply any engaged Phase invert - the crosspoint restore loop
	 * above already re-wrote xpoint[][] as PLAIN values, so a phase
	 * negation needs to be re-asserted on top, the same way the ON
	 * state itself is applied (bf_phase_apply).
	 */
	{
		int mic;

		for (mic = 0; mic < 4; mic++) {
			if (!chip->phase[mic])
				continue;
			ret = bf_phase_apply(chip, mic, true);
			if (ret < 0)
				return ret;
		}
	}

	/* Re-apply any engaged stereo split (fixed constants, no fader
	 * dependency - see bf_split_apply's own comment).
	 */
	{
		int pb;

		for (pb = 0; pb < 6; pb++) {
			if (!chip->split[pb])
				continue;
			ret = bf_split_apply(chip, pb, true);
			if (ret < 0)
				return ret;
		}
	}

	/* Re-apply any non-zero Trim (fader+trim combined, same reasoning
	 * as phase - see bf_trim_apply's own comment). Only the pair's
	 * even index needs to fire this (it always writes both channels),
	 * and bf_trim_put keeps both entries of a pair equal, so the even
	 * index holds the value that is really on the wire even when the
	 * odd channel's control was the one the user touched.
	 */
	{
		int mic;

		for (mic = 0; mic < 4; mic += 2) {
			if (!chip->trim[mic])
				continue;
			ret = bf_trim_apply(chip, mic, chip->trim[mic] * 2);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

void bf_state_save(struct snd_usb_babyface *chip)
{
	struct bf_saved *s;
	const char *key = chip->dev->serial ? chip->dev->serial :
			  dev_name(&chip->dev->dev);
	bool found = false;

	mutex_lock(&bf_saved_mutex);
	list_for_each_entry(s, &bf_saved_list, list) {
		if (strcmp(s->key, key))
			continue;
		found = true;
		break;
	}
	if (!found) {
		s = kzalloc_obj(*s, GFP_KERNEL);
		if (!s) {
			mutex_unlock(&bf_saved_mutex);
			return;
		}
		strscpy(s->key, key, sizeof(s->key));
		list_add_tail(&s->list, &bf_saved_list);
	}

	s->preamp = chip->preamp;
	memcpy(s->gain, chip->gain, sizeof(s->gain));
	s->flag_cnt = chip->flag_cnt;
	memcpy(s->master, chip->master, sizeof(s->master));
	memcpy(s->muted, chip->muted, sizeof(s->muted));
	memcpy(s->xpoint, chip->xpoint, sizeof(s->xpoint));
	memcpy(s->phase, chip->phase, sizeof(s->phase));
	memcpy(s->trim, chip->trim, sizeof(s->trim));
	memcpy(s->split, chip->split, sizeof(s->split));
	s->ref_level = chip->ref_level;
	s->pitch = chip->pitch;
	memcpy(s->loopback, chip->loopback, sizeof(s->loopback));
	s->an12 = chip->an12;
	s->linked = chip->linked;
	s->ms_proc = chip->ms_proc;
	s->clock_optical = chip->clock_optical;
	s->width = chip->width;
	s->fx_send = chip->fx_send;
	s->dim = chip->dim;
	mutex_unlock(&bf_saved_mutex);
}

/* Copy a saved state (if any) into a freshly probed chip and push it
 * to the device.  Returns 1 when restored, -ENOENT when there is none,
 * or a negative error from the vendor writes.
 */
int bf_state_restore(struct snd_usb_babyface *chip)
{
	struct bf_saved *s;
	const char *key = chip->dev->serial ? chip->dev->serial :
			  dev_name(&chip->dev->dev);
	int ret = -ENOENT;

	mutex_lock(&bf_saved_mutex);
	list_for_each_entry(s, &bf_saved_list, list) {
		if (strcmp(s->key, key))
			continue;
		chip->preamp = s->preamp;
		memcpy(chip->gain, s->gain, sizeof(chip->gain));
		chip->flag_cnt = s->flag_cnt;
		memcpy(chip->master, s->master, sizeof(chip->master));
		memcpy(chip->muted, s->muted, sizeof(chip->muted));
		memcpy(chip->xpoint, s->xpoint, sizeof(chip->xpoint));
		memcpy(chip->phase, s->phase, sizeof(chip->phase));
		memcpy(chip->trim, s->trim, sizeof(chip->trim));
		memcpy(chip->split, s->split, sizeof(chip->split));
		chip->ref_level = s->ref_level;
		chip->pitch = s->pitch;
		memcpy(chip->loopback, s->loopback, sizeof(chip->loopback));
		chip->an12 = s->an12;
		chip->linked = s->linked;
		chip->ms_proc = s->ms_proc;
		chip->clock_optical = s->clock_optical;
		chip->width = s->width;
		chip->fx_send = s->fx_send;
		chip->dim = s->dim;
		ret = 1;
		break;
	}
	mutex_unlock(&bf_saved_mutex);
	if (ret != 1)
		return ret;

	mutex_lock(&chip->mutex);
	ret = babyface_restore_state(chip);
	if (ret == 0)
		ret = bf_state_apply_flags(chip);
	mutex_unlock(&chip->mutex);
	return ret ? ret : 1;
}

void bf_state_purge(void)
{
	struct bf_saved *s, *tmp;

	mutex_lock(&bf_saved_mutex);
	list_for_each_entry_safe(s, tmp, &bf_saved_list, list) {
		list_del(&s->list);
		kfree(s);
	}
	mutex_unlock(&bf_saved_mutex);
}

/* -- PCM data path ------------------------- */

static bool babyface_capture_copy(struct snd_usb_babyface *chip,
				  struct snd_pcm_substream *subs,
				  const u8 *data, unsigned int frames)
{
	struct snd_pcm_runtime *rt = subs->runtime;
	unsigned int buf_frames = rt->buffer_size;
	unsigned int words = chip->frame_bytes / 4;
	unsigned int chans = rt->channels;
	unsigned int pos, f, i;
	unsigned long new_period;
	bool crossed = false;
	u8 *dst;

	spin_lock(&chip->lock);
	pos = chip->hw_ptr[SNDRV_PCM_STREAM_CAPTURE] % buf_frames;
	for (f = 0; f < frames; f++) {
		const __le32 *w = (const __le32 *)(data + f * chip->frame_bytes);

		dst = rt->dma_area + frames_to_bytes(rt, pos);
		for (i = 0; i < chans; i++) {
			/* Channel map: app ch0-3 = device words 0-3 (AN1-4);
			 * app ch4-9 = words 6-11 (ADAT/SPDIF); app ch10/11 =
			 * words 12/13 = a FIXED-GAIN playback tap (observed
			 * 2026-08-25: the playback echoes there at ~-27 dB,
			 * independent of the output masters - NOT the output
			 * bus; the ADAT/SPDIF range is words 6-11 only).  The
			 * device words 4/5 are a fixed marker, not audio -
			 * skipped.  At 96/192 kHz the frame has fewer words;
			 * missing ones read as zero.
			 */
			static const u8 map[12] = { 0, 1, 2, 3, 6, 7, 8, 9,
						   10, 11, 12, 13 };
			u8 wi = i < 12 ? map[i] : 0xff;
			s32 s = 0;

			if (wi < words) {
				/* 24-bit sample in bits 31-8 of the 32-bit word;
				 * S32_LE with 24 msbits, so copy the word as-is.
				 */
				s = (s32)le32_to_cpu(w[wi]);
			}
			put_unaligned_le32((u32)s, dst + i * 4);
		}
		pos++;
		if (pos >= buf_frames)
			pos = 0;
	}
	chip->hw_ptr[SNDRV_PCM_STREAM_CAPTURE] += frames;
	new_period = chip->hw_ptr[SNDRV_PCM_STREAM_CAPTURE] / rt->period_size;
	if (new_period != chip->prev_period[SNDRV_PCM_STREAM_CAPTURE]) {
		chip->prev_period[SNDRV_PCM_STREAM_CAPTURE] = new_period;
		crossed = true;
	}
	spin_unlock(&chip->lock);

	return crossed;
}

static bool babyface_playback_copy(struct snd_usb_babyface *chip,
				   struct snd_pcm_substream *subs,
				   u8 *data, unsigned int frames)
{
	struct snd_pcm_runtime *rt = subs->runtime;
	unsigned int buf_frames = rt->buffer_size;
	unsigned int words = chip->frame_bytes / 4;
	unsigned int chans = rt->channels;
	unsigned int pos, f, i;
	unsigned int total = frames;	/* the URB buffer's full frame count */
	unsigned long new_period;
	bool crossed = false;
	const u8 *src;

	spin_lock(&chip->lock);
	/* Never copy frames the app has not prepared.  With several URBs in
	 * flight the device can transiently be ahead of the app ring at low
	 * periods, so hw_ptr may reach past appl_ptr; reading beyond appl_ptr
	 * would trip a spurious XRUN.  The device just repeats the last
	 * frames instead.  (Both counters are unbounded, so clamp on the
	 * signed difference.)
	 */
	{
		snd_pcm_sframes_t avail =
			(snd_pcm_sframes_t)(rt->control->appl_ptr -
					    chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK]);

		if (avail < 0)
			avail = 0;
		else if (avail > (snd_pcm_sframes_t)buf_frames)
			avail = buf_frames;
		if ((snd_pcm_uframes_t)avail < frames)
			frames = (unsigned int)avail;
	}
	pos = chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] % buf_frames;
	for (f = 0; f < frames; f++) {
		__le32 *w = (__le32 *)(data + f * chip->frame_bytes);

		src = rt->dma_area + frames_to_bytes(rt, pos);
		/* App ch n feeds the device word n (PB1-6 = words 0-11);
		 * words 12/13 stay zero.  At 96/192 kHz the frame is
		 * shorter - the extra app channels are dropped.
		 */
		for (i = 0; i < chans && i < words; i++) {
			u32 s = get_unaligned_le32(src + i * 4);

			/* S32_LE msbits=24: the app's 24 valid bits are already
			 * left-justified (bits 31-8) - copy the word as-is.
			 */
			w[i] = cpu_to_le32(s);
		}
		for (; i < words; i++)
			w[i] = 0;
		pos++;
		if (pos >= buf_frames)
			pos = 0;
	}
	/* A clamped copy wrote only [0, frames); clear the tail, or the device
	 * plays whatever the previous completion left in the URB buffer.
	 */
	if (frames < total)
		memset(data + frames * chip->frame_bytes, 0,
		       (total - frames) * chip->frame_bytes);
	chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] += frames;
	new_period = chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] / rt->period_size;
	if (new_period != chip->prev_period[SNDRV_PCM_STREAM_PLAYBACK]) {
		chip->prev_period[SNDRV_PCM_STREAM_PLAYBACK] = new_period;
		crossed = true;
	}
	spin_unlock(&chip->lock);

	return crossed;
}

/* Count a bad URB.  Once BF_URB_ERR_STOP of them are in a row, queue
 * stream_work to stop the session and return true: the URB must not be
 * resubmitted.  Some host controllers fail a URB on a broken endpoint
 * at once, without waiting for the bus, and a handler that resubmits
 * regardless can then spin in interrupt context until the work runs.
 */
static bool babyface_urb_error(struct snd_usb_babyface *chip)
{
	if (atomic_inc_return(&chip->urb_err) < BF_URB_ERR_STOP)
		return false;
	schedule_work(&chip->stream_work);
	return true;
}

/* A good URB ends a run of errors - unless the run has already reached
 * BF_URB_ERR_STOP.  The URBs that got it there were not resubmitted, so
 * the count has to stand until stream_work has seen it; clearing it
 * here would leave the session running on fewer and fewer URBs.
 */
static void babyface_urb_ok(struct snd_usb_babyface *chip)
{
	int err = atomic_read(&chip->urb_err);

	while (err && err < BF_URB_ERR_STOP &&
	       !atomic_try_cmpxchg(&chip->urb_err, &err, 0))
		;
}

/* An OUT URB that is not resubmitted leaves the queue. */
static void bf_out_lost(struct snd_usb_babyface *chip)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	if (chip->out_inflight)
		chip->out_inflight--;
	spin_unlock_irqrestore(&chip->lock, flags);
}

static void babyface_complete_in(struct urb *urb)
{
	struct snd_usb_babyface *chip = urb->context;
	struct snd_pcm_substream *subs;
	unsigned long flags;
	unsigned int frames;
	bool crossed = false;
	int ret;

	if (urb->status < 0) {
		if (urb->status == -ESHUTDOWN || urb->status == -ENOENT ||
		    urb->status == -ECONNRESET || urb->status == -ENODEV)
			return;		/* killed, or the device is gone */
		dev_dbg_ratelimited(&chip->dev->dev, "IN urb status %d\n",
				    urb->status);
		if (babyface_urb_error(chip))
			return;
		goto resubmit;
	}
	babyface_urb_ok(chip);
	spin_lock_irqsave(&chip->lock, flags);
	chip->in_done = ktime_get();
	spin_unlock_irqrestore(&chip->lock, flags);

	/* The session outlives a substream that is closed while the other
	 * direction still runs: close() waits out this RCU section.
	 */
	rcu_read_lock();
	subs = rcu_dereference(chip->subs[SNDRV_PCM_STREAM_CAPTURE]);
	if (subs) {
		snd_pcm_stream_lock_irqsave(subs, flags);
		if (snd_pcm_running(subs)) {
			frames = urb->actual_length / chip->frame_bytes;
			if (frames)
				crossed = babyface_capture_copy(chip, subs,
								urb->transfer_buffer,
								frames);
		}
		snd_pcm_stream_unlock_irqrestore(subs, flags);
		if (crossed)
			snd_pcm_period_elapsed(subs);
	}
	rcu_read_unlock();
resubmit:
	/* The session is being stopped: babyface_stream_kill() clears
	 * ->streaming before it kills the URBs.  A resubmit now would only
	 * fail against the kill and be counted as a stream error.
	 */
	if (!READ_ONCE(chip->streaming))
		return;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(&chip->dev->dev,
				    "IN resubmit failed: %d\n", ret);
		babyface_urb_error(chip);
	}
}

static void babyface_complete_out(struct urb *urb)
{
	struct snd_usb_babyface *chip = urb->context;
	struct snd_pcm_substream *subs;
	unsigned long flags;
	unsigned int frames;
	bool crossed = false;
	bool fed = false;
	int ret;

	if (urb->status < 0) {
		if (urb->status == -ESHUTDOWN || urb->status == -ENOENT ||
		    urb->status == -ECONNRESET || urb->status == -ENODEV)
			return;		/* killed, or the device is gone */
		dev_dbg_ratelimited(&chip->dev->dev, "OUT urb status %d\n",
				    urb->status);
		if (babyface_urb_error(chip)) {
			bf_out_lost(chip);
			return;
		}
		goto resubmit;
	}
	babyface_urb_ok(chip);

	rcu_read_lock();
	subs = rcu_dereference(chip->subs[SNDRV_PCM_STREAM_PLAYBACK]);
	if (subs) {
		snd_pcm_stream_lock_irqsave(subs, flags);
		if (snd_pcm_running(subs)) {
			frames = urb->transfer_buffer_length / chip->frame_bytes;
			crossed = babyface_playback_copy(chip, subs,
							 urb->transfer_buffer, frames);
			fed = true;
		}
		snd_pcm_stream_unlock_irqrestore(subs, flags);
		if (crossed)
			snd_pcm_period_elapsed(subs);
	}
	rcu_read_unlock();
	/* Nothing playing (no substream, or one that is set up but stopped,
	 * e.g. between an xrun and the restart): send silence, not the
	 * audio this URB carried last time round.
	 */
	if (!fed)
		memset(urb->transfer_buffer, 0, urb->transfer_buffer_length);
resubmit:
	/* The session is being stopped: babyface_stream_kill() clears
	 * ->streaming before it kills the URBs.  A resubmit now would only
	 * fail against the kill and be counted as a stream error.
	 */
	if (!READ_ONCE(chip->streaming))
		return;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(&chip->dev->dev,
				    "OUT resubmit failed: %d\n", ret);
		bf_out_lost(chip);
		babyface_urb_error(chip);
	}
}

void babyface_stream_kill(struct snd_usb_babyface *chip)
{
	int i;

	/* Clear the flag before the kills, not after.  The completion
	 * handlers test it before resubmitting, so a URB that completes
	 * while this loop runs retires instead of resubmitting into its
	 * own kill, which fails with -EPERM and counts towards
	 * stream_work.
	 */
	WRITE_ONCE(chip->streaming, false);
	for (i = 0; i < chip->nurbs; i++) {
		usb_kill_urb(chip->urbs_in[i]);
		usb_kill_urb(chip->urbs_out[i]);
	}
	chip->stream_stopped = ktime_get();
}

/* Stop both PCM substreams (if running) so apps blocked in read/write
 * wake with a clean error: XRUN for a recoverable stream error, or
 * DISCONNECTED when the card is going away.
 *
 * Call this after babyface_stream_kill(), not before: trigger START tests
 * ->streaming under the stream lock taken here, so with the flag already
 * clear a START either fails with -EPIPE or has finished and is stopped
 * below.  The other way round it can slip in between and leave a substream
 * running on a session that is gone.
 */
void babyface_pcm_stop_both(struct snd_usb_babyface *chip, snd_pcm_state_t state)
{
	unsigned long flags;
	int s;

	/* close() waits out this RCU section, as for the URB handlers. */
	rcu_read_lock();
	for (s = 0; s < 2; s++) {
		struct snd_pcm_substream *subs = rcu_dereference(chip->subs[s]);

		if (!subs)
			continue;
		snd_pcm_stream_lock_irqsave(subs, flags);
		if (snd_pcm_running(subs))
			snd_pcm_stop(subs, state);
		snd_pcm_stream_unlock_irqrestore(subs, flags);
	}
	rcu_read_unlock();
}

/* Stream model (big picture):
 *
 * The device has one USB session, shared by both directions.  Its
 * lifetime follows the substreams' setup, not their triggers - the
 * same pattern as the FireWire audio drivers:
 *
 *  - hw_params counts a substream as a user of the session;
 *  - prepare starts the session if it is not running: the rate write
 *    and the session trigger pair, then the interrupt URBs (nurbs in
 *    each direction) are submitted and the session is armed;
 *  - trigger START/STOP only decides whether the URB handlers move that
 *    substream's audio.  The URBs keep running either way, carrying
 *    silence while nothing plays;
 *  - hw_free drops the user, and the last one stops the session.
 *
 * A sound server restarts its streams with prepare + START after every
 * xrun.  That must not restart the session: PipeWire does not ride
 * through it, and an already connected client stays silent afterwards.
 */

/* Start the session.  Caller holds chip->mutex. */
static int babyface_stream_start(struct snd_usb_babyface *chip)
{
	unsigned int urbsize = chip->frame_bytes * chip->urb_frames;
	s64 since;
	int i, ret;

	/* A session triggered right after the previous one stopped starts
	 * with the outputs silent: give the device BF_SESSION_GAP_MS.
	 * Only a stop followed at once by a new setup waits (an application
	 * restarting, a probe opening the device repeatedly).
	 */
	since = ktime_ms_delta(ktime_get(), chip->stream_stopped);
	if (since < BF_SESSION_GAP_MS)
		msleep(BF_SESSION_GAP_MS - since);

	/* A session start is what the RME Windows driver sends
	 * (cap_audio): the trigger pair, the URBs, the arm.  The device
	 * keeps its mixer state between sessions, so nothing is cleared
	 * or re-applied here; the cold init runs at probe only.  The rate
	 * is written first: hw_params only re-points the speed multiplier.
	 */
	ret = bf_clock_write(chip);
	if (ret < 0)
		goto err;

	/* Stream trigger pair (cap_audio): 0x10 0x8000 + 0x1D. */
	ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0000, 0x8000);
	if (ret < 0)
		goto err;
	ret = bf_vendor_write(chip, BF_REQ_SESSION_START, 0x0000, 0x0000);
	if (ret < 0)
		goto err;

	for (i = 0; i < chip->urbs_active; i++) {
		/* Do not replay data from the previous stream/format. */
		memset(chip->buf_out[i], 0, urbsize);
		usb_fill_int_urb(chip->urbs_in[i], chip->dev,
				 usb_rcvintpipe(chip->dev, BF_EP_IN),
				 chip->buf_in[i], urbsize,
				 babyface_complete_in, chip, 1);
		usb_fill_int_urb(chip->urbs_out[i], chip->dev,
				 usb_sndintpipe(chip->dev, BF_EP_OUT),
				 chip->buf_out[i], urbsize,
				 babyface_complete_out, chip, 1);
		/* The buffers come from usb_alloc_coherent(), so
		 * they are already DMA-mapped: hand the HCD the
		 * mapping instead of letting it map them again.
		 * Without this, usb_hcd_map_urb_for_dma() calls
		 * dma_map_single() on a coherent allocation, which
		 * fails with -EAGAIN on any host where that
		 * allocation is a vmap (IOMMU-backed dma-iommu,
		 * e.g. amd_iommu in its default translated mode).
		 */
		chip->urbs_in[i]->transfer_dma = chip->dma_in[i];
		chip->urbs_in[i]->transfer_flags |=
			URB_NO_TRANSFER_DMA_MAP;
		chip->urbs_out[i]->transfer_dma = chip->dma_out[i];
		chip->urbs_out[i]->transfer_flags |=
			URB_NO_TRANSFER_DMA_MAP;
	}
	/* A count left over from the previous session would stop this one
	 * as soon as a URB errors, or through a stream_work still queued.
	 */
	atomic_set(&chip->urb_err, 0);
	/* Mark the session live before the first URB goes out, not once the
	 * state is restored: the URBs complete while the vendor writes below
	 * are still running, and ->streaming is what says the session is
	 * theirs to keep going.  The error path clears it again.
	 */
	WRITE_ONCE(chip->streaming, true);
	for (i = 0; i < chip->urbs_active; i++) {
		ret = usb_submit_urb(chip->urbs_in[i], GFP_KERNEL);
		if (ret < 0)
			goto err;
		ret = usb_submit_urb(chip->urbs_out[i], GFP_KERNEL);
		if (ret < 0)
			goto err;
	}
	spin_lock_irq(&chip->lock);
	chip->out_inflight = chip->urbs_active;
	spin_unlock_irq(&chip->lock);
	/* Session arm (cap_audio frame 5829, after the URBs). */
	ret = bf_vendor_write(chip, BF_REQ_SESSION_ARM, 0x0000, 0xc000);
	if (ret < 0)
		goto err;

	dev_dbg(&chip->dev->dev, "stream started (%u frames/URB, %u URBs)\n",
		chip->urb_frames, chip->urbs_active);
	return 0;

err:
	dev_err_ratelimited(&chip->dev->dev, "failed to start stream: %d\n",
			    ret);
	babyface_stream_kill(chip);
	return ret;
}

/* Persistent URB errors (bad link, device wedged): stop the session and
 * wake the apps with -EPIPE.  Their recovery (prepare + START) starts a
 * new session from a clean slate.  Runs in process context because
 * killing the URBs sleeps.
 */
void babyface_stream_work(struct work_struct *work)
{
	struct snd_usb_babyface *chip =
		container_of(work, struct snd_usb_babyface, stream_work);

	mutex_lock(&chip->mutex);
	if (!chip->shutdown &&
	    atomic_read(&chip->urb_err) >= BF_URB_ERR_STOP) {
		dev_err(&chip->dev->dev,
			"stream error: %d consecutive bad URBs, stopping (apps re-arm)\n",
			BF_URB_ERR_STOP);
		if (chip->streaming)
			babyface_stream_kill(chip);
		babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_XRUN);
		atomic_set(&chip->urb_err, 0);
	}
	mutex_unlock(&chip->mutex);
}

/* -- PCM --------------------------- */

static const struct snd_pcm_hardware babyface_pcm_hw = {
	/* The URB handlers copy to and from the vmalloc'ed ring the core
	 * allocates (rt->dma_area), and read appl_ptr from the control page,
	 * so the ring can be mapped by the application as well - which JACK
	 * and other mmap-only clients need.
	 */
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rate_min = 32000,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = 12,
	.buffer_bytes_max = 1 << 20,
	.period_bytes_max = 1 << 18,
	.periods_min = 2,
	.periods_max = 16,
};

/* The period of the direction that set the session up, in whole URBs:
 * half the URBs in flight, rounded up.  The other direction's period may
 * not be shorter.  Caller holds chip->mutex.
 */
static unsigned int bf_session_period(struct snd_usb_babyface *chip)
{
	return chip->urb_frames * DIV_ROUND_UP(chip->urbs_active, 2);
}

/* The URB size for a session, chosen from the period the application
 * asked for, as snd-usb-audio sizes its playback URBs: split the period
 * into the fewest URBs of at most frames_per_urb, and make each a whole
 * number of the device's IN packets (8/16/32 frames at alt 1/2/3; a URB
 * that ends inside a packet gets -EOVERFLOW), but not below
 * urb_frames_min.  A URB never exceeds the period, so a completion
 * crosses at most one period boundary.  Returns 0 if the rate's packet
 * does not fit.
 */
static unsigned int bf_urb_frames(struct snd_usb_babyface *chip,
				  const struct bf_rate *r,
				  unsigned int period)
{
	unsigned int pkt = r->min_fpu;
	unsigned int hi = rounddown(chip->frames_per_urb, pkt);
	unsigned int lo = roundup(max(chip->urb_frames_min, pkt), pkt);
	unsigned int urb;

	if (!hi || lo > hi || period < pkt)
		return 0;
	urb = rounddown(period / DIV_ROUND_UP(period, hi), pkt);
	urb = clamp(urb, lo, hi);
	return urb <= period ? urb : 0;
}

/* Does ANOTHER application hold the clock through the other direction?
 * The session lives from the first prepare to the last hw_free, so a
 * substream that is set up but not running - between an xrun and the
 * sound server's restart, say - still owns the rate it negotiated.  Not
 * "is it open": a client that has the device open but has not called
 * hw_params must still be able to pick the rate.
 *
 * The application that set the other direction up may change the rate
 * itself, as snd-hdspm allows for the process that holds both
 * directions: a DAW switches rate from its own settings without closing
 * the device.  Processes, not threads, since an application may open
 * playback and capture from different threads.
 *
 * Caller holds chip->mutex, which is what stream_setup[] and owner[] are
 * updated under.
 */
static bool bf_other_holds_clock(struct snd_usb_babyface *chip,
				 struct snd_pcm_substream *subs)
{
	int other = !subs->stream;

	return chip->stream_setup[other] &&
	       chip->owner[other] != chip->owner[subs->stream];
}

/* While another application holds the clock, offer its rate alone.  A
 * rule, not a constraint set in open(): it is evaluated each time the
 * application asks, so it follows the other direction being set up and
 * released, and who set it up.
 */
static int bf_hw_rule_rate(struct snd_pcm_hw_params *params,
			   struct snd_pcm_hw_rule *rule)
{
	struct snd_pcm_substream *subs = rule->private;
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	struct snd_interval t = { .integer = 1 };
	bool held;

	mutex_lock(&chip->mutex);
	held = bf_other_holds_clock(chip, subs);
	t.min = chip->rate;
	t.max = chip->rate;
	mutex_unlock(&chip->mutex);
	if (!held)
		return 0;
	return snd_interval_refine(hw_param_interval(params,
				   SNDRV_PCM_HW_PARAM_RATE), &t);
}

/* While another application holds the session, a period must span its
 * URBs: at least the session's period, in whole URBs.  A rule for the
 * same reason as the rate.
 */
static int bf_hw_rule_period(struct snd_pcm_hw_params *params,
			     struct snd_pcm_hw_rule *rule)
{
	struct snd_pcm_substream *subs = rule->private;
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	struct snd_interval t = { .integer = 1, .max = UINT_MAX };
	bool held;

	mutex_lock(&chip->mutex);
	held = bf_other_holds_clock(chip, subs);
	t.min = bf_session_period(chip);
	mutex_unlock(&chip->mutex);
	if (!held)
		return 0;
	return snd_interval_refine(hw_param_interval(params,
				   SNDRV_PCM_HW_PARAM_PERIOD_SIZE), &t);
}

static int babyface_pcm_open(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	struct snd_pcm_runtime *rt = subs->runtime;
	unsigned long flags;
	int ret;

	rt->hw = babyface_pcm_hw;
	ret = snd_pcm_hw_constraint_list(rt, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &bf_rates_constraint);
	if (ret < 0)
		return ret;
	/* The stream is 24-bit audio in a 32-bit container, left-justified
	 * (the device word carries the sample in bits 31-8).  Declare the
	 * valid-bit width so user-space knows to ignore the low 8 bits.
	 */
	ret = snd_pcm_hw_constraint_msbits(rt, 0, 32, 24);
	if (ret < 0)
		return ret;
	/* A period must span at least one URB, so a completion crosses at
	 * most one period boundary.  The URB size follows the period down to
	 * urb_frames_min; while another application holds the session, its
	 * period (in whole URBs) is the floor.  Constrain in frames (not
	 * bytes) so the minimum period does not balloon at low channel
	 * counts.
	 */
	ret = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					   chip->urb_frames_min, 1 << 18);
	if (ret < 0)
		return ret;
	ret = snd_pcm_hw_rule_add(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  bf_hw_rule_period, subs,
				  SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
	if (ret < 0)
		return ret;

	/* Both directions share one clock, so while another application's
	 * audio is set up the rate belongs to it.  Offer that rate alone: a
	 * client arriving later then negotiates down to it, and the sound
	 * server resamples, rather than the device changing rate underneath
	 * a running stream.  Advertising the rate rather than failing in
	 * hw_params() is what keeps a PipeWire sink alive - it picks the rate
	 * on offer instead of asking for one that has to be refused.
	 *
	 * This is what the vendor drivers do.  RME's settings panel greys the
	 * sample rate out during record/playback, and their manual is explicit
	 * that all active ASIO clients share one rate, with WDM (or CoreAudio)
	 * doing any conversion - never the driver.
	 */
	mutex_lock(&chip->mutex);
	chip->owner[subs->stream] = task_tgid_nr(current);
	mutex_unlock(&chip->mutex);
	ret = snd_pcm_hw_rule_add(rt, 0, SNDRV_PCM_HW_PARAM_RATE,
				  bf_hw_rule_rate, subs,
				  SNDRV_PCM_HW_PARAM_RATE, -1);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&chip->lock, flags);
	rcu_assign_pointer(chip->subs[subs->stream], subs);
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int babyface_pcm_close(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;

	flush_work(&chip->stream_work);
	mutex_lock(&chip->mutex);
	chip->owner[subs->stream] = 0;
	mutex_unlock(&chip->mutex);
	spin_lock_irqsave(&chip->lock, flags);
	RCU_INIT_POINTER(chip->subs[subs->stream], NULL);
	spin_unlock_irqrestore(&chip->lock, flags);
	/* The session keeps running while the other direction is set up,
	 * so a URB handler may still hold this substream: wait for it
	 * before the core frees the runtime.
	 */
	synchronize_rcu();
	return 0;
}

static int babyface_pcm_hw_params(struct snd_pcm_substream *subs,
				  struct snd_pcm_hw_params *params)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	const struct bf_rate *r;
	unsigned int urb, n;
	bool new_rate = false;
	int ret = 0;

	r = bf_rate_lookup(params_rate(params));
	if (!r)
		return -EINVAL;

	/* The stream URBs must be at least one alt packet wide: the device
	 * delivers its IN data in alt-sized packets (448/640/1024 B for
	 * alt 1/2/3), and a smaller URB buffer makes the host controller
	 * discard the transfer with -EOVERFLOW (babble) - seen at
	 * 176.4/192 kHz with frames_per_urb below 32.  Return a clean
	 * error instead of a silently dead capture stream.
	 */
	urb = bf_urb_frames(chip, r, params_period_size(params));
	if (!urb) {
		dev_err(&chip->dev->dev,
			"rate %u Hz, period %u: no URB size fits (packet %u frames, URBs %u..%u)\n",
			r->rate, params_period_size(params), r->min_fpu,
			chip->urb_frames_min, chip->frames_per_urb);
		return -EINVAL;
	}

	mutex_lock(&chip->mutex);
	/* disconnect() has let go of the interface: do not re-point it. */
	if (chip->shutdown) {
		ret = -ENODEV;
		goto out;
	}
	if (r->rate != chip->rate) {
		/* The rule in open() normally means no other application
		 * asks for a rate other than the running one.  It can still
		 * happen when the other direction was set up between this
		 * one's refine and its hw_params.  Refuse rather than retune
		 * - the alternative is a running stream silently changing
		 * pitch, with no error and no xrun, because a negotiated rate
		 * is never revised for a live substream.
		 */
		if (bf_other_holds_clock(chip, subs)) {
			dev_dbg(&chip->dev->dev,
				"rate %u Hz refused: %u Hz stream set up\n",
				r->rate, chip->rate);
			ret = -EBUSY;
			goto out;
		}
		/* Nothing else holds the clock.  Stop the session and
		 * re-point the bandwidth class; prepare starts a new session
		 * at the new rate.  If this application's other direction is
		 * still running, stop it with an xrun rather than let it
		 * change pitch; prepare() then refuses it until it is set up
		 * again at the new rate.
		 */
		if (chip->streaming) {
			babyface_stream_kill(chip);
			babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_XRUN);
		}
		ret = usb_set_interface(chip->dev, BF_IFACE, r->alt);
		if (ret < 0)
			goto out;
		chip->rate = r->rate;
		new_rate = true;
		chip->alt = r->alt;
		chip->frame_bytes = r->frame_bytes;
		dev_dbg(&chip->dev->dev, "rate %u Hz (alt %u)\n",
			chip->rate, chip->alt);
	}
	/* The first direction to be set up chooses the URB size and count
	 * from its period; the other one then needs a period at least as
	 * long, in whole URBs.  Another application must fit the session
	 * (open()'s rule offers no shorter period).  The application that
	 * holds it chooses again when it sets up the direction that chose,
	 * as a DAW changing its buffer does, when the other direction no
	 * longer fits, or when the rate changed (the URBs are whole packets
	 * of the rate).  A new size needs a new session: stop this one, and
	 * prepare starts the next.
	 */
	if (bf_other_holds_clock(chip, subs)) {
		if (params_period_size(params) < bf_session_period(chip)) {
			ret = -EBUSY;
			goto out;
		}
	} else if (new_rate || !chip->stream_setup[!subs->stream] ||
		   chip->session_dir == subs->stream ||
		   params_period_size(params) < bf_session_period(chip)) {
		/* URBs in flight for two periods, in whole URBs, and no more:
		 * the queue is latency.  Not the whole buffer - the other
		 * direction may be set up later with a smaller one, and it
		 * shares this queue.  Every buffer holds at least two periods,
		 * and the other direction's period is at least this one (in
		 * whole URBs, see bf_session_period()), so neither direction
		 * gets more queued than its own buffer.
		 */
		n = clamp_t(unsigned int,
			    2 * (params_period_size(params) / urb),
			    2, chip->nurbs);
		if (urb != chip->urb_frames || n != chip->urbs_active) {
			/* This application's other direction may still
			 * run: stop it with an xrun, prepare() refuses it
			 * until it fits the new session.
			 */
			if (chip->streaming) {
				babyface_stream_kill(chip);
				babyface_pcm_stop_both(chip,
						       SNDRV_PCM_STATE_XRUN);
			}
			chip->urb_frames = urb;
			chip->urbs_active = n;
			dev_dbg(&chip->dev->dev,
				"URBs: %u x %u frames for period %u, buffer %u\n",
				n, urb, params_period_size(params),
				params_buffer_size(params));
		}
		chip->session_dir = subs->stream;
	}
	/* hw_params can repeat without an hw_free in between: count the
	 * substream once.
	 */
	if (!chip->stream_setup[subs->stream]) {
		chip->stream_setup[subs->stream] = true;
		chip->stream_users++;
	}
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* The last substream to be released stops the session. */
static int babyface_pcm_hw_free(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);

	mutex_lock(&chip->mutex);
	if (chip->stream_setup[subs->stream]) {
		chip->stream_setup[subs->stream] = false;
		if (--chip->stream_users == 0 && chip->streaming) {
			babyface_stream_kill(chip);
			dev_dbg(&chip->dev->dev, "stream stopped\n");
		}
	}
	mutex_unlock(&chip->mutex);
	return 0;
}

/* Start the session unless it already runs - after an xrun the app
 * prepares again and this is a no-op for the hardware.
 */
static int babyface_pcm_prepare(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (chip->shutdown)
		ret = -ENODEV;
	else if (subs->runtime->rate != chip->rate ||
		 subs->runtime->period_size < bf_session_period(chip))
		/* The other direction changed the rate or the URBs since
		 * this one was set up: it has to be set up again.
		 */
		ret = -EINVAL;
	else if (!chip->streaming)
		ret = babyface_stream_start(chip);
	mutex_unlock(&chip->mutex);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&chip->lock, flags);
	chip->hw_ptr[subs->stream] = 0;
	chip->prev_period[subs->stream] = 0;
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int babyface_pcm_trigger(struct snd_pcm_substream *subs, int cmd)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;

	switch (cmd) {
	/* The session runs from prepare to hw_free; START and STOP only
	 * decide whether the URB handlers move this substream's audio.
	 */
	case SNDRV_PCM_TRIGGER_START:
		/* A URB error may have stopped the session since prepare. */
		if (!READ_ONCE(chip->streaming))
			return -EPIPE;
		spin_lock_irqsave(&chip->lock, flags);
		chip->hw_ptr[subs->stream] = 0;
		chip->prev_period[subs->stream] = 0;
		spin_unlock_irqrestore(&chip->lock, flags);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		return 0;
	}
	return -EINVAL;
}

/* The audio between hw_ptr and the converters, for runtime->delay.
 * hw_ptr moves a URB at a time: playback when a URB is refilled, capture
 * when one completes.  A refilled OUT URB goes to the back of the queue,
 * so the next frame the application writes waits behind the URBs in
 * flight, whatever they carry.
 *
 * Both directions take their time from the last IN completion, the
 * point where the device has finished a URB's worth of recording.  By
 * then it has also played one of the OUT URBs still counted in flight;
 * that URB's completion only says its data reached the device.  Since
 * the IN completion, the device has recorded, and played, the time
 * elapsed, measured with ktime and never more than one URB.
 * snd-usb-audio estimates it from the USB frame counter, which counts
 * in 1 ms steps, longer than a 32-frame URB at 48 kHz.  With one time
 * reference the estimate cancels in the sum of the two delays, the
 * round trip an application compensates a recording with.
 *
 * On top of that queue each direction adds a fixed delay inside the
 * device, per speed, in frames at the stream rate.  The values are what
 * RME's Windows driver reports for its ASIO buffer on a 2015 Babyface
 * Pro, which RTL Utility measured to within one frame.  Playback is
 * 8 + 32 x the speed: the device's OUT buffer and the DA converter,
 * the same on the FS.  Capture is lowered by 7 frames to the FS, whose
 * AD converter is 5 samples against 12 on the 2015 model, since the
 * two share VID:PID and bcdDevice.  So the 2015 model is reported a
 * little short, and neither model too long.  Caller holds chip->lock.
 */
static const struct {
	u8 playback;
	u8 capture;
} bf_device_delay[] = {
	[BF_ALT_1] = { 40, 16 },
	[BF_ALT_2] = { 72, 24 },
	[BF_ALT_3] = { 136, 38 },
};

static snd_pcm_uframes_t bf_delay(struct snd_usb_babyface *chip,
				  struct snd_pcm_substream *subs)
{
	s64 ns = ktime_to_ns(ktime_sub(ktime_get(), chip->in_done));
	unsigned int urb = chip->urb_frames;
	unsigned int moved;

	moved = ns > 0 ? min_t(u64, div_u64((u64)ns * subs->runtime->rate,
					    NSEC_PER_SEC), urb) : 0;
	if (subs->stream == SNDRV_PCM_STREAM_CAPTURE)
		return moved + bf_device_delay[chip->alt].capture;
	if (chip->out_inflight < 2)
		return 0;
	return (chip->out_inflight - 1) * urb - moved +
	       bf_device_delay[chip->alt].playback;
}

static snd_pcm_uframes_t babyface_pcm_pointer(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&chip->lock, flags);
	pos = chip->hw_ptr[subs->stream] % subs->runtime->buffer_size;
	subs->runtime->delay = bf_delay(chip, subs);
	spin_unlock_irqrestore(&chip->lock, flags);
	return pos;
}

static const struct snd_pcm_ops babyface_pcm_ops = {
	.open = babyface_pcm_open,
	.close = babyface_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = babyface_pcm_hw_params,
	.hw_free = babyface_pcm_hw_free,
	.prepare = babyface_pcm_prepare,
	.trigger = babyface_pcm_trigger,
	.pointer = babyface_pcm_pointer,
};

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static int frames_per_urb = BF_FRAMES_PER_URB_DEFAULT;
static int urb_frames_min = 32;
static int nurbs = BF_NURBS_DEFAULT;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the Babyface Pro sound card.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the Babyface Pro sound card.");
module_param(frames_per_urb, int, 0444);
MODULE_PARM_DESC(frames_per_urb, "Largest URB in audio frames, 8..1024 (256 = default); the period chooses the size below it.");
module_param(urb_frames_min, int, 0444);
MODULE_PARM_DESC(urb_frames_min, "Smallest URB in audio frames, 8..1024 (32 = default, 16 = low-latency floor).");
module_param(nurbs, int, 0444);
MODULE_PARM_DESC(nurbs, "Most URBs in flight per direction, 2..16 (8 = default).");

/* -- USB driver ------------------------- */

static void babyface_private_free(struct snd_card *card)
{
	struct snd_usb_babyface *chip = card->private_data;
	unsigned int urbsize;
	int i;

	if (!chip)
		return;

	/* The URB arrays are NULL when the probe failed before allocating
	 * them (snd_card_free runs private_free on any probe error).
	 */
	if (chip->urbs_in) {
		urbsize = BF_WORDS_PER_FRAME * sizeof(u32) * chip->frames_per_urb;
		for (i = 0; i < chip->nurbs; i++) {
			if (chip->urbs_in[i]) {
				usb_kill_urb(chip->urbs_in[i]);
				usb_free_urb(chip->urbs_in[i]);
			}
			if (chip->urbs_out && chip->urbs_out[i]) {
				usb_kill_urb(chip->urbs_out[i]);
				usb_free_urb(chip->urbs_out[i]);
			}
			/* probe() allocates the six arrays separately and can
			 * fail between them, so each may be NULL here - this runs
			 * on the probe error path via snd_card_free().
			 */
			if (chip->buf_in && chip->dma_in)
				usb_free_coherent(chip->dev, urbsize,
						  chip->buf_in[i], chip->dma_in[i]);
			if (chip->buf_out && chip->dma_out)
				usb_free_coherent(chip->dev, urbsize,
						  chip->buf_out[i], chip->dma_out[i]);
		}
	}
	kfree(chip->urbs_in);
	kfree(chip->urbs_out);
	kfree(chip->buf_in);
	kfree(chip->buf_out);
	kfree(chip->dma_in);
	kfree(chip->dma_out);
	usb_put_dev(chip->dev);
}

static int babyface_probe(struct usb_interface *intf,
			  const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct snd_usb_babyface *chip;
	struct snd_card *card;
	struct snd_pcm *pcm;
	unsigned int urbsize;
	u8 st[4];
	int i, err;

	if (intf->cur_altsetting->desc.bInterfaceNumber != BF_IFACE) {
		/* Only the proprietary audio interface is ours; the MIDI
		 * (standard class) and bulk interfaces stay unclaimed so
		 * snd-usb-audio can take the MIDI one.
		 */
		return -ENODEV;
	}

	err = snd_card_new(&intf->dev, index[0], id[0], THIS_MODULE,
			   sizeof(*chip), &card);
	if (err < 0) {
		dev_err(&intf->dev, "snd_card_new failed: %d\n", err);
		return err;
	}
	chip = card->private_data;
	chip->card = card;

	chip->dev = usb_get_dev(dev);
	/* USB autosuspend is untested: nothing in this driver holds a PM
	 * reference while streaming, so an autosuspend request could race
	 * a live stream.  Disable it explicitly rather than ship an
	 * untested code path - full autosuspend support (correct
	 * autopm_get/put pairing around the stream) is a deliberate
	 * follow-up, not an oversight.
	 */
	usb_disable_autosuspend(chip->dev);
	chip->iface = intf;
	chip->nurbs = clamp(nurbs, 2, 16);
	chip->frames_per_urb = clamp(frames_per_urb, 8, 1024) & ~7;
	chip->urb_frames_min = min_t(unsigned int,
				     clamp(urb_frames_min, 8, 1024) & ~7,
				     chip->frames_per_urb);
	chip->urb_frames = chip->frames_per_urb;
	chip->urbs_active = chip->nurbs;
	chip->rate = 48000;
	chip->alt = BF_ALT_1;
	chip->frame_bytes = 56;
	chip->preamp = BF_PREAMP_BASE;
	mutex_init(&chip->mutex);
	spin_lock_init(&chip->lock);
	atomic_set(&chip->urb_err, 0);
	INIT_WORK(&chip->stream_work, babyface_stream_work);
	chip->card->private_free = babyface_private_free;

	/* Model-neutral on purpose.  The FS and the original (2015)
	 * Babyface Pro share VID:PID 2a39:3fc0, report the same bcdDevice
	 * and the same iProduct shape, and neither says "FS" anywhere, so
	 * there is nothing to tell them apart at probe time - and the
	 * driver is reported to work unmodified on both.  card->driver in
	 * particular is what alsa-lib configs and UCM profiles match on,
	 * so it has to be right before this reaches a released kernel.
	 */
	strscpy(chip->card->driver, "BabyfacePro",
		sizeof(chip->card->driver));
	strscpy(chip->card->shortname, "Babyface Pro",
		sizeof(chip->card->shortname));
	snprintf(chip->card->longname, sizeof(chip->card->longname),
		 "RME Babyface Pro (proprietary mode) at %s",
		 dev_name(&dev->dev));

	/* If the id was not passed as a module option, derive it from the
	 * shortname with the whitespace removed, the way snd-usb-caiaq
	 * does.  Letting the core derive it instead yields the last word
	 * of the shortname ("Pro"), which is no use as an hw: name.
	 */
	if (*chip->card->id == '\0') {
		char cid[sizeof(chip->card->id)];
		const char *c;
		size_t len;

		memset(cid, 0, sizeof(cid));
		for (c = chip->card->shortname, len = 0;
		     *c && len < sizeof(cid) - 1; c++)
			if (*c != ' ')
				cid[len++] = *c;
		snd_card_set_id(chip->card, cid);
	}
	strscpy(chip->card->mixername, "Babyface Pro",
		sizeof(chip->card->mixername));

	/* alt 1 = the default 48-kHz bandwidth class. */
	err = usb_set_interface(dev, BF_IFACE, BF_ALT_1);
	if (err < 0) {
		dev_err(&intf->dev, "usb_set_interface failed: %d\n", err);
		goto error;
	}

	err = bf_cold_init(chip);
	if (err < 0) {
		dev_err(&intf->dev, "cold init failed: %d\n", err);
		goto error;
	}

	/* Sync the preamp state from the 0x17 readback (byte 0 mirrors
	 * the 48V/PAD bits; it persists across power cycles).
	 */
	err = bf_vendor_read(chip, BF_REQ_PREAMP, BF_REG_PREAMP, st);
	if (err < 0)
		dev_dbg(&intf->dev, "preamp readback failed: %d\n", err);
	else
		chip->preamp = st[0];

	/* Restore the mixer state saved at the last disconnect (if any);
	 * the device keeps its registers across a usbfs detach, but the
	 * cold init above cleared them, so push the user's settings back.
	 */
	err = bf_state_restore(chip);
	if (err == -ENOENT) {
		/* No saved state: the 0x16 clear zeroed the mixer registers,
		 * so restore the factory default routing to keep the outputs
		 * live out of the box.
		 */
		err = babyface_write_default_mixer(chip);
		if (err < 0) {
			dev_err(&intf->dev, "default mixer restore failed: %d\n", err);
			goto error;
		}
		/* The cold init cleared the flag registers as well. */
		mutex_lock(&chip->mutex);
		err = bf_state_apply_flags(chip);
		mutex_unlock(&chip->mutex);
		if (err < 0) {
			dev_err(&intf->dev, "flag restore failed: %d\n", err);
			goto error;
		}
	} else if (err < 0) {
		dev_err(&intf->dev, "mixer state restore failed: %d\n", err);
		goto error;
	}

	/* Keep the allocation size independent of the active USB mode. */
	urbsize = BF_WORDS_PER_FRAME * sizeof(u32) * chip->frames_per_urb;
	chip->urbs_in = kcalloc(chip->nurbs, sizeof(*chip->urbs_in), GFP_KERNEL);
	chip->urbs_out = kcalloc(chip->nurbs, sizeof(*chip->urbs_out), GFP_KERNEL);
	chip->buf_in = kcalloc(chip->nurbs, sizeof(*chip->buf_in), GFP_KERNEL);
	chip->buf_out = kcalloc(chip->nurbs, sizeof(*chip->buf_out), GFP_KERNEL);
	chip->dma_in = kcalloc(chip->nurbs, sizeof(*chip->dma_in), GFP_KERNEL);
	chip->dma_out = kcalloc(chip->nurbs, sizeof(*chip->dma_out), GFP_KERNEL);
	if (!chip->urbs_in || !chip->urbs_out || !chip->buf_in ||
	    !chip->buf_out || !chip->dma_in || !chip->dma_out)
		goto error;

	for (i = 0; i < chip->nurbs; i++) {
		chip->urbs_in[i] = usb_alloc_urb(0, GFP_KERNEL);
		chip->urbs_out[i] = usb_alloc_urb(0, GFP_KERNEL);
		chip->buf_in[i] = usb_alloc_coherent(dev, urbsize, GFP_KERNEL,
						     &chip->dma_in[i]);
		chip->buf_out[i] = usb_alloc_coherent(dev, urbsize, GFP_KERNEL,
						      &chip->dma_out[i]);
		if (!chip->urbs_in[i] || !chip->urbs_out[i] ||
		    !chip->buf_in[i] || !chip->buf_out[i])
			goto error;
	}

	err = snd_pcm_new(chip->card, "Babyface Pro", 0, 1, 1, &pcm);
	if (err < 0) {
		dev_err(&intf->dev, "snd_pcm_new failed: %d\n", err);
		goto error;
	}
	pcm->private_data = chip;
	strscpy(pcm->name, "Babyface Pro", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &babyface_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &babyface_pcm_ops);

	/* The PCM buffer is host-side (the URB callbacks copy in/out of
	 * it); vmalloc is the standard choice for that.
	 */
	err = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC,
					     NULL, 0, 1 << 20);
	if (err < 0) {
		dev_err(&intf->dev, "buffer allocation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_masters(chip);
	if (err < 0) {
		dev_err(&intf->dev, "master control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_xpoints(chip);
	if (err < 0) {
		dev_err(&intf->dev, "crosspoint creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_trim(chip);
	if (err < 0) {
		dev_err(&intf->dev, "phase/split/trim control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_preamp(chip);
	if (err < 0) {
		dev_err(&intf->dev, "preamp control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_flags(chip);
	if (err < 0) {
		dev_err(&intf->dev, "flag control creation failed: %d\n", err);
		goto error;
	}

	err = snd_card_register(chip->card);
	if (err < 0) {
		dev_err(&intf->dev, "snd_card_register failed: %d\n", err);
		goto error;
	}

	usb_set_intfdata(intf, chip);
	dev_info(&intf->dev,
		 "Babyface Pro: card %i, %u frames/URB, %u URBs/direction\n",
		 chip->card->number, chip->frames_per_urb, chip->nurbs);
	return 0;

error:
	usb_set_intfdata(intf, NULL);
	/* Balance the probe()-time usb_disable_autosuspend(): disconnect()
	 * is never called for a failed probe, so the disable would leak and
	 * leave autosuspend off on this usb_device until a physical unplug.
	 */
	usb_enable_autosuspend(chip->dev);
	snd_card_free(chip->card);
	return err;
}

static void babyface_disconnect(struct usb_interface *intf)
{
	struct snd_usb_babyface *chip = usb_get_intfdata(intf);

	if (!chip)
		return;

	/* Idempotence guard: a disconnect can race a re-probe (usbfs
	 * detach/re-attach) - tear the card down exactly once.
	 */
	usb_set_intfdata(intf, NULL);
	if (chip->shutdown)
		return;

	chip->shutdown = true;
	/* Kill the session before cancelling the work: the URB handlers
	 * queue it, and an unplug is exactly when they see errors.  Taking
	 * the mutex also waits out a prepare that was already past its
	 * ->shutdown check.
	 */
	mutex_lock(&chip->mutex);
	if (chip->streaming)
		babyface_stream_kill(chip);
	mutex_unlock(&chip->mutex);
	cancel_work_sync(&chip->stream_work);
	/* Keep the mixer state for the next probe: a userspace usbfs claim
	 * (PipeWire sink grab, TuxMix daemon) detaches us and the cold init
	 * of the re-probe would otherwise wipe the settings.
	 */
	bf_state_save(chip);
	/* Balance the probe()-time usb_disable_autosuspend(): the usb_device
	 * outlives this interface claim (a usbfs detach re-probes without
	 * the physical device ever disconnecting), so leaving autosuspend
	 * disabled here would wrongly affect whatever claims the device next.
	 */
	usb_enable_autosuspend(chip->dev);
	/* Wake apps blocked in read/write: the card is going away. */
	dev_info(&chip->dev->dev, "disconnect: stopping PCM substreams\n");
	babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_DISCONNECTED);

	snd_card_disconnect(chip->card);
	/* NEVER snd_card_free() here: it blocks until the last user
	 * closes the card, and an open client (e.g. PipeWire) deadlocks
	 * the disconnect (seen live: pipewire stuck in snd_card_free,
	 * D state).  free_when_closed frees on the last close.
	 */
	snd_card_free_when_closed(chip->card);
}

static const struct usb_device_id babyface_ids[] = {
	{ USB_DEVICE(USB_VENDOR_RME, USB_PRODUCT_BABYFACE_PRO_FS) },
	{ }
};
MODULE_DEVICE_TABLE(usb, babyface_ids);

static struct usb_driver babyface_driver = {
	.name = "snd-usb-babyface-pro",
	.probe = babyface_probe,
	.disconnect = babyface_disconnect,
	.id_table = babyface_ids,
};

static int __init babyface_init(void)
{
	return usb_register(&babyface_driver);
}

static void __exit babyface_exit(void)
{
	/* Deregister first: each disconnect() calls bf_state_save(), which
	 * allocates a node for a device it has not seen before.  Purging
	 * before that frees the list and then leaks those fresh nodes.
	 */
	usb_deregister(&babyface_driver);
	bf_state_purge();
}

module_init(babyface_init);
module_exit(babyface_exit);

MODULE_AUTHOR("Ismaïl Bahloul <i.bahloul01@gmail.com>");
MODULE_DESCRIPTION("RME Babyface Pro / Pro FS (proprietary mode) USB audio driver");
MODULE_LICENSE("GPL");
