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

/* Control indices in chip->panel_kctl[] (for snd_ctl_notify). */
enum {
	BF_PANEL_KCTL_BUTTON,
	BF_PANEL_KCTL_WHEEL,
	BF_PANEL_KCTL_IN,
	BF_PANEL_KCTL_OUT,
	BF_PANEL_KCTL_MIX,
	BF_PANEL_KCTL_DIM,
	BF_PANEL_KCTL_SELECT,
	BF_PANEL_KCTL_NUM,
};

static const char *const bf_panel_in_texts[] = {
	"Unknown", "Ch 1/2", "Ch 3/4", "Opt", NULL
};

static const char *const bf_panel_out_texts[] = {
	"Unknown", "Ch 1/2", "Phones", "Opt", NULL
};

static const char *const bf_panel_select_texts[] = {
	"Left", "Right", "Both", "None", NULL
};

/* byte3 button flash -> event code (0 = none).  The idle byte3 is 0x40;
 * a press flashes the value below the base for one or two poll frames.
 */
static int bf_panel_button_decode(u8 flash)
{
	switch (flash) {
	case BF_PANEL_FLASH_IN:		return BF_PANEL_BTN_IN;
	case BF_PANEL_FLASH_SET:	return BF_PANEL_BTN_SET;
	case BF_PANEL_FLASH_MIX:	return BF_PANEL_BTN_MIX;
	case BF_PANEL_FLASH_OUT:	return BF_PANEL_BTN_OUT;
	case BF_PANEL_FLASH_SELECT:	return BF_PANEL_BTN_SELECT;
	case BF_PANEL_FLASH_DIM:	return BF_PANEL_BTN_DIM;
	default:			return BF_PANEL_BTN_NONE;
	}
}

/* (byte2 >> 4) & 7 = IN position 4/5/6 -> enum index (0 = not in range). */
static int bf_panel_in_decode(u8 nib)
{
	switch (nib) {
	case BF_PANEL_IN_CH12:		return 1;
	case BF_PANEL_IN_CH34:		return 2;
	case BF_PANEL_IN_OPT:		return 3;
	default:			return 0;
	}
}

/* byte1 & 7 = OUT position.  Two encodings seen in captures: the
 * gain-display mode 0x04/0x05/0x06 (cap_dim.pcap, cap_buttons2.pcap)
 * and the base mode 0x01/0x02/0x00 (cap_buttons.pcap; 0x01 is also the
 * idle byte1 of cap_padpan.pcap and the live device).  Accept both;
 * 0x00 is ambiguous (could be Opt or no selection) so keep previous.
 */
