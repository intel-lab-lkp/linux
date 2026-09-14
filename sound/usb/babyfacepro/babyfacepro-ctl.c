// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS - proprietary-mode USB audio driver
 *
 * ALSA control surface: mixer (masters, preamp, crosspoints, flags,
 * gains)
 * (3-band + low cut).
 *
 * See babyfacepro.h for the shared device state and register map,
 * and babyfacepro.c for the core driver (protocol, PCM streaming,
 * state persistence, card lifecycle).
 */
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "babyfacepro.h"

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
 * default.
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

	/* Host settings word - composed from tracked state (clock defaults
	 * to Internal, chip->clock_optical is zero-initialized).
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
	/* A Phones change while DIM is engaged re-bases the restore point. */
	if (chip->dim && out == 1) {
		chip->dim_saved[0] = l;
		chip->dim_saved[1] = r;
	}
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

int bf_preamp_state_write(struct snd_usb_babyface *chip)
{
	int ret;

	ret = bf_vendor_write(chip, BF_REQ_PREAMP, chip->preamp, BF_REG_PREAMP);
	if (ret < 0)
		return ret;
	/* Boost's 0x21 commit value (0x0003) is NOT a persisted register
	 * bit - PROTOCOL.md's "Ref level" section found it only in the
	 * one-shot 0x21 value alongside the 0x17 state write, so it has
	 * to be re-sent alongside EVERY preamp write (phantom/PAD toggles
	 * included), or Boost would silently degrade to plain -10dBV the
	 * next time anything else touches this shared byte.
	 */
	return bf_vendor_write(chip, BF_REQ_PREAMP_COMMIT,
			       chip->ref_level == BF_REF_LEVEL_BOOST ?
			       0x0003 : 0x0000, 0x0000);
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

/* Phase (polarity) invert (AN1-4 only, PROTOCOL.md "Phase toggle",
 * hardware-verified 2026-08-23): NEGATE (bitwise NOT, not two's
 * complement) the L crosspoint register on every output pair's
 * standard map, plus the AN1/2 low-map shadow specifically (the same
 * single low-map register set CUE/mute/solo already use for that
 * monitor bus - see this driver's own bf_ms_put for the address
 * pattern). `chip->xpoint[out][mic][0]` is deliberately left holding
 * the PLAIN value the user actually set - only the value WRITTEN to
 * hardware is negated - so the crosspoint control's own readback still
 * reports the real fader position while phase is engaged.
 *
 * KNOWN LIMITATION, same class TuxMix's own USB backend already has
 * in `usb.rs::set_phase` (not fixed there either, as of this writing):
 * this negates the CURRENT register value once, at toggle time. A
 * later `bf_xpoint_put` on the same [out][mic] slot (i.e. the user
 * drags that fader again while phase is engaged) writes the plain
 * value, silently un-inverting phase until the user re-toggles it.
 * Making the crosspoint hot path itself phase-aware would close this
 * properly, but touches every one of the 84 crosspoint controls'
 * write path - out of scope for this pass; flagged rather than
 * silently shipped.
 */
int bf_phase_apply(struct snd_usb_babyface *chip, int mic, bool invert)
{
	const struct bf_source *s = &bf_sources[mic];
	int out, ret;
	u16 flag;

	for (out = 0; out < 6; out++) {
		unsigned int blk = bf_xpoint_block[out];
		u16 plain = chip->xpoint[out][mic][0];
		u16 value = invert ? (u16)~plain : plain;

		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, value,
				      (BF_REG_CROSS_BASE_L +
				       BF_REG_CROSS_STRIDE * blk + s->idx_l) |
				      flag);
		if (ret < 0)
			return ret;

		if (out == 0) {
			ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, value,
					      s->idx_l);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

static int bf_phase_info(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int bf_phase_get(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int mic = kctl->private_value;

	ucontrol->value.integer.value[0] = chip->phase[mic];
	return 0;
}

static int bf_phase_put(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int mic = kctl->private_value;
	bool invert = ucontrol->value.integer.value[0];
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (invert == chip->phase[mic])
		goto out;
	ret = bf_phase_apply(chip, mic, invert);
	if (ret < 0)
		goto out;
	chip->phase[mic] = invert;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* Stereo split (PROTOCOL.md "Stereo split", cap_ctrl3.pcap, hardware-
 * verified): a playback pair's signal into the AN1/2 monitor bus goes
 * hard-split (L=0x2000/R=0x0000, "split-mono") instead of the normal
 * stereo pair (L=R=0x1000, -6 dB each side) - fixed constants, not
 * derived from the current fader value (unlike Phase, there's nothing
 * to preserve), matching TuxMix's own `usb.rs::set_stereo_split`
 * exactly. Only reaches the AN1/2 destination (low map + that output's
 * standard crosspoint block) - same scope as CUE/mute/solo's own
 * low-map-only reach. `chip->xpoint[][]` is deliberately left
 * untouched, same reasoning as Phase.
 */
int bf_split_apply(struct snd_usb_babyface *chip, int pb, bool split)
{
	const struct bf_source *s = &bf_sources[8 + pb];
	unsigned int blk = bf_xpoint_block[0]; /* AN1/2 output */
	u16 l = split ? 0x2000 : 0x1000;
	u16 r = split ? 0x0000 : 0x1000;
	int ret;

	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
			      BF_REG_LOWMAP_BASE_L + s->idx_l);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
			      BF_REG_LOWMAP_BASE_R + s->idx_r);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
			      (BF_REG_CROSS_BASE_L + BF_REG_CROSS_STRIDE * blk +
			       s->idx_l));
	if (ret < 0)
		return ret;
	return bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
			       (BF_REG_CROSS_BASE_R + BF_REG_CROSS_STRIDE * blk +
				s->idx_r));
}

