// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro / Pro FS - proprietary-mode USB audio driver
 *
 * Core driver: USB vendor requests + cold init, interrupt-URB PCM
 * streaming, and the card lifecycle (probe/disconnect/module entry).
 * No mixer controls, front-panel support or suspend/resume yet - see
 * babyfacepro.h and the cover letter for the rest of the series.
 */
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
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

/* Sends the BF_REG_KEEPALIVE_SETTINGS word (PROTOCOL.md: "keepalive
 * 0x10 0x05CF wVal = host settings-state register" - a single shared
 * word, not independent per-setting writes).  Hardcoded to Internal
 * clock for now; the clock-source control and the chip->clock_optical
 * bit it composes in land with a later patch in this series, at which
 * point this grows the same way babyface_restore_state() does further
 * down the series.
 */
int bf_settings_write(struct snd_usb_babyface *chip)
{
	return bf_vendor_write(chip, BF_REQ_KEEPALIVE, BF_SETTINGS_CLOCK_INTERNAL,
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

/* The cold-start session init (cap_coldplug.pcap), verbatim from the
 * user-space reference (protocol::streaming_init).  Without it the
 * firmware never validates a stream.
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
			      (bf_rate_family(r) << 4) | BF_SETTINGS_CLOCK_INTERNAL,
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

/* -- PCM data path ------------------------- */

static bool babyface_capture_copy(struct snd_usb_babyface *chip,
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

/* Stream model (big picture):
 *
 * The PCM stream is entirely asynchronous.  A trigger(START/STOP) does
 * not touch the hardware; it only changes the shared stream_users
 * counter (0..2, one per running substream, counted under chip->lock)
 * and schedules stream_work.  The work runs in process context (the
 * control transfers and usb_submit_urb() calls must sleep):
 *
 *  - 1st user (users 0 -> 1): cold-init + the session trigger pair,
 *    then the interrupt URBs (nurbs in each direction) are submitted
 *    and the session is armed.
 *  - last user (users 1 -> 0): the URBs are killed and the session
 *    disarmed.
 *
 * So the device has exactly one live stream regardless of how many
 * substreams run, and a playback+capture pair shares it.  An app that
 * triggers while the work is running just increments stream_users; the
 * work's re-count on the error path (bf_recount_users) reflects the
 * RUNNING state so a recovered app re-arms from a clean slate.
 */
void babyface_stream_work(struct work_struct *work)
{
	struct snd_usb_babyface *chip =
		container_of(work, struct snd_usb_babyface, stream_work);
	unsigned int urbsize;
	unsigned long flags;
	int i, ret;
	int users;

	mutex_lock(&chip->mutex);
	urbsize = chip->frame_bytes * chip->frames_per_urb;

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
		 * sends streaming_init at every session start - without it
		 * the outputs stay silent).
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
	/* The apps already got a successful trigger - wake them with an
	 * XRUN so a failed start (device wedged, cold-init error) does not
	 * leave them hung in read/write with no URBs in flight.
	 */
	babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_XRUN);
	bf_recount_users(chip);
	mutex_unlock(&chip->mutex);
}

/* -- PCM --------------------------- */

static const struct snd_pcm_hardware babyface_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
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

/* Is the OTHER direction actually transferring?  Not "is it open": a client
 * that has the device open but has not started must still be able to pick the
 * rate, so that an application opening playback and capture before starting
 * either one can take both to 96 kHz.
 */
static bool bf_other_running(struct snd_usb_babyface *chip,
			     struct snd_pcm_substream *subs)
{
	struct snd_pcm_substream *other = READ_ONCE(chip->subs[!subs->stream]);
	unsigned long flags;
	bool running;

	if (!other)
		return false;
	snd_pcm_stream_lock_irqsave(other, flags);
	running = snd_pcm_running(other);
	snd_pcm_stream_unlock_irqrestore(other, flags);
	return running;
}

static int babyface_pcm_open(struct snd_pcm_substream *subs)
{
	struct snd_usb_babyface *chip = snd_pcm_substream_chip(subs);
	struct snd_pcm_runtime *rt = subs->runtime;
	unsigned int locked;
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
	/* One URB delivers frames_per_urb frames per interrupt; a period must
	 * span at least one URB so a completion crosses at most one period
	 * boundary.  Constrain in frames (not bytes) so the minimum period
	 * does not balloon at low channel counts: 2 ch @ 48 kHz -> 256
	 * frames (5.3 ms) instead of 1536 frames from a 12-ch byte clamp.
	 */
	ret = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					   chip->frames_per_urb, 1 << 18);
	if (ret < 0)
		return ret;

	/* Both directions share one clock, so while audio is flowing the rate
	 * belongs to whoever started it.  Offer that rate alone: a client
	 * arriving later then negotiates down to it, and the sound server
	 * resamples, rather than the device being retuned underneath a running
	 * stream.  Advertising the constraint here rather than failing in
	 * hw_params() is what keeps a PipeWire sink alive - it picks the rate
	 * on offer instead of asking for one that has to be refused.
	 *
	 * This is what the vendor drivers do.  RME's settings panel greys the
	 * sample rate out during record/playback, and their manual is explicit
	 * that all active ASIO clients share one rate, with WDM (or CoreAudio)
	 * doing any conversion - never the driver.
	 */
	mutex_lock(&chip->mutex);
	locked = chip->streaming ? chip->rate : 0;
	mutex_unlock(&chip->mutex);
	if (locked) {
		ret = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_RATE,
						   locked, locked);
		if (ret < 0)
			return ret;
	}

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
	 * discard the transfer with -EOVERFLOW (babble) - seen at
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
		/* The constraint in open() normally means nobody asks for a
		 * rate other than the running one.  It can still happen, when
		 * this substream was opened before the other one started: the
		 * constraint is evaluated at open, so it did not apply then.
		 * Refuse rather than retune - the alternative is a running
		 * stream silently changing pitch, with no error and no xrun,
		 * because a negotiated rate is never revised for a live
		 * substream.
		 */
		if (bf_other_running(chip, subs)) {
			dev_dbg(&chip->dev->dev,
				"rate %u Hz refused: %u Hz stream running\n",
				r->rate, chip->rate);
			ret = -EBUSY;
			goto out;
		}
		/* Nothing is transferring, so the clock is free.  Stop any
		 * armed URBs, re-point the bandwidth class and let the stream
		 * work restart the session at the new rate.
		 */
		if (chip->streaming)
			babyface_stream_kill(chip);
		ret = usb_set_interface(chip->dev, BF_IFACE, r->alt);
		if (ret < 0) {
			babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_XRUN);
			bf_recount_users(chip);
			goto out;
		}
		chip->rate = r->rate;
		chip->alt = r->alt;
		chip->frame_bytes = r->frame_bytes;
		/* Publish the new geometry before restarting either stream. */
		schedule_work(&chip->stream_work);
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
		 * locks) - serialize the ++/-- so a concurrent trigger on
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

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the Babyface Pro sound card.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the Babyface Pro sound card.");
module_param(frames_per_urb, int, 0644);
MODULE_PARM_DESC(frames_per_urb, "Audio frames per URB, 8..1024 (16 = low-latency floor, 256 = default).");
module_param(nurbs, int, 0644);
MODULE_PARM_DESC(nurbs, "URBs in flight per direction, 1..16 (16 = low-latency).");

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
	chip->nurbs = clamp(nurbs, 1, 16);
	chip->frames_per_urb = clamp(frames_per_urb, 8, 1024) & ~7;
	chip->rate = 48000;
	chip->alt = BF_ALT_1;
	chip->frame_bytes = 56;
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
	cancel_work_sync(&chip->stream_work);
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
	usb_deregister(&babyface_driver);
}

module_init(babyface_init);
module_exit(babyface_exit);

MODULE_AUTHOR("Ismaïl Bahloul <i.bahloul01@gmail.com>");
MODULE_DESCRIPTION("RME Babyface Pro / Pro FS (proprietary mode) USB audio driver");
MODULE_LICENSE("GPL");