static int bf_panel_out_decode(u8 v)
{
	switch (v) {
	case BF_PANEL_OUT_CH12:		return 1;
	case BF_PANEL_OUT_PHONES:	return 2;
	case BF_PANEL_OUT_OPT:		return 3;
	case 0x01:			return 1;	/* base-mode Ch 1/2 */
	case 0x02:			return 2;	/* base-mode Phones */
	default:			return 0;
	}
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

/* MIX-mode VU display law - monitoring dBx2 -> the 0x1A 0x000A display
 * value.  Piecewise-linear through the captured (dB, display) points
 * (cap_mix.pcap 2026-08-23: (-62,0) (-54,1) (-48,2) (-42.5,3)
 * (-35,4) (-28.4,5); cap_panel.pcap: (-7.4,10) (-6.7,11)
 * (-4.6,12)) - a log-ish VU scale (coarse at the bottom, ~1.4 dB/step
 * near 0).  The -28..-8 dB middle is interpolated; the exact law is
 * pending the cap_mixdisp.pcap full-range sweep (TODO 0g).
 */
static int bf_mix_display(int db2)
{
	static const struct {
		s16 db2;
		u8 disp;
	} pts[] = {
		{ -124, 0 }, { -108, 1 }, {  -96, 2 }, {  -85, 3 },
		{  -70, 4 }, {  -57, 5 }, {  -15, 10 }, {  -13, 11 },
		{   -9, 12 },
	};
	int i;

	if (db2 <= pts[0].db2)
		return 0;
	for (i = 0; i < ARRAY_SIZE(pts) - 1; i++) {
		if (db2 <= pts[i + 1].db2) {
			u32 num = (u32)(db2 - pts[i].db2) *
				  (u32)(pts[i + 1].disp - pts[i].disp);
			u32 den = pts[i + 1].db2 - pts[i].db2;

			return pts[i].disp + (int)((num + den / 2) / den);
		}
	}
	/* Above -4.6 dB: keep the last slope (2 dB/step) up to +6 dB. */
	return pts[ARRAY_SIZE(pts) - 1].disp +
	       clamp((db2 - pts[ARRAY_SIZE(pts) - 1].db2) / 4, 0, 12);
}

/* The kernel driver plays the TotalMix role for the MIX button (the
 * standalone emulator is hardware-validated in tuxmix-core/src/panel.rs
 * + usb.rs): one wheel click in fader mode = +/-0.5 dB on the SELECT-
 * chosen channel(s) of the IN-selected pair, into the OUT-selected
 * output's crosspoint block - the STANDARD map only (cap_mix.pcap /
 * cap_select2.pcap, no low-map mirror).  Mirrors the change into the
 * xpoint cache so the ALSA controls follow the wheel.  Takes the mutex
 * (the 0x12 writes cycle the transaction flag like the mixer puts).
 */
static void bf_panel_mix_wheel(struct snd_usb_babyface *chip, int delta)
{
	/* Canonical output of the OUT selection (enum 1 = Ch1/2,
	 * 2 = Phones, 3 = Opt): AN1/2, PH3/4, ADAT7/8 (the optical
	 * output) respectively.
	 */
	int out = chip->panel_out == 3 ? 5 :
		  chip->panel_out == 2 ? 1 : 0;
	unsigned int blk = bf_xpoint_block[out];
	u8 targets[2];
	int n = 0;
	int db2;
	u16 raw, flag;
	int i;

	/* SELECT-chosen channel(s) of the IN pair (manual sec. 5.1: SELECT
	 * steps left/right/both; none = nothing selected = no-op wheel).
	 * Source indices: AN1/AN2 = 0/1, AN3/AN4 = 2/3, AS1/2 = 4.
	 */
	if (chip->panel_in == 3) {
		targets[0] = 4;		/* Opt: the AS1/2 pair */
		n = 1;
	} else if (chip->panel_select != 3) {
		int base = chip->panel_in == 2 ? 2 : 0;

		targets[0] = base + (chip->panel_select == 1 ? 1 : 0);
		n = 1;
		if (chip->panel_select == 2)
			targets[n++] = base + 1;
	}

	mutex_lock(&chip->mutex);
	db2 = bf_fader_raw_to_db2(chip->panel_mix_raw);
	db2 = clamp(db2 + delta, BF_FADER_DB2_INF, 12);
	raw = bf_fader_db2_to_raw(db2);
	chip->panel_mix_raw = raw;
	for (i = 0; i < n; i++) {
		const struct bf_source *s = &bf_sources[targets[i]];

		flag = bf_flag_cycle[chip->flag_cnt];
		chip->flag_cnt = (chip->flag_cnt + 1) & 3;
		bf_vendor_write(chip, BF_REQ_CROSSPOINT, raw,
				(BF_REG_CROSS_BASE_L + BF_REG_CROSS_STRIDE * blk +
				 s->idx_l) | flag);
		bf_vendor_write(chip, BF_REQ_CROSSPOINT, raw,
				(BF_REG_CROSS_BASE_R + BF_REG_CROSS_STRIDE * blk +
				 s->idx_r) | flag);
		chip->xpoint[out][targets[i]][0] = raw;
		chip->xpoint[out][targets[i]][1] = raw;
		/* MIX-mode VU display shadow (0x1A 0x000A+mic): TotalMix
		 * mirrors the monitoring level into the panel display family
		 * (cap_mix/cap_panel.pcap) - the input VU segments follow it.
		 * Written only on change (the captures show TotalMix updating
		 * it on segment crossings).  Law = bf_mix_display (TODO 0g
		 * pending the exact full-range capture).
		 */
		if (targets[i] < 4) {
			int disp = bf_mix_display(db2);

			if (disp != chip->panel_mix_disp[targets[i]]) {
				bf_vendor_write(chip, BF_REQ_GAIN,
						(u16)disp,
						BF_REG_PANEL_GAIN + targets[i]);
				chip->panel_mix_disp[targets[i]] = disp;
			}
		}
	}
	mutex_unlock(&chip->mutex);
}

/* Write an output's L/R masters (8-bit companions + 16-bit with the
 * transaction flag) and mirror into the cache - shared by the OUT
 * volume wheel and the balance wheel.  Caller holds the mutex.
 */
static void bf_panel_write_master(struct snd_usb_babyface *chip, int out,
				  u16 l, u16 r)
{
	u16 flag;

	flag = bf_flag_cycle[chip->flag_cnt];
	chip->flag_cnt = (chip->flag_cnt + 1) & 3;
	bf_vendor_write(chip, BF_REQ_GAIN, bf_master_8bit(l),
			BF_REG_MASTER_8 + 2 * out);
	bf_vendor_write(chip, BF_REQ_GAIN, bf_master_8bit(r),
			BF_REG_MASTER_8 + 2 * out + 1);
	bf_vendor_write(chip, BF_REQ_CROSSPOINT, l,
			(BF_REG_MASTER_16 + 2 * out) | flag);
	bf_vendor_write(chip, BF_REQ_CROSSPOINT, r,
			(BF_REG_MASTER_16 + 2 * out + 1) | flag);
	chip->master[out][0] = l;
	chip->master[out][1] = r;
	chip->muted[out] = false;
	/* A Phones change while DIM is engaged re-bases the restore. */
	if (chip->dim && out == 1) {
		chip->dim_saved[0] = l;
		chip->dim_saved[1] = r;
	}
}

/* OUT-mode wheel: the master fader of the OUT-selected output, +/-0.5 dB
 * per click (cap_set2/cap_dim.pcap: the wheel writes the 16-bit master
 * 0x03E0+2*out on the master curve 0x2000*2^(dB/6); the driver keeps
 * the 8-bit companion in sync like bf_master_put - the 8-bit is the
 * real volume).  BOTH sides move by the same dB so an existing
 * balance (hold-SELECT) is preserved.  Same output mapping as the MIX
 * wheel (Phones = canon 1, Opt = ADAT7/8 = canon 5, else AN1/2).
 */
static void bf_panel_out_wheel(struct snd_usb_babyface *chip, int delta)
{
	int out = chip->panel_out == 3 ? 5 :
		  chip->panel_out == 2 ? 1 : 0;
	int hl, hr;
	u16 l, r;

	mutex_lock(&chip->mutex);
	hl = bf_master_half_db(chip->master[out][0]) + delta;
	hr = bf_master_half_db(chip->master[out][1]) + delta;
	l = bf_master_16bit(clamp(hl, -128, 12));
	r = bf_master_16bit(clamp(hr, -128, 12));
	bf_panel_write_master(chip, out, l, r);
	mutex_unlock(&chip->mutex);
}

/* IN-mode wheel: the gain of the SELECT-chosen channel(s) of the
 * IN-selected pair, +/-1 dB per click (manual sec. 5.1: SELECT steps
 * left/right/both, then the wheel changes the gain).  Writes the PANEL
 * gain registers 0x1A 0x000A+mic (cap_select.pcap 2026-08-24 - the
 * "ADC gain" family, which drives the same preamp as the GUI
 * 0x0000+mic; the cache tracks the raw either way).  Opt has no
 * preamp and SELECT None = no target.
 */
static void bf_panel_gain_wheel(struct snd_usb_babyface *chip, int delta)
{
	u8 mics[2];
	int n = 0;
	int i;

	if (chip->panel_in == 3 || chip->panel_select == 3)
		return;
	{
		int base = chip->panel_in == 2 ? 2 : 0;

		mics[0] = base + (chip->panel_select == 1 ? 1 : 0);
		n = 1;
		if (chip->panel_select == 2)
			mics[n++] = base + 1;
	}

	mutex_lock(&chip->mutex);
	for (i = 0; i < n; i++) {
		int mic = mics[i];
		int db = clamp((int)chip->gain[mic] + delta,
				0, bf_gain_max_db(mic));
		u8 raw = bf_gain_raw(mic, db);

		bf_vendor_write(chip, BF_REQ_GAIN, raw, BF_REG_PANEL_GAIN + mic);
		chip->gain[mic] = db;
	}
	mutex_unlock(&chip->mutex);
}

/* OUT-balance wheel (hold SELECT + wheel - manual sec. 5.1 "Output
 * Balance"): moves the stereo image of the OUT-selected output by
 * attenuating ONE side, linear in raw (cap_pan_stereo.pcap: the varied
 * side = fixed*(1-|pan|), ~0x9C raw step per click at 0 dB - the PAN
 * of the stereo hardware output in TotalMix).  The balance position is
 * derived from the L/R master ratio (the louder side is the fixed
 * one), so the gesture needs no extra state - and the OUT wheel below
 * moves both sides by the same dB to preserve an existing balance.
 */
static void bf_panel_balance_wheel(struct snd_usb_babyface *chip, int delta)
{
	int out = chip->panel_out == 3 ? 5 :
		  chip->panel_out == 2 ? 1 : 0;
	u16 l, r;
	int bal;		/* -100..+100; + = image right (left varies) */
	u16 fixed, varied;

	mutex_lock(&chip->mutex);
	/* Read under the lock so the L/R pair is consistent with the
	 * master/mute/dim writers (they update chip->master[] under the
	 * same mutex).
	 */
	l = chip->master[out][0];
	r = chip->master[out][1];
	/* Balance from the L/R ratio: the louder side is the fixed one. */
	if (l >= r) {
		bal = r ? -(100 - (100 * r) / l) : -100;
		fixed = l;
	} else {
		bal = l ? (100 - (100 * l) / r) : 100;
		fixed = r;
	}
	bal = clamp(bal + delta * 2, -100, 100);
	varied = (u16)((u32)fixed * (100 - abs(bal)) / 100);
	l = bal >= 0 ? varied : fixed;
	r = bal >= 0 ? fixed : varied;

	bf_panel_write_master(chip, out, l, r);
	mutex_unlock(&chip->mutex);
}

/* SET press (byte3 0x42 flash): toggle 48V phantom on the
 * SELECT-chosen mic(s) of the IN-selected pair.  The hardware only
 * does this in standalone mode (online, TotalMix ignores SET - no USB
 * write in the captures), but the driver IS the host: it writes the
 * preamp state itself and the P48 LEDs follow (the tuxmix-core
 * emulator, hardware-verified).  Restricted to IN mode + Ch1/2 (the
 * phantom-capable pair); Opt/Ch3/4 and SELECT None = no target.
 */
static void bf_panel_set_phantom(struct snd_usb_babyface *chip)
{
	u16 bits = 0;
	int m;

	if (chip->panel_mix || chip->panel_in != 1 ||
	    chip->panel_select == 3)
		return;
	if (chip->panel_select != 1)
		bits |= BF_PREAMP_48V_MIC1;
	if (chip->panel_select != 0)
		bits |= BF_PREAMP_48V_MIC2;

	mutex_lock(&chip->mutex);
	/* One channel selected: toggle it.  Both selected: ALIGN both to
	 * the same state, so repeated SET presses cycle all-on <-> all-off
	 * (a mixed phantom state cannot persist with both selected).
	 */
	if (chip->panel_select == 2) {
		if ((chip->preamp & bits) == bits)
			chip->preamp &= ~bits;
		else
			chip->preamp |= bits;
	} else {
		chip->preamp ^= bits;
	}
	bf_preamp_state_write(chip);
	for (m = 0; m < 4; m++)
		chip->panel_mix_disp[m] = 0;
	mutex_unlock(&chip->mutex);
}

static void bf_panel_notify(struct snd_usb_babyface *chip, int ctl)
{
	if (chip->panel_kctl[ctl])
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       &chip->panel_kctl[ctl]->id);
}

