// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro / Pro FS - proprietary-mode USB audio driver
 *
 * ALSA control surface: this patch adds the output masters (volume +
 * mute) and the crosspoint routing matrix - the minimum needed for
 * the card to produce controllable, routed audio.  They ship together
 * because babyface_write_default_mixer() (the cold-init default
 * mixer) has to set up both in one pass: masters with no signal
 * routed to them would just be a silent volume knob.
 *
 * Preamp/gain, the routing-flag switches (loopback, AN1>2, link,
 * clock source, width, FX send, DIM), varispeed pitch, the front
 * panel and the DSP EQ are added by later patches in this series -
 * see babyfacepro.h and the cover letter for the full plan.
 *
 * See babyfacepro.h for the shared device state and register map,
 * and babyfacepro.c for the core driver (protocol, PCM streaming,
 * state persistence, card lifecycle).
 */
#include <linux/log2.h>
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

/* The flag-cycle counter, its writer and the per-block crosspoint
 * "cross" register clear are crosspoint/mixer-only, so they live here
 * rather than in the core driver file.
 */

/* The transaction-flag counter cycle on 16-bit writes. */
const u16 bf_flag_cycle[4] = { 0xc000, 0x4000, 0x8000, 0x0000 };

/* Write with the per-transaction flag-cycle word OR'd into idx.  The
 * device wants the flag word (0xc000/0x4000/0x8000/0x0000, rotating)
 * set on every 0x12/0x1a write; this is the hot path for the mixer
 * puts, so it is factored out.
 */
int bf_vendor_write_cycle(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx)
{
	u16 flag = bf_flag_cycle[chip->flag_cnt];

	chip->flag_cnt = (chip->flag_cnt + 1) & 3;
	return bf_vendor_write(chip, req, val, idx | flag);
}

/* The 0x16 cold-init clear covers only 0x00-0x3D - the "cross"
 * registers of a block (L-reg odd / R-reg even of the stereo
 * sources) survive from the previous session and would sum L+R into
 * BOTH channels of the output (mono).  Zero them explicitly: 10 odd
 * L-registers (5,7,...23) + 10 even R-registers (4,6,...22).
 */
int bf_crosspoint_clear_cross(struct snd_usb_babyface *chip,
			      unsigned int blk)
{
	int ret, k;

	for (k = BF_CROSS_L_FIRST; k <= BF_CROSS_L_LAST; k += 2) {
		ret = bf_vendor_write_cycle(chip, BF_REQ_CROSSPOINT, 0x0000,
					    BF_REG_CROSS_BASE_L +
					    BF_REG_CROSS_STRIDE * blk + k);
		if (ret < 0)
			return ret;
	}
	for (k = BF_CROSS_R_FIRST; k <= BF_CROSS_R_LAST; k += 2) {
		ret = bf_vendor_write_cycle(chip, BF_REQ_CROSSPOINT, 0x0000,
					    BF_REG_CROSS_BASE_R +
					    BF_REG_CROSS_STRIDE * blk + k);
		if (ret < 0)
			return ret;
	}
	return 0;
}

const struct bf_source bf_sources[14] = {
	{ "AN1",     0,  0 },
	{ "AN2",     1,  1 },
	{ "AN3",     2,  2 },
	{ "AN4",     3,  3 },
	{ "AS1/2",   4,  5 },
	{ "ADAT3/4", 6,  7 },
	{ "ADAT5/6", 8,  9 },
	{ "ADAT7/8", 10, 11 },
	{ "PB1",    12, 13 },
	{ "PB2",    14, 15 },
	{ "PB3",    16, 17 },
	{ "PB4",    18, 19 },
	{ "PB5",    20, 21 },
	{ "PB6",    22, 23 },
};

/* Crosspoint-map output order vs the master-map order - HARDWARE-
 * VERIFIED 2026-08-24: the block that feeds the Phones is the FIRST
 * crosspoint block (0x34), while the Phones master is the SECOND
 * (0x03E2/0x0006).  The crosspoint map lists the Phones first (the
 * monitor output); the master map lists AN1/2 first.  Control index =
 * the canonical order (AN1/2=0, PH3/4=1, ...) so the crosspoint and
 * master controls line up; this table maps to the register block.
 */
const u8 bf_xpoint_block[6] = { 1, 0, 2, 3, 4, 5 };

