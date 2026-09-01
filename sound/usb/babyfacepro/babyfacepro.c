// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS — proprietary-mode USB audio driver
 *
 * Core driver: USB vendor requests + cold init, interrupt-URB PCM
 * streaming, mixer-state persistence across re-probes/resume, and
 * the card lifecycle (probe/disconnect/PM/module entry).
 *
 * See babyfacepro.h for the shared device state and register map,
 * and babyfacepro-ctl.c for the ALSA control surface (mixer, front
 * panel, DSP EQ).
 */
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "babyfacepro.h"

/* The transaction-flag counter cycle on 16-bit writes. */
const u16 bf_flag_cycle[4] = { 0xc000, 0x4000, 0x8000, 0x0000 };

/* ── sample-rate / alt classes ──────────────────── */

static const struct bf_rate bf_rates[] = {
	{  32000, BF_ALT_1, 56,  8 },
	{  44100, BF_ALT_1, 56,  8 },
	{  48000, BF_ALT_1, 56,  8 },
	{  64000, BF_ALT_1, 56,  8 },
	{  88200, BF_ALT_1, 56,  8 },
	{  96000, BF_ALT_2, 40, 16 },
	{ 128000, BF_ALT_2, 40, 16 },
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

/* ── vendor requests ─────────────────────── */

int bf_vendor_write(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx)
{
	return usb_control_msg_send(chip->dev, 0, req,
				    USB_DIR_OUT | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE,
				    val, idx, NULL, 0, 1000, GFP_KERNEL);
}

int bf_vendor_read(struct snd_usb_babyface *chip, u8 req, u16 idx, u8 *buf)
{
	return usb_control_msg_recv(chip->dev, 0, req,
				    USB_DIR_IN | USB_TYPE_VENDOR |
				    USB_RECIP_DEVICE,
				    0, idx, buf, 4, 1000, GFP_KERNEL);
}

/* The cold-start session init (cap_coldplug.pcap), verbatim from the
 * user-space reference (protocol::streaming_init).  Without it the
 * firmware never validates a stream.
 */
int bf_cold_init(struct snd_usb_babyface *chip)
{
	int ret, i;

	for (i = 0; i <= 0x3d; i++) {
		if (i == 0x1e || i == 0x1f)
			continue;
		ret = bf_vendor_write(chip, BF_REQ_REG_CLEAR, 0x0000, i);
		if (ret < 0)
			return ret;
	}
	/* 48-kHz DDS clock quads (banked 0x1B). */
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0xc350, 0x0000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0x8db8, 0xd201);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0x8234, 0xd302);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0x7cff, 0xf803);
	if (ret < 0)
		return ret;
	/* 0x1C status — the hardware-validated reference (protocol::
	 * streaming_init) sends it as an OUT write; Windows reads it.
	 * Both are tolerated; match the validated path.
	 */
	ret = bf_vendor_write(chip, BF_REQ_STATUS_2, 0x0000, 0x0000);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0021, BF_REG_KEEPALIVE_INIT);
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

/* The 0x16 cold-init clear covers only 0x00-0x3D — the "cross"
 * registers of a block (L-reg odd / R-reg even of the stereo
 * sources) survive from the previous session and would sum L+R into
 * BOTH channels of the output (mono).  Zero them explicitly: 10 odd
 * L-registers (5,7,…23) + 10 even R-registers (4,6,…22).
 */