/* One 0x17 read + decode.  Called from the poll work; no locking needed -
 * the worker is the only writer and the control get callbacks run under
 * the ALSA controls lock (chip->panel_button/wheel are consumed there).
 */
static void bf_panel_tick(struct snd_usb_babyface *chip)
{
	u8 st[4];
	int delta, in, out;
	bool dim;
	u8 cls, pcls;
	int btn;
	bool mix_flash, fader_now;

	if (bf_vendor_read(chip, BF_REQ_PREAMP, BF_REG_PANEL_READ, st) < 0)
		return;	/* device gone / busy - retry next tick */

	if (!chip->panel_seen) {
		chip->panel_seen = true;
		memcpy(chip->panel_prev, st, sizeof(st));
		/* Seed the state controls from the first snapshot. */
		in = bf_panel_in_decode((st[2] >> BF_PANEL_IN_SHIFT) & 0x7);
		if (in)
			chip->panel_in = in;
		out = bf_panel_out_decode(st[1] & 0x07);
		if (out)
			chip->panel_out = out;
		chip->panel_mix = !!(st[0] & 0x80);
		chip->panel_saw_fader = (st[2] >> 4) == 0x0;
		chip->panel_dim = !!(st[1] & 0x20);
		return;
	}

	/* The udev alsactl restore (~100 ms after probe) clobbers the host
	 * SELECT with a stale stored value (the control is VOLATILE but
	 * this alsactl stores/restores it anyway) - re-assert the device's
	 * power-on state (nothing selected, cycle ARMED) for the first
	 * ~3 s so the boot always starts in sync.
	 */
	if (time_is_after_jiffies(chip->panel_start + 3 * HZ))
		chip->panel_select = 3;

	/* Button flash (byte3 over the 0x40 idle base). */
	btn = bf_panel_button_decode(st[3]);
	if (btn)
		chip->panel_button = btn;

	/* Wheel: signed 4-bit wrap delta of the byte2 low nibble - only
	 * while the mode class is unchanged.  A mode switch (IN 0x4x ->
	 * fader 0x0x on a MIX press, or the OUT counter carrying 0x8F ->
	 * 0x90 - the OUT counter is a full byte, cap_set2.pcap) must not
	 * be read as a wheel jump.  Class: 0 = fader (0x0x), 1 = OUT
	 * (0x8x/0x9x), 2 = IN (0x4x/0x5x/0x6x).
	 */
	cls = (st[2] >> 4) == 0x8 || (st[2] >> 4) == 0x9 ? 1 :
	      (st[2] >> 4) == 0x0 ? 0 : 2;
	pcls = (chip->panel_prev[2] >> 4) == 0x8 ||
	       (chip->panel_prev[2] >> 4) == 0x9 ? 1 :
	       (chip->panel_prev[2] >> 4) == 0x0 ? 0 : 2;
	delta = (int)(st[2] & 0x0f) - (int)(chip->panel_prev[2] & 0x0f);
	if (delta > 8)
		delta -= 16;
	else if (delta < -8)
		delta += 16;
	if (delta && cls == pcls) {
		chip->panel_wheel = clamp(chip->panel_wheel + delta,
					  SHRT_MIN, SHRT_MAX);
		bf_panel_notify(chip, BF_PANEL_KCTL_WHEEL);
		/* Wheel by mode (LINUX-VALIDATION sec. 12, the TotalMix
		 * emulator): MIX -> monitoring level, OUT (0x8x/0x9x) -> the
		 * selected output master (or its balance while SELECT is
		 * held), IN (0x4x/0x5x/0x6x) -> the SELECT-chosen preamp
		 * gain.
		 */
		if (chip->panel_mix)
			bf_panel_mix_wheel(chip, delta);
		else if (chip->panel_sel_hold >= 10 && cls == 1)
			bf_panel_balance_wheel(chip, delta);
		else if (cls == 1)
			bf_panel_out_wheel(chip, delta);
		else if (cls == 2)
			bf_panel_gain_wheel(chip, delta);
	}

	/* Selections - keep the previous when the field is not in range
	 * (the fader-mode readback drops the IN position bits).
	 */
	in = bf_panel_in_decode((st[2] >> BF_PANEL_IN_SHIFT) & 0x7);
	if (in && in != chip->panel_in) {
		chip->panel_in = in;
		/* The card CLEARS its L/R/both selection on an IN pair
		 * switch (user-verified 2026-08-27): re-sync the host-
		 * tracked SELECT so SET / the wheel / MIX target nothing
		 * until the user picks a channel again.  This is the main
		 * anti-desync hook (the physical state is not readable).
		 */
		if (chip->panel_select != 3) {
			chip->panel_select = 3;
			bf_panel_notify(chip, BF_PANEL_KCTL_SELECT);
		}
		/* An IN-pair switch disarms the device's SELECT cycle: the
		 * next press only re-arms it (no step), the one after that
		 * cycles (device behavior, user-verified 2026-08-28).
		 */
		chip->panel_select_armed = false;
		bf_panel_notify(chip, BF_PANEL_KCTL_IN);
	}
	out = bf_panel_out_decode(st[1] & 0x07);
	if (out && out != chip->panel_out) {
		chip->panel_out = out;
		bf_panel_notify(chip, BF_PANEL_KCTL_OUT);
	}

	/* SELECT press cycles the channel selection L -> R -> both -> none
	 * -> L (manual sec. 5.1).  The state is NOT in the readback
	 * (panelprobe 2026-08-24), so it is tracked host-side.
	 */
	if (st[3] == BF_PANEL_FLASH_SELECT &&
	    chip->panel_prev[3] != BF_PANEL_FLASH_SELECT) {
		if (!chip->panel_select_armed) {
			/* Disarmed (IN switch since the last step): the press
			 * only re-arms the cycle - the device steps on the
			 * NEXT press (user-verified 2026-08-28).
			 */
			chip->panel_select_armed = true;
		} else {
			chip->panel_select = (chip->panel_select + 1) & 3;
		}
		bf_panel_notify(chip, BF_PANEL_KCTL_SELECT);
	}
	/* SELECT hold (the OUT-balance gesture, manual sec. 5.1 "Output
	 * Balance"): a tap flashes byte3 0x50 for ~2-3 frames at 20 Hz
	 * (~100-150 ms - selhold_probe2), a hold keeps it sustained, and
	 * byte0 does NOT gain the 0x80 engaged bit - so the duration is
	 * the only discriminator: >= 10 ticks (200 ms at 50 Hz) = held.
	 */
	if (st[3] == BF_PANEL_FLASH_SELECT)
		chip->panel_sel_hold++;
	else
		chip->panel_sel_hold = 0;

	/* SET (A) press: host-side 48V phantom toggle on the
	 * SELECT-chosen mic(s) (see bf_panel_set_phantom).
	 */
	if (st[3] == BF_PANEL_FLASH_SET &&
	    chip->panel_prev[3] != BF_PANEL_FLASH_SET)
		bf_panel_set_phantom(chip);

	/* MIX (fader mode) - HOST-latched, like TotalMix (cap_mix.pcap,
	 * cap_select2.pcap): the raw press readback is `0D 0D 41 44` -
	 * byte3 flash 0x44, NO engaged bit, byte2 still in the current
	 * mode.  The host acks the flash with `0x17 0x8480 0x8C80` -> the
	 * device latches fader mode (byte0/1 gain the 0x80 bit, byte2 =
	 * 0x00+n counter) and STAYS there after the physical release; the
	 * SECOND 0x44 flash exits it (`0x17 0x0400 0x8000` + `0x8080`).
	 * A mode button (IN/OUT/SET) pressed during MIX makes the device
	 * leave fader mode by itself -> same exit writes (the user: IN
	 * must return to gain control).  `panel_saw_fader` gates the
	 * device-driven exit so a pre-ack readback (byte2 still 0x4x
	 * while the 0x44 flash shows) never ends MIX before it started.
	 */
	mix_flash = st[3] == BF_PANEL_FLASH_MIX &&
		    chip->panel_prev[3] != BF_PANEL_FLASH_MIX;
	fader_now = (st[2] >> 4) == 0x0;

	if (mix_flash) {
		if (chip->panel_mix) {
			bf_vendor_write(chip, BF_REQ_PREAMP, 0x0400, 0x8000);
			bf_vendor_write(chip, BF_REQ_PREAMP, 0x0400, 0x8080);
			chip->panel_mix = false;
			chip->panel_saw_fader = false;
		} else {
			int ref, out;
			int m;

			bf_vendor_write(chip, BF_REQ_PREAMP, 0x8480, 0x8c80);
			chip->panel_mix = true;
			/* Seed the monitoring level at the reference
			 * crosspoint's current value so the first wheel
			 * click doesn't jump from -inf (the reference =
			 * the first SELECT-chosen channel of the IN pair;
			 * Opt = the AS1/2 pair).
			 */
			out = chip->panel_out == 3 ? 5 :
			      chip->panel_out == 2 ? 1 : 0;
			ref = chip->panel_in == 3 ? 4 :
			      (chip->panel_in == 2 ? 2 : 0) +
			      (chip->panel_select == 1 ? 1 : 0);
			chip->panel_mix_raw = chip->xpoint[out][ref][0];
			/* Seed the VU display shadow at the CURRENT level
			 * (cap_panel.pcap: TotalMix writes the display value of
			 * the current fader on engage - 10 in that session -
			 * not a hard 0; cap_mix's 0 was because the fader sat
			 * at the bottom).  Only the channels the wheel can move.
			 */
			for (m = 0; m < 4; m++)
				chip->panel_mix_disp[m] = 0;
			if (ref < 4) {
				int db2 = bf_fader_raw_to_db2(chip->panel_mix_raw);
				int disp = bf_mix_display(db2);

				bf_vendor_write(chip, BF_REQ_GAIN, (u16)disp,
						BF_REG_PANEL_GAIN + ref);
				chip->panel_mix_disp[ref] = disp;
			}
		}
		bf_panel_notify(chip, BF_PANEL_KCTL_MIX);
	}
	if (fader_now) {
		chip->panel_saw_fader = true;
	} else if (chip->panel_mix && chip->panel_saw_fader &&
		   st[3] != BF_PANEL_FLASH_MIX) {
		/* device left fader mode by itself (IN/OUT/SET press) */
		bf_vendor_write(chip, BF_REQ_PREAMP, 0x0400, 0x8000);
		bf_vendor_write(chip, BF_REQ_PREAMP, 0x0400, 0x8080);
		chip->panel_mix = false;
		chip->panel_saw_fader = false;
		bf_panel_notify(chip, BF_PANEL_KCTL_MIX);
	}

	dim = !!(st[1] & 0x20);
	if (dim != chip->panel_dim) {
		chip->panel_dim = dim;
		bf_panel_notify(chip, BF_PANEL_KCTL_DIM);
	}

	memcpy(chip->panel_prev, st, sizeof(st));
}

