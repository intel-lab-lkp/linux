// SPDX-License-Identifier: GPL-2.0
//
// ALSA SoC Texas Instruments TAS2563/TAS2781 Audio Smart Amplifier
//
// Copyright (C) 2022 - 2024 Texas Instruments Incorporated
// https://www.ti.com
//
// The TAS2563/TAS2781 driver implements a flexible and configurable
// algo coefficient setting for one, two, or even multiple
// TAS2563/TAS2781 chips.
//
// Author: Shenghao Ding <shenghao-ding@ti.com>
// Author: Kevin Lu <kevin-lu@ti.com>
//

#include <asm/unaligned.h>
#include <linux/crc8.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tas2781.h>
#include <sound/tlv.h>
#include <sound/tas2781-tlv.h>

static const struct bulk_reg_val tas2563_cali_start_reg[] = {
	{
		.reg = TAS2563_PRM_ENFF_REG,
		.val = { 0x40, 0x00, 0x00, 0x00 },
	},
	{
		.reg = TAS2563_PRM_DISTCK_REG,
		.val = { 0x40, 0x00, 0x00, 0x00 },
	},
	{
		.reg = TAS2563_PRM_TE_SCTHR_REG,
		.val = { 0x7f, 0xff, 0xff, 0xff },
	},
	{
		.reg = TAS2563_PRM_PLT_FLAG_REG,
		.val = { 0x40, 0x00, 0x00, 0x00 },
	},
	{
		.reg = TAS2563_PRM_SINEGAIN_REG,
		.val = { 0x0a, 0x3d, 0x70, 0xa4 },
	},
	{
		.reg = TAS2563_TE_TA1_REG,
		.val = { 0x00, 0x36, 0x91, 0x5e },
	},
	{
		.reg = TAS2563_TE_TA1_AT_REG,
		.val = { 0x00, 0x36, 0x91, 0x5e },
	},
	{
		.reg = TAS2563_TE_TA2_REG,
		.val = { 0x00, 0x06, 0xd3, 0x72 },
	},
	{
		.reg = TAS2563_TE_AT_REG,
		.val = { 0x00, 0x36, 0x91, 0x5e },
	},
	{
		.reg = TAS2563_TE_DT_REG,
		.val = { 0x00, 0x36, 0x91, 0x5e },
	},
};

static const struct bulk_reg_val tas2781_cali_start_reg[] = {
	{
		.reg = TAS2781_PRM_INT_MASK_REG,
		.val = { 0xfe },
		.val_len = 1,
		.is_locked = false
	},
	{
		.reg = TAS2781_PRM_CLK_CFG_REG,
		.val = { 0xdd },
		.val_len = 1,
		.is_locked = false
	},
	{
		.reg = TAS2781_PRM_RSVD_REG,
		.val = { 0x20 },
		.val_len = 1,
		.is_locked = false
	},
	{
		.reg = TAS2781_PRM_TEST_57_REG,
		.val = { 0x14 },
		.val_len = 1,
		.is_locked = true
	},
	{
		.reg = TAS2781_PRM_TEST_62_REG,
		.val = { 0x45 },
		.val_len = 1,
		.is_locked = true
	},
	{
		.reg = TAS2781_PRM_PVDD_UVLO_REG,
		.val = { 0x03 },
		.val_len = 1,
		.is_locked = false
	},
	{
		.reg = TAS2781_PRM_CHNL_0_REG,
		.val = { 0xA8 },
		.val_len = 1,
		.is_locked = false
	},
	{
		.reg = TAS2781_PRM_NG_CFG0_REG,
		.val = { 0xb9 },
		.val_len = 1,
		.is_locked = false
	},
	{
		.reg = TAS2781_PRM_IDLE_CH_DET_REG,
		.val = { 0x92 },
		.val_len = 1,
		.is_locked = false
	},
	{
		.reg	= TAS2781_PRM_PLT_FLAG_REG,
		.val = { 0x40, 0x00, 0x00, 0x00 },
		.val_len = 4,
		.is_locked = false
	},
	{
		.reg	= TAS2781_PRM_SINEGAIN_REG,
		.val_len = 4,
		.is_locked = false
	},
	{
		.reg	= TAS2781_PRM_SINEGAIN2_REG,
		.val_len = 4,
		.is_locked = false
	},
};

static const struct i2c_device_id tasdevice_id[] = {
	{ "tas2563", TAS2563 },
	{ "tas2781", TAS2781 },
	{}
};
MODULE_DEVICE_TABLE(i2c, tasdevice_id);

#ifdef CONFIG_OF
static const struct of_device_id tasdevice_of_match[] = {
	{ .compatible = "ti,tas2563" },
	{ .compatible = "ti,tas2781" },
	{},
};
MODULE_DEVICE_TABLE(of, tasdevice_of_match);
#endif

static const int tas2563_cali_data_reg[] = {
	TAS2563_PRM_R0_REG,
	TAS2563_PRM_R0_LOW_REG,
	TAS2563_PRM_INVR0_REG,
	TAS2563_PRM_POW_REG,
	TAS2563_PRM_TLIMIT_REG,
};

static const int tas2781_cali_data_reg[] = {
	TAS2781_PRM_R0_REG,
	TAS2781_PRM_R0_LOW_REG,
	TAS2781_PRM_INVR0_REG,
	TAS2781_PRM_POW_REG,
	TAS2781_PRM_TLIMIT_REG,
};

/**
 * tas2781_digital_getvol - get the volum control
 * @kcontrol: control pointer
 * @ucontrol: User data
 * Customer Kcontrol for tas2781 is primarily for regmap booking, paging
 * depends on internal regmap mechanism.
 * tas2781 contains book and page two-level register map, especially
 * book switching will set the register BXXP00R7F, after switching to the
 * correct book, then leverage the mechanism for paging to access the
 * register.
 */