static int bf_split_get(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int pb = kctl->private_value;

	ucontrol->value.integer.value[0] = chip->split[pb];
	return 0;
}

static int bf_split_put(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int pb = kctl->private_value;
	bool split = ucontrol->value.integer.value[0];
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (split == chip->split[pb])
		goto out;
	ret = bf_split_apply(chip, pb, split);
	if (ret < 0)
		goto out;
	chip->split[pb] = split;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* bf_fader_raw_to_db2()/bf_fader_db2_to_raw() (the crosspoint fader
 * curve) are defined further down in this file, alongside the
 * front-panel wheel code that was their first user - forward-declared
 * here rather than moved, to keep this diff to additions only.
 */
static int bf_fader_raw_to_db2(u16 raw);
static u16 bf_fader_db2_to_raw(int db2);

static int bf_trim_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo);
static int bf_trim_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol);
static int bf_trim_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol);

/* Input Trim (T button, AN1-4): PROTOCOL.md "Trim (T) write for the
 * AN1/2 pair" (cap_trim2/3/4.pcap, hardware-verified) - the analog
 * input's own gain-trim, applied through the crosspoint registers
 * exactly like a fader (there is no separate trim register). Two
 * different curves combine: the low map holds the trim ALONE on the
 * MASTER curve (0x2000 = 0 dB, `bf_master_16bit`); the standard map
 * holds fader+trim SUMMED on the FADER curve (`bf_fader_db2_to_raw`).
 * Always writes all 8 registers for the pair (both AN1+AN2 or both
 * AN3+AN4, matching the vendor software's linked-strip behaviour);
 * `mic` may be either channel of the pair, and the base is derived
 * (`mic & ~1`) so the write always lands on the correct pair's
 * registers regardless of which channel's control triggered it.
 * Destination is always the AN1/2 monitor bus, the same scope the
 * MS-processor and CUE writes have, and the same one the vendor
 * software's Trim reaches.
 *
 * Trim is a genuinely SHARED value per pair on real hardware (one
 * write always touches both channels' registers) but is exposed as 2
 * per-channel ALSA controls, one per input strip. `bf_trim_put` keeps
 * the pair's two cache entries equal and notifies the sibling control,
 * so the cache never claims a per-channel split the hardware cannot
 * represent - and `bf_state_apply_flags`, which replays the pair from
 * its even index, always replays the value that is actually on the
 * wire.
 *
 * ONE KNOWN LIMITATION, kept rather than silently hidden:
 *    Same class as Phase (see `bf_phase_apply`'s own comment):
 *    `chip->xpoint[0][mic][0]` is read here for the CURRENT fader
 *    value but never written back - the standard-map register ends up
 *    holding fader+trim while the cache still holds the plain fader,
 *    so a later `bf_xpoint_put` on the same slot writes the plain
 *    value, silently dropping trim from the combined register until
 *    this is re-applied. Not fixed for the same reason Phase wasn't:
 *    touches the shared 84-crosspoint write path, out of scope here.
 */
