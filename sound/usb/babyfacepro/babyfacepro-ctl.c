// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS - proprietary-mode USB audio driver
 *
 * ALSA control surface: mixer (masters, preamp, crosspoints, flags,
 * gains), front-panel poll + controls, and the hardware DSP EQ
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
 * readback for faders), so it applies TotalMix's factory default:
 * every source routed to every output at unity, masters at 0 dB and
 * unmuted - the user/TuxMix can restore its own scene on top.
 */
int babyface_write_default_mixer(struct snd_usb_babyface *chip)
{
	int out, src, ret;
	u16 flag;

	/* Output masters: 0 dB (0x2000) + the unmute companion (0xf3). */
	for (out = 0; out < 6; out++) {
		ret = bf_vendor_write(chip, BF_REQ_GAIN, BF_MASTER_UNMUTE,
				      BF_REG_MASTER_8 + 2 * out);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_GAIN, BF_MASTER_UNMUTE,
				      BF_REG_MASTER_8 + 2 * out + 1);
		if (ret < 0)
			return ret;
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, BF_MASTER_0DB,
				      (BF_REG_MASTER_16 + 2 * out) | flag);
		if (ret < 0)
			return ret;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, BF_MASTER_0DB,
				      (BF_REG_MASTER_16 + 2 * out + 1) | flag);
		if (ret < 0)
			return ret;
		chip->master[out][0] = BF_MASTER_0DB;
		chip->master[out][1] = BF_MASTER_0DB;
		chip->muted[out] = false;
	}

	/* Every source into every output pair, L and R, at 0 dB (the
	 * standard map; the low map is only a shadow).  The addresses use
	 * the source's idx_l/idx_r on the canonical block - writing the raw
	 * index on both bases would put PB1 R on the L side and PB1 L on
	 * the R side (L+R on both = mono).  The "cross" registers
	 * (L-reg idx_r / R-reg idx_l) are left at 0; the restore at stream
	 * start re-writes the same addresses from the cache.
	 */
	for (out = 0; out < 6; out++) {
		unsigned int blk = bf_xpoint_block[out];

		for (src = 0; src < 14; src++) {
			flag = bf_flag_cycle[chip->flag_cnt];
			chip->flag_cnt = (chip->flag_cnt + 1) & 3;
			ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, BF_FADER_0DB,
					      (BF_REG_CROSS_BASE_L +
					       BF_REG_CROSS_STRIDE * blk +
					       bf_sources[src].idx_l) | flag);
			if (ret < 0)
				return ret;
			ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, BF_FADER_0DB,
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

	/* Mirror the defaults into the control cache (14 controls/output). */
	for (out = 0; out < 6; out++)
		for (src = 0; src < 14; src++) {
			chip->xpoint[out][src][0] = BF_FADER_0DB;
			chip->xpoint[out][src][1] = BF_FADER_0DB;
		}

	/* Host settings word: clock Internal (0x0001). */
	return bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0001,
			       BF_REG_KEEPALIVE_SETTINGS);
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
	return bf_vendor_write(chip, BF_REQ_PREAMP_COMMIT, 0x0000, 0x0000);
}

/* -- crosspoint matrix (6 outputs x 14 sources) -------------- */

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
	unsigned int blk = bf_xpoint_block[out];
	const struct bf_source *s = &bf_sources[src];
	u16 l = ucontrol->value.integer.value[0];
	u16 r = ucontrol->value.integer.value[1];
	u16 flag;
	int ret = 0;

	if (l > BF_FADER_TOP || r > BF_FADER_TOP)
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (l == chip->xpoint[out][src][0] && r == chip->xpoint[out][src][1])
		goto out;

	flag = bf_flag_cycle[chip->flag_cnt];
	chip->flag_cnt = (chip->flag_cnt + 1) & 3;

	/* L register = 0x0034 + 0x34*blk + idx, R = 0x004E + 0x34*blk + idx
	 * (mono sources use the same idx on both sides).
	 */
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
			      (BF_REG_CROSS_BASE_L + BF_REG_CROSS_STRIDE * blk +
			       s->idx_l) | flag);
	if (ret < 0)
		goto out;
	ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
			      (BF_REG_CROSS_BASE_R + BF_REG_CROSS_STRIDE * blk +
			       s->idx_r) | flag);
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
				.info = bf_xpoint_info,
				.get = bf_xpoint_get,
				.put = bf_xpoint_put,
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
	/* Every quad must be followed by the clock keepalive. */
	ret = bf_vendor_write(chip, BF_REQ_KEEPALIVE, 0x0001,
			      BF_REG_KEEPALIVE_SETTINGS);
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