int bf_crosspoint_clear_cross(struct snd_usb_babyface *chip,
			      unsigned int blk)
{
	int ret, k;
	u16 flag;

	for (k = 5; k < 24; k += 2) {
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000,
				      (BF_REG_CROSS_BASE_L +
				       BF_REG_CROSS_STRIDE * blk + k) | flag);
		if (ret < 0)
			return ret;
	}
	for (k = 4; k < 24; k += 2) {
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000,
				      (BF_REG_CROSS_BASE_R +
				       BF_REG_CROSS_STRIDE * blk + k) | flag);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* ── mixer-state persistence across interface re-probes ────────
 * A userspace client can claim the proprietary interface via usbfs
 * (USBDEVFS_DISCONNECT_CLAIM — seen with PipeWire grabbing the
 * device when a stream targets the sink, and with the TuxMix
 * user-space daemon's libusb).  That detaches us and the card
 * disappears for the duration; on release the interface re-probes.
 * The device keeps its registers across the detach, but our cold
 * init clears them — so save the mixer state at disconnect and
 * restore it at the next probe.
 */

static LIST_HEAD(bf_saved_list);
static DEFINE_MUTEX(bf_saved_mutex);

/* Re-apply the whole cached mixer state after a resume (the device
 * lost its registers across a system suspend — TotalMix does the same
 * re-apply).  Caller holds chip->mutex.
 */
int babyface_restore_state(struct snd_usb_babyface *chip)
{
	int out, src, mic, ret;
	u16 flag;

	/* Preamp state + commit. */
	ret = bf_preamp_state_write(chip);
	if (ret < 0)
		return ret;

	/* The four mic gains (the counter restarts). */
	for (mic = 0; mic < 4; mic++) {
		u8 counter = (mic % 3 == 0) ? 0x20 : (mic % 3 == 1) ? 0x00 : 0x40;

		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      (u16)((bf_gain_raw(mic, chip->gain[mic]) & 0x1f) |
					    counter),
				      BF_REG_GAIN + mic);
		if (ret < 0)
			return ret;
	}
	chip->gain_cycle = 1;

	/* Masters (8-bit = the real volume) + mutes. */
	ret = bf_apply_masters(chip);
	if (ret < 0)
		return ret;

	/* Crosspoints (canonical out → register block). */
	for (out = 0; out < 6; out++) {
		unsigned int blk = bf_xpoint_block[out];

		for (src = 0; src < 14; src++) {
			flag = bf_flag_cycle[chip->flag_cnt];
			chip->flag_cnt = (chip->flag_cnt + 1) & 3;
			ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
					      chip->xpoint[out][src][0],
					      (BF_REG_CROSS_BASE_L +
					       BF_REG_CROSS_STRIDE * blk +
					       bf_sources[src].idx_l) | flag);
			if (ret < 0)
				return ret;
			ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
					      chip->xpoint[out][src][1],
					      (BF_REG_CROSS_BASE_R +
					       BF_REG_CROSS_STRIDE * blk +
					       bf_sources[src].idx_r) | flag);
			if (ret < 0)
				return ret;
		}
		ret = bf_crosspoint_clear_cross(chip, blk);
		if (ret < 0)
			return ret;
	}

	/* Pitch (the DDS quad) + the clock keepalive. */
	if (chip->pitch) {
		u32 dds24 = div_u64(12800000000ULL + (u32)(1000 + chip->pitch) / 2,
				    1000 + chip->pitch);
		u16 dds16 = dds24 >> 8;
		u16 frac = dds24 & 0xff;

		ret = bf_vendor_write(chip, BF_REQ_DDS, dds16, (frac << 8) | 0);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_DDS,
				      (u16)div_u64(dds16 * 72562ull + 50000, 100000), 0x0001);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_DDS, (u16)((dds16 * 2 + 1) / 3),
				      0x0002);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_DDS, 0x7cff, 0x0003);
		if (ret < 0)
			return ret;
	}
	return bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0001,
			       BF_REG_KEEPALIVE_SETTINGS);
}