int bf_trim_apply(struct snd_usb_babyface *chip, int mic, int trim_db2)
{
	int base = mic & ~1;
	int sib = base + 1;
	const struct bf_source *sb = &bf_sources[base];
	const struct bf_source *ss = &bf_sources[sib];
	unsigned int blk = bf_xpoint_block[0]; /* AN1/2 output */
	u16 trim_raw = bf_master_16bit(trim_db2);
	int fader_db2 = bf_fader_raw_to_db2(chip->xpoint[0][base][0]);
	u16 standard_raw = bf_fader_db2_to_raw(fader_db2 + trim_db2);
	int ret;

	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, trim_raw,
			      BF_REG_LOWMAP_BASE_L + sb->idx_l);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, trim_raw,
			      BF_REG_LOWMAP_BASE_R + sb->idx_r);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, trim_raw,
			      BF_REG_LOWMAP_BASE_L + ss->idx_l);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, trim_raw,
			      BF_REG_LOWMAP_BASE_R + ss->idx_r);
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, standard_raw,
			      (BF_REG_CROSS_BASE_L + BF_REG_CROSS_STRIDE * blk +
			       sb->idx_l));
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, standard_raw,
			      (BF_REG_CROSS_BASE_R + BF_REG_CROSS_STRIDE * blk +
			       sb->idx_r));
	if (ret < 0)
		return ret;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, standard_raw,
			      (BF_REG_CROSS_BASE_L + BF_REG_CROSS_STRIDE * blk +
			       ss->idx_l));
	if (ret < 0)
		return ret;
	return bf_vendor_write(chip, BF_REQ_CROSSPOINT, standard_raw,
			       (BF_REG_CROSS_BASE_R + BF_REG_CROSS_STRIDE * blk +
				ss->idx_r));
}

/* Trim's control value is dB as well, -65..+6. */
static const DECLARE_TLV_DB_SCALE(bf_trim_tlv, -6500, 100, 0);

static int bf_trim_info(struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = -65;
	uinfo->value.integer.max = 6;
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_trim_get(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int mic = kctl->private_value;

	ucontrol->value.integer.value[0] = chip->trim[mic];
	return 0;
}

static int bf_trim_put(struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int mic = kctl->private_value;
	int sib = mic ^ 1;
	int db = ucontrol->value.integer.value[0];
	int ret = 0;

	if (db < -65 || db > 6)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (db == chip->trim[mic])
		goto out;
	ret = bf_trim_apply(chip, mic, db * 2);
	if (ret < 0)
		goto out;
	/* One register per pair on the wire, so both channels of the pair
	 * really did change: mirror the cache (the state restore replays
	 * the pair from the even index) and tell user space about the
	 * sibling control.
	 */
	chip->trim[mic] = db;
	chip->trim[sib] = db;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	if (ret == 1 && chip->trim_kctl[sib])
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       &chip->trim_kctl[sib]->id);
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

	for (src = 0; src < 4; src++) {
		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Phase Switch",
			.index = src,
			.info = bf_phase_info,
			.get = bf_phase_get,
			.put = bf_phase_put,
			.private_value = src,
		}, chip);
		strscpy(kctl->id.name, bf_sources[src].name, sizeof(kctl->id.name));
		strlcat(kctl->id.name, " Phase Switch", sizeof(kctl->id.name));
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;
	}

	for (src = 0; src < 6; src++) {
		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Stereo Split Switch",
			.index = src,
			.info = bf_phase_info, /* plain boolean, same shape */
			.get = bf_split_get,
			.put = bf_split_put,
			.private_value = src,
		}, chip);
		strscpy(kctl->id.name, bf_sources[8 + src].name, sizeof(kctl->id.name));
		strlcat(kctl->id.name, " Stereo Split Switch", sizeof(kctl->id.name));
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;
	}

	for (src = 0; src < 4; src++) {
		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Trim Volume",
			.index = src,
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
				  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
			.info = bf_trim_info,
			.get = bf_trim_get,
			.put = bf_trim_put,
			.tlv.p = bf_trim_tlv,
			.private_value = src,
		}, chip);
		chip->trim_kctl[src] = kctl;
		strscpy(kctl->id.name, bf_sources[src].name, sizeof(kctl->id.name));
		strlcat(kctl->id.name, " Trim Volume", sizeof(kctl->id.name));
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;
	}
	return 0;
}

/* -- flags / special controls (pitch, loopback, link, width, FX) -- */