static int tas2781_digital_getvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int rc;

	mutex_lock(&tas_priv->codec_lock);
	rc = tasdevice_digital_getvol(tas_priv, ucontrol, mc);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tas2781_digital_putvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int rc;

	mutex_lock(&tas_priv->codec_lock);
	rc = tasdevice_digital_putvol(tas_priv, ucontrol, mc);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tas2781_amp_getvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int rc;

	mutex_lock(&tas_priv->codec_lock);
	rc = tasdevice_amp_getvol(tas_priv, ucontrol, mc);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tas2781_amp_putvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv =
		snd_soc_component_get_drvdata(codec);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int rc;

	mutex_lock(&tas_priv->codec_lock);
	rc = tasdevice_amp_putvol(tas_priv, ucontrol, mc);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tasdev_force_fwload_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv =
		snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = (int)tas_priv->force_fwload_status;
	dev_dbg(tas_priv->dev, "%s : Force FWload %s\n", __func__,
			tas_priv->force_fwload_status ? "ON" : "OFF");

	return 0;
}

static int tasdev_force_fwload_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv =
		snd_soc_component_get_drvdata(component);
	bool change, val = (bool)ucontrol->value.integer.value[0];

	mutex_lock(&tas_priv->codec_lock);
	if (tas_priv->force_fwload_status == val)
		change = false;
	else {
		change = true;
		tas_priv->force_fwload_status = val;
	}
	mutex_unlock(&tas_priv->codec_lock);
	dev_dbg(tas_priv->dev, "%s : Force FWload %s\n", __func__,
		tas_priv->force_fwload_status ? "ON" : "OFF");

	return change;
}

static int tasdev_cali_data_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	unsigned char *dst = ucontrol->value.bytes.data;
	unsigned char *data = tas_priv->cali_data.data;

	mutex_lock(&tas_priv->codec_lock);
	if (!tas_priv->is_user_space_calidata ||
		tas_priv->cali_data.total_sz != bytes_ext->max) {
		goto out;
	}
	memcpy(dst, data, bytes_ext->max);
out:
	mutex_unlock(&tas_priv->codec_lock);
	return 0;
}

static int calib_data_get(struct tasdevice_priv *tas_priv, int reg,
	unsigned char *dst, int data_len)
{
	struct i2c_client *clt = (struct i2c_client *)tas_priv->client;
	struct tasdevice *tasdev = tas_priv->tasdevice;
	int rc = -1;
	int i;

	if (data_len != 4)
		return rc;

	for (i = 0; i < tas_priv->ndev; i++) {
		if (clt->addr == tasdev[i].dev_addr) {
			/* First byte is the device index. */
			dst[0] = i;
			tasdevice_dev_bulk_read(tas_priv, i, reg, &dst[1],
				4);
			rc = 0;
			break;
		}
	}

	return rc;
}

static int tas2781_calib_start_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	const int sum = ARRAY_SIZE(tas2781_cali_start_reg);
	unsigned char *dat = ucontrol->value.bytes.data;
	int rc = 1;
	int i, j;

	mutex_lock(&tas_priv->codec_lock);
	if (tas_priv->chip_id != TAS2781 &&
		bytes_ext->max != 8 * tas_priv->ndev) {
		rc = -1;
		goto out;
	}

	for (i = 0; i < tas_priv->ndev; i++) {
		struct tasdevice *tasdev = tas_priv->tasdevice;
		struct bulk_reg_val *p = tasdev[i].cali_data_restore;
		int k = i * 9;

		if (p == NULL)
			continue;

		for (j = 0; j < sum; j++) {
			if (p[j].val_len == 1) {
				if (p[j].is_locked)
					tasdevice_dev_write(tas_priv, i,
						TAS2781_TEST_UNLOCK_REG,
						TAS2781_TEST_PAGE_UNLOCK);
				tasdevice_dev_read(tas_priv, i, p[j].reg,
					(int *)&p[j].val[0]);
			} else
				tasdevice_dev_bulk_read(tas_priv, i, p[j].reg,
					p[j].val, 4);
		}

		for (j = 0; j < sum - 2; j++) {
			if (p[j].val_len == 1) {
				if (p[j].is_locked)
					tasdevice_dev_write(tas_priv, i,
						TAS2781_TEST_UNLOCK_REG,
						TAS2781_TEST_PAGE_UNLOCK);
				tasdevice_dev_write(tas_priv, i, p[j].reg,
					tas2781_cali_start_reg[j].val[0]);
			} else
				tasdevice_dev_bulk_write(tas_priv, i, p[j].reg,
					(unsigned char *)
					tas2781_cali_start_reg[j].val, 4);
		}

		if (dat[k] != i) {
			dev_err(tas_priv->dev,
				"%s: no cal-setting for dev %d\n", __func__,
				i);
			continue;
		}
		tasdevice_dev_bulk_write(tas_priv, i, p[j].reg,
			&dat[i * 9 + 1], 4);
		tasdevice_dev_bulk_write(tas_priv, i, p[j + 1].reg,
			&dat[i * 9 + 5], 4);
	}
out:
	mutex_unlock(&tas_priv->codec_lock);
	return rc;
}

static void tas2781_calib_stop_put(struct tasdevice_priv *tas_priv)
{
	const int sum = ARRAY_SIZE(tas2781_cali_start_reg);
	int i, j;

	for (i = 0; i < tas_priv->ndev; i++) {
		struct tasdevice *tasdev = tas_priv->tasdevice;
		struct bulk_reg_val *p = tasdev[i].cali_data_restore;

		if (p == NULL)
			continue;

		for (j = 0; j < sum; j++) {
			if (p[j].val_len == 1) {
				if (p[j].is_locked)
					tasdevice_dev_write(tas_priv, i,
						TAS2781_TEST_UNLOCK_REG,
						TAS2781_TEST_PAGE_UNLOCK);
				tasdevice_dev_write(tas_priv, i, p[j].reg,
					p[j].val[0]);
			} else
				tasdevice_dev_bulk_write(tas_priv, i, p[j].reg,
					p[j].val, 4);
		}
	}
}