/* Re-apply the non-master flags (loopback / AN1>2 / link / width /
 * FX send / MS) after a state restore.  The write patterns mirror the
 * corresponding _put() handlers.  Caller holds chip->mutex.
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

	/* Re-apply an engaged DIM (the fixed -20 dB Phones pair + flag). */
	if (chip->dim) {
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
	s->gain_cycle = chip->gain_cycle;
	s->flag_cnt = chip->flag_cnt;
	memcpy(s->master, chip->master, sizeof(s->master));
	memcpy(s->muted, chip->muted, sizeof(s->muted));
	memcpy(s->xpoint, chip->xpoint, sizeof(s->xpoint));
	s->pitch = chip->pitch;
	memcpy(s->loopback, chip->loopback, sizeof(s->loopback));
	s->an12 = chip->an12;
	s->linked = chip->linked;
	s->ms_proc = chip->ms_proc;
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
		chip->gain_cycle = s->gain_cycle;
		chip->flag_cnt = s->flag_cnt;
		memcpy(chip->master, s->master, sizeof(chip->master));
		memcpy(chip->muted, s->muted, sizeof(chip->muted));
		memcpy(chip->xpoint, s->xpoint, sizeof(chip->xpoint));
		chip->pitch = s->pitch;
		memcpy(chip->loopback, s->loopback, sizeof(chip->loopback));
		chip->an12 = s->an12;
		chip->linked = s->linked;
		chip->ms_proc = s->ms_proc;
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

/* ── stream (interrupt URBs, caiaq-style) ──────────────── */

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
			 * 2026-08-25: the playback echoes there at ~−27 dB,
			 * independent of the output masters — NOT the output
			 * bus; the ADAT/SPDIF range is words 6-11 only).  The
			 * device words 4/5 are a fixed marker, not audio —
			 * skipped.  At 96/192 kHz the frame has fewer words;
			 * missing ones read as zero.
			 */
			static const u8 map[12] = { 0, 1, 2, 3, 6, 7, 8, 9,
						   10, 11, 12, 13 };
			u8 wi = i < 12 ? map[i] : 0xff;
			s32 s = 0;

			if (wi < words) {
				/* 24-bit sample in bytes 1-3; arithmetic shift
				 * sign-extends from bit 23.  S24_LE container.
				 */
				s = (s32)le32_to_cpu(w[wi]) >> 8;
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
	unsigned long new_period;
	bool crossed = false;
	const u8 *src;

	spin_lock(&chip->lock);
	/* Clamp to what the app has actually written: the in-flight URBs
	 * (nurbs × frames_per_urb) can exceed the app ring, and without
	 * this the driver advances hw_ptr past appl_ptr — the ALSA core
	 * then flags a spurious XRUN on the next app interaction even
	 * though the app refills on schedule (seen at period 16-128 /
	 * 96-192 kHz with nurbs=16).  The device just repeats the last
	 * frames (stale audio) instead of corrupting the stream state.
	 * NB: subtract the unbounded counters directly — modulo arithmetic
	 * is ambiguous at exact buffer multiples (appl=512, hw=0 → both
	 * wrap to 0).
	 */
	{
		snd_pcm_sframes_t data =
			(snd_pcm_sframes_t)(rt->control->appl_ptr -
					    chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK]);
		if (data < 0)
			data = 0;
		if (data > (snd_pcm_sframes_t)buf_frames)
			data = (snd_pcm_sframes_t)buf_frames;
		if ((unsigned int)data < frames)
			frames = (unsigned int)data;
	}
	pos = chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] % buf_frames;
	for (f = 0; f < frames; f++) {
		__le32 *w = (__le32 *)(data + f * chip->frame_bytes);

		src = rt->dma_area + frames_to_bytes(rt, pos);
		/* App ch n feeds the device word n (PB1-6 = words 0-11);
		 * words 12/13 stay zero.  At 96/192 kHz the frame is
		 * shorter — the extra app channels are dropped.
		 */
		for (i = 0; i < chans && i < words; i++) {
			u32 s = get_unaligned_le32(src + i * 4);

			/* 24-bit sample into bytes 1-3, byte 0 = 0. */
			w[i] = cpu_to_le32((s & 0x00ffffff) << 8);
		}
		for (; i < words; i++)
			w[i] = 0;
		pos++;
		if (pos >= buf_frames)
			pos = 0;
	}
	chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] += frames;
	new_period = chip->hw_ptr[SNDRV_PCM_STREAM_PLAYBACK] / rt->period_size;
	if (new_period != chip->prev_period[SNDRV_PCM_STREAM_PLAYBACK]) {
		chip->prev_period[SNDRV_PCM_STREAM_PLAYBACK] = new_period;
		crossed = true;
	}
	spin_unlock(&chip->lock);

	return crossed;
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
		    urb->status == -ECONNRESET)
			return;		/* killed */
		dev_dbg_ratelimited(&chip->dev->dev, "IN urb status %d\n",
				    urb->status);
		if (atomic_inc_return(&chip->urb_err) >= BF_URB_ERR_STOP)
			schedule_work(&chip->stream_work);
		goto resubmit;
	}
	atomic_set(&chip->urb_err, 0);

	subs = READ_ONCE(chip->subs[SNDRV_PCM_STREAM_CAPTURE]);
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
resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(&chip->dev->dev,
				    "IN resubmit failed: %d\n", ret);
		if (atomic_inc_return(&chip->urb_err) >= BF_URB_ERR_STOP)
			schedule_work(&chip->stream_work);
	}
}