static int bf_switch_info(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int bf_pitch_info(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = -50;		/* -5.0 % */
	uinfo->value.integer.max = 50;		/* +5.0 % */
	uinfo->value.integer.step = 1;		/* 0.1 % */
	return 0;
}

static int bf_pitch_get(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->pitch;
	return 0;
}

static int bf_pitch_put(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int p = ucontrol->value.integer.value[0];
	u32 dds24, dds16;
	u16 frac, b1, b2;
	int ret = 0;

	if (p < -50 || p > 50)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (p == chip->pitch)
		goto out;

	/* The 0x1B DDS quad (16.8 fixed point, banked).  p is 0.1 % steps:
	 * DDS_24 = round(50000*256/(1+p/1000)) = round(12800000000/(1000+p)).
	 */
	dds24 = div_u64(12800000000ULL + (u32)(1000 + p) / 2, 1000 + p);
	dds16 = dds24 >> 8;
	frac = dds24 & 0xff;
	b1 = (u16)div_u64(dds16 * 72562ull + 50000, 100000);
	b2 = (u16)((dds16 * 2 + 1) / 3);

	ret = bf_vendor_write(chip, BF_REQ_DDS, (u16)dds16, (frac << 8) | 0);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_DDS, b1, 0x0001);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_DDS, b2, 0x0002);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_DDS, 0x7cff, 0x0003);
	if (ret < 0)
		goto out;
	/* Every quad must be followed by the settings keepalive - composed
	 * from tracked state so this doesn't silently force the clock back
	 * to Internal if Optical was engaged (the bug the hardcoded 0x0001
	 * here used to have, same class as the settings-word flag-stomping
	 * this driver's sibling TuxMix project already hit and fixed).
	 */
	ret = bf_settings_write(chip);
	if (ret < 0)
		goto out;

	chip->pitch = p;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int bf_loopback_get(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int out = kctl->private_value;

	ucontrol->value.integer.value[0] = chip->loopback[out];
	ucontrol->value.integer.value[1] = chip->loopback[out];
	return 0;
}

/* Write the full 30-channel loopback map: pair (2*out, 2*out+1) at
 * `on` (0x0001/0x0000), all other channels cleared - exactly what
 * TotalMix sends on every loopback toggle (cap_loopback2.pcap).  The
 * full-map write is also the reliable OFF (the old per-pair write
 * sometimes failed to disengage on the hardware).
 */
int bf_loopback_write_map(struct snd_usb_babyface *chip, int out,
			  bool on)
{
	int ch, ret;

	for (ch = 0; ch < BF_LOOPBACK_CHANNELS; ch++) {
		u16 val = (on && (ch == out * 2 || ch == out * 2 + 1))
			  ? 0x0001 : 0x0000;

		ret = bf_vendor_write(chip, BF_REQ_LOOPBACK, val, ch);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int bf_loopback_put(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int out = kctl->private_value;
	bool on = ucontrol->value.integer.value[0];
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (on == chip->loopback[out])
		goto out;
	ret = bf_loopback_write_map(chip, out, on);
	if (ret < 0)
		goto out;
	/* Single-active model (TotalMix writes one pair at 0x0001, the
	 * rest 0x0000): toggling one output clears the others.
	 */
	memset(chip->loopback, 0, sizeof(chip->loopback));
	chip->loopback[out] = on;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int bf_an12_get(struct snd_kcontrol *kctl,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->an12;
	return 0;
}

static int bf_an12_put(struct snd_kcontrol *kctl,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	bool an12 = ucontrol->value.integer.value[0];
	u16 v;
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (an12 == chip->an12)
		goto out;
	v = (chip->linked ? 0x0400 : 0x0000) | (an12 ? 0x1000 : 0x0000);
	ret = bf_vendor_write(chip, BF_REQ_PREAMP, v, 0x1000);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_PREAMP_COMMIT, 0x0000, 0x0000);
	if (ret < 0)
		goto out;
	chip->an12 = an12;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* Clock source (PROTOCOL.md "Clock source / no-lock state",
 * hardware-verified 2026-08-22, clktest.c): NOT a register write at
 * all - only the BF_REG_KEEPALIVE_SETTINGS word changes (bit 2 =
 * Optical). Matches the naming TuxMix's ALSA backend already looks
 * for ("Sample Clock Source", the same name found on the stock
 * snd-usb-audio Class-Compliant driver) so it picks this control up
 * with zero changes on that side.
 */
static const char *const bf_clock_texts[] = {
	"Internal", "Optical In", NULL
};

static int bf_clock_info(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 2, bf_clock_texts);
}

static int bf_clock_get(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->clock_optical ? 1 : 0;
	return 0;
}

static int bf_clock_put(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	bool optical = ucontrol->value.enumerated.item[0] != 0;
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (optical == chip->clock_optical)
		goto out;
	chip->clock_optical = optical;
	ret = bf_settings_write(chip);
	if (ret < 0) {
		chip->clock_optical = !optical;
		goto out;
	}
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int bf_link_get(struct snd_kcontrol *kctl,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->linked;
	return 0;
}

static int bf_link_put(struct snd_kcontrol *kctl,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	bool linked = ucontrol->value.integer.value[0];
	u16 v;
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (linked == chip->linked)
		goto out;
	v = (linked ? 0x0400 : 0x0000) | (chip->an12 ? 0x1000 : 0x0000);
	ret = bf_vendor_write(chip, BF_REQ_PREAMP, v, 0x1000);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_PREAMP_COMMIT, 0x0000, 0x0000);
	if (ret < 0)
		goto out;
	chip->linked = linked;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int bf_ms_get(struct snd_kcontrol *kctl,
		     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->ms_proc;
	return 0;
}

/* MS-proc: engage per the cap_ms2.pcap ON pattern - write 0x0000 to
 * ALL FOUR AN2 (side) crosspoints: standard map 0x0035/0x004F (L/R)
 * + low map 0x0001/0x001B (L/R) - the side path is muted (ear-
 * verified 2026-08-26 with the mic on AN2: MS ON = silence); release
 * restores the cached fader values (host-side, like TotalMix).
 * (The 0x1000/0x0004 writes are the DISENGAGE restore values seen in
 * cap_ms2 - the driver had them inverted on the engage path.)
 */
static int bf_ms_put(struct snd_kcontrol *kctl,
		     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	bool on = ucontrol->value.integer.value[0];
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (on == chip->ms_proc)
		goto out;
	if (on) {
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x0035);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x004f);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x0001);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0000, 0x001b);
		if (ret < 0)
			goto out;
	} else {
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->xpoint[1][1][0], 0x0001);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->xpoint[1][1][0], 0x0035);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->xpoint[1][1][1], 0x001b);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->xpoint[1][1][1], 0x004f);
		if (ret < 0)
			goto out;
	}
	chip->ms_proc = on;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* DIM - cap_dim2.pcap: an absolute -20 dB on the Phones master
 * (out 1: 8-bit 0xCB / 16-bit 0x0333) regardless of the current level,
 * plus the 0x17 wVal=0x2000 wIdx=0x2000 flag; release restores the
 * pre-DIM master host-side.  The master cache keeps the real volume.
 */