static int tas2563_calib_start_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	const int sum = ARRAY_SIZE(tas2563_cali_start_reg);
	int rc = 1;
	int i, j;

	mutex_lock(&tas_priv->codec_lock);
	if (tas_priv->chip_id != TAS2563) {
		rc = -1;
		goto out;
	}

	for (i = 0; i < tas_priv->ndev; i++) {
		struct tasdevice *tasdev = tas_priv->tasdevice;
		struct bulk_reg_val *p = tasdev[i].cali_data_restore;

		if (p == NULL)
			continue;
		for (j = 0; j < sum; j++) {
			tasdevice_dev_bulk_read(tas_priv, i, p[j].reg,
				p[j].val, 4);
		}

		for (j = 0; j < sum; j++) {
			tasdevice_dev_bulk_write(tas_priv, i, p[j].reg,
				(unsigned char *)tas2563_cali_start_reg[j].val,
				4);
		}
	}
out:
	mutex_unlock(&tas_priv->codec_lock);
	return rc;
}

static void tas2563_calib_stop_put(struct tasdevice_priv *tas_priv)
{
	const int sum = ARRAY_SIZE(tas2563_cali_start_reg);
	int i, j;

	for (i = 0; i < tas_priv->ndev; i++) {
		struct tasdevice *tasdev = tas_priv->tasdevice;
		struct bulk_reg_val *p = tasdev[i].cali_data_restore;

		if (p == NULL)
			continue;

		for (j = 0; j < sum; j++) {
			tasdevice_dev_bulk_write(tas_priv, i, p[j].reg,
				p[j].val, 4);
		}
	}
}

static int tasdev_calib_stop_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);

	mutex_lock(&tas_priv->codec_lock);
	if (tas_priv->chip_id == TAS2563)
		tas2563_calib_stop_put(tas_priv);
	else
		tas2781_calib_stop_put(tas_priv);
	mutex_unlock(&tas_priv->codec_lock);

	return 1;
}

static int tasdev_cali_data_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	unsigned char *data = tas_priv->cali_data.data;
	int rc = 1;

	mutex_lock(&tas_priv->codec_lock);
	if (tas_priv->cali_data.total_sz != bytes_ext->max) {
		rc = -1;
		goto out;
	}
	tas_priv->is_user_space_calidata = true;
	memcpy(data, ucontrol->value.bytes.data, bytes_ext->max);
out:
	mutex_unlock(&tas_priv->codec_lock);
	return rc;
}

static int tas2781_latch_reg_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct i2c_client *clt = (struct i2c_client *)tas_priv->client;
	struct tasdevice *tasdev = tas_priv->tasdevice;
	unsigned char *dst = ucontrol->value.bytes.data;
	int i, val, rc = -1;

	mutex_lock(&tas_priv->codec_lock);
	for (i = 0; i < tas_priv->ndev; i++) {
		if (clt->addr == tasdev[i].dev_addr) {
			/* First byte is the device index. */
			dst[0] = i;
			tasdevice_dev_read(tas_priv, i,
				TAS2781_RUNTIME_LATCH_RE_REG, &val);
			dst[1] = val;
			rc = 0;
			break;
		}
	}
	mutex_unlock(&tas_priv->codec_lock);
	return rc;
}