/* Master-register output order - the master map lists AN1/2 first
 * (0x03E0) and the Phones master SECOND (0x03E2, HARDWARE-VERIFIED
 * 2026-08-24); the crosspoint blocks are in the opposite order
 * (Phones = block 0x34 first, hence bf_xpoint_block above).  Control
 * index -> canonical output (AN1/2=0, PH3/4=1, ...) = the master
 * register position directly: the names 'AN1/2 Playback Volume' etc.
 * must match the register they write (corrected 2026-08-26 - the
 * previous {1,0,...} swap made 'AN1/2' drive the Phones and 'PH3/4'
 * drive the AN1/2 analog out).
 */
static const u8 bf_master_out[6] = { 0, 1, 2, 3, 4, 5 };

/* The 16-bit master value -> the 8-bit companion code (0.5 dB/step).
 * Integer-only: half_db = 12*log2(v/0x2000) via ilog2 + an 8-bit
 * fractional-octave table (12*log2(1 + n/256), ~0.05 dB resolution -
 * fine enough for the +/-0.5 dB panel wheel to track the round-trip).
 */
static const u8 bf_lg2_frac[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
	8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10,
	10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
	11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
	11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
};

/* 16-bit master -> dBx2 (12 half-dB per octave; 0x2000 = 0 dB).
 * Shared by the 8-bit companion and the front-panel OUT wheel.
 */
int bf_master_half_db(u16 vol16)
{
	unsigned int k, frac;

	vol16 = clamp(vol16, 1, 0x4000);
	k = ilog2(vol16);
	frac = ((vol16 - (1u << k)) << 8) >> k;
	return 12 * (int)k - 156 + bf_lg2_frac[frac];
}

/* dBx2 -> 16-bit master (0x2000*2^(half_db/12), rounded).  The
 * inverse of bf_master_half_db - the 12th-root table 2^(n/12).
 */
static const u16 bf_twelfth[12] = {
	0x1000, 0x10f4, 0x11f6, 0x1307, 0x1429, 0x155c,
	0x16a1, 0x17f9, 0x1966, 0x1ae9, 0x1c82, 0x1e34,
};

int bf_master_16bit(int half_db)
{
	int k = half_db / 12;
	int n = half_db % 12;
	u32 v;

	if (n < 0) {
		n += 12;
		k--;
	}
	v = (u32)bf_twelfth[n] << 1;	/* 0x2000*2^(n/12) */
	if (k >= 0) {
		v <<= k;
	} else {
		v += 1u << (-k - 1);	/* round-half-up */
		v >>= -k;
	}
	return (u16)clamp(v, 1, 0x4000);
}

u8 bf_master_8bit(u16 vol16)
{
	if (vol16 == 0)
		return BF_MASTER_MUTE;
	return (u8)clamp(0xf3 + bf_master_half_db(vol16), BF_MASTER_8_MIN, 0xff);
}

/* The cold-init register clear zeroes the mixer registers TotalMix
 * re-uploads afterwards.  The kernel driver has no saved scene (no
 * readback for faders), so it applies TotalMix's factory routing -
 * every source to every output at unity - so the card makes sound
 * without any user-space mixer at all.
 *
 * The two analog masters (AN1/2, the main out; PH3/4, the headphone
 * out) come up at -20 dB rather than TotalMix's 0 dB.  Routing all 14
 * sources into an output at unity means they SUM, and this runs on
 * every fresh load, before alsa-restore has had a chance to put the
 * user's own levels back.  On monitors or headphones with no volume
 * control of their own that is a real hazard, and the failure is
 * asymmetric: a default that is too quiet is turned up in a second,
 * one that is too loud cannot be taken back.  -20 dB is still plainly
 * audible, and it is not an invented number - it is the exact
 * 8-bit/16-bit pair the hardware's own DIM button writes.
 *
 * The other four outputs (AS1/2, ADAT3/4, ADAT5/6, ADAT7/8) are all
 * digital, carried over the single optical port - nothing downstream
 * of them can be damaged by a loud signal the way a speaker or a pair
 * of headphones can, so there is no hazard to mitigate, only a
 * digital feed that would otherwise arrive 20 dB quiet for no reason
 * a downstream device could infer.  They keep TotalMix's own 0 dB
 * default (raised 2026-09-15 after David Fredman pointed out the
 * blanket -20 dB reached them too, on his report of the AN1/2/PH3/4
 * default - issue #4).
 */