void babyface_panel_work(struct work_struct *work)
{
	struct snd_usb_babyface *chip = container_of(work,
			struct snd_usb_babyface, panel_work.work);

	if (chip->shutdown)
		return;
	bf_panel_tick(chip);
	schedule_delayed_work(&chip->panel_work,
			      msecs_to_jiffies(chip->panel_poll_ms));
}

void babyface_panel_start(struct snd_usb_babyface *chip)
{
	chip->panel_seen = false;
	/* The device boots with NOTHING selected (the SELECT cycle starts
	 * at none -> AN1 -> AN2 -> both -> none) - the unreadable selection
	 * must start there too, or every later SET is off by one channel
	 * (host at AN1 while the LEDs show nothing -> first SELECT makes
	 * the device blink AN1 but the host believes AN2).
	 */
	chip->panel_select = 3;	/* none */
	chip->panel_select_armed = true;
	chip->panel_start = jiffies;
	schedule_delayed_work(&chip->panel_work, 0);
}

void babyface_panel_stop(struct snd_usb_babyface *chip)
{
	cancel_delayed_work_sync(&chip->panel_work);
}

/* -- controls -------------------------- */

/* The button/wheel controls hold the LATEST state and are NOT consumed
 * on read: wireplumber subscribes to every notifying control and reads
 * it, so a clear-on-get would let another reader eat the event.  Each
 * consumer tracks its own baseline and acts on changes (the button is a
 * last-press code, the wheel an accumulated signed delta).  VOLATILE
 * keeps alsactl from caching them.
 */
