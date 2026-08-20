// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESS Technology ES9039Q2M 32-bit 2-channel audio DAC
 *
 * Copyright (C) 2026 Karl Asseily <karl@asseily.com>
 *
 * Every register number, bit field and default in this file was taken from
 * ES9039Q2M datasheet v0.2.3 and then verified by reading the defaults back off
 * a live part over I2C.
 *
 * The part has two control personalities selected by the MODE pin: hardware
 * mode (strapped by HW0/HW1/HW2, no bus at all) and software mode (I2C or SPI).
 * This driver implements software mode over I2C, which MODE = GND selects.
 *
 * Three properties shape the driver:
 *
 *  - There is an ASRC in front of the DAC, so MCLK need not be synchronous with
 *    BCLK or LRCK. A board can feed it a fixed oscillator and never touch the
 *    clock again, which is why the part will pass audio with no register writes
 *    at all.
 *
 *  - INPUT_SEL chooses PCM / DSD / DoP / S/PDIF. It does NOT choose I2S vs
 *    left-justified - that is TDM_LJ_MODE in register 60, and there is no
 *    right-justified mode to map onto at all.
 *
 *  - Several registers have non-zero reserved defaults (register 88 reads
 *    0xb8 at reset), so every write here is read-modify-write.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* ------------------------------------------------- read/write registers ---- */

#define ES9039_SYSTEM_CONFIG		0x00	/* reg 0 */
#define   ES9039_64FS_MODE		BIT(6)

/*
 * Register 1 selects which DECODERS are running, and it is separate from
 * INPUT_SEL in register 57, which only says which port to listen to. Both are
 * needed: on reset only ENABLE_TDM_DECODE is set, so selecting DoP as the input
 * while leaving bit 2 clear leaves the part hunting for a marker with the
 * marker decoder switched off. It then finds no valid DoP and mutes - silence,
 * DOP_VALID reading 0, and nothing anywhere saying why. Measured on B1,
 * 2026-08-19.
 */
#define ES9039_SYS_MODE			0x01	/* reg 1, reset 0xb1 */
#define   ES9039_ENABLE_TDM_DECODE	BIT(0)	/* set at reset */
#define   ES9039_ENABLE_DSD_DECODE	BIT(1)
#define   ES9039_ENABLE_DOP_DECODE	BIT(2)
#define   ES9039_ENABLE_SPDIF_DECODE	BIT(3)
#define   ES9039_SYNC_MODE		BIT(6)	/* 0 = ASYNC, which DATUM uses */
#define   ES9039_ENABLE_DAC_CLK		BIT(7)	/* set at reset */
#define   ES9039_DECODE_MASK		(ES9039_ENABLE_TDM_DECODE | \
					 ES9039_ENABLE_DSD_DECODE | \
					 ES9039_ENABLE_DOP_DECODE)

#define ES9039_AUTO_FS_DETECT		0x03	/* reg 3 */
#define   ES9039_AUTO_FS_DETECT_EN	BIT(7)

#define ES9039_INPUT_SEL		0x39	/* reg 57 */
#define   ES9039_AUTO_INPUT_SEL		BIT(0)
#define   ES9039_INPUT_SEL_MASK		GENMASK(2, 1)
#define     ES9039_INPUT_PCM		0x0
#define     ES9039_INPUT_DSD		0x1
#define     ES9039_INPUT_DOP		0x2
#define     ES9039_INPUT_SPDIF		0x3
#define   ES9039_PCM_MASTER_MODE	BIT(4)
#define   ES9039_DSD_MASTER_MODE	BIT(5)
#define   ES9039_DSD_FAULT_DETECT	BIT(6)	/* set at reset */

#define ES9039_MASTER_ENC		0x3a	/* reg 58 */
#define   ES9039_BCK_INV		BIT(6)

#define ES9039_TDM_CH_NUM		0x3b	/* reg 59, slots = value + 1 */
#define   ES9039_TDM_CH_NUM_MASK	GENMASK(4, 0)

#define ES9039_TDM_CONFIG1		0x3c	/* reg 60 */
#define   ES9039_TDM_VALID_EDGE		BIT(6)
#define   ES9039_TDM_LJ_MODE		BIT(7)	/* 0 = standard I2S */

#define ES9039_TDM_CONFIG2		0x3d	/* reg 61 */
#define   ES9039_TDM_BIT_WIDTH_MASK	GENMASK(6, 5)
#define     ES9039_WIDTH_32		0x0
#define     ES9039_WIDTH_24		0x1
#define     ES9039_WIDTH_16		0x2

#define ES9039_MONITOR_CFG		0x3e	/* reg 62 */
#define   ES9039_DISABLE_PCM_DC		BIT(3)
#define   ES9039_ENABLE_BCK_MONITOR	BIT(4)	/* set at reset */
#define   ES9039_ENABLE_WS_MONITOR	BIT(5)	/* set at reset */
#define   ES9039_DISABLE_DSD_MUTE	BIT(6)
#define   ES9039_DISABLE_DSD_DC		BIT(7)

#define ES9039_VOLUME_CH1		0x4a	/* reg 74, 0x00 = 0 dB */
#define ES9039_VOLUME_CH2		0x4b	/* reg 75, 0xff = -127.5 dB */
#define   ES9039_VOL_MAX		0xff

#define ES9039_VOL_RATE_UP		0x52	/* reg 82 */
#define ES9039_VOL_RATE_DOWN		0x53	/* reg 83 */

#define ES9039_DAC_MUTE			0x56	/* reg 86, 1 = muted */
#define   ES9039_MUTE_CH1		BIT(0)
#define   ES9039_MUTE_CH2		BIT(1)
#define   ES9039_MUTE_BOTH		(ES9039_MUTE_CH1 | ES9039_MUTE_CH2)

#define ES9039_DAC_INVERT		0x57	/* reg 87 */