static int bf_dim_get(struct snd_kcontrol *kctl,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->dim;
	return 0;
}

/* Apply DIM on the wire.  chip->mutex must be held: this is reached both
 * from the ALSA control and from the front-panel poll, and taking the
 * lock here instead would self-deadlock one of the two.
 */
static int bf_dim_apply(struct snd_usb_babyface *chip, bool on)
{
	u16 flag;
	int ret;

	lockdep_assert_held(&chip->mutex);
	if (on) {
		chip->dim_saved[0] = chip->master[1][0];
		chip->dim_saved[1] = chip->master[1][1];
		ret = bf_vendor_write(chip, BF_REQ_GAIN, BF_MASTER_MINUS20_8,
				      BF_REG_MASTER_8 + 2 * 1);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_GAIN, BF_MASTER_MINUS20_8,
				      BF_REG_MASTER_8 + 2 * 1 + 1);
		if (ret < 0)
			return ret;
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      BF_MASTER_MINUS20_16,
				      (BF_REG_MASTER_16 + 2 * 1) | flag);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      BF_MASTER_MINUS20_16,
				      (BF_REG_MASTER_16 + 2 * 1 + 1) | flag);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_PREAMP, 0x2000, 0x2000);
		if (ret < 0)
			return ret;
	} else {
		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      bf_master_8bit(chip->dim_saved[0]),
				      BF_REG_MASTER_8 + 2 * 1);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      bf_master_8bit(chip->dim_saved[1]),
				      BF_REG_MASTER_8 + 2 * 1 + 1);
		if (ret < 0)
			return ret;
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->dim_saved[0],
				      (BF_REG_MASTER_16 + 2 * 1) | flag);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->dim_saved[1],
				      (BF_REG_MASTER_16 + 2 * 1 + 1) | flag);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_PREAMP, 0x0000, 0x2000);
		if (ret < 0)
			return ret;
	}
	chip->dim = on;
	return 0;
}