static void babyface_complete_out(struct urb *urb)
{
	struct snd_usb_babyface *chip = urb->context;
	struct snd_pcm_substream *subs;
	unsigned long flags;
	unsigned int frames;
	bool crossed = false;
	int ret;

	if (urb->status < 0) {
		if (urb->status == -ESHUTDOWN || urb->status == -ENOENT ||
		    urb->status == -ECONNRESET)
			return;		/* killed */
		dev_dbg_ratelimited(&chip->dev->dev, "OUT urb status %d\n",
				    urb->status);
		if (atomic_inc_return(&chip->urb_err) >= BF_URB_ERR_STOP)
			schedule_work(&chip->stream_work);
		goto resubmit;
	}
	atomic_set(&chip->urb_err, 0);

	subs = READ_ONCE(chip->subs[SNDRV_PCM_STREAM_PLAYBACK]);
	if (subs) {
		snd_pcm_stream_lock_irqsave(subs, flags);
		if (snd_pcm_running(subs)) {
			frames = chip->frames_per_urb;
			crossed = babyface_playback_copy(chip, subs,
							 urb->transfer_buffer, frames);
		}
		snd_pcm_stream_unlock_irqrestore(subs, flags);
		if (crossed)
			snd_pcm_period_elapsed(subs);
	} else {
		/* No consumer: silence the OUT frames. */
		memset(urb->transfer_buffer, 0, urb->transfer_buffer_length);
	}
resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err_ratelimited(&chip->dev->dev,
				    "OUT resubmit failed: %d\n", ret);
		if (atomic_inc_return(&chip->urb_err) >= BF_URB_ERR_STOP)
			schedule_work(&chip->stream_work);
	}
}

void babyface_stream_kill(struct snd_usb_babyface *chip)
{
	int i;

	for (i = 0; i < chip->nurbs; i++) {
		usb_kill_urb(chip->urbs_in[i]);
		usb_kill_urb(chip->urbs_out[i]);
	}
	chip->streaming = false;
}

/* Stream start/stop run in process context (control transfers sleep).
 * The trigger only toggles stream_users and schedules this work.
 */

/* Stop both PCM substreams (if running) so apps blocked in read/write
 * wake with a clean error: XRUN for a recoverable stream error, or
 * DISCONNECTED when the card is going away.
 */
void babyface_pcm_stop_both(struct snd_usb_babyface *chip, snd_pcm_state_t state)
{
	int s;

	for (s = 0; s < 2; s++) {
		struct snd_pcm_substream *subs = READ_ONCE(chip->subs[s]);

		if (subs && snd_pcm_running(subs))
			snd_pcm_stop(subs, state);
	}
}

/* Re-count stream_users from the substream running states.  The apps
 * can recover (re-prepare + trigger) while the stream work runs, so a
 * hard `= 0` would wipe a fresh increment and leave a RUNNING
 * substream with no URBs (hang).  Called on the error paths with the
 * mutex held.
 */
static void bf_recount_users(struct snd_usb_babyface *chip)
{
	unsigned long flags;
	int s, users = 0;

	for (s = 0; s < 2; s++) {
		struct snd_pcm_substream *subs = READ_ONCE(chip->subs[s]);

		if (subs && snd_pcm_running(subs))
			users++;
	}
	spin_lock_irqsave(&chip->lock, flags);
	chip->stream_users = users;
	spin_unlock_irqrestore(&chip->lock, flags);
}