#define ES9039_FILTER_SHAPE		0x58	/* reg 88, [7:3] reset to 10111 */
#define   ES9039_FILTER_SHAPE_MASK	GENMASK(2, 0)
#define     ES9039_FILTER_APODIZING	1	/* linear phase apodizing fast */

#define ES9039_IIR_SPDIF		0x59	/* reg 89 */
#define   ES9039_IIR_BW_MASK		GENMASK(2, 0)
#define   ES9039_VOLUME_HOLD		BIT(3)
#define   ES9039_SPDIF_SEL_MASK		GENMASK(7, 4)

#define ES9039_DAC_PATH			0x5a	/* reg 90 */
#define   ES9039_BYPASS_FIR2X		BIT(0)
#define   ES9039_BYPASS_FIR4X		BIT(1)
#define   ES9039_BYPASS_IIR		BIT(2)

#define ES9039_THD_C2			0x5b	/* regs 91-94: CH1 lo, CH2 hi */
#define ES9039_THD_C3			0x6b	/* regs 107-110 */

#define ES9039_AUTOMUTE_EN		0x7b	/* reg 123, both set at reset */
#define ES9039_AUTOMUTE_TIME		0x7c	/* regs 124-125 */
#define   ES9039_AUTOMUTE_TIME_MASK	GENMASK(10, 0)
#define   ES9039_MUTE_RAMP_TO_GND	BIT(11)	/* set at reset */
#define ES9039_AUTOMUTE_LEVEL		0x7e	/* regs 126-127 */
#define ES9039_AUTOMUTE_OFF_LEVEL	0x80	/* regs 128-129 */

#define ES9039_SOFT_RAMP		0x82	/* reg 130, valid 0..12 */
#define   ES9039_SOFT_RAMP_MASK		GENMASK(4, 0)
#define   ES9039_SOFT_RAMP_MAX		12

#define ES9039_NSMOD			0x83	/* reg 131 */
#define   ES9039_NSMOD_WIDE_BW_MASK	GENMASK(4, 1)
#define     ES9039_NSMOD_DEFAULT	0x4
#define     ES9039_NSMOD_WIDE		0xc

#define ES9039_PROG_RAM_CTRL		0x87	/* reg 135 */
#define   ES9039_PROG_COEFF_EN		BIT(0)
#define   ES9039_PROG_COEFF_WE		BIT(1)

#define ES9039_PROG_RAM_ADDR		0x89	/* reg 137 */
#define   ES9039_PROG_ADDR_MASK		GENMASK(6, 0)
#define   ES9039_PROG_STAGE_4X		BIT(7)

#define ES9039_PROG_RAM_DATA		0x8a	/* regs 138-140, 24-bit signed */

#define ES9039_LAST_RW			0x8e	/* reg 145 */

/* ----------------------------------------------------- readback registers -- */

#define ES9039_READBACK_BASE		0xe0	/* reg 224 */

#define ES9039_CHIP_ID			0xe1	/* reg 225 */
#define   ES9039_CHIP_ID_ES9039Q2M	0x63

#define ES9039_IRQ_SOURCES		0xea	/* regs 234-235, 16-bit */
#define   ES9039_SRC_VOL_MIN_MASK	GENMASK(1, 0)
#define   ES9039_SRC_AUTOMUTE_MASK	GENMASK(3, 2)
#define   ES9039_SRC_SS_RAMP_MASK	GENMASK(5, 4)
#define   ES9039_SRC_DOP_VALID		BIT(6)
#define   ES9039_SRC_BCK_WS_FAIL	BIT(7)
#define   ES9039_SRC_TDM_VALID		BIT(11)

#define ES9039_AUTO_FS_READ		0xef	/* reg 239 */
#define   ES9039_FS_DIV_MASK		GENMASK(5, 0)
#define   ES9039_FS_HALF_DIV		BIT(6)
#define   ES9039_FS_DIV_VALID		BIT(7)

#define ES9039_AUTOMUTE_READ		0xf2	/* reg 242 */

#define ES9039_INPUT_STREAM_READ	0xf5	/* reg 245 */
#define   ES9039_RD_INPUT_SEL_MASK	GENMASK(1, 0)
#define   ES9039_RD_DOP_VALID		BIT(2)
#define   ES9039_RD_TDM_VALID		BIT(3)
#define   ES9039_RD_SPDIF_VALID		BIT(4)

#define ES9039_MAX_REGISTER		0xfb	/* reg 251 */

/* Programmable oversampling FIR: 128 taps in the 2x stage, 32 in the 4x. */
#define ES9039_FIR2X_TAPS		128
#define ES9039_FIR4X_TAPS		32
#define ES9039_COEFF_BYTES		3

/* ------------------------------------------------------------------ private */

struct es9039q2m_priv {
	struct regmap *regmap;
	struct clk *mclk;
	unsigned int mclk_rate;
	unsigned int fmt;
	unsigned int stream_rate;	/* last rate from hw_params */
	unsigned int bclk_ratio;	/* bit clocks per frame, 0 = unknown */
	bool dop_auto;		/* let the part detect DoP itself */
};

/*
 * Multi-byte fields are little-endian across ascending register addresses:
 * register N holds bits [7:0], N+1 holds [15:8], and so on.
 */
static int es9039_read_le(struct regmap *map, unsigned int reg, int n, u32 *out)
{
	u8 buf[4];
	int ret, i;

	ret = regmap_bulk_read(map, reg, buf, n);
	if (ret)
		return ret;

	*out = 0;
	for (i = 0; i < n; i++)
		*out |= (u32)buf[i] << (8 * i);

	return 0;
}

static int es9039_write_le(struct regmap *map, unsigned int reg, int n, u32 val)
{
	u8 buf[4];
	int i;

	for (i = 0; i < n; i++)
		buf[i] = (val >> (8 * i)) & 0xff;

	return regmap_bulk_write(map, reg, buf, n);
}

/* ------------------------------------------------------------------ regmap */