static int bf_dim_put(struct snd_kcontrol *kctl,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	bool on = ucontrol->value.integer.value[0];
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (on == chip->dim)
		goto out;
	ret = bf_dim_apply(chip, on);
	if (ret == 0)
		ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int bf_width_info(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = -100;
	uinfo->value.integer.max = 100;
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_width_get(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->width;
	return 0;
}

static int bf_width_put(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int w = ucontrol->value.integer.value[0];
	u16 l, r;
	int ret = 0;

	if (w < -100 || w > 100)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (w == chip->width)
		goto out;
	/* Width spread: L = 0x1000*(1+w), R = 0x1000*(1-w), L+R = 0x2000.
	 * TotalMix writes the strip's src pair on BOTH maps (cap_width3-7,
	 * PROTOCOL.md "Width strip mapping"): the low map (0x0000+src L /
	 * 0x001A+src R) and the std block-0 map (0x0034+src L /
	 * 0x004E+src R) - the stereo pair spreads L/R in opposition, the
	 * mirror src (AN2) gets the swapped values.
	 */
	l = (u16)(((0x2000 * (100 + w) / 2) + 50) / 100);
	r = 0x2000 - l;
	/* Low map: AN1 L=0x0000, R=0x001A; AN2 L=0x0001, R=0x001B. */
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l, 0x0000);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r, 0x001a);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r, 0x0001);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l, 0x001b);
	if (ret < 0)
		goto out;
	/* Std block-0 map (item 0b, the missing half): AN1 L=0x0034,
	 * R=0x004E; AN2 L=0x0035, R=0x004F.  (The playback strips PB2-6
	 * target block n-2 - 0x00AE family - reserved for the per-strip
	 * controls.)
	 */
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l, 0x0034);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r, 0x004e);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r, 0x0035);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l, 0x004f);
	if (ret < 0)
		goto out;
	chip->width = w;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

static int bf_fx_send_info(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 0x1000;
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_fx_send_get(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->fx_send;
	return 0;
}

static int bf_fx_send_put(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	u16 v = ucontrol->value.integer.value[0];
	int ret = 0;

	if (v > 0x1000)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (v == chip->fx_send)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, v, 0x0138);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, v, 0x0153);
	if (ret < 0)
		goto out;
	chip->fx_send = v;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

int babyface_create_flags(struct snd_usb_babyface *chip)
{
	struct snd_kcontrol *kctl;
	int i, err;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Varispeed Pitch",
		.info = bf_pitch_info,
		.get = bf_pitch_get,
		.put = bf_pitch_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	for (i = 0; i < 6; i++) {
		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Loopback Switch",
			.index = i,
			.info = bf_mute_info,
			.get = bf_loopback_get,
			.put = bf_loopback_put,
			.private_value = i,
		}, chip);
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;
	}

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "AN 1>2 Switch",
		.info = bf_switch_info,
		.get = bf_an12_get,
		.put = bf_an12_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Sample Clock Source",
		.info = bf_clock_info,
		.get = bf_clock_get,
		.put = bf_clock_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "AN1/2 Link Switch",
		.info = bf_switch_info,
		.get = bf_link_get,
		.put = bf_link_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "MS Processor Switch",
		.info = bf_switch_info,
		.get = bf_ms_get,
		.put = bf_ms_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Dim Switch",
		.info = bf_switch_info,
		.get = bf_dim_get,
		.put = bf_dim_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Width",
		.info = bf_width_info,
		.get = bf_width_get,
		.put = bf_width_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "FX Send Volume",
		.info = bf_fx_send_info,
		.get = bf_fx_send_get,
		.put = bf_fx_send_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	return 0;
}

static int bf_bool_info(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int bf_phantom_get(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] =
		!!(chip->preamp & kctl->private_value);
	return 0;
}

static int bf_phantom_put(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	u16 bit = kctl->private_value;
	bool on = ucontrol->value.integer.value[0];
	bool cur = !!(chip->preamp & bit);
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (on == cur)
		goto out;
	chip->preamp = on ? (chip->preamp | bit) : (chip->preamp & ~bit);
	ret = bf_preamp_state_write(chip);
	if (ret < 0)
		goto out;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* Ref Level (Instr 3/4) - see the constants' own comment in the
 * header. A single shared 3-state switch, not per-channel (the
 * protocol has no independent bits for IN3 vs IN4).
 */
static const char *const bf_reflevel_texts[] = {
	"+4dBu", "-10dBV", "Boost", NULL
};

static int bf_reflevel_info(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 3, bf_reflevel_texts);
}

static int bf_reflevel_get(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->ref_level;
	return 0;
}