int babyface_write_default_mixer(struct snd_usb_babyface *chip)
{
	int out, src, ret;
	u16 flag;

	/* Output masters: the two analog outputs at -20 dB, the four
	 * digital ones at 0 dB (see the comment above).  Unmuted either
	 * way.
	 */
	for (out = 0; out < 6; out++) {
		bool analog = out < 2;
		u8 gain8 = analog ? BF_MASTER_MINUS20_8 : BF_MASTER_UNMUTE;
		u16 gain16 = analog ? BF_MASTER_MINUS20_16 : BF_MASTER_0DB;

		ret = bf_vendor_write(chip, BF_REQ_GAIN, gain8,
				      BF_REG_MASTER_8 + 2 * out);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_GAIN, gain8,
				      BF_REG_MASTER_8 + 2 * out + 1);
		if (ret < 0)
			return ret;
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, gain16,
				      (BF_REG_MASTER_16 + 2 * out) | flag);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, gain16,
				      (BF_REG_MASTER_16 + 2 * out + 1) | flag);
		if (ret < 0)
			return ret;
		chip->master[out][0] = gain16;
		chip->master[out][1] = gain16;
		chip->muted[out] = false;
	}

	/* Every source into every output pair, L and R, at 0 dB (the
	 * standard map, plus the low map on AN1/2 - see bf_xpoint_write's
	 * own comment for why AN1/2 needs both).  The addresses use the
	 * source's idx_l/idx_r on the canonical block - writing the raw
	 * index on both bases would put PB1 R on the L side and PB1 L on
	 * the R side (L+R on both = mono).  The "cross" registers
	 * (L-reg idx_r / R-reg idx_l) are left at 0; the restore at stream
	 * start re-writes the same addresses from the cache.
	 */
	for (out = 0; out < 6; out++) {
		unsigned int blk = bf_xpoint_block[out];

		for (src = 0; src < 14; src++) {
			ret = bf_xpoint_write(chip, out, src, BF_FADER_0DB,
					      BF_FADER_0DB);
			if (ret < 0)
				return ret;
		}
		ret = bf_crosspoint_clear_cross(chip, blk);
		if (ret < 0)
			return ret;
	}

	/* Mirror the defaults into the control cache (14 controls/output). */
	for (out = 0; out < 6; out++)
		for (src = 0; src < 14; src++) {
			chip->xpoint[out][src][0] = BF_FADER_0DB;
			chip->xpoint[out][src][1] = BF_FADER_0DB;
		}

	/* Host settings word - bf_settings_write() is Internal-only at
	 * this point in the series (the clock-source control lands with
	 * a later patch).
	 */
	return bf_settings_write(chip);
}

/* The device resets its output masters to mute when a stream session
 * starts (hardware-verified 2026-08-24: after a stream start the
 * output stays silent until a master write lands - only a write
 * un-mutes the 8-bit register).  Re-apply the six output masters +
 * mutes from the cache; also used by the PM restore path.
 */
int bf_apply_masters(struct snd_usb_babyface *chip)
{
	int out, ret;
	u16 flag;

	for (out = 0; out < 6; out++) {
		u16 l = chip->muted[out] ? 0 : chip->master[out][0];
		u16 r = chip->muted[out] ? 0 : chip->master[out][1];
		u8 l8 = chip->muted[out] ? BF_MASTER_MUTE : bf_master_8bit(l);
		u8 r8 = chip->muted[out] ? BF_MASTER_MUTE : bf_master_8bit(r);

		ret = bf_vendor_write(chip, BF_REQ_GAIN, l8,
				      BF_REG_MASTER_8 + 2 * out);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_GAIN, r8,
				      BF_REG_MASTER_8 + 2 * out + 1);
		if (ret < 0)
			return ret;
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
				      (BF_REG_MASTER_16 + 2 * out) | flag);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
				      (BF_REG_MASTER_16 + 2 * out + 1) | flag);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* -- mixer controls ------------------------ */

/* dB TLV for the output masters: 0x2000 = 0 dB, 0x4000 = +6 dB
 * (CALIBRATION.md) with the hardware 20*log10(v/0x2000) law - the raw
 * 16-bit value IS the linear amplitude.  WirePlumber needs this to map
 * the volume 1:1 to the hardware control instead of applying a software
 * volume on top (which left the output ~30 dB down).
 */
static const DECLARE_TLV_DB_RANGE(bf_master_tlv,
	0, 0x2000, TLV_DB_LINEAR_ITEM(-6500, 0),
	0x2000, 0x4000, TLV_DB_LINEAR_ITEM(0, 600)
);