static bool es9039q2m_writeable_reg(struct device *dev, unsigned int reg)
{
	return reg <= ES9039_LAST_RW;
}

static bool es9039q2m_readable_reg(struct device *dev, unsigned int reg)
{
	return reg <= ES9039_LAST_RW || reg >= ES9039_READBACK_BASE;
}

static bool es9039q2m_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg >= ES9039_READBACK_BASE;
}

static const struct regmap_config es9039q2m_regmap = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= ES9039_MAX_REGISTER,
	.writeable_reg	= es9039q2m_writeable_reg,
	.readable_reg	= es9039q2m_readable_reg,
	.volatile_reg	= es9039q2m_volatile_reg,
	.cache_type	= REGCACHE_MAPLE,
};

/* --------------------------------------------------- signed 16-bit controls */

struct es9039_s16_ctl {
	unsigned int reg;
	unsigned int shift;	/* 0 for CH1, 16 for CH2 within the 32-bit pair */
};

static int es9039_s16_info(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = S16_MIN;
	uinfo->value.integer.max = S16_MAX;
	return 0;
}

static int es9039_s16_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	struct es9039_s16_ctl *p = (void *)kcontrol->private_value;
	u32 v;
	int ret;

	ret = es9039_read_le(priv->regmap, p->reg + (p->shift / 8), 2, &v);
	if (ret)
		return ret;

	ucontrol->value.integer.value[0] = (s16)v;
	return 0;
}

static int es9039_s16_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	struct es9039_s16_ctl *p = (void *)kcontrol->private_value;
	long v = ucontrol->value.integer.value[0];
	u32 old;
	int ret;

	if (v < S16_MIN || v > S16_MAX)
		return -EINVAL;

	ret = es9039_read_le(priv->regmap, p->reg + (p->shift / 8), 2, &old);
	if (ret)
		return ret;

	if ((s16)old == (s16)v)
		return 0;

	ret = es9039_write_le(priv->regmap, p->reg + (p->shift / 8), 2,
			      (u16)v);
	return ret ? ret : 1;
}

#define ES9039_S16(xname, xreg, xshift)					\
{									\
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,				\
	.name	= xname,						\
	.info	= es9039_s16_info,					\
	.get	= es9039_s16_get,					\
	.put	= es9039_s16_put,					\
	.private_value = (unsigned long)&(struct es9039_s16_ctl)	\
			 { .reg = xreg, .shift = xshift },		\
}

/* ------------------------------------------------- multi-register integers */

struct es9039_wide_ctl {
	unsigned int reg;
	unsigned int bytes;
	unsigned int mask;
	unsigned int max;
};

static int es9039_wide_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	struct es9039_wide_ctl *p = (void *)kcontrol->private_value;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = p->max;
	return 0;
}

static int es9039_wide_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	struct es9039_wide_ctl *p = (void *)kcontrol->private_value;
	u32 v;
	int ret;

	ret = es9039_read_le(priv->regmap, p->reg, p->bytes, &v);
	if (ret)
		return ret;

	ucontrol->value.integer.value[0] = v & p->mask;
	return 0;
}

static int es9039_wide_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	struct es9039_wide_ctl *p = (void *)kcontrol->private_value;
	long v = ucontrol->value.integer.value[0];
	u32 old;
	int ret;

	if (v < 0 || v > p->max)
		return -EINVAL;

	ret = es9039_read_le(priv->regmap, p->reg, p->bytes, &old);
	if (ret)
		return ret;

	if ((old & p->mask) == (u32)v)
		return 0;

	/* Preserve the bits outside the field - reg 124 carries MUTE_RAMP. */
	ret = es9039_write_le(priv->regmap, p->reg, p->bytes,
			      (old & ~p->mask) | (u32)v);
	return ret ? ret : 1;
}

#define ES9039_WIDE(xname, xreg, xbytes, xmask, xmax)			\
{									\
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,				\
	.name	= xname,						\
	.info	= es9039_wide_info,					\
	.get	= es9039_wide_get,					\
	.put	= es9039_wide_put,					\
	.private_value = (unsigned long)&(struct es9039_wide_ctl)	\
			 { .reg = xreg, .bytes = xbytes,		\
			   .mask = xmask, .max = xmax },		\
}

/* ------------------------------------------------ programmable FIR upload --
 *
 * Write-only, deliberately. The chip has a PROG_COEFF_OUT register (248-246)
 * described only as "Programmable FIR coefficient readback", but it is not a
 * RAM read port: it returns the LAST COEFFICIENT WRITTEN, whatever address is
 * selected in PROG_COEFF_ADDR. Measured on B1, 2026-08-19, over raw I2C with
 * this driver out of the path - two different coefficients written to
 * addresses 0 and 1, then read back with five different sequences (plain, with
 * a settle delay, with PROG_COEFF_EN set, with the address written twice, and
 * with a WE pulse after the address). All ten reads returned the value written
 * to address 1.
 *
 * A get() built on that register would return something with the shape of data
 * and none of its meaning, so there is no get(). If ESS documents a real
 * readback sequence, add one - tools/es9039-coeff-probe.sh in the DATUM
 * repository is the test it has to pass.
 */

/*
 * Per-control data rides in our own struct with the soc_bytes_ext EMBEDDED,
 * recovered by container_of. Not in soc_bytes_ext.dobj: that field belongs to
 * the topology subsystem and only exists under CONFIG_SND_SOC_TOPOLOGY, so a
 * driver stashing its own data there fails to build on any config without it.
 */
struct es9039_fir_ctl {
	struct soc_bytes_ext be;
	unsigned int taps;
	bool stage_4x;
};