static int bf_reflevel_put(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	unsigned int item = ucontrol->value.enumerated.item[0];
	u16 old_preamp;
	int old_ref_level;
	int ret = 0;

	if (item > BF_REF_LEVEL_BOOST)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if ((int)item == chip->ref_level)
		goto out;
	old_preamp = chip->preamp;
	old_ref_level = chip->ref_level;
	chip->preamp = (chip->preamp & ~BF_PREAMP_REF_MASK) |
		       (item == BF_REF_LEVEL_4DBU ? BF_PREAMP_REF_4DBU : 0);
	chip->ref_level = item;
	ret = bf_preamp_state_write(chip);
	if (ret < 0) {
		chip->preamp = old_preamp;
		chip->ref_level = old_ref_level;
		goto out;
	}
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

/* Gain scales.
 *
 * The mic preamps (AN1/2) span 0-65 dB in 1 dB steps, carried in a
 * packed byte rather than a plain count:
 *
 *	coarse = min(db / 3, 20)	bits 0-4, 3 dB per step
 *	fine   = db - 3 * coarse	bits 5-7, the 0-2 dB remainder
 *	value  = (fine << 5) | coarse
 *
 * Above 60 dB coarse saturates at 20 and fine continues 3, 4, 5, so
 * 65 dB is 0xb4.  Decoded from USBPcap captures of TotalMix on
 * Windows (bbf-gain2/3/4.pcap, 48 writes, all matching).
 *
 * Bits 5-7 were previously read as a transaction counter and written
 * with a rotating 0x20/0x00/0x40, which both discarded the fine part
 * of the setting and applied 0-2 dB of error depending on where the
 * rotation happened to be.
 *
 * The Hi-Z instrument inputs (AN3/4) are not packed: the value is the
 * gain in 0.5 dB units, 0-9 dB over 0-18.
 */
int bf_gain_max_db(int mic)
{
	return mic < 2 ? BF_GAIN_MAX_DB : 9;
}

int bf_gain_db(int mic, u8 raw)
{
	if (mic >= 2)
		return raw / 2;
	return 3 * (raw & BF_GAIN_COARSE_MASK) + (raw >> BF_GAIN_FINE_SHIFT);
}

u8 bf_gain_raw(int mic, int db)
{
	int coarse, fine;

	if (mic >= 2)
		return db * 2;
	coarse = min(db / 3, BF_GAIN_COARSE_MAX);
	fine = db - 3 * coarse;
	return (u8)((fine << BF_GAIN_FINE_SHIFT) | coarse);
}

/* The preamp control's value already IS the gain in dB (0..65 for the mic
 * inputs, 0..9 for the instrument ones), and the hardware really does
 * resolve every one of those steps - bf_gain_raw() packs it into the
 * register's coarse and fine fields.
 */
static const DECLARE_TLV_DB_SCALE(bf_gain_tlv, 0, 100, 0);

static int bf_gain_info(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = bf_gain_max_db(kctl->private_value);
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_gain_get(struct snd_kcontrol *kctl,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int mic = kctl->private_value;

	/* chip->gain[] tracks the dB; the packed register value is derived
	 * at write time (bf_gain_raw).
	 */
	ucontrol->value.integer.value[0] = chip->gain[mic];
	return 0;
}

static int bf_gain_put(struct snd_kcontrol *kctl,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int mic = kctl->private_value;
	int db = ucontrol->value.integer.value[0];
	u8 raw;
	int ret = 0;

	if (db < 0 || db > bf_gain_max_db(mic))
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (db == chip->gain[mic])
		goto out;
	raw = bf_gain_raw(mic, db);

	ret = bf_vendor_write(chip, BF_REQ_GAIN, (u16)raw,
			      BF_REG_GAIN + mic);
	if (ret < 0)
		goto out;
	chip->gain[mic] = db;
	ret = 1;
out:
	mutex_unlock(&chip->mutex);
	return ret;
}

int babyface_create_controls(struct snd_usb_babyface *chip)
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

	for (i = 0; i < 2; i++) {
		u16 bit = i == 0 ? BF_PREAMP_48V_MIC1 : BF_PREAMP_48V_MIC2;

		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Phantom Power Mic 1",
			.index = i,
			.info = bf_bool_info,
			.get = bf_phantom_get,
			.put = bf_phantom_put,
			.private_value = bit,
		}, chip);
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;
	}

	for (i = 0; i < 2; i++) {
		u16 bit = i == 0 ? BF_PREAMP_PAD_MIC1 : BF_PREAMP_PAD_MIC2;

		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Pad Mic 1",
			.index = i,
			.info = bf_bool_info,
			.get = bf_phantom_get,
			.put = bf_phantom_put,
			.private_value = bit,
		}, chip);
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;
	}

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Instrument Ref Level",
		.info = bf_reflevel_info,
		.get = bf_reflevel_get,
		.put = bf_reflevel_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;

	for (i = 0; i < 4; i++) {
		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Mic 1 Capture Volume",
			.index = i,
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
				  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
			.info = bf_gain_info,
			.get = bf_gain_get,
			.put = bf_gain_put,
			.tlv.p = bf_gain_tlv,
			.private_value = i,
		}, chip);
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;
	}
	return 0;
}

/* -- MIX-mode monitoring level (fader curve) ----------------
 * Calibrated crosspoint-fader curve (AN1->AN1/2, cap_calib.pcap
 * 2026-08-22; the same table as tuxmix-core/src/usb.rs FADER_CURVE).
 * dB stored x2 (half-dB grid): the MIX wheel steps +/-0.5 dB per click
 * on this curve (cap_mix.pcap).  0x0000 = -inf (digital mute),
 * 0x0003 = -62 dB, ... 0x2D41 = +6 dB.  Raw values interpolate linearly
 * between the 1-dB points.
 */