static int tasdev_tf_data_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	unsigned char *dst = ucontrol->value.bytes.data;
	unsigned int reg;
	int rc = -1;

	if (tas_priv->chip_id == TAS2781)
		reg = TAS2781_RUNTIME_RE_REG_TF;
	else
		reg = TAS2563_RUNTIME_RE_REG_TF;

	mutex_lock(&tas_priv->codec_lock);
	rc = calib_data_get(tas_priv, reg, dst, bytes_ext->max - 1);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tasdev_re_data_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	unsigned char *dst = ucontrol->value.bytes.data;
	unsigned int reg;
	int rc = -1;

	if (tas_priv->chip_id == TAS2781)
		reg = TAS2781_RUNTIME_RE_REG;
	else
		reg = TAS2563_RUNTIME_RE_REG;
	mutex_lock(&tas_priv->codec_lock);
	rc = calib_data_get(tas_priv, reg, dst, bytes_ext->max - 1);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tasdev_r0_data_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	unsigned char *dst = ucontrol->value.bytes.data;
	unsigned int reg;
	int rc = -1;

	if (tas_priv->chip_id == TAS2781)
		reg = TAS2781_PRM_R0_REG;
	else
		reg = TAS2563_PRM_R0_REG;
	mutex_lock(&tas_priv->codec_lock);
	rc = calib_data_get(tas_priv, reg, dst, bytes_ext->max - 1);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tasdev_XMA1_data_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	unsigned char *dst = ucontrol->value.bytes.data;
	unsigned int reg = TASDEVICE_XM_A1_REG;
	int rc = -1;

	mutex_lock(&tas_priv->codec_lock);
	rc = calib_data_get(tas_priv, reg, dst, bytes_ext->max - 1);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tasdev_XMA2_data_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(comp);
	struct soc_bytes_ext *bytes_ext =
		(struct soc_bytes_ext *) kcontrol->private_value;
	unsigned char *dst = ucontrol->value.bytes.data;
	unsigned int reg = TASDEVICE_XM_A2_REG;
	int rc = -1;

	mutex_lock(&tas_priv->codec_lock);
	rc = calib_data_get(tas_priv, reg, dst, bytes_ext->max - 1);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tasdev_nop_get(
	struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int tas2563_digital_gain_get(
	struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_dev = snd_soc_component_get_drvdata(codec);
	unsigned int l = 0, r = mc->max;
	unsigned int target, ar_mid, mid, ar_l, ar_r;
	unsigned int reg = mc->reg;
	unsigned char data[4];
	int ret;

	mutex_lock(&tas_dev->codec_lock);
	/* Read the primary device */
	ret =  tasdevice_dev_bulk_read(tas_dev, 0, reg, data, 4);
	if (ret) {
		dev_err(tas_dev->dev, "%s, get AMP vol error\n", __func__);
		goto out;
	}

	target = get_unaligned_be32(&data[0]);

	while (r > 1 + l) {
		mid = (l + r) / 2;
		ar_mid = get_unaligned_be32(tas2563_dvc_table[mid]);
		if (target < ar_mid)
			r = mid;
		else
			l = mid;
	}

	ar_l = get_unaligned_be32(tas2563_dvc_table[l]);
	ar_r = get_unaligned_be32(tas2563_dvc_table[r]);

	ucontrol->value.integer.value[0] =
		abs(target - ar_l) <= abs(target - ar_r) ? l : r;
out:
	mutex_unlock(&tas_dev->codec_lock);
	return 0;
}

static int tas2563_digital_gain_put(
	struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_dev = snd_soc_component_get_drvdata(codec);
	unsigned int reg = mc->reg;
	unsigned int volrd, volwr;
	int vol = ucontrol->value.integer.value[0];
	int max = mc->max, i, ret = 1;
	unsigned char data[4];

	vol = clamp(vol, 0, max);
	mutex_lock(&tas_dev->codec_lock);
	/* Read the primary device */
	ret =  tasdevice_dev_bulk_read(tas_dev, 0, reg, data, 4);
	if (ret) {
		dev_err(tas_dev->dev, "%s, get AMP vol error\n", __func__);
		goto out;
	}

	volrd = get_unaligned_be32(&data[0]);
	volwr = get_unaligned_be32(tas2563_dvc_table[vol]);

	if (volrd == volwr) {
		ret = 0;
		goto out;
	}

	for (i = 0; i < tas_dev->ndev; i++) {
		ret = tasdevice_dev_bulk_write(tas_dev, i, reg,
			(unsigned char *)tas2563_dvc_table[vol], 4);
		if (ret)
			dev_err(tas_dev->dev,
				"%s, set digital vol error in device %d\n",
				__func__, i);
	}

out:
	mutex_unlock(&tas_dev->codec_lock);
	return ret;
}

static const struct snd_kcontrol_new tasdevice_snd_controls[] = {
	SOC_SINGLE_BOOL_EXT("Speaker Force Firmware Load", 0,
		tasdev_force_fwload_get, tasdev_force_fwload_put),
};

static const struct snd_kcontrol_new tasdevice_cali_controls[] = {
	SOC_SINGLE_EXT("Calibration Stop", SND_SOC_NOPM, 0, 1, 0,
		tasdev_nop_get, tasdev_calib_stop_put),
	SND_SOC_BYTES_EXT("Amp TF Data", 5, tasdev_tf_data_get, NULL),
	SND_SOC_BYTES_EXT("Amp RE Data", 5, tasdev_re_data_get, NULL),
	SND_SOC_BYTES_EXT("Amp R0 Data", 5, tasdev_r0_data_get, NULL),
	SND_SOC_BYTES_EXT("Amp XMA1 Data", 5, tasdev_XMA1_data_get, NULL),
	SND_SOC_BYTES_EXT("Amp XMA2 Data", 5, tasdev_XMA2_data_get, NULL),
};

static const struct snd_kcontrol_new tas2781_snd_controls[] = {
	SOC_SINGLE_RANGE_EXT_TLV("Speaker Analog Gain", TAS2781_AMP_LEVEL,
		1, 0, 20, 0, tas2781_amp_getvol,
		tas2781_amp_putvol, amp_vol_tlv),
	SOC_SINGLE_RANGE_EXT_TLV("Speaker Digital Gain", TAS2781_DVC_LVL,
		0, 0, 200, 1, tas2781_digital_getvol,
		tas2781_digital_putvol, dvc_tlv),
};

static const struct snd_kcontrol_new tas2781_cali_controls[] = {
	SND_SOC_BYTES_EXT("Amp Latch Data", 2, tas2781_latch_reg_get, NULL),
};

static const struct snd_kcontrol_new tas2563_snd_controls[] = {
	SOC_SINGLE_RANGE_EXT_TLV("Speaker Digital Gain", TAS2563_DVC_LVL, 0,
		0, ARRAY_SIZE(tas2563_dvc_table) - 1, 0,
		tas2563_digital_gain_get, tas2563_digital_gain_put,
		tas2563_dvc_tlv),
};

static const struct snd_kcontrol_new tas2563_cali_controls[] = {
	SOC_SINGLE_EXT("Calibration Start", SND_SOC_NOPM, 0, 1, 0,
		tasdev_nop_get, tas2563_calib_start_put),
};

static int tasdevice_set_profile_id(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	int ret = 0;

	if (tas_priv->rcabin.profile_cfg_id !=
		ucontrol->value.integer.value[0]) {
		tas_priv->rcabin.profile_cfg_id =
			ucontrol->value.integer.value[0];
		ret = 1;
	}

	return ret;
}

static int tasdevice_info_active_id(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = (int)tas_priv->ndev - 1;

	return 0;
}

static int tasdevice_info_chip_id(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = TAS2563;
	uinfo->value.integer.max = TAS2781;

	return 0;
}

static int tasdevice_info_programs(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	struct tasdevice_fw *tas_fw = tas_priv->fmw;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = (int)tas_fw->nr_programs;

	return 0;
}

static int tasdevice_info_configurations(
	struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *codec =
		snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	struct tasdevice_fw *tas_fw = tas_priv->fmw;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = (int)tas_fw->nr_configurations - 1;

	return 0;
}

static int tasdevice_info_profile(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = tas_priv->rcabin.ncfgs - 1;

	return 0;
}

static int tasdevice_get_profile_id(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);

	ucontrol->value.integer.value[0] = tas_priv->rcabin.profile_cfg_id;

	return 0;
}

static int tasdevice_get_chip_id(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);

	ucontrol->value.integer.value[0] = tas_priv->chip_id;

	return 0;
}