static int es9039_fir_put(struct snd_kcontrol *kcontrol,
			  const unsigned int __user *bytes, unsigned int size)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	struct soc_bytes_ext *be = (void *)kcontrol->private_value;
	struct es9039_fir_ctl *p = container_of(be, struct es9039_fir_ctl, be);
	u8 *buf;
	int ret, i;

	if (size != p->taps * ES9039_COEFF_BYTES)
		return -EINVAL;

	buf = memdup_user(bytes, size);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	/*
	 * PROG_COEFF_WE is a per-coefficient strobe, not a gate held open
	 * across the upload. The datasheet's sequence is address, data, raise
	 * WE, lower WE, once per coefficient. Holding it high for the whole
	 * loop also appears to work on ES9039Q2M silicon, but "appears to
	 * work" is not a specification.
	 */
	for (i = 0; i < p->taps; i++) {
		ret = regmap_write(priv->regmap, ES9039_PROG_RAM_ADDR,
				   (p->stage_4x ? ES9039_PROG_STAGE_4X : 0) |
				   FIELD_PREP(ES9039_PROG_ADDR_MASK, i));
		if (ret)
			goto out;

		ret = regmap_bulk_write(priv->regmap, ES9039_PROG_RAM_DATA,
					&buf[i * ES9039_COEFF_BYTES],
					ES9039_COEFF_BYTES);
		if (ret)
			goto out;

		ret = regmap_update_bits(priv->regmap, ES9039_PROG_RAM_CTRL,
					 ES9039_PROG_COEFF_WE,
					 ES9039_PROG_COEFF_WE);
		if (ret)
			goto out;

		ret = regmap_update_bits(priv->regmap, ES9039_PROG_RAM_CTRL,
					 ES9039_PROG_COEFF_WE, 0);
		if (ret)
			goto out;
	}

out:
	regmap_update_bits(priv->regmap, ES9039_PROG_RAM_CTRL,
			   ES9039_PROG_COEFF_WE, 0);
	kfree(buf);
	return ret ? ret : 1;
}

static struct es9039_fir_ctl es9039_fir2x = {
	.be = { .max = ES9039_FIR2X_TAPS * ES9039_COEFF_BYTES,
		.put = es9039_fir_put },
	.taps = ES9039_FIR2X_TAPS,
};

static struct es9039_fir_ctl es9039_fir4x = {
	.be = { .max = ES9039_FIR4X_TAPS * ES9039_COEFF_BYTES,
		.put = es9039_fir_put },
	.taps = ES9039_FIR4X_TAPS,
	.stage_4x = true,
};

#define ES9039_FIR(xname, xctl)						\
{									\
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,				\
	.name	= xname,						\
	.info	= snd_soc_bytes_info_ext,				\
	.tlv.c	= snd_soc_bytes_tlv_callback,				\
	.access	= SNDRV_CTL_ELEM_ACCESS_TLV_WRITE |			\
		  SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,			\
	.private_value = (unsigned long)&(xctl).be,			\
}

/* ------------------------------------------------------- status (read-only) */

struct es9039_stat_ctl {
	unsigned int reg;
	unsigned int mask;
	unsigned int max;
};

static int es9039_stat_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	struct es9039_stat_ctl *p = (void *)kcontrol->private_value;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = p->max;
	return 0;
}

static int es9039_stat_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	struct es9039_stat_ctl *p = (void *)kcontrol->private_value;
	unsigned int v;
	int ret;

	ret = regmap_read(priv->regmap, p->reg, &v);
	if (ret)
		return ret;

	ucontrol->value.integer.value[0] =
		(v & p->mask) >> (ffs(p->mask) - 1);
	return 0;
}

#define ES9039_STAT(xname, xreg, xmask, xmax)				\
{									\
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,				\
	.name	= xname,						\
	.access	= SNDRV_CTL_ELEM_ACCESS_READ |				\
		  SNDRV_CTL_ELEM_ACCESS_VOLATILE,			\
	.info	= es9039_stat_info,					\
	.get	= es9039_stat_get,					\
	.private_value = (unsigned long)&(struct es9039_stat_ctl)	\
			 { .reg = xreg, .mask = xmask, .max = xmax },	\
}

/*
 * Detected sample rate.
 *
 * When the part's own rate detector has a valid ratio, use it - it is measured
 * from the incoming frame clock and is the ground truth:
 *
 *   FS = Y * SYS_CLK / ((X + 1) * (128 >> 64FS_MODE))
 *
 * with X = IDAC_DIV_AUTO and Y = 2 when IDAC_HALF_DIV_AUTO reports a
 * half-integer multiple.
 *
 * That detector is UNAVAILABLE on any board running the DAC asynchronously -
 * register 3[7] AUTO_FS_DETECT carries the note "Cannot be used in ASYNC mode".
 * A board feeding a free-running oscillator and letting the ASRC absorb the
 * difference is precisely that case, and it is the preferable design, so the
 * detector reading 0 there is expected rather than a fault. Fall back to the
 * rate the stream was opened at, which is what a front panel wants to show.
 * Reports 0 only when nothing is playing and the chip has no lock either.
 */
static int es9039_rate_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1536000;
	return 0;
}

static int es9039_rate_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	unsigned int fsreg, sysreg, div, y, den;
	int ret;

	ucontrol->value.integer.value[0] = priv->stream_rate;

	if (!priv->mclk_rate)
		return 0;

	ret = regmap_read(priv->regmap, ES9039_AUTO_FS_READ, &fsreg);
	if (ret)
		return ret;

	if (!(fsreg & ES9039_FS_DIV_VALID))
		return 0;	/* async mode: keep the stream rate set above */

	ret = regmap_read(priv->regmap, ES9039_SYSTEM_CONFIG, &sysreg);
	if (ret)
		return ret;

	div = FIELD_GET(ES9039_FS_DIV_MASK, fsreg) + 1;
	y   = (fsreg & ES9039_FS_HALF_DIV) ? 2 : 1;
	den = div * ((sysreg & ES9039_64FS_MODE) ? 64 : 128);

	ucontrol->value.integer.value[0] =
		DIV_ROUND_CLOSEST(priv->mclk_rate * y, den);
	return 0;
}