void babyface_stream_work(struct work_struct *work)
{
	struct snd_usb_babyface *chip =
		container_of(work, struct snd_usb_babyface, stream_work);
	unsigned int urbsize = chip->frame_bytes * chip->frames_per_urb;
	unsigned long flags;
	int i, ret;
	int users;

	mutex_lock(&chip->mutex);

	if (chip->shutdown) {
		mutex_unlock(&chip->mutex);
		return;
	}

	/* Persistent URB errors (bad link, device wedged): stop the stream
	 * and wake the apps with -EPIPE.  stream_users is re-counted from
	 * the (now stopped) substreams so an app recovery (prepare+start)
	 * re-arms the session from a clean slate.
	 */
	if (atomic_read(&chip->urb_err) >= BF_URB_ERR_STOP) {
		dev_err(&chip->dev->dev,
			"stream error: %d consecutive bad URBs, stopping (apps re-arm)\n",
			BF_URB_ERR_STOP);
		babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_XRUN);
		if (chip->streaming)
			babyface_stream_kill(chip);
		bf_recount_users(chip);
		atomic_set(&chip->urb_err, 0);
		mutex_unlock(&chip->mutex);
		return;
	}

	spin_lock_irqsave(&chip->lock, flags);
	users = chip->stream_users;
	spin_unlock_irqrestore(&chip->lock, flags);

	if (users > 0 && !chip->streaming) {
		/* The firmware only validates a stream session that is
		 * preceded by the full cold-init (the user-space reference
		 * sends streaming_init at every session start — without it
		 * the outputs stay silent).  The 0x16 clear wipes the mixer
		 * registers, so the cached state is re-applied after the arm.
		 */
		ret = bf_cold_init(chip);
		if (ret < 0)
			goto err;

		/* Stream trigger pair (cap_audio): 0x10 0x8000 + 0x1D. */
		ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0000, 0x8000);
		if (ret < 0)
			goto err;
		ret = bf_vendor_write(chip, BF_REQ_SESSION_START, 0x0000, 0x0000);
		if (ret < 0)
			goto err;

		for (i = 0; i < chip->nurbs; i++) {
			usb_fill_int_urb(chip->urbs_in[i], chip->dev,
					 usb_rcvintpipe(chip->dev, BF_EP_IN),
					 chip->buf_in[i], urbsize,
					 babyface_complete_in, chip, 1);
			usb_fill_int_urb(chip->urbs_out[i], chip->dev,
					 usb_sndintpipe(chip->dev, BF_EP_OUT),
					 chip->buf_out[i], urbsize,
					 babyface_complete_out, chip, 1);
		}
		for (i = 0; i < chip->nurbs; i++) {
			ret = usb_submit_urb(chip->urbs_in[i], GFP_KERNEL);
			if (ret < 0)
				goto err;
			ret = usb_submit_urb(chip->urbs_out[i], GFP_KERNEL);
			if (ret < 0)
				goto err;
		}
		/* Session arm (cap_audio frame 5829, after the URBs). */
		ret = bf_vendor_write(chip, BF_REQ_SESSION_ARM, 0x0000, 0xc000);
		if (ret < 0)
			goto err;

		/* The cold init above cleared the mixer registers; push the
		 * cached state back (preamp, gains, masters, crosspoints,
		 * pitch) so the session starts at the user's levels.
		 */
		ret = babyface_restore_state(chip);
		if (ret < 0)
			goto err;

		/* The 0x16 clear also wipes the flag registers (loopback,
		 * AN1>2, stereo link, width, FX send, MS) — re-apply them.
		 */
		ret = bf_state_apply_flags(chip);
		if (ret < 0)
			goto err;

		chip->streaming = true;
		dev_dbg(&chip->dev->dev, "stream started (%u frames/URB, %u URBs)\n",
			chip->frames_per_urb, chip->nurbs);
	} else if (users == 0 && chip->streaming) {
		babyface_stream_kill(chip);
		dev_dbg(&chip->dev->dev, "stream stopped\n");
	}

	mutex_unlock(&chip->mutex);
	return;

err:
	dev_err(&chip->dev->dev, "failed to start stream: %d\n", ret);
	babyface_stream_kill(chip);
	/* The apps already got a successful trigger — wake them with an
	 * XRUN so a failed start (device wedged, cold-init error) does not
	 * leave them hung in read/write with no URBs in flight.
	 */
	babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_XRUN);
	bf_recount_users(chip);
	mutex_unlock(&chip->mutex);
}

/* ── PCM ─────────────────────────── */