static int tasdevice_create_control(struct tasdevice_priv *tas_priv)
{
	struct snd_kcontrol_new *prof_ctrls;
	int nr_controls = 1;
	int mix_index = 0;
	int ret;
	char *name;

	prof_ctrls = devm_kcalloc(tas_priv->dev, nr_controls,
		sizeof(prof_ctrls[0]), GFP_KERNEL);
	if (!prof_ctrls) {
		ret = -ENOMEM;
		goto out;
	}

	/* Create a mixer item for selecting the active profile */
	name = devm_kzalloc(tas_priv->dev, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto out;
	}
	scnprintf(name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "Speaker Profile Id");
	prof_ctrls[mix_index].name = name;
	prof_ctrls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	prof_ctrls[mix_index].info = tasdevice_info_profile;
	prof_ctrls[mix_index].get = tasdevice_get_profile_id;
	prof_ctrls[mix_index].put = tasdevice_set_profile_id;
	mix_index++;

	ret = snd_soc_add_component_controls(tas_priv->codec,
		prof_ctrls, nr_controls < mix_index ? nr_controls : mix_index);

out:
	return ret;
}

static int tasdevice_program_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);

	ucontrol->value.integer.value[0] = tas_priv->cur_prog;

	return 0;
}

static int tasdevice_program_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	unsigned int nr_program = ucontrol->value.integer.value[0];
	int ret = 0;

	if (tas_priv->cur_prog != nr_program) {
		tas_priv->cur_prog = nr_program;
		ret = 1;
	}

	return ret;
}

static int tasdevice_configuration_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{

	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);

	ucontrol->value.integer.value[0] = tas_priv->cur_conf;

	return 0;
}

static int tasdevice_configuration_put(
	struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	unsigned int nr_configuration = ucontrol->value.integer.value[0];
	int ret = 0;

	if (tas_priv->cur_conf != nr_configuration) {
		tas_priv->cur_conf = nr_configuration;
		ret = 1;
	}

	return ret;
}

static int tasdevice_active_id_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	struct i2c_client *clt = (struct i2c_client *)tas_priv->client;
	struct tasdevice *tasdev = tas_priv->tasdevice;
	int i;

	for (i = 0; i < tas_priv->ndev; i++) {
		if (clt->addr == tasdev[i].dev_addr) {
			ucontrol->value.integer.value[0] = i;
			return 0;
		}
	}

	return -1;
}

static int tasdevice_active_id_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	int nr_dev = ucontrol->value.integer.value[0];
	int max = tas_priv->ndev - 1, rc;

	nr_dev = clamp(nr_dev, 0, max);

	mutex_lock(&tas_priv->codec_lock);
	rc = tasdevice_chn_switch(tas_priv, nr_dev);
	mutex_unlock(&tas_priv->codec_lock);

	return rc;
}

static int tasdevice_dsp_create_ctrls(struct tasdevice_priv *tas_priv)
{
	char *active_dev_name, *cali_name, *chip_id, *conf_name, *prog_name;
	struct calidata *cali_data = &tas_priv->cali_data;
	struct snd_kcontrol_new *dsp_ctrls;
	struct soc_bytes_ext *ext_cali_data;
	int nr_controls = 5;
	int mix_index = 0;
	int ret;

	if (tas_priv->chip_id == TAS2781)
		nr_controls++;
	/* Alloc kcontrol via devm_kzalloc, which don't manually
	 * free the kcontrol
	 */
	dsp_ctrls = devm_kcalloc(tas_priv->dev, nr_controls,
		sizeof(dsp_ctrls[0]), GFP_KERNEL);
	if (!dsp_ctrls) {
		ret = -ENOMEM;
		goto out;
	}

	/* Create a mixer item for selecting the active profile */
	active_dev_name = devm_kzalloc(tas_priv->dev,
		SNDRV_CTL_ELEM_ID_NAME_MAXLEN, GFP_KERNEL);
	cali_name = devm_kzalloc(tas_priv->dev, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		GFP_KERNEL);
	chip_id = devm_kzalloc(tas_priv->dev, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		GFP_KERNEL);
	conf_name = devm_kzalloc(tas_priv->dev,
		SNDRV_CTL_ELEM_ID_NAME_MAXLEN, GFP_KERNEL);
	prog_name = devm_kzalloc(tas_priv->dev, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		GFP_KERNEL);
	ext_cali_data = devm_kzalloc(tas_priv->dev, sizeof(*ext_cali_data),
		GFP_KERNEL);
	if (!active_dev_name || !cali_name || !conf_name || !chip_id ||
		!ext_cali_data || !prog_name) {
		ret = -ENOMEM;
		goto out;
	}

	scnprintf(prog_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		"Speaker Program Id");
	dsp_ctrls[mix_index].name = prog_name;
	dsp_ctrls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	dsp_ctrls[mix_index].info = tasdevice_info_programs;
	dsp_ctrls[mix_index].get = tasdevice_program_get;
	dsp_ctrls[mix_index].put = tasdevice_program_put;
	mix_index++;

	scnprintf(conf_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		"Speaker Config Id");
	dsp_ctrls[mix_index].name = conf_name;
	dsp_ctrls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	dsp_ctrls[mix_index].info = tasdevice_info_configurations;
	dsp_ctrls[mix_index].get = tasdevice_configuration_get;
	dsp_ctrls[mix_index].put = tasdevice_configuration_put;
	mix_index++;

	scnprintf(active_dev_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		"Activate Tasdevice Id");
	dsp_ctrls[mix_index].name = active_dev_name;
	dsp_ctrls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	dsp_ctrls[mix_index].info = tasdevice_info_active_id;
	dsp_ctrls[mix_index].get = tasdevice_active_id_get;
	dsp_ctrls[mix_index].put = tasdevice_active_id_put;
	mix_index++;

	scnprintf(chip_id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		"Tasdevice Chip Id");
	dsp_ctrls[mix_index].name = chip_id;
	dsp_ctrls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	dsp_ctrls[mix_index].info = tasdevice_info_chip_id;
	dsp_ctrls[mix_index].get = tasdevice_get_chip_id;
	mix_index++;

	scnprintf(cali_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
		"Speaker Calibrated Data");
	ext_cali_data->max = tas_priv->ndev * CAL_DAT_SZ;
	tas_priv->cali_data.total_sz = ext_cali_data->max;
	tas_priv->cali_data.data = devm_kzalloc(tas_priv->dev,
		ext_cali_data->max, GFP_KERNEL);
	dsp_ctrls[mix_index].name = cali_name;
	dsp_ctrls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	dsp_ctrls[mix_index].info = snd_soc_bytes_info_ext;
	dsp_ctrls[mix_index].get = tasdev_cali_data_get;
	dsp_ctrls[mix_index].put = tasdev_cali_data_put;
	dsp_ctrls[mix_index].private_value = (unsigned long)ext_cali_data;
	mix_index++;

	cali_data->data = devm_kzalloc(tas_priv->dev, tas_priv->ndev *
		(cali_data->reg_array_sz * 4 + 1), GFP_KERNEL);
	if (!cali_data->data)
		return -ENOMEM;

	if (tas_priv->chip_id == TAS2781) {
		struct soc_bytes_ext *ext_cali_start;
		char *cali_start_name;

		cali_start_name = devm_kzalloc(tas_priv->dev,
			SNDRV_CTL_ELEM_ID_NAME_MAXLEN, GFP_KERNEL);
		ext_cali_start = devm_kzalloc(tas_priv->dev,
			sizeof(*ext_cali_start), GFP_KERNEL);
		if (!cali_start_name || !ext_cali_start) {
			ret = -ENOMEM;
			goto out;
		}

		scnprintf(cali_start_name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN,
			"Calibration Start");
		ext_cali_data->max = tas_priv->ndev * 9;
		dsp_ctrls[mix_index].name = cali_start_name;
		dsp_ctrls[mix_index].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		dsp_ctrls[mix_index].info = snd_soc_bytes_info_ext;
		dsp_ctrls[mix_index].put = tas2781_calib_start_put;
		dsp_ctrls[mix_index].get = tasdev_nop_get;
		dsp_ctrls[mix_index].private_value =
			(unsigned long)ext_cali_start;
		mix_index++;
	}

	ret = snd_soc_add_component_controls(tas_priv->codec, dsp_ctrls,
		nr_controls < mix_index ? nr_controls : mix_index);

out:
	return ret;
}