static const char * const es9039_stream_texts[] = {
	"PCM", "DSD", "DoP", "S/PDIF",
};

static int es9039_stream_info(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(es9039_stream_texts),
				 es9039_stream_texts);
}

static int es9039_stream_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	unsigned int v;
	int ret;

	ret = regmap_read(priv->regmap, ES9039_INPUT_STREAM_READ, &v);
	if (ret)
		return ret;

	ucontrol->value.enumerated.item[0] =
		FIELD_GET(ES9039_RD_INPUT_SEL_MASK, v);
	return 0;
}

/* ----------------------------------------------------------------- controls */

static const DECLARE_TLV_DB_SCALE(es9039_vol_tlv, -12750, 50, 1);

static const char * const es9039_filter_texts[] = {
	"Minimum Phase",
	"Linear Phase Apodizing Fast Roll-Off",
	"Linear Phase Fast Roll-Off",
	"Linear Phase Fast Roll-Off Low Ripple",
	"Linear Phase Slow Roll-Off",
	"Minimum Phase Fast Roll-Off",
	"Minimum Phase Slow Roll-Off",
	"Minimum Phase Slow Roll-Off Low Dispersion",
};

static SOC_ENUM_SINGLE_DECL(es9039_filter_enum, ES9039_FILTER_SHAPE, 0,
			    es9039_filter_texts);

/* IIR_BW is a multiple of the datapath bandwidth, not a frequency. */
static const char * const es9039_iir_texts[] = {
	"Reserved", "BW x8", "BW x4", "BW x2", "BW", "BW /2", "BW /4", "BW /8",
};

static SOC_ENUM_SINGLE_DECL(es9039_iir_enum, ES9039_IIR_SPDIF, 0,
			    es9039_iir_texts);

static const char * const es9039_nsmod_texts[] = {
	"Default", "Wide Bandwidth",
};

static const unsigned int es9039_nsmod_values[] = {
	ES9039_NSMOD_DEFAULT, ES9039_NSMOD_WIDE,
};

static SOC_VALUE_ENUM_SINGLE_DECL(es9039_nsmod_enum, ES9039_NSMOD, 1,
				  GENMASK(3, 0), es9039_nsmod_texts,
				  es9039_nsmod_values);

static int es9039_dop_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);

	ucontrol->value.integer.value[0] = priv->dop_auto;
	return 0;
}

static int es9039_dop_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(c);
	bool on = !!ucontrol->value.integer.value[0];
	int ret;

	if (on == priv->dop_auto)
		return 0;

	priv->dop_auto = on;

	ret = regmap_update_bits(priv->regmap, ES9039_INPUT_SEL,
				 ES9039_AUTO_INPUT_SEL,
				 on ? ES9039_AUTO_INPUT_SEL : 0);
	if (ret)
		return ret;

	return 1;
}

static const struct snd_kcontrol_new es9039q2m_controls[] = {
	/* --- level --- */
	SOC_DOUBLE_R_TLV("Master Playback Volume",
			 ES9039_VOLUME_CH1, ES9039_VOLUME_CH2,
			 0, ES9039_VOL_MAX, 1, es9039_vol_tlv),
	SOC_DOUBLE("Master Playback Switch", ES9039_DAC_MUTE, 0, 1, 1, 1),
	SOC_DOUBLE("DAC Invert Switch", ES9039_DAC_INVERT, 0, 1, 1, 0),
	SOC_SINGLE("Volume Ramp Up Rate", ES9039_VOL_RATE_UP, 0, 255, 0),
	SOC_SINGLE("Volume Ramp Down Rate", ES9039_VOL_RATE_DOWN, 0, 255, 0),
	SOC_SINGLE("Soft Ramp Time", ES9039_SOFT_RAMP, 0,
		   ES9039_SOFT_RAMP_MAX, 0),

	/* --- reconstruction filter --- */
	SOC_ENUM("Filter Shape", es9039_filter_enum),
	SOC_ENUM("IIR Bandwidth", es9039_iir_enum),
	SOC_ENUM("Modulator Bandwidth", es9039_nsmod_enum),
	SOC_SINGLE("IIR Filter Bypass Switch", ES9039_DAC_PATH, 2, 1, 0),
	SOC_SINGLE("FIR 2x Bypass Switch", ES9039_DAC_PATH, 0, 1, 0),
	SOC_SINGLE("FIR 4x Bypass Switch", ES9039_DAC_PATH, 1, 1, 0),
	SOC_SINGLE("Custom FIR Switch", ES9039_PROG_RAM_CTRL, 0, 1, 0),
	ES9039_FIR("FIR 2x Coefficients", es9039_fir2x),
	ES9039_FIR("FIR 4x Coefficients", es9039_fir4x),

	/*
	 * Not "... Volume". ALSA reserves that suffix for gain controls
	 * carrying a dB scale, and these are signed correction coefficients
	 * for the second and third harmonic - they have no dB meaning and no
	 * TLV. Useful values come from measuring a given board's distortion on
	 * an analyser and solving for them; zero, the reset value, is the only
	 * honest default until someone has.
	 */
	/* --- distortion compensation --- */
	ES9039_S16("THD Compensation C2 CH1", ES9039_THD_C2, 0),
	ES9039_S16("THD Compensation C2 CH2", ES9039_THD_C2, 16),
	ES9039_S16("THD Compensation C3 CH1", ES9039_THD_C3, 0),
	ES9039_S16("THD Compensation C3 CH2", ES9039_THD_C3, 16),

	/* --- automute --- */
	SOC_DOUBLE("Automute Switch", ES9039_AUTOMUTE_EN, 0, 1, 1, 0),
	ES9039_WIDE("Automute Time", ES9039_AUTOMUTE_TIME, 2,
		    ES9039_AUTOMUTE_TIME_MASK, 2047),
	ES9039_WIDE("Automute Level", ES9039_AUTOMUTE_LEVEL, 2, 0xffff, 65535),
	ES9039_WIDE("Automute Off Level", ES9039_AUTOMUTE_OFF_LEVEL, 2,
		    0xffff, 65535),
	SOC_SINGLE("Mute Ramp To Ground Switch", ES9039_AUTOMUTE_TIME + 1,
		   3, 1, 0),
	SOC_SINGLE("DSD DC Automute Switch", ES9039_MONITOR_CFG, 7, 1, 1),
	SOC_SINGLE("DSD Mute Pattern Switch", ES9039_MONITOR_CFG, 6, 1, 1),
	SOC_SINGLE("PCM DC Automute Switch", ES9039_MONITOR_CFG, 3, 1, 1),

	/* --- stream --- */
	/*
	 * On by default. DoP is designed to be detected, not announced: the
	 * player just sends it and a DoP-aware DAC notices the marker, which
	 * is why a DAC that does not notice plays it as near-silence rather
	 * than noise. Roon and every other player rely on that, and none of
	 * them can reach into ALSA to flip a mode first.
	 *
	 * Left switchable because automatic detection is a pattern match, and
	 * anyone worried about PCM material that happens to look like DoP can
	 * turn it off and get strictly PCM.
	 */
	SOC_SINGLE_BOOL_EXT("DoP Auto Detect Switch", 0,
			    es9039_dop_get, es9039_dop_put),

	/* --- status, read-only --- */
	ES9039_STAT("Automute Active CH1", ES9039_AUTOMUTE_READ, BIT(0), 1),
	ES9039_STAT("Automute Active CH2", ES9039_AUTOMUTE_READ, BIT(1), 1),
	ES9039_STAT("DoP Valid", ES9039_INPUT_STREAM_READ,
		    ES9039_RD_DOP_VALID, 1),
	ES9039_STAT("TDM Data Valid", ES9039_INPUT_STREAM_READ,
		    ES9039_RD_TDM_VALID, 1),
	ES9039_STAT("SPDIF Valid", ES9039_INPUT_STREAM_READ,
		    ES9039_RD_SPDIF_VALID, 1),
	ES9039_STAT("Clock Fault", ES9039_IRQ_SOURCES + 1, BIT(7), 1),
	{
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Detected Sample Rate",
		.access	= SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info	= es9039_rate_info,
		.get	= es9039_rate_get,
	},
	{
		.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name	= "Detected Input Format",
		.access	= SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info	= es9039_stream_info,
		.get	= es9039_stream_get,
	},
};