static int bf_panel_button_info(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = BF_PANEL_BTN_DIM;
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_panel_button_get(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->panel_button;
	return 0;
}

static int bf_panel_wheel_info(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = SHRT_MIN;
	uinfo->value.integer.max = SHRT_MAX;
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_panel_wheel_get(struct snd_kcontrol *kctl,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] = chip->panel_wheel;
	return 0;
}

static int bf_panel_in_info(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 4, bf_panel_in_texts);
}

static int bf_panel_in_get(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->panel_in;
	return 0;
}

static int bf_panel_out_info(struct snd_kcontrol *kctl,
			     struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 4, bf_panel_out_texts);
}

static int bf_panel_out_get(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->panel_out;
	return 0;
}

static int bf_panel_select_info(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, 4, bf_panel_select_texts);
}

static int bf_panel_select_get(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.enumerated.item[0] = chip->panel_select;
	return 0;
}

/* Writable so software (or the user, after a driver reload) can
 * re-sync the host-tracked SELECT state to the physical card - the
 * L/R/both/none state is NOT in the 0x17 readback, so a reload starts
 * at "Left" while the card may sit at any position; a desync makes
 * SET / the wheel / MIX target the wrong channel.  Writing the
 * physical state re-aligns the emulation (TotalMix parity: it also
 * lets software select channels directly).
 */
static int bf_panel_select_put(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	unsigned int v = ucontrol->value.enumerated.item[0];
	int ret = 0;

	if (v > 3)
		return -EINVAL;
	if (v != chip->panel_select) {
		chip->panel_select = v;
		bf_panel_notify(chip, BF_PANEL_KCTL_SELECT);
		ret = 1;
	}
	return ret;
}

/* Shared boolean get - private_value selects mix (0) / dim (1). */
static int bf_panel_bool_get(struct snd_kcontrol *kctl,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);

	ucontrol->value.integer.value[0] =
		kctl->private_value ? chip->panel_dim : chip->panel_mix;
	return 0;
}

int babyface_create_panel(struct snd_usb_babyface *chip)
{
	struct snd_kcontrol *kctl;
	int err;

	memset(chip->panel_kctl, 0, sizeof(chip->panel_kctl));

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Button",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_button_info,
		.get = bf_panel_button_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_BUTTON] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Wheel",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_wheel_info,
		.get = bf_panel_wheel_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_WHEEL] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel In",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_in_info,
		.get = bf_panel_in_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_IN] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Out",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_out_info,
		.get = bf_panel_out_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_OUT] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Mix",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = snd_ctl_boolean_mono_info,
		.get = bf_panel_bool_get,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_MIX] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Dim",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = snd_ctl_boolean_mono_info,
		.get = bf_panel_bool_get,
		.private_value = 1,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_DIM] = kctl;

	kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Front Panel Select",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_WRITE |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = bf_panel_select_info,
		.get = bf_panel_select_get,
		.put = bf_panel_select_put,
	}, chip);
	err = snd_ctl_add(chip->card, kctl);
	if (err < 0)
		return err;
	chip->panel_kctl[BF_PANEL_KCTL_SELECT] = kctl;

	return 0;
}

#define BF_EQ_Q27		(1 << 27)
#define BF_EQ_LC_OFF		0x04000000
#define BF_EQ_BLOCK_LEN		64

/* atan(2^-i) x 2^27 (CORDIC). */
static const s64 bf_atan_tab[28] = {
	0x6487ED5, 0x3B58CE1, 0x1F5B760, 0xFEADD5,
	0x7FD56F, 0x3FFAAB, 0x1FFF55, 0xFFFEB,
	0x7FFFD, 0x40000, 0x20000, 0x10000,
	0x8000, 0x4000, 0x2000, 0x1000,
	0x800, 0x400, 0x200, 0x100,
	0x80, 0x40, 0x20, 0x10,
	0x8, 0x4, 0x2, 0x1,
};