static int tasdevice_create_cali_ctrls(struct tasdevice_priv *tas_priv)
{
	struct calidata *cali_data = &tas_priv->cali_data;
	struct snd_kcontrol_new *cali_ctrls;
	unsigned int num_controls;
	int rc;

	rc = snd_soc_add_component_controls(tas_priv->codec,
		tasdevice_cali_controls, ARRAY_SIZE(tasdevice_cali_controls));
	if (rc < 0) {
		dev_err(tas_priv->dev, "%s: Add cali control err rc = %d",
			__func__, rc);
		return rc;
	}

	if (tas_priv->chip_id == TAS2781) {
		cali_ctrls = (struct snd_kcontrol_new *)tas2781_cali_controls;
		num_controls = ARRAY_SIZE(tas2781_cali_controls);
		cali_data->reg_array = (unsigned int *)tas2781_cali_data_reg;
		cali_data->reg_array_sz = ARRAY_SIZE(tas2781_cali_data_reg);
	} else {
		struct tasdevice *tasdev = tas_priv->tasdevice;
		int i;

		cali_ctrls = (struct snd_kcontrol_new *)tas2563_cali_controls;
		num_controls = ARRAY_SIZE(tas2563_cali_controls);
		cali_data->reg_array = (unsigned int *)tas2563_cali_data_reg;
		cali_data->reg_array_sz = sizeof(tas2563_cali_data_reg);
		for (i = 0; i < tas_priv->ndev; i++) {
			tasdev[i].cali_data_restore =
				kmemdup(tas2563_cali_start_reg,
				sizeof(tas2563_cali_start_reg), GFP_KERNEL);
			if (!tasdev[i].cali_data_restore)
				return -ENOMEM;
		}
	}

	rc = snd_soc_add_component_controls(tas_priv->codec, cali_ctrls,
		num_controls);
	if (rc < 0) {
		dev_err(tas_priv->dev, "%s: Add control err rc = %d",
			__func__, rc);
		return rc;
	}

	return rc;
}

static void tasdevice_fw_ready(const struct firmware *fmw,
	void *context)
{
	struct tasdevice_priv *tas_priv = context;
	int ret = 0;
	int i;

	mutex_lock(&tas_priv->codec_lock);

	ret = tasdevice_rca_parser(tas_priv, fmw);
	if (ret) {
		tasdevice_config_info_remove(tas_priv);
		goto out;
	}
	tasdevice_create_control(tas_priv);

	tasdevice_dsp_remove(tas_priv);
	tasdevice_calbin_remove(tas_priv);
	tas_priv->fw_state = TASDEVICE_RCA_FW_OK;
	scnprintf(tas_priv->coef_binaryname, 64, "%s_coef.bin",
		tas_priv->dev_name);

	ret = tasdevice_dsp_parser(tas_priv);
	if (ret) {
		dev_err(tas_priv->dev, "dspfw load %s error\n",
			tas_priv->coef_binaryname);
		goto out;
	}

	ret = tasdevice_dsp_create_ctrls(tas_priv);
	if (ret) {
		dev_err(tas_priv->dev, "dsp controls error\n");
		goto out;
	}

	ret = tasdevice_create_cali_ctrls(tas_priv);
	if (ret) {
		dev_err(tas_priv->dev, "cali controls error\n");
		goto out;
	}

	tas_priv->fw_state = TASDEVICE_DSP_FW_ALL_OK;

	/* If calibrated data occurs error, dsp will still works with default
	 * calibrated data inside algo.
	 */
	for (i = 0; i < tas_priv->ndev; i++) {
		scnprintf(tas_priv->cal_binaryname[i], 64, "%s_cal_0x%02x.bin",
			tas_priv->dev_name, tas_priv->tasdevice[i].dev_addr);
		ret = tas2781_load_calibration(tas_priv,
			tas_priv->cal_binaryname[i], i);
		if (ret != 0)
			dev_err(tas_priv->dev,
				"%s: load %s error, default will effect\n",
				__func__, tas_priv->cal_binaryname[i]);
	}

	tasdevice_prmg_load(tas_priv, 0);
	tas_priv->cur_prog = 0;
out:
	if (tas_priv->fw_state == TASDEVICE_RCA_FW_OK) {
		/*If DSP FW fail, DSP kcontrol won't be created */
		tasdevice_dsp_remove(tas_priv);
	}
	mutex_unlock(&tas_priv->codec_lock);
	if (fmw)
		release_firmware(fmw);
}