/* --------------------------------------------------------------------- DAPM */

static const struct snd_soc_dapm_widget es9039q2m_widgets[] = {
	SND_SOC_DAPM_DAC("DAC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("AOUTL"),
	SND_SOC_DAPM_OUTPUT("AOUTR"),
};

static const struct snd_soc_dapm_route es9039q2m_routes[] = {
	{ "DAC",   NULL, "Playback" },
	{ "AOUTL", NULL, "DAC" },
	{ "AOUTR", NULL, "DAC" },
};

/* ---------------------------------------------------------------------- DAI */

/*
 * In asynchronous mode - a fixed oscillator with no relationship to the incoming
 * frame clock, which is how any board using the ASRC properly is wired - the
 * datasheet requires MCLK >= 130 * FS (hardware mode table, ASYNC rows). Cap the
 * rate accordingly so a machine driver cannot open a stream the clock cannot
 * legally carry. With a 24.576 MHz oscillator that ceiling is 189 kHz, which
 * means 176.4 kHz is available and 192 kHz is not.
 */
#define ES9039_ASYNC_MIN_MCLK_FS	130

static int es9039q2m_startup(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(dai->component);
	unsigned int max_rate;

	if (!priv->mclk_rate)
		return 0;

	max_rate = priv->mclk_rate / ES9039_ASYNC_MIN_MCLK_FS;

	return snd_pcm_hw_constraint_minmax(substream->runtime,
					    SNDRV_PCM_HW_PARAM_RATE,
					    8000, max_rate);
}

static int es9039q2m_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(dai->component);
	unsigned int cfg1 = 0, enc = 0;
	int ret;

	/*
	 * The part can generate BCLK/WS (PCM_MASTER_MODE), but on a board fed a
	 * fixed oscillator that means deriving them from MCLK and throwing away
	 * the ASRC's entire purpose. Consumer only, and say so.
	 */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC)
		return -EINVAL;

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		cfg1 |= ES9039_TDM_LJ_MODE;
		break;
	default:
		/* Register 60 offers I2S or LJ. There is no RJ mode. */
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_NF:
		enc |= ES9039_BCK_INV;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(priv->regmap, ES9039_TDM_CONFIG1,
				 ES9039_TDM_LJ_MODE, cfg1);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, ES9039_MASTER_ENC,
				 ES9039_BCK_INV, enc);
	if (ret)
		return ret;

	priv->fmt = fmt;
	return 0;
}

/*
 * TDM_BIT_WIDTH describes the SLOT width on the wire, not the sample size. Those
 * are routinely different: 16-bit samples are usually carried left-justified in
 * 32-bit slots. Getting this wrong misaligns the channel boundaries and the
 * result is one channel, or noise.
 *
 * ASoC does not hand the codec the bit clock ratio unless a machine driver sets
 * it, so take it when offered and otherwise assume 32-bit slots - by far the
 * most common arrangement, and what the RK3588 I2S does unconditionally
 * (rockchip_i2s.c sets bclk_ratio = 64 at probe and never varies it with
 * format).
 */
static int es9039q2m_set_bclk_ratio(struct snd_soc_dai *dai, unsigned int ratio)
{
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(dai->component);

	priv->bclk_ratio = ratio;
	return 0;
}