static const struct snd_pcm_hardware babyface_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = 32000,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = 12,
	.buffer_bytes_max = 1 << 20,
	.period_bytes_max = 1 << 18,
	.periods_min = 2,
	.periods_max = 16,
};

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
	/* One URB delivers frames_per_urb frames per interrupt; a period must
	 * span at least one URB so a completion crosses at most one period
	 * boundary.  Constrain in frames (not bytes) so the minimum period
	 * does not balloon at low channel counts: 2 ch @ 48 kHz → 256
	 * frames (5.3 ms) instead of 1536 frames from a 12-ch byte clamp.
	 */
	ret = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					   chip->frames_per_urb, 1 << 18);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&chip->lock, flags);
	chip->subs[subs->stream] = subs;
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int babyface_pcm_close(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;

	/* Wait for the stream stop work so the URB callbacks (which
	 * touch subs) are done before the substream can be freed.
	 */
	flush_work(&chip->stream_work);
	spin_lock_irqsave(&chip->lock, flags);
	chip->subs[subs->stream] = NULL;
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int babyface_pcm_hw_params(struct snd_pcm_substream *subs,
				  struct snd_pcm_hw_params *params)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	const struct bf_rate *r;
	int ret = 0;

	r = bf_rate_lookup(params_rate(params));
	if (!r)
		return -EINVAL;

	/* The stream URBs must be at least one alt packet wide: the device
	 * delivers its IN data in alt-sized packets (448/640/1024 B for
	 * alt 1/2/3), and a smaller URB buffer makes the host controller
	 * discard the transfer with -EOVERFLOW (babble) — seen at
	 * 176.4/192 kHz with frames_per_urb below 32.  Return a clean
	 * error instead of a silently dead capture stream.
	 */
	if (chip->frames_per_urb < r->min_fpu) {
		dev_err(&chip->dev->dev,
			"rate %u Hz needs frames_per_urb >= %u (module has %u)\n",
			r->rate, r->min_fpu, chip->frames_per_urb);
		return -EINVAL;
	}

	mutex_lock(&chip->mutex);
	if (r->rate != chip->rate) {
		/* Both directions share one clock, so a rate change must not
		 * race live transfers.  Stop the URBs, re-point the bandwidth
		 * class and let the stream work restart the session at the
		 * new rate — the other running substream briefly sees a rate
		 * step (PipeWire re-negotiates via its resampler) instead of
		 * this open failing with -EBUSY (which killed the PW sink).
		 */
		if (chip->streaming) {
			unsigned long flags;

			babyface_stream_kill(chip);
			spin_lock_irqsave(&chip->lock, flags);
			if (chip->stream_users > 0)
				schedule_work(&chip->stream_work);
			spin_unlock_irqrestore(&chip->lock, flags);
		}
		ret = usb_set_interface(chip->dev, BF_IFACE, r->alt);
		if (ret < 0)
			goto out;
		chip->rate = r->rate;
		chip->alt = r->alt;
		chip->frame_bytes = r->frame_bytes;
		/* The DSP EQ coefficients depend on fs: re-upload. */
		bf_eq_reupload(chip);
		dev_dbg(&chip->dev->dev, "rate %u Hz (alt %u)\n",
			chip->rate, chip->alt);
	}
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int babyface_pcm_hw_free(struct snd_pcm_substream *subs)
{
	/* The device buffer is host-side; nothing to release here. */
	return 0;
}

static int babyface_pcm_prepare(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;

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
	case SNDRV_PCM_TRIGGER_START:
		spin_lock_irqsave(&chip->lock, flags);
		chip->hw_ptr[subs->stream] = 0;
		chip->prev_period[subs->stream] = 0;
		/* stream_users is shared by the two substreams (separate
		 * locks) — serialize the ++/-- so a concurrent trigger on
		 * the other direction can't lose an increment (which would
		 * stop the stream while a substream still runs).
		 */
		if (chip->stream_users++ == 0)
			schedule_work(&chip->stream_work);
		spin_unlock_irqrestore(&chip->lock, flags);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock_irqsave(&chip->lock, flags);
		if (chip->stream_users > 0 && --chip->stream_users == 0)
			schedule_work(&chip->stream_work);
		spin_unlock_irqrestore(&chip->lock, flags);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t babyface_pcm_pointer(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	unsigned long flags;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&chip->lock, flags);
	pos = chip->hw_ptr[subs->stream] % subs->runtime->buffer_size;
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
static int nurbs = BF_NURBS_DEFAULT;
static int panel_poll_ms = BF_PANEL_POLL_MS_DEFAULT;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the Babyface Pro FS sound card.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the Babyface Pro FS sound card.");
module_param(frames_per_urb, int, 0644);
MODULE_PARM_DESC(frames_per_urb, "Audio frames per URB, 8..1024 (16 = low-latency floor, 256 = default).");
module_param(nurbs, int, 0644);
MODULE_PARM_DESC(nurbs, "URBs in flight per direction, 1..16 (16 = low-latency).");
module_param(panel_poll_ms, int, 0644);
MODULE_PARM_DESC(panel_poll_ms, "Front-panel poll interval in ms, 10..1000 (20 = default, matches Windows' ~50 Hz).");

/* ── USB driver ───────────────────────── */

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
		urbsize = chip->frame_bytes * chip->frames_per_urb;
		for (i = 0; i < chip->nurbs; i++) {
			if (chip->urbs_in[i]) {
				usb_kill_urb(chip->urbs_in[i]);
				usb_free_urb(chip->urbs_in[i]);
			}
			if (chip->urbs_out[i]) {
				usb_kill_urb(chip->urbs_out[i]);
				usb_free_urb(chip->urbs_out[i]);
			}
			usb_free_coherent(chip->dev, urbsize, chip->buf_in[i],
					  chip->dma_in[i]);
			usb_free_coherent(chip->dev, urbsize, chip->buf_out[i],
					  chip->dma_out[i]);
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
	/* USB autosuspend is untested: babyface_suspend()/_resume() don't
	 * check PMSG_IS_AUTO, and nothing in this driver holds a PM
	 * reference while streaming or while the panel poll/keepalive
	 * timers are running, so an autosuspend request could race a
	 * live stream or panel tick. Disable it explicitly rather than
	 * ship an untested code path — full autosuspend support (correct
	 * autopm_get/put pairing around the stream and the panel/keepalive
	 * work) is a deliberate follow-up, not an oversight.
	 */
	usb_disable_autosuspend(chip->dev);
	chip->iface = intf;
	chip->nurbs = clamp(nurbs, 1, 16);
	chip->frames_per_urb = clamp(frames_per_urb, 8, 1024) & ~7;
	chip->panel_poll_ms = clamp(panel_poll_ms, 10, 1000);
	chip->rate = 48000;
	chip->alt = BF_ALT_1;
	chip->frame_bytes = 56;
	chip->preamp = BF_PREAMP_BASE;
	mutex_init(&chip->mutex);
	spin_lock_init(&chip->lock);
	atomic_set(&chip->urb_err, 0);
	INIT_WORK(&chip->stream_work, babyface_stream_work);
	INIT_DELAYED_WORK(&chip->panel_work, babyface_panel_work);
	chip->card->private_free = babyface_private_free;

	strscpy(chip->card->driver, "BabyfaceProFS",
		sizeof(chip->card->driver));
	strscpy(chip->card->shortname, "Babyface Pro FS",
		sizeof(chip->card->shortname));
	snprintf(chip->card->longname, sizeof(chip->card->longname),
		 "RME Babyface Pro FS (proprietary mode) at %s",
		 dev_name(&dev->dev));
	strscpy(chip->card->mixername, "Babyface Pro FS",
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
	} else if (err < 0) {
		dev_err(&intf->dev, "mixer state restore failed: %d\n", err);
		goto error;
	}

	urbsize = chip->frame_bytes * chip->frames_per_urb;
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

	err = snd_pcm_new(chip->card, "Babyface Pro FS", 0, 1, 1, &pcm);
	if (err < 0) {
		dev_err(&intf->dev, "snd_pcm_new failed: %d\n", err);
		goto error;
	}
	pcm->private_data = chip;
	strscpy(pcm->name, "Babyface Pro FS", sizeof(pcm->name));
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

	err = babyface_create_controls(chip);
	if (err < 0) {
		dev_err(&intf->dev, "control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_xpoints(chip);
	if (err < 0) {
		dev_err(&intf->dev, "crosspoint creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_flags(chip);
	if (err < 0) {
		dev_err(&intf->dev, "flag control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_panel(chip);
	if (err < 0) {
		dev_err(&intf->dev, "front-panel control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_eq(chip);
	if (err < 0) {
		dev_err(&intf->dev, "EQ control creation failed: %d\n", err);
		goto error;
	}

	/* The DSP coefficient stream (EQ, bulk ep 0x0A) lives on interface
	 * 1, which has a single altsetting (alt 0) already active in the
	 * default configuration — the endpoint is scheduled, no
	 * SET_INTERFACE or interface claim is needed (the earlier
	 * -EAGAIN was the on-stack transfer buffer, and SET_INTERFACE on
	 * interface 1 wedged the iface-5 audio stream — playback URBs
	 * never completed).
	 */

	err = snd_card_register(chip->card);
	if (err < 0) {
		dev_err(&intf->dev, "snd_card_register failed: %d\n", err);
		goto error;
	}

	/* The panel poll mirrors the physical buttons/wheel into the
	 * Front Panel controls; it runs for the whole card lifetime.
	 */
	babyface_panel_start(chip);

	usb_set_intfdata(intf, chip);
	dev_info(&intf->dev,
		 "Babyface Pro FS: card %i, %u frames/URB, %u URBs/direction\n",
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
	 * detach/re-attach) — tear the card down exactly once.
	 */
	usb_set_intfdata(intf, NULL);
	if (chip->shutdown)
		return;

	/* Keep the mixer state for the next probe: a userspace usbfs
	 * claim (PipeWire sink grab, TuxMix daemon) detaches us and the
	 * cold init of the re-probe would otherwise wipe the settings.
	 */
	bf_state_save(chip);

	chip->shutdown = true;
	cancel_work_sync(&chip->stream_work);
	babyface_panel_stop(chip);
	/* Balance the probe()-time usb_disable_autosuspend(): the usb_device
	 * outlives this interface claim (a usbfs detach re-probes without
	 * the physical device ever disconnecting), so leaving autosuspend
	 * disabled here would wrongly affect whatever claims the device next.
	 */
	usb_enable_autosuspend(chip->dev);
	/* Wake apps blocked in read/write: the card is going away. */
	dev_info(&chip->dev->dev, "disconnect: stopping PCM substreams\n");
	babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_DISCONNECTED);
	mutex_lock(&chip->mutex);
	if (chip->streaming)
		babyface_stream_kill(chip);
	mutex_unlock(&chip->mutex);

	snd_card_disconnect(chip->card);
	/* NEVER snd_card_free() here: it blocks until the last user
	 * closes the card, and an open client (e.g. PipeWire) deadlocks
	 * the disconnect (seen live: pipewire stuck in snd_card_free,
	 * D state).  free_when_closed frees on the last close.
	 */
	snd_card_free_when_closed(chip->card);
}

static int babyface_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct snd_usb_babyface *chip = usb_get_intfdata(intf);

	struct snd_device *sdev;

	if (!chip)
		return 0;
	list_for_each_entry(sdev, &chip->card->devices, list) {
		if (sdev->type == SNDRV_DEV_PCM)
			snd_pcm_suspend_all(sdev->device_data);
	}
	cancel_work_sync(&chip->stream_work);
	babyface_panel_stop(chip);
	mutex_lock(&chip->mutex);
	if (chip->streaming)
		babyface_stream_kill(chip);
	mutex_unlock(&chip->mutex);
	return 0;
}

static int babyface_resume(struct usb_interface *intf)
{
	struct snd_usb_babyface *chip = usb_get_intfdata(intf);
	int err;

	if (!chip)
		return 0;

	/* The device lost its state across the suspend; re-run the cold
	 * init and re-apply the cached mixer state.  Suspended PCM
	 * substreams are woken by the core — apps get -ESTRPIPE and
	 * restart (the trigger re-arms the stream).
	 */
	mutex_lock(&chip->mutex);
	err = usb_set_interface(chip->dev, BF_IFACE, chip->alt);
	if (err < 0)
		goto out;
	err = bf_cold_init(chip);
	if (err < 0)
		goto out;
	err = babyface_restore_state(chip);
out:
	mutex_unlock(&chip->mutex);
	if (!err)
		babyface_panel_start(chip);
	return err;
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
	.suspend = babyface_suspend,
	.resume = babyface_resume,
	.id_table = babyface_ids,
};

static int __init babyface_init(void)
{
	return usb_register(&babyface_driver);
}

static void __exit babyface_exit(void)
{
	bf_state_purge();
	usb_deregister(&babyface_driver);
}

module_init(babyface_init);
module_exit(babyface_exit);

MODULE_AUTHOR("Ismaïl Bahloul <i.bahloul01@gmail.com>");
MODULE_DESCRIPTION("RME Babyface Pro FS (proprietary mode) USB audio driver");
MODULE_LICENSE("GPL");