static int tasdevice_dapm_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *codec = snd_soc_dapm_to_component(w->dapm);
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	int state = 0;

	/* Codec Lock Hold */
	mutex_lock(&tas_priv->codec_lock);
	if (event == SND_SOC_DAPM_PRE_PMD)
		state = 1;
	tasdevice_tuning_switch(tas_priv, state);
	/* Codec Lock Release*/
	mutex_unlock(&tas_priv->codec_lock);

	return 0;
}

static const struct snd_soc_dapm_widget tasdevice_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("ASI", "ASI Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT_E("ASI OUT", "ASI Capture", 0, SND_SOC_NOPM,
		0, 0, tasdevice_dapm_event,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SPK("SPK", tasdevice_dapm_event),
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_INPUT("DMIC")
};

static const struct snd_soc_dapm_route tasdevice_audio_map[] = {
	{"SPK", NULL, "ASI"},
	{"OUT", NULL, "SPK"},
	{"ASI OUT", NULL, "DMIC"}
};

static int tasdevice_startup(struct snd_pcm_substream *substream,
						struct snd_soc_dai *dai)
{
	struct snd_soc_component *codec = dai->component;
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);

	if (!(tas_priv->fw_state == TASDEVICE_DSP_FW_ALL_OK ||
		tas_priv->fw_state == TASDEVICE_RCA_FW_OK)) {
		dev_err(tas_priv->dev, "Bin file not loaded\n");
		return -EINVAL;
	}

	return 0;
}

static int tasdevice_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct tasdevice_priv *tas_priv = snd_soc_dai_get_drvdata(dai);
	unsigned int slot_width;
	unsigned int fsrate;
	int bclk_rate;
	int rc = 0;

	fsrate = params_rate(params);
	switch (fsrate) {
	case 48000:
	case 44100:
		break;
	default:
		dev_err(tas_priv->dev, "%s: incorrect sample rate = %u\n",
			__func__, fsrate);
		rc = -EINVAL;
		goto out;
	}

	slot_width = params_width(params);
	switch (slot_width) {
	case 16:
	case 20:
	case 24:
	case 32:
		break;
	default:
		dev_err(tas_priv->dev, "%s: incorrect slot width = %u\n",
			__func__, slot_width);
		rc = -EINVAL;
		goto out;
	}

	bclk_rate = snd_soc_params_to_bclk(params);
	if (bclk_rate < 0) {
		dev_err(tas_priv->dev, "%s: incorrect bclk rate = %d\n",
			__func__, bclk_rate);
		rc = bclk_rate;
		goto out;
	}

out:
	return rc;
}

static int tasdevice_set_dai_sysclk(struct snd_soc_dai *codec_dai,
	int clk_id, unsigned int freq, int dir)
{
	struct tasdevice_priv *tas_priv = snd_soc_dai_get_drvdata(codec_dai);

	tas_priv->sysclk = freq;

	return 0;
}

static const struct snd_soc_dai_ops tasdevice_dai_ops = {
	.startup = tasdevice_startup,
	.hw_params = tasdevice_hw_params,
	.set_sysclk = tasdevice_set_dai_sysclk,
};

static struct snd_soc_dai_driver tasdevice_dai_driver[] = {
	{
		.name = "tasdev_codec",
		.id = 0,
		.playback = {
			.stream_name = "Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates	 = TASDEVICE_RATES,
			.formats	= TASDEVICE_FORMATS,
		},
		.capture = {
			.stream_name = "Capture",
			.channels_min = 1,
			.channels_max = 4,
			.rates	 = TASDEVICE_RATES,
			.formats	= TASDEVICE_FORMATS,
		},
		.ops = &tasdevice_dai_ops,
		.symmetric_rate = 1,
	},
};

static int tasdevice_codec_probe(struct snd_soc_component *codec)
{
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);
	int rc;

	if (tas_priv->chip_id == TAS2781) {
		rc = snd_soc_add_component_controls(codec,
			tas2781_snd_controls,
			ARRAY_SIZE(tas2781_snd_controls));
		if (rc < 0) {
			dev_err(tas_priv->dev, "%s: Add control err rc = %d",
				__func__, rc);
			return rc;
		}
	} else {
		rc = snd_soc_add_component_controls(codec,
			tas2563_snd_controls,
			ARRAY_SIZE(tas2563_snd_controls));
		if (rc < 0) {
			dev_err(tas_priv->dev, "%s: Add control err rc = %d",
				__func__, rc);
			return rc;
		}
	}

	rc = tascodec_init(tas_priv, codec, THIS_MODULE, tasdevice_fw_ready);

	return rc;
}

static void tasdevice_deinit(void *context)
{
	struct tasdevice_priv *tas_priv = (struct tasdevice_priv *) context;
	struct tasdevice *tasdev = tas_priv->tasdevice;
	int i;

	for (i = 0; i < tas_priv->ndev; i++)
		kfree(tasdev[i].cali_data_restore);

	tasdevice_config_info_remove(tas_priv);
	tasdevice_dsp_remove(tas_priv);
	tasdevice_calbin_remove(tas_priv);
	tas_priv->fw_state = TASDEVICE_DSP_FW_PENDING;
}

static void tasdevice_codec_remove(
	struct snd_soc_component *codec)
{
	struct tasdevice_priv *tas_priv = snd_soc_component_get_drvdata(codec);

	tasdevice_deinit(tas_priv);
}

static const struct snd_soc_component_driver
	soc_codec_driver_tasdevice = {
	.probe			= tasdevice_codec_probe,
	.remove			= tasdevice_codec_remove,
	.controls		= tasdevice_snd_controls,
	.num_controls		= ARRAY_SIZE(tasdevice_snd_controls),
	.dapm_widgets		= tasdevice_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tasdevice_dapm_widgets),
	.dapm_routes		= tasdevice_audio_map,
	.num_dapm_routes	= ARRAY_SIZE(tasdevice_audio_map),
	.idle_bias_on		= 1,
	.endianness		= 1,
};