#define BF_FADER_DB2_INF	(-130)	/* -65 dB = the wheel's -inf floor */

static const struct bf_fader_pt {
	s16 db2;	/* dB x 2 */
	u16 raw;
} bf_fader_curve[] = {
	{ -124, 0x0003 }, { -122, 0x0004 }, { -120, 0x0005 },
	{ -118, 0x0006 }, { -116, 0x0007 }, { -114, 0x0008 },
	{ -112, 0x0009 }, { -110, 0x000a }, { -108, 0x000b },
	{ -106, 0x000d }, { -104, 0x000e }, { -102, 0x0010 },
	{ -100, 0x0012 }, {  -98, 0x0014 }, {  -96, 0x0017 },
	{  -94, 0x0019 }, {  -92, 0x001d }, {  -90, 0x0020 },
	{  -88, 0x0024 }, {  -86, 0x0029 }, {  -84, 0x002e },
	{  -82, 0x0033 }, {  -80, 0x003a }, {  -78, 0x0041 },
	{  -76, 0x0049 }, {  -74, 0x0051 }, {  -72, 0x005b },
	{  -70, 0x0067 }, {  -68, 0x0073 }, {  -66, 0x0081 },
	{  -64, 0x0091 }, {  -62, 0x00a3 }, {  -60, 0x00b7 },
	{  -58, 0x00cd }, {  -56, 0x00e6 }, {  -54, 0x0102 },
	{  -52, 0x0122 }, {  -50, 0x0145 }, {  -48, 0x016d },
	{  -46, 0x019a }, {  -44, 0x01cc }, {  -42, 0x0204 },
	{  -40, 0x0243 }, {  -38, 0x028a }, {  -36, 0x02d9 },
	{  -34, 0x0332 }, {  -32, 0x0396 }, {  -30, 0x0406 },
	{  -28, 0x0483 }, {  -26, 0x0510 }, {  -24, 0x05af },
	{  -22, 0x0660 }, {  -20, 0x0727 }, {  -18, 0x0807 },
	{  -16, 0x0902 }, {  -14, 0x0a1b }, {  -12, 0x0b57 },
	{  -10, 0x0cb9 }, {   -8, 0x0e47 }, {   -6, 0x1004 },
	{   -4, 0x11f9 }, {   -2, 0x142a }, {    0, 0x16a0 },
	{    2, 0x1963 }, {    4, 0x1c7c }, {    6, 0x1ff6 },
	{    8, 0x23dc }, {   10, 0x283d }, {   12, 0x2d41 },
};

/* Fader raw -> dBx2 (linear interpolation; raw 0 = -inf). */
static int bf_fader_raw_to_db2(u16 raw)
{
	int i;

	if (raw == 0 || raw < bf_fader_curve[0].raw)
		return BF_FADER_DB2_INF;
	for (i = 0; i < ARRAY_SIZE(bf_fader_curve) - 1; i++) {
		if (raw <= bf_fader_curve[i + 1].raw) {
			u32 num = (u32)(raw - bf_fader_curve[i].raw) *
				  (u32)(bf_fader_curve[i + 1].db2 - bf_fader_curve[i].db2);
			u32 den = bf_fader_curve[i + 1].raw - bf_fader_curve[i].raw;

			return bf_fader_curve[i].db2 + (int)((num + den / 2) / den);
		}
	}
	return bf_fader_curve[ARRAY_SIZE(bf_fader_curve) - 1].db2;
}

/* dBx2 -> fader raw (linear interpolation; below -62 dB = mute 0). */
static u16 bf_fader_db2_to_raw(int db2)
{
	int i;

	if (db2 <= bf_fader_curve[0].db2)
		return db2 < bf_fader_curve[0].db2 ? 0 : bf_fader_curve[0].raw;
	for (i = 0; i < ARRAY_SIZE(bf_fader_curve) - 1; i++) {
		if (db2 <= bf_fader_curve[i + 1].db2) {
			u32 num = (u32)(db2 - bf_fader_curve[i].db2) *
				  (u32)(bf_fader_curve[i + 1].raw - bf_fader_curve[i].raw);
			u32 den = bf_fader_curve[i + 1].db2 - bf_fader_curve[i].db2;

			return bf_fader_curve[i].raw + (u16)((num + den / 2) / den);
		}
	}
	return bf_fader_curve[ARRAY_SIZE(bf_fader_curve) - 1].raw;
}

/* -- controls -------------------------- */