static int bf_master_info(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 0x4000;	/* +6 dB = 2 x 0dB(0x2000) */
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_master_get(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int out = bf_master_out[kctl->private_value];

	ucontrol->value.integer.value[0] = chip->master[out][0];
	ucontrol->value.integer.value[1] = chip->master[out][1];
	return 0;
}

static int bf_master_put(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int out = bf_master_out[kctl->private_value];
	u16 l = ucontrol->value.integer.value[0];
	u16 r = ucontrol->value.integer.value[1];
	u16 flag;
	int ret = 0;

	/* The control is declared 0..0x4000 (+6 dB); reject anything outside
	 * so the 16-bit companion register and the cache stay in spec (the
	 * ALSA core only enforces this with CONFIG_SND_CTL_INPUT_VALIDATION).
	 */
	if (l > 0x4000 || r > 0x4000)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (l == chip->master[out][0] && r == chip->master[out][1])
		goto out;

	flag = bf_flag_cycle[chip->flag_cnt];
	chip->flag_cnt = (chip->flag_cnt + 1) & 3;

	/* The 8-bit register is the real volume; the 16-bit is its
	 * companion (kept in sync like TotalMix).
	 */
	ret = bf_vendor_write(chip, BF_REQ_GAIN, bf_master_8bit(l),
			      BF_REG_MASTER_8 + 2 * out);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_GAIN, bf_master_8bit(r),
			      BF_REG_MASTER_8 + 2 * out + 1);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
			      (BF_REG_MASTER_16 + 2 * out) | flag);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
			      (BF_REG_MASTER_16 + 2 * out + 1) | flag);
	if (ret < 0)
		goto out;

	chip->master[out][0] = l;
	chip->master[out][1] = r;
	chip->muted[out] = false;
	/* The DIM engaged/dim_saved re-base hook lands with the DIM
	 * switch (a later patch in this series) - chip->dim does not
	 * exist yet at this point in the split.
	 */
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int bf_mute_info(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int bf_mute_get(struct snd_kcontrol *kctl,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int out = bf_master_out[kctl->private_value];

	/* ALSA convention: 1 = enabled (sound on) = not muted. */
	ucontrol->value.integer.value[0] = !chip->muted[out];
	ucontrol->value.integer.value[1] = !chip->muted[out];
	return 0;
}

static int bf_mute_put(struct snd_kcontrol *kctl,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int out = bf_master_out[kctl->private_value];
	bool muted = !ucontrol->value.integer.value[0];
	u16 flag;
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (muted == chip->muted[out])
		goto out;

	flag = bf_flag_cycle[chip->flag_cnt];
	chip->flag_cnt = (chip->flag_cnt + 1) & 3;

	if (muted) {
		ret = bf_vendor_write(chip, BF_REQ_GAIN, BF_MASTER_MUTE,
				      BF_REG_MASTER_8 + 2 * out);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_GAIN, BF_MASTER_MUTE,
				      BF_REG_MASTER_8 + 2 * out + 1);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000,
				      (BF_REG_MASTER_16 + 2 * out) | flag);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000,
				      (BF_REG_MASTER_16 + 2 * out + 1) | flag);
		if (ret < 0)
			goto out;
	} else {
		/* Unmute restores the cached volume (TotalMix keeps the
		 * pre-mute fader value host-side), 8-bit + 16-bit.
		 */
		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      bf_master_8bit(chip->master[out][0]),
				      BF_REG_MASTER_8 + 2 * out);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      bf_master_8bit(chip->master[out][1]),
				      BF_REG_MASTER_8 + 2 * out + 1);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->master[out][0],
				      (BF_REG_MASTER_16 + 2 * out) | flag);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->master[out][1],
				      (BF_REG_MASTER_16 + 2 * out + 1) | flag);
		if (ret < 0)
			goto out;
	}
	chip->muted[out] = muted;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* -- crosspoint matrix (6 outputs x 14 sources) -------------- */

/* The crosspoint fader is linear in amplitude: BF_FADER_0DB (0x16a0) is
 * unity and BF_FADER_TOP (0x2d41) is exactly twice that, i.e. +6 dB - see
 * bf_fader_curve, whose whole span follows raw = BF_FADER_0DB * 10^(dB/20).
 * Raw 0 is off.
 */
static const DECLARE_TLV_DB_LINEAR(bf_xpoint_tlv, TLV_DB_GAIN_MUTE, 600);

/* Write a crosspoint slot on the wire: the standard map always, and -
 * for the AN1/2 output only - the low map as well.
 *
 * HARDWARE-VERIFIED 2026-09-14: AN1/2 is not just another output with
 * a redundant "shadow" register, despite what this file used to say.
 * Sweeping only the standard map (BF_REG_CROSS_BASE_*) into AN1/2
 * produced no audible change at all, off through +6 dB, with two
 * independent sources (a generated tone via PB1, a live mic via AN2);
 * the exact same code path targeting any other output (verified on
 * PH3/4) tracked the fader correctly, off to +6 dB within 0.6 dB.
 * PROTOCOL.md's "Scene load" capture explains why: the vendor software
 * always writes BOTH the standard map and the low map
 * (BF_REG_LOWMAP_BASE_*) together for AN1/2's own crosspoints, at the
 * same value - the low map is what actually feeds that output's sum;
 * the standard map alone is not enough. Every other output only has a
 * standard map.
 *
 * bf_split_apply() already knew this (it writes both for AN1/2's
 * playback pairs); this generalises the same pattern to the plain
 * fader.
 */