static void tasdevice_parse_dt(struct tasdevice_priv *tas_priv)
{
	struct i2c_client *client = (struct i2c_client *)tas_priv->client;
	unsigned int dev_addrs[TASDEVICE_MAX_CHANNELS];
	int rc, i, ndev = 0;

	if (tas_priv->isacpi) {
		ndev = device_property_read_u32_array(&client->dev,
			"ti,audio-slots", NULL, 0);
		if (ndev <= 0) {
			ndev = 1;
			dev_addrs[0] = client->addr;
		} else {
			ndev = (ndev < ARRAY_SIZE(dev_addrs))
				? ndev : ARRAY_SIZE(dev_addrs);
			ndev = device_property_read_u32_array(&client->dev,
				"ti,audio-slots", dev_addrs, ndev);
		}

		tas_priv->irq_info.irq_gpio =
			acpi_dev_gpio_irq_get(ACPI_COMPANION(&client->dev), 0);
	} else {
		struct device_node *np = tas_priv->dev->of_node;
#ifdef CONFIG_OF
		const __be32 *reg, *reg_end;
		int len, sw, aw;

		aw = of_n_addr_cells(np);
		sw = of_n_size_cells(np);
		if (sw == 0) {
			reg = (const __be32 *)of_get_property(np,
				"reg", &len);
			reg_end = reg + len/sizeof(*reg);
			ndev = 0;
			do {
				dev_addrs[ndev] = of_read_number(reg, aw);
				reg += aw;
				ndev++;
			} while (reg < reg_end);
		} else {
			ndev = 1;
			dev_addrs[0] = client->addr;
		}
#else
		ndev = 1;
		dev_addrs[0] = client->addr;
#endif
		tas_priv->irq_info.irq_gpio = of_irq_get(np, 0);
	}
	tas_priv->ndev = ndev;
	for (i = 0; i < ndev; i++)
		tas_priv->tasdevice[i].dev_addr = dev_addrs[i];

	tas_priv->reset = devm_gpiod_get_optional(&client->dev,
			"reset-gpios", GPIOD_OUT_HIGH);
	if (IS_ERR(tas_priv->reset))
		dev_err(tas_priv->dev, "%s Can't get reset GPIO\n",
			__func__);

	strcpy(tas_priv->dev_name, tasdevice_id[tas_priv->chip_id].name);

	if (gpio_is_valid(tas_priv->irq_info.irq_gpio)) {
		rc = gpio_request(tas_priv->irq_info.irq_gpio,
				"AUDEV-IRQ");
		if (!rc) {
			gpio_direction_input(
				tas_priv->irq_info.irq_gpio);

			tas_priv->irq_info.irq =
				gpio_to_irq(tas_priv->irq_info.irq_gpio);
		} else
			dev_err(tas_priv->dev, "%s: GPIO %d request error\n",
				__func__, tas_priv->irq_info.irq_gpio);
	} else
		dev_err(tas_priv->dev,
			"Looking up irq-gpio property failed %d\n",
			tas_priv->irq_info.irq_gpio);
}

static int tasdevice_i2c_probe(struct i2c_client *i2c)
{
	const struct i2c_device_id *id = i2c_match_id(tasdevice_id, i2c);
	const struct acpi_device_id *acpi_id;
	struct tasdevice_priv *tas_priv;
	int ret;

	tas_priv = tasdevice_kzalloc(i2c);
	if (!tas_priv)
		return -ENOMEM;

	dev_set_drvdata(&i2c->dev, tas_priv);

	if (ACPI_HANDLE(&i2c->dev)) {
		acpi_id = acpi_match_device(i2c->dev.driver->acpi_match_table,
				&i2c->dev);
		if (!acpi_id) {
			dev_err(&i2c->dev, "No driver data\n");
			ret = -EINVAL;
			goto err;
		}
		tas_priv->chip_id = acpi_id->driver_data;
		tas_priv->isacpi = true;
	} else {
		tas_priv->chip_id = id ? id->driver_data : 0;
		tas_priv->isacpi = false;
	}

	tasdevice_parse_dt(tas_priv);

	ret = tasdevice_init(tas_priv);
	if (ret)
		goto err;

	ret = devm_snd_soc_register_component(tas_priv->dev,
		&soc_codec_driver_tasdevice,
		tasdevice_dai_driver, ARRAY_SIZE(tasdevice_dai_driver));
	if (ret) {
		dev_err(tas_priv->dev, "%s: codec register error:0x%08x\n",
			__func__, ret);
		goto err;
	}
err:
	if (ret < 0)
		tasdevice_remove(tas_priv);
	return ret;
}

static void tasdevice_i2c_remove(struct i2c_client *client)
{
	struct tasdevice_priv *tas_priv = i2c_get_clientdata(client);

	tasdevice_remove(tas_priv);
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id tasdevice_acpi_match[] = {
	{ "TAS2781", TAS2781 },
	{},
};

MODULE_DEVICE_TABLE(acpi, tasdevice_acpi_match);
#endif

static struct i2c_driver tasdevice_i2c_driver = {
	.driver = {
		.name = "tas2781-codec",
		.of_match_table = of_match_ptr(tasdevice_of_match),
#ifdef CONFIG_ACPI
		.acpi_match_table = ACPI_PTR(tasdevice_acpi_match),
#endif
	},
	.probe	= tasdevice_i2c_probe,
	.remove = tasdevice_i2c_remove,
	.id_table = tasdevice_id,
};

module_i2c_driver(tasdevice_i2c_driver);

MODULE_AUTHOR("Shenghao Ding <shenghao-ding@ti.com>");
MODULE_AUTHOR("Kevin Lu <kevin-lu@ti.com>");
MODULE_DESCRIPTION("ASoC TAS2781 Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(SND_SOC_TAS2781_FMWLIB);