static int bf_dim_put(struct snd_kcontrol *kctl,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	bool on = ucontrol->value.integer.value[0];
	u16 flag;
	int ret = 0;

	mutex_lock(&chip->mutex);
	if (on == chip->dim)
		goto out;
	if (on) {
		chip->dim_saved[0] = chip->master[1][0];
		chip->dim_saved[1] = chip->master[1][1];
		ret = bf_vendor_write(chip, BF_REQ_GAIN, 0xcb,
				      BF_REG_MASTER_8 + 2 * 1);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_GAIN, 0xcb,
				      BF_REG_MASTER_8 + 2 * 1 + 1);
		if (ret < 0)
			goto out;
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0333,
				      (BF_REG_MASTER_16 + 2 * 1) | flag);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT, 0x0333,
				      (BF_REG_MASTER_16 + 2 * 1 + 1) | flag);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_PREAMP, 0x2000, 0x2000);
		if (ret < 0)
			goto out;
	} else {
		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      bf_master_8bit(chip->dim_saved[0]),
				      BF_REG_MASTER_8 + 2 * 1);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_GAIN,
				      bf_master_8bit(chip->dim_saved[1]),
				      BF_REG_MASTER_8 + 2 * 1 + 1);
		if (ret < 0)
			goto out;
		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->dim_saved[0],
				      (BF_REG_MASTER_16 + 2 * 1) | flag);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_CROSSPOINT,
				      chip->dim_saved[1],
				      (BF_REG_MASTER_16 + 2 * 1 + 1) | flag);
		if (ret < 0)
			goto out;
		ret = bf_vendor_write(chip, BF_REQ_PREAMP, 0x0000, 0x2000);
		if (ret < 0)
			goto out;
	}
	chip->dim = on;
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

/* Gain scales: the mic preamps (AN1/2) span 0-65 dB over raw 0-20
 * (3.25 dB/step); the Hi-Z instrument inputs (AN3/4) are digitally
 * limited to 9 dB over raw 0-18 (0.5 dB/step) - manual sec. 10, raw
 * ranges verified from cap_gain12/cap_gain34.pcap.  Shared by the GUI
 * controls and the front-panel gain wheel.
 */
int bf_gain_max_db(int mic)
{
	return mic < 2 ? BF_GAIN_MAX_DB : 9;
}

int bf_gain_db(int mic, u8 raw)
{
	return mic < 2 ? (raw * 13) / 4 : raw / 2;
}

u8 bf_gain_raw(int mic, int db)
{
	return mic < 2 ? (db * 8 + 13) / 26 : db * 2;
}

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

	/* chip->gain[] tracks the dB (the raw is derived at write time -
	 * the 3.25 dB/step mic grid would otherwise make a +/-1 dB wheel
	 * stick on a raw boundary).
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
	u8 raw, counter;
	int ret = 0;

	if (db < 0 || db > bf_gain_max_db(mic))
		return -EINVAL;

	mutex_lock(&chip->mutex);
	if (db == chip->gain[mic])
		goto out;
	raw = bf_gain_raw(mic, db);
	counter = (chip->gain_cycle % 3 == 0) ? 0x20 :
		  (chip->gain_cycle % 3 == 1) ? 0x00 : 0x40;
	chip->gain_cycle = (chip->gain_cycle + 1) % 3;

	ret = bf_vendor_write(chip, BF_REQ_GAIN,
			      (u16)((raw & 0x1f) | counter),
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

	for (i = 0; i < 4; i++) {
		kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Mic 1 Capture Volume",
			.index = i,
			.info = bf_gain_info,
			.get = bf_gain_get,
			.put = bf_gain_put,
			.private_value = i,
		}, chip);
		err = snd_ctl_add(chip->card, kctl);
		if (err < 0)
			return err;
	}
	return 0;
}


/* ── front-panel stubs ────────────────────────────────────────
 * Replaced by the panel patch.  The core driver calls these from
 * probe()/disconnect()/suspend()/resume() and the delayed-work init.
 */
int babyface_create_panel(struct snd_usb_babyface *chip)
{
	return 0;
}

void babyface_panel_start(struct snd_usb_babyface *chip)
{
}

void babyface_panel_stop(struct snd_usb_babyface *chip)
{
}

void babyface_panel_work(struct work_struct *work)
{
}

/* ── DSP-EQ stubs ─────────────────────────────────────────────
 * Replaced by the eq patch.  The core driver calls these from
 * probe() and hw_params() (fs-dependent re-upload).
 */
int babyface_create_eq(struct snd_usb_babyface *chip)
{
	return 0;
}

void bf_eq_reupload(struct snd_usb_babyface *chip)
{
}