/* ---- fixed-point helpers (Q27 in/out, s64 intermediates) ---- */

/* sin/cos of an angle in [0, pi/2] (Q27).  Simultaneous CORDIC, 28
 * iterations (~1e-8 residual).  eq_selftest.c verifies the whole
 * pipeline against the double-precision reference.
 */
static void bf_sincos(s64 ang, s64 *sn, s64 *cs)
{
	s64 x = 0x4DBA76D;	/* 1/1.64676 x 2^27 (CORDIC gain) */
	s64 y = 0;
	s64 z = ang;
	int i;

	for (i = 0; i < 28; i++) {
		s64 d = z >= 0 ? 1 : -1;
		s64 nx = x - d * (y >> i);
		s64 ny = y + d * (x >> i);

		x = nx;
		y = ny;
		z -= d * bf_atan_tab[i];
	}
	*cs = x;
	*sn = y;
}

/* 2^u for u in Q27, u in [-2, 2] (gain-amplitude range). */
static s64 bf_exp2(s64 u)
{
	s64 n = u >> 27;
	s64 r = u - (n << 27);
	s64 rl = (r * 0x58B90C0 + (1 << 26)) >> 27;	/* r.ln2 */
	s64 e = BF_EQ_Q27;
	s64 term = BF_EQ_Q27;
	int k;

	for (k = 1; k <= 10; k++) {
		term = div_s64((term * rl + (1 << 26)) >> 27, k);
		e += term;
	}
	return n >= 0 ? e << n : e >> -n;
}

/* The 5 stored words (c0..c3 + shared c4) for one band.
 * type: 1 bell, 2 low shelf, 3 high shelf.  freq_hz, fs in Hz;
 * q100 = Q x 100; gain_x10 = dB x 10.  fs is the stream rate.
 */
void bf_eq_band_words(s32 *w, int type, s32 freq_hz, s32 q100,
		      s32 gain_x10, s32 fs)
{
	s64 f = freq_hz;
	s64 w0, c, s, alpha, A, sq;
	s64 b0, b1, b2, a0, a1, a2;
	s64 pi = 0x1921FB54;	/* pi, Q27 */
	s64 hpi = 0xC90FDAA;	/* pi/2, Q27 */
	s64 t;
	int both = 0, cflip = 0;

	if (gain_x10 == 0 || q100 <= 0) {
		/* Inactive band: identity words (also guards the alpha
		 * division below against the default Q=0 the controls start
		 * with - a user setting gain before Q used to hit a kernel
		 * divide-by-zero oops).
		 */
		w[0] = 0;
		w[1] = 0;
		w[2] = 0;
		w[3] = 0;
		return;
	}

	/* w0 = 2.pi.f/fs (Q27), reduced to [0, pi/2]. */
	w0 = div_s64(f * BF_EQ_Q27, fs);
	w0 = (w0 * 0x3243F6A9) >> 27;	/* x 2.pi */
	t = w0;
	if (t > pi) {
		t -= pi;
		both = 1;
	}
	if (t > hpi) {
		t = pi - t;
		cflip = 1;
	}
	bf_sincos(t, &s, &c);
	if (both) {
		s = -s;
		c = -c;
	}
	if (cflip)
		c = -c;

	alpha = div64_s64(s * 100 + q100, 2 * (s64)q100);	/* sin(w0)/(2Q) */
	/* A = 10^(g/40), sqrt(A): g = gain_x10/10 dB */
	A = bf_exp2((s64)gain_x10 * 0x11021E);
	sq = bf_exp2((s64)gain_x10 * 0x8810F);

	if (type == 1) {
		s64 ta = (alpha * A + (1 << 26)) >> 27;

		b0 = BF_EQ_Q27 + ta;
		b1 = -2 * c;
		b2 = BF_EQ_Q27 - ta;
		a0 = BF_EQ_Q27 + div64_s64(alpha * BF_EQ_Q27 + A / 2, A);
		a1 = -2 * c;
		a2 = BF_EQ_Q27 - div64_s64(alpha * BF_EQ_Q27 + A / 2, A);
	} else {
		s64 ap1 = A + BF_EQ_Q27;
		s64 am1 = A - BF_EQ_Q27;
		s64 cp0 = (am1 * c + (1 << 26)) >> 27;	/* (A-1).c */
		s64 cp1 = (ap1 * c + (1 << 26)) >> 27;	/* (A+1).c */
		s64 ab = (2 * sq * alpha + (1 << 26)) >> 27;

		if (type == 2) {	/* low shelf */
			b0 = (A * (ap1 - cp0 + ab) + (1 << 26)) >> 27;
			b1 = (2 * A * (am1 - cp1) + (1 << 26)) >> 27;
			b2 = (A * (ap1 - cp0 - ab) + (1 << 26)) >> 27;
			a0 = ap1 + cp0 + ab;
			a1 = -2 * (am1 + cp1);
			a2 = ap1 + cp0 - ab;
		} else {		/* high shelf */
			b0 = (A * (ap1 + cp0 + ab) + (1 << 26)) >> 27;
			b1 = (-2 * A * (am1 + cp1) + (1 << 26)) >> 27;
			b2 = (A * (ap1 + cp0 - ab) + (1 << 26)) >> 27;
			a0 = ap1 - cp0 + ab;
			a1 = -2 * (am1 - cp1);
			a2 = ap1 - cp0 - ab;
		}
	}

	w[0] = (s32)div64_s64(a1 * BF_EQ_Q27 + a0 / 2, a0);
	w[1] = (s32)div64_s64(a2 * BF_EQ_Q27 + a0 / 2, a0);
	w[2] = (s32)div64_s64(b1 * BF_EQ_Q27 + b0 / 2, b0);
	w[3] = (s32)div64_s64(b2 * BF_EQ_Q27 + b0 / 2, b0);
	w[4] = (s32)div64_s64(b0 * BF_EQ_Q27 + a0 / 2, a0);
}

/* ---- low cut ---- */

/* Slope byte: 2^n-1 (n poles) -> 6/12/18/24 dB per oct; 0 = off. */
static u8 bf_eq_lc_slope_byte(s32 slope_db)
{
	switch (slope_db) {
	case 6:  return 0x01;
	case 12: return 0x03;
	case 18: return 0x07;
	case 24: return 0x0F;
	}
	return 0;
}

/* The 0x38 low-cut frequency word: round(K.f'.(11656)/(11656+f')) with
 * K = 11508, f' = f x slope-compensation factor (cap_eq9 fit, 0.003%;
 * the slope factor keeps the composite -3 dB point constant).
 */