static int es9039q2m_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai)
{
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(dai->component);
	unsigned int input_sel, decode, width, slot_bits, isel;
	bool auto_sel;
	int ret;

	priv->stream_rate = params_rate(params);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_DSD_U8:
	case SNDRV_PCM_FORMAT_DSD_U16_LE:
	case SNDRV_PCM_FORMAT_DSD_U32_LE:
		/*
		 * Forced, not auto-detected: the datasheet requires DSD data on
		 * DATA1 and DATA2 for AUTO_INPUT_SEL to identify it, which a
		 * two-channel I2S link does not provide.
		 */
		input_sel = ES9039_INPUT_DSD;
		decode = ES9039_ENABLE_DSD_DECODE;
		auto_sel = false;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_3LE:
	case SNDRV_PCM_FORMAT_S32_LE:
		/*
		 * DoP arrives inside ordinary PCM frames and is indistinguishable
		 * from PCM until the part finds the marker, so both decoders run
		 * and AUTO_INPUT_SEL picks between them. INPUT_SEL is programmed
		 * anyway as the fallback the part uses when auto-detection is
		 * switched off.
		 */
		input_sel = ES9039_INPUT_PCM;
		decode = ES9039_ENABLE_TDM_DECODE |
			 (priv->dop_auto ? ES9039_ENABLE_DOP_DECODE : 0);
		auto_sel = priv->dop_auto;
		break;
	default:
		return -EINVAL;
	}

	slot_bits = priv->bclk_ratio ?
		    priv->bclk_ratio / params_channels(params) : 32;

	switch (slot_bits) {
	case 16:
		width = ES9039_WIDTH_16;
		break;
	case 24:
		width = ES9039_WIDTH_24;
		break;
	case 32:
		width = ES9039_WIDTH_32;
		break;
	default:
		dev_err(dai->dev, "unsupported slot width %u\n", slot_bits);
		return -EINVAL;
	}

	/*
	 * AUTO_INPUT_SEL belongs in the value as well as the mask. It was in
	 * the mask alone, so every hw_params quietly cleared it and undid what
	 * the component probe had set - which is why enabling auto-detection
	 * by hand mid-stream worked while enabling it in probe() did not.
	 * INPUT_SEL is still programmed underneath: it is what the part falls
	 * back to when auto-detection is switched off.
	 */
	isel = FIELD_PREP(ES9039_INPUT_SEL_MASK, input_sel);
	if (auto_sel)
		isel |= ES9039_AUTO_INPUT_SEL;

	ret = regmap_update_bits(priv->regmap, ES9039_INPUT_SEL,
				 ES9039_AUTO_INPUT_SEL | ES9039_INPUT_SEL_MASK,
				 isel);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, ES9039_TDM_CONFIG2,
				 ES9039_TDM_BIT_WIDTH_MASK,
				 FIELD_PREP(ES9039_TDM_BIT_WIDTH_MASK, width));
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, ES9039_TDM_CH_NUM,
				 ES9039_TDM_CH_NUM_MASK,
				 params_channels(params) - 1);
	if (ret)
		return ret;

	/*
	 * Above 705.6 kHz there is not enough MCLK for the normal oversampling
	 * chain, so the part runs at 64FS and forces a minimum phase filter
	 * regardless of FILTER_SHAPE.
	 */
	ret = regmap_update_bits(priv->regmap, ES9039_SYSTEM_CONFIG,
				 ES9039_64FS_MODE,
				 params_rate(params) > 705600 ?
					ES9039_64FS_MODE : 0);
	if (ret)
		return ret;

	/* Switch the right decoder on for this stream, and the others off. */
	return regmap_update_bits(priv->regmap, ES9039_SYS_MODE,
				  ES9039_DECODE_MASK, decode);
}

static int es9039q2m_mute_stream(struct snd_soc_dai *dai, int mute, int dir)
{
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(dai->component);

	return regmap_update_bits(priv->regmap, ES9039_DAC_MUTE,
				  ES9039_MUTE_BOTH, mute ? ES9039_MUTE_BOTH : 0);
}

static const struct snd_soc_dai_ops es9039q2m_dai_ops = {
	.startup	 = es9039q2m_startup,
	.set_fmt	 = es9039q2m_set_fmt,
	.set_bclk_ratio	 = es9039q2m_set_bclk_ratio,
	.hw_params	 = es9039q2m_hw_params,
	.mute_stream	 = es9039q2m_mute_stream,
	.no_capture_mute = 1,
};

#define ES9039_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE     | \
			 SNDRV_PCM_FMTBIT_S24_LE     | \
			 SNDRV_PCM_FMTBIT_S24_3LE    | \
			 SNDRV_PCM_FMTBIT_S32_LE     | \
			 SNDRV_PCM_FMTBIT_DSD_U8     | \
			 SNDRV_PCM_FMTBIT_DSD_U16_LE | \
			 SNDRV_PCM_FMTBIT_DSD_U32_LE)

static struct snd_soc_dai_driver es9039q2m_dai = {
	.name = "es9039q2m-hifi",
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 2,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_768000,
		.formats	= ES9039_FORMATS,
	},
	.ops = &es9039q2m_dai_ops,
};

/* ---------------------------------------------------------------- component */