int bf_xpoint_write(struct snd_usb_babyface *chip, int out, int src,
		    u16 l, u16 r)
{
	unsigned int blk = bf_xpoint_block[out];
	const struct bf_source *s = &bf_sources[src];
	u16 flag;
	int ret;

	if (out == 0) {
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
				      BF_REG_LOWMAP_BASE_L + s->idx_l);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
				      BF_REG_LOWMAP_BASE_R + s->idx_r);
		if (ret < 0)
			return ret;
	}

	flag = bf_flag_cycle[chip->flag_cnt];
	chip->flag_cnt = (chip->flag_cnt + 1) & 3;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
			      (BF_REG_CROSS_BASE_L + BF_REG_CROSS_STRIDE * blk +
			       s->idx_l) | flag);
	if (ret < 0)
		return ret;
	return bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
			       (BF_REG_CROSS_BASE_R + BF_REG_CROSS_STRIDE * blk +
				s->idx_r) | flag);
}

static int bf_xpoint_info(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = BF_FADER_TOP;	/* +6 dB fader top */
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_xpoint_get(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int out = kctl->private_value >> 8;
	int src = kctl->private_value & 0xff;

	ucontrol->value.integer.value[0] = chip->xpoint[out][src][0];
	ucontrol->value.integer.value[1] = chip->xpoint[out][src][1];
	return 0;
}

static int bf_xpoint_put(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int out = kctl->private_value >> 8;
	int src = kctl->private_value & 0xff;
	u16 l = ucontrol->value.integer.value[0];
	u16 r = ucontrol->value.integer.value[1];
	int ret = 0;

	if (l > BF_FADER_TOP || r > BF_FADER_TOP)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (l == chip->xpoint[out][src][0] && r == chip->xpoint[out][src][1])
		goto out;

	ret = bf_xpoint_write(chip, out, src, l, r);
	if (ret < 0)
		goto out;

	chip->xpoint[out][src][0] = l;
	chip->xpoint[out][src][1] = r;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

int babyface_create_xpoints(struct snd_usb_babyface *chip)
{
	struct snd_kcontrol *kctl;
	int out, src, err;

	for (out = 0; out < 6; out++) {
		for (src = 0; src < 14; src++) {
			kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
				.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
				.name = "Playback Volume",
				.index = out * 14 + src,
				.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
					  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
				.info = bf_xpoint_info,
				.get = bf_xpoint_get,
				.put = bf_xpoint_put,
				.tlv.p = bf_xpoint_tlv,
				.private_value = (out << 8) | src,
			}, chip);
			/* Name the control by its source: "AN1 Playback Volume",
			 * "PB1 Playback Volume"... with a unique index.
			 */
			strscpy(kctl->id.name, bf_sources[src].name,
				sizeof(kctl->id.name));
			strlcat(kctl->id.name, " Playback Volume",
				sizeof(kctl->id.name));
			err = snd_ctl_add(chip->card, kctl);
			if (err < 0)
				return err;
		}
	}

	return 0;
}

int babyface_create_masters(struct snd_usb_babyface *chip)
{
	static const char * const out_names[6] = {
		"AN1/2", "PH3/4", "AS1/2", "ADAT3/4", "ADAT5/6", "ADAT7/8"
	};
	struct snd_kcontrol *kctl;
	int i, err;

	for (i = 0; i < 6; i++) {
		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = out_names[i],
			.index = i,
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
				  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
			.info = bf_master_info,
			.get = bf_master_get,
			.put = bf_master_put,
			.tlv.p = bf_master_tlv,
			.private_value = i,
		}, chip);
		strlcat(kctl->id.name, " Playback Volume", sizeof(kctl->id.name));
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;

		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = out_names[i],
			.index = i,
			.info = bf_mute_info,
			.get = bf_mute_get,
			.put = bf_mute_put,
			.private_value = i,
		}, chip);
		strlcat(kctl->id.name, " Playback Switch", sizeof(kctl->id.name));
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;

		dev_dbg(&chip->dev->dev, "output %d = %s\n", i, out_names[i]);
	}

	return 0;
}