static u32 bf_eq_lc_freq_raw(s32 freq_hz, s32 slope_db)
{
	s64 f, word;

	if (freq_hz <= 0)
		return BF_EQ_LC_OFF;
	f = freq_hz;
	switch (slope_db) {
	case 6:
		f = f * 15267 / 10000;
		break;

	case 18:
		f = f * 8061 / 10000;
		break;

	case 24:
		f = f * 6977 / 10000;
		break;
	}
	word = (11508 * f * 11656 + (11656 + f) / 2) / (11656 + f);
	return (u32)word;
}

/* ---- block build + bulk write ---- */

static void bf_eq_build_block(u8 *b, int ch, u8 slope,
			      const s32 bands[3][4], s32 shared, u32 lc)
{
	int slot, k;

	memset(b, 0, BF_EQ_BLOCK_LEN);
	b[0] = ch;
	b[1] = slope;
	b[2] = ch;
	b[3] = 0x80;	/* EQ engine active */
	for (slot = 0; slot < 3; slot++) {
		for (k = 0; k < 4; k++) {
			put_unaligned_le32((u32)bands[slot][k],
					   b + 0x04 + slot * 0x10 + 4 * k);
		}
	}
	put_unaligned_le32((u32)shared, b + 0x34);
	put_unaligned_le32(lc, b + 0x38);
}