static int es9039q2m_component_probe(struct snd_soc_component *component)
{
	struct es9039q2m_priv *priv = snd_soc_component_get_drvdata(component);
	int ret;

	/*
	 * Come up muted. The volume registers default to 0 dB, and an unmuted
	 * DAC arriving into an already-powered analogue stage is how you get a
	 * thump. ASoC unmutes via mute_stream() when a stream starts.
	 */
	ret = regmap_update_bits(priv->regmap, ES9039_DAC_MUTE,
				 ES9039_MUTE_BOTH, ES9039_MUTE_BOTH);
	if (ret)
		return ret;

	/*
	 * VOLUME_HOLD makes both channel volumes latch together, so a stereo
	 * change can never momentarily skew the image.
	 */
	ret = regmap_update_bits(priv->regmap, ES9039_IIR_SPDIF,
				 ES9039_VOLUME_HOLD, 0);
	if (ret)
		return ret;

	/*
	 * Let the part identify DoP for itself. Register 57[0] AUTO_INPUT_SEL
	 * makes it choose between PCM and DoP from the data, which is the only
	 * way DoP can work in practice: players send DoP-encoded PCM and expect
	 * the DAC to notice, and none of them can flip an ALSA control first.
	 * Without this the part is told "PCM", never looks for the marker, and
	 * renders a DoP stream as the near-silent hiss the DoP marker design
	 * deliberately degrades to. Verified on hardware by enabling this
	 * mid-stream and watching reg 245 flip from PCM to DoP with DOP_VALID
	 * set. tools/datum-dop-test.sh in the DATUM repository is the standing
	 * test: it plays DoP with nothing flipped and expects the part to
	 * identify it unaided.
	 *
	 * The datasheet's "data must be provided on the DATA2 pin" applies to
	 * identifying DSD, whose two channels arrive on separate data lines.
	 * DoP is ordinary stereo I2S on one line and detects correctly without
	 * it, which the same measurement establishes.
	 */
	priv->dop_auto = true;
	ret = regmap_update_bits(priv->regmap, ES9039_INPUT_SEL,
				 ES9039_AUTO_INPUT_SEL, ES9039_AUTO_INPUT_SEL);
	if (ret)
		return ret;

	/*
	 * Board defaults, applied once at probe so the part is deterministic
	 * from cold instead of inheriting whatever its reset value happens to
	 * be. Both stay user-settable through their kcontrols; these are
	 * defaults, not policy.
	 *
	 * Reconstruction filter: linear phase apodizing fast roll-off. It keeps
	 * the sharp cut and flat passband of the plain fast linear-phase filter
	 * while suppressing pre-ringing, and an apodizing response also
	 * suppresses pre-ringing already baked into the source material by the
	 * recording chain - which none of the other seven addresses. The cost
	 * is a little stopband rejection right at the band edge, well above
	 * where it can matter. Chosen this way because a blind A/B listening
	 * test on DATUM found no audible difference between any of the eight,
	 * so the tie is broken on theory rather than on preference.
	 *
	 * Modulator: wide bandwidth, which is ESS's own recommendation. It
	 * moves the modulator's noise further out of band and improves
	 * linearity at high frequencies.
	 */
	ret = regmap_update_bits(priv->regmap, ES9039_FILTER_SHAPE,
				 ES9039_FILTER_SHAPE_MASK,
				 ES9039_FILTER_APODIZING);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, ES9039_NSMOD,
				 ES9039_NSMOD_WIDE_BW_MASK,
				 FIELD_PREP(ES9039_NSMOD_WIDE_BW_MASK,
					    ES9039_NSMOD_WIDE));
	if (ret)
		return ret;

	/* Let the part work out the incoming rate; the ASRC does the rest. */
	return regmap_update_bits(priv->regmap, ES9039_AUTO_FS_DETECT,
				  ES9039_AUTO_FS_DETECT_EN,
				  ES9039_AUTO_FS_DETECT_EN);
}

static const struct snd_soc_component_driver es9039q2m_component = {
	.probe			= es9039q2m_component_probe,
	.controls		= es9039q2m_controls,
	.num_controls		= ARRAY_SIZE(es9039q2m_controls),
	.dapm_widgets		= es9039q2m_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(es9039q2m_widgets),
	.dapm_routes		= es9039q2m_routes,
	.num_dapm_routes	= ARRAY_SIZE(es9039q2m_routes),
	.idle_bias_on		= 1,
	.endianness		= 1,
};

/* --------------------------------------------------------------------- I2C */

static int es9039q2m_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct es9039q2m_priv *priv;
	unsigned int id;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(i2c, &es9039q2m_regmap);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to init regmap\n");

	priv->mclk = devm_clk_get_optional_enabled(dev, "mclk");
	if (IS_ERR(priv->mclk))
		return dev_err_probe(dev, PTR_ERR(priv->mclk),
				     "failed to get mclk\n");

	if (priv->mclk) {
		priv->mclk_rate = clk_get_rate(priv->mclk);
		if (priv->mclk_rate > 50000000)
			return dev_err_probe(dev, -EINVAL,
					     "mclk %u Hz exceeds the 50 MHz maximum\n",
					     priv->mclk_rate);
	}

	i2c_set_clientdata(i2c, priv);

	ret = regmap_read(priv->regmap, ES9039_CHIP_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "no response at 0x%02x\n",
				     i2c->addr);

	if (id != ES9039_CHIP_ID_ES9039Q2M)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected chip id 0x%02x, want 0x%02x\n",
				     id, ES9039_CHIP_ID_ES9039Q2M);

	dev_info(dev, "ES9039Q2M at 0x%02x, mclk %u Hz\n",
		 i2c->addr, priv->mclk_rate);

	return devm_snd_soc_register_component(dev, &es9039q2m_component,
					       &es9039q2m_dai, 1);
}

static const struct of_device_id es9039q2m_of_match[] = {
	{ .compatible = "ess,es9039q2m" },
	{ }
};
MODULE_DEVICE_TABLE(of, es9039q2m_of_match);

static const struct i2c_device_id es9039q2m_i2c_id[] = {
	{ "es9039q2m" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, es9039q2m_i2c_id);

static struct i2c_driver es9039q2m_i2c_driver = {
	.driver = {
		.name		= "es9039q2m",
		.of_match_table	= es9039q2m_of_match,
	},
	.probe		= es9039q2m_i2c_probe,
	.id_table	= es9039q2m_i2c_id,
};
module_i2c_driver(es9039q2m_i2c_driver);

MODULE_DESCRIPTION("ASoC ES9039Q2M driver");
MODULE_AUTHOR("Karl Asseily <karl@asseily.com>");
MODULE_LICENSE("GPL");