/* Upload one 64-byte block on bulk OUT ep 0x0A (interface 1). */
static int bf_eq_upload(struct snd_usb_babyface *chip, const u8 *block)
{
	u8 *buf;
	int ret, len;

	/* usb_bulk_msg DMA-maps the buffer: it must not be on the stack
	 * (usb_hcd_map_urb_for_dma returns -EAGAIN for stack buffers).
	 */
	buf = kmemdup(block, BF_EQ_BLOCK_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = usb_bulk_msg(chip->dev, usb_sndbulkpipe(chip->dev, 0x0a),
			   buf, BF_EQ_BLOCK_LEN, &len, 1000);
	kfree(buf);
	if (ret < 0)
		dev_err(&chip->dev->dev, "EQ bulk upload failed: %d\n", ret);
	return ret;
}

/* Write the L+R block pair for one strip (channel base = strip x 2). */
static int bf_eq_write_strip(struct snd_usb_babyface *chip, int strip)
{
	struct bf_eq_channel *e = &chip->eq[strip];
	u8 b[BF_EQ_BLOCK_LEN];
	s32 identity[3][4] = { { 0 }, { 0 }, { 0 } };
	s32 shared = e->on ? e->shared : BF_EQ_Q27;
	u32 lc = e->on ? e->lc_raw : BF_EQ_LC_OFF;
	/* The header slope byte (b[1]) is only valid while the low cut is
	 * engaged: a stale slope with 0x38 = off made the device apply a
	 * garbage-frequency cut (ear-verified: "low cut off" left only
	 * highs).  cap_eq7: byte1 = 0x00 + 0x38 = 0x04000000 when off.
	 */
	u8 slope = (e->on && e->lc_hz > 0) ? e->slope : 0;
	int ch, ret;

	for (ch = 0; ch < 2; ch++) {
		bf_eq_build_block(b, strip * 2 + ch, slope,
				  e->on ? e->words : identity, shared, lc);
		ret = bf_eq_upload(chip, b);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* Recompute one strip's words + low cut from its params, re-upload.
 * Lock-free by convention: every caller must already hold chip->mutex
 * (bf_eq_put() and bf_eq_reupload() do) - asserting it here catches a
 * future caller that forgets, instead of a silent self-deadlock.
 */
static void bf_eq_update_strip(struct snd_usb_babyface *chip, int strip)
{
	struct bf_eq_channel *e = &chip->eq[strip];
	s32 fs = chip->rate ? chip->rate : 48000;
	s32 last_c4 = BF_EQ_Q27;
	int band, i;

	lockdep_assert_held(&chip->mutex);

	for (band = 0; band < 3; band++) {
		s32 w[5];

		bf_eq_band_words(w, e->band_type[band], e->band_freq[band],
				 e->band_q[band], e->band_gain[band], fs);
		for (i = 0; i < 4; i++)
			e->words[band][i] = w[i];
		if (e->band_type[band] && e->band_gain[band])
			last_c4 = w[4];	/* shared scale: the last band */
	}
	e->shared = last_c4;
	e->lc_raw = bf_eq_lc_freq_raw(e->lc_hz, e->slope_db);
	e->slope = bf_eq_lc_slope_byte(e->slope_db);
	bf_eq_write_strip(chip, strip);
}

/* Recompute + re-upload all four strips (rate change). Caller must
 * hold chip->mutex - bf_eq_update_strip()/bf_eq_write_strip() are
 * lock-free by convention (see bf_eq_put()) and the only caller,
 * babyface_pcm_hw_params(), already holds the lock across the rate
 * change; locking here too self-deadlocked it (hung-task: "blocked
 * on a mutex likely owned by" itself, hit via regress.sh's rate
 * sweep).
 */
void bf_eq_reupload(struct snd_usb_babyface *chip)
{
	int strip;

	for (strip = 0; strip < 4; strip++)
		bf_eq_update_strip(chip, strip);
}

/* ---- ALSA controls (4 strips x 19 controls) ---- */

#define EQ_STRIP(pv)	((pv) >> 8)
#define EQ_PARAM(pv)	((pv) & 0xff)
/* params: 0 enable, 1-3 type, 4-6 freq, 7-9 q, 10-12 gain, 13 lc freq, 14 lc slope */

static const char *const bf_eq_type_texts[] = {
	"Off", "Bell", "Low Shelf", "High Shelf", NULL
};

static const char *const bf_eq_slope_texts[] = {
	"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", NULL
};

static int bf_eq_info(struct snd_kcontrol *kctl,
		      struct snd_ctl_elem_info *uinfo)
{
	int param = EQ_PARAM(kctl->private_value);

	if (param == 0) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
		uinfo->count = 1;
		return 0;
	}
	if (param == 1 || param == 2 || param == 3)
		return snd_ctl_enum_info(uinfo, 1, 4, bf_eq_type_texts);
	if (param == 14)
		return snd_ctl_enum_info(uinfo, 1, 4, bf_eq_slope_texts);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = (param == 10 || param == 11 ||
				    param == 12) ? -240 :
				   (param == 7 || param == 8 ||
				    param == 9) ? 5 : 0;
	uinfo->value.integer.max = (param == 7 || param == 8 ||
				    param == 9) ? 1000 :
				   (param == 10 || param == 11 ||
				    param == 12) ? 240 : 20000;
	uinfo->value.integer.step = 1;
	return 0;
}

static int bf_eq_get(struct snd_kcontrol *kctl,
		     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	struct bf_eq_channel *e = &chip->eq[EQ_STRIP(kctl->private_value)];
	int param = EQ_PARAM(kctl->private_value);
	int band = (param - 1) % 3;
	s32 *v = NULL;

	switch (param) {
	case 0:
		break;

	case 1:
	case 2:
	case 3:
		v = &e->band_type[band];
		break;

	case 4:
	case 5:
	case 6:
		v = &e->band_freq[band];
		break;

	case 7:
	case 8:
	case 9:
		v = &e->band_q[band];
		break;

	case 10:
	case 11:
	case 12:
		v = &e->band_gain[band];
		break;

	case 13:
		v = &e->lc_hz;
		break;

	case 14:
		v = &e->slope_db;
		break;
	}
	if (param == 0) {
		ucontrol->value.integer.value[0] = e->on;
	} else if (param == 14) {
		/* Inverse of put's index->dB map: slope_db stores the raw
		 * 6/12/18/24 dB/oct value, but an ENUMERATED control's .get
		 * must return the enum item index (0-3), same as .put
		 * receives - returning the raw dB value here (the bug this
		 * replaces) fed back an out-of-range index to every ALSA
		 * consumer (confirmed via amixer: writing index 1 read back
		 * as value 12, not 1).
		 */
		s32 slope = v ? *v : 6;

		ucontrol->value.enumerated.item[0] =
			slope >= 24 ? 3 : slope >= 18 ? 2 : slope >= 12 ? 1 : 0;
	} else if (param == 1 || param == 2 || param == 3) {
		/* ENUMERATED band type: use the enumerated union member. */
		ucontrol->value.enumerated.item[0] = v ? *v : 0;
	} else {
		ucontrol->value.integer.value[0] = v ? *v : 0;
	}
	return 0;
}

static int bf_eq_put(struct snd_kcontrol *kctl,
		     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_usb_babyface *chip = snd_kcontrol_chip(kctl);
	int strip = EQ_STRIP(kctl->private_value);
	int param = EQ_PARAM(kctl->private_value);
	struct bf_eq_channel *e = &chip->eq[strip];
	int band = (param - 1) % 3;
	s32 nv;
	s32 *v = NULL;
	int ret = 0;

	/* Read from the union member matching the control type: ENUMERATED
	 * params (band type 1-3, slope 14) use .enumerated.item, everything
	 * else (BOOL 0, INTEGER) uses .integer.value.
	 */
	if (param == 1 || param == 2 || param == 3 || param == 14)
		nv = (s32)ucontrol->value.enumerated.item[0];
	else
		nv = (s32)ucontrol->value.integer.value[0];

	/* Validate against the bounds bf_eq_info() declares.  The ALSA core
	 * only checks these when CONFIG_SND_CTL_INPUT_VALIDATION is set, so
	 * an out-of-range value here could otherwise reach the Q27
	 * coefficient math (bf_eq_band_words/bf_exp2) and shift by >= width
	 * (undefined behaviour).
	 */
	switch (param) {
	case 0:
		if (nv < 0 || nv > 1)
			return -EINVAL;
		break;
	case 1:
	case 2:
	case 3:
	case 14:
		if (nv < 0 || nv > 3)
			return -EINVAL;
		break;
	case 4:
	case 5:
	case 6:
	case 13:
		if (nv < 0 || nv > 20000)
			return -EINVAL;
		break;
	case 7:
	case 8:
	case 9:
		if (nv < 5 || nv > 1000)
			return -EINVAL;
		break;
	case 10:
	case 11:
	case 12:
		if (nv < -240 || nv > 240)
			return -EINVAL;
		break;
	}

	switch (param) {
	case 0:
		v = NULL;
		break;

	case 1:
	case 2:
	case 3:
		v = &e->band_type[band];
		break;

	case 4:
	case 5:
	case 6:
		v = &e->band_freq[band];
		break;

	case 7:
	case 8:
	case 9:
		v = &e->band_q[band];
		break;

	case 10:
	case 11:
	case 12:
		v = &e->band_gain[band];
		break;

	case 13:
		v = &e->lc_hz;
		break;

	case 14:
		v = &e->slope_db;
		break;
	}
	if (param == 14)	/* slope enum items are 6/12/18/24 */
		nv = nv == 0 ? 6 : nv == 1 ? 12 : nv == 2 ? 18 : 24;

	mutex_lock(&chip->mutex);
	if (param == 0) {
		if (e->on != !!nv) {
			e->on = !!nv;
			bf_eq_update_strip(chip, strip);
			ret = 1;
		}
	} else if (v && *v != nv) {
		*v = nv;
		bf_eq_update_strip(chip, strip);
		ret = 1;
	}
	mutex_unlock(&chip->mutex);
	return ret;
}

int babyface_create_eq(struct snd_usb_babyface *chip)
{
	static const char *const names[4] = { "AN1", "AN2", "AN3", "AN4" };
	static const char *const params[] = {
		"EQ Enable",
		"EQ Band 1 Type", "EQ Band 2 Type", "EQ Band 3 Type",
		"EQ Band 1 Freq", "EQ Band 2 Freq", "EQ Band 3 Freq",
		"EQ Band 1 Q", "EQ Band 2 Q", "EQ Band 3 Q",
		"EQ Band 1 Gain", "EQ Band 2 Gain", "EQ Band 3 Gain",
		"EQ Low Cut Freq", "EQ Low Cut Slope",
	};
	int strip, i, err;

	for (strip = 0; strip < 4; strip++) {
		for (i = 0; i < 15; i++) {
			struct snd_kcontrol *kctl;
			char name[64];

			snprintf(name, sizeof(name), "%s %s", names[strip],
				 params[i]);
			kctl = snd_ctl_new1(&(struct snd_kcontrol_new){
				.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
				.name = "EQ",
				.index = 0,
				.info = bf_eq_info,
				.get = bf_eq_get,
				.put = bf_eq_put,
				.private_value = (strip << 8) | i,
			}, chip);
			if (!kctl)
				return -ENOMEM;
			strscpy(kctl->id.name, name, sizeof(kctl->id.name));
			err = snd_ctl_add(chip->card, kctl);
			if (err < 0)
				return err;
		}
	}
	return 0;
}
