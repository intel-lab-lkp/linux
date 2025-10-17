// SPDX-License-Identifier: GPL-2.0-only
//
// aw88166.c --  ALSA SoC AW88166 codec support
//
// Copyright (c) 2025 AWINIC Technology CO., LTD
//
// Author: Weidong Wang <wangweidong.a@awinic.com>
//

#include <linux/crc32.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include "aw88166.h"
#include "aw-common-device.h"
#include "aw-common-firmware.h"

struct aw88166 {
	struct aw_device *aw_pa;
	struct mutex lock;
	struct gpio_desc *reset_gpio;
	struct delayed_work start_work;
	struct regmap *regmap;
	struct aw_container *aw_cfg;

	unsigned int check_val;
	unsigned int crc_init_val;
	unsigned int vcalb_init_val;
	unsigned int re_init_val;
	unsigned int dither_st;
	bool phase_sync;
};

static const struct regmap_config aw88166_remap_config = {
	.val_bits = 16,
	.reg_bits = 8,
	.max_register = AW88166_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static int aw_dev_get_iis_status(struct aw_device *aw_dev)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW88166_SYSST_REG, &reg_val);
	if (ret)
		return ret;
	if ((reg_val & AW88166_BIT_PLL_CHECK) != AW88166_BIT_PLL_CHECK) {
		dev_err(aw_dev->dev, "check pll lock fail, reg_val:0x%04x", reg_val);
		return -EINVAL;
	}

	return 0;
}

static int aw_dev_check_mode1_pll(struct aw_device *aw_dev)
{
	int ret, i;

	for (i = 0; i < AW88166_DEV_SYSST_CHECK_MAX; i++) {
		ret = aw_dev_get_iis_status(aw_dev);
		if (ret) {
			dev_err(aw_dev->dev, "mode1 iis signal check error");
			usleep_range(AW_2000_US, AW_2000_US + 10);
		} else {
			return 0;
		}
	}

	return -EPERM;
}

static int aw_dev_check_mode2_pll(struct aw_device *aw_dev)
{
	unsigned int reg_val;
	int ret, i;

	ret = regmap_read(aw_dev->regmap, AW88166_PLLCTRL2_REG, &reg_val);
	if (ret)
		return ret;

	reg_val &= (~AW88166_CCO_MUX_MASK);
	if (reg_val == AW88166_CCO_MUX_DIVIDED_VALUE) {
		dev_dbg(aw_dev->dev, "CCO_MUX is already divider");
		return -EPERM;
	}

	/* change mode2 */
	ret = regmap_update_bits(aw_dev->regmap, AW88166_PLLCTRL2_REG,
			~AW88166_CCO_MUX_MASK, AW88166_CCO_MUX_DIVIDED_VALUE);
	if (ret)
		return ret;

	for (i = 0; i < AW88166_DEV_SYSST_CHECK_MAX; i++) {
		ret = aw_dev_get_iis_status(aw_dev);
		if (ret) {
			dev_err(aw_dev->dev, "mode2 iis signal check error");
			usleep_range(AW_2000_US, AW_2000_US + 10);
		} else {
			break;
		}
	}

	/* change mode1 */
	regmap_update_bits(aw_dev->regmap, AW88166_PLLCTRL2_REG,
			~AW88166_CCO_MUX_MASK, AW88166_CCO_MUX_BYPASS_VALUE);
	if (ret == 0) {
		usleep_range(AW_2000_US, AW_2000_US + 10);
		for (i = 0; i < AW88166_DEV_SYSST_CHECK_MAX; i++) {
			ret = aw_dev_get_iis_status(aw_dev);
			if (ret) {
				dev_err(aw_dev->dev, "mode2 switch to mode1, iis signal check error");
				usleep_range(AW_2000_US, AW_2000_US + 10);
			} else {
				break;
			}
		}
	}

	return ret;
}

static int aw_dev_check_syspll(struct aw_device *aw_dev)
{
	int ret;

	ret = aw_dev_check_mode1_pll(aw_dev);
	if (ret) {
		dev_dbg(aw_dev->dev, "mode1 check iis failed try switch to mode2 check");
		ret = aw_dev_check_mode2_pll(aw_dev);
		if (ret) {
			dev_err(aw_dev->dev, "mode2 check iis failed");
			return ret;
		}
	}

	return 0;
}

static int aw_dev_check_sysst(struct aw_device *aw_dev)
{
	unsigned int check_val;
	unsigned int reg_val;
	int ret, i;

	ret = regmap_read(aw_dev->regmap, AW88166_PWMCTRL3_REG, &reg_val);
	if (ret)
		return ret;

	if (reg_val & (~AW88166_NOISE_GATE_EN_MASK))
		check_val = AW88166_BIT_SYSST_NOSWS_CHECK;
	else
		check_val = AW88166_BIT_SYSST_SWS_CHECK;

	for (i = 0; i < AW88166_DEV_SYSST_CHECK_MAX; i++) {
		ret = regmap_read(aw_dev->regmap, AW88166_SYSST_REG, &reg_val);
		if (ret)
			return ret;

		if ((reg_val & (~AW88166_BIT_SYSST_CHECK_MASK) & check_val) != check_val) {
			dev_err(aw_dev->dev, "check sysst fail, cnt=%d, reg_val=0x%04x, check:0x%x",
				i, reg_val, AW88166_BIT_SYSST_NOSWS_CHECK);
			usleep_range(AW_2000_US, AW_2000_US + 10);
		} else {
			return 0;
		}
	}

	return -EPERM;
}

static int aw88166_dev_get_icalk(struct aw88166 *aw88166, int16_t *icalk)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	unsigned int efrm_reg_val, efrl_reg_val;
	uint16_t ef_isn_geslp, ef_isn_h5bits;
	uint16_t icalk_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW88166_EFRM2_REG, &efrm_reg_val);
	if (ret)
		return ret;

	ef_isn_geslp = (efrm_reg_val & (~AW88166_EF_ISN_GESLP_MASK)) >>
						AW88166_EF_ISN_GESLP_SHIFT;

	ret = regmap_read(aw_dev->regmap, AW88166_EFRL_REG, &efrl_reg_val);
	if (ret)
		return ret;

	ef_isn_h5bits = (efrl_reg_val & (~AW88166_EF_ISN_H5BITS_MASK)) >>
						AW88166_EF_ISN_H5BITS_SHIFT;

	if (aw88166->check_val == AW_EF_AND_CHECK)
		icalk_val = ef_isn_geslp & (ef_isn_h5bits | AW88166_EF_ISN_H5BITS_SIGN_MASK);
	else
		icalk_val = ef_isn_geslp | (ef_isn_h5bits & (~AW88166_EF_ISN_H5BITS_SIGN_MASK));

	if (icalk_val & (~AW88166_ICALK_SIGN_MASK))
		icalk_val = icalk_val | AW88166_ICALK_NEG_MASK;
	*icalk = (int16_t)icalk_val;

	return 0;
}

static int aw88166_dev_get_vcalk(struct aw88166 *aw88166, int16_t *vcalk)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	unsigned int efrm_reg_val, efrl_reg_val;
	uint16_t ef_vsn_geslp, ef_vsn_h3bits;
	uint16_t vcalk_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW88166_EFRM2_REG, &efrm_reg_val);
	if (ret)
		return ret;

	ef_vsn_geslp = (efrm_reg_val & (~AW88166_EF_VSN_GESLP_MASK)) >>
					AW88166_EF_VSN_GESLP_SHIFT;

	ret = regmap_read(aw_dev->regmap, AW88166_EFRL_REG, &efrl_reg_val);
	if (ret)
		return ret;

	ef_vsn_h3bits = (efrl_reg_val & (~AW88166_EF_VSN_H3BITS_MASK)) >>
					AW88166_EF_VSN_H3BITS_SHIFT;

	if (aw88166->check_val == AW_EF_AND_CHECK)
		vcalk_val = ef_vsn_geslp & (ef_vsn_h3bits | AW88166_EF_VSN_H3BITS_SIGN_MASK);
	else
		vcalk_val = ef_vsn_geslp | (ef_vsn_h3bits & (~AW88166_EF_VSN_H3BITS_SIGN_MASK));

	if (vcalk_val & (~AW88166_VCALK_SIGN_MASK))
		vcalk_val = vcalk_val | AW88166_VCALK_NEG_MASK;
	*vcalk = (int16_t)vcalk_val;

	return 0;
}

static int aw88166_dev_set_vcalb(struct aw88166 *aw88166)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	int32_t ical_k, vcal_k, vcalb;
	int16_t icalk, vcalk;
	unsigned int reg_val;
	int ret;

	ret = aw88166_dev_get_icalk(aw88166, &icalk);
	if (ret) {
		dev_err(aw_dev->dev, "get icalk failed\n");
		return ret;
	}
	ical_k = icalk * AW88166_ICABLK_FACTOR + AW88166_CABL_BASE_VALUE;

	ret = aw88166_dev_get_vcalk(aw88166, &vcalk);
	if (ret) {
		dev_err(aw_dev->dev, "get vbcalk failed\n");
		return ret;
	}
	vcal_k = vcalk * AW88166_VCABLK_FACTOR + AW88166_CABL_BASE_VALUE;

	vcalb = AW88166_VCALB_ACCURACY * AW88166_VSCAL_FACTOR /
			AW88166_ISCAL_FACTOR * ical_k / vcal_k * aw88166->vcalb_init_val;

	vcalb = vcalb >> AW88166_VCALB_ADJ_FACTOR;
	reg_val = (uint32_t)vcalb;

	regmap_write(aw_dev->regmap, AW88166_DSPVCALB_REG, reg_val);

	return 0;
}

static int aw_dev_init_vcalb_update(struct aw88166 *aw88166, int flag)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	int ret;

	switch (flag) {
	case AW88166_RECOVERY_SEC_DATA:
		ret = regmap_write(aw_dev->regmap, AW88166_DSPVCALB_REG, aw88166->vcalb_init_val);
		break;
	case AW88166_RECORD_SEC_DATA:
		ret = regmap_read(aw_dev->regmap, AW88166_DSPVCALB_REG, &aw88166->vcalb_init_val);
		break;
	default:
		dev_err(aw_dev->dev, "unsupported type:%d\n", flag);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int aw_dev_init_re_update(struct aw88166 *aw88166, int flag)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	unsigned int re_temp_h, re_temp_l;
	int ret;

	switch (flag) {
	case AW88166_RECOVERY_SEC_DATA:
		ret = regmap_write(aw_dev->regmap, AW88166_ACR1_REG, aw88166->re_init_val >> 16);
		if (ret)
			return ret;
		ret = regmap_write(aw_dev->regmap, AW88166_ACR2_REG,
						(uint16_t)aw88166->re_init_val);
		if (ret)
			return ret;
		break;
	case AW88166_RECORD_SEC_DATA:
		ret = regmap_read(aw_dev->regmap, AW88166_ACR1_REG, &re_temp_h);
		if (ret)
			return ret;
		ret = regmap_read(aw_dev->regmap, AW88166_ACR2_REG, &re_temp_l);
		if (ret)
			return ret;
		aw88166->re_init_val = (re_temp_h << 16) + re_temp_l;
		break;
	default:
		dev_err(aw_dev->dev, "unsupported type:%d\n", flag);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static void aw_dev_backup_sec_record(struct aw88166 *aw88166)
{
	aw_dev_init_vcalb_update(aw88166, AW88166_RECORD_SEC_DATA);
	aw_dev_init_re_update(aw88166, AW88166_RECOVERY_SEC_DATA);
}

static void aw_dev_backup_sec_recovery(struct aw88166 *aw88166)
{
	aw_dev_init_vcalb_update(aw88166, AW88166_RECOVERY_SEC_DATA);
	aw_dev_init_re_update(aw88166, AW88166_RECOVERY_SEC_DATA);
}

static int aw_dev_update_cali_re(struct aw_cali_desc *cali_desc)
{
	struct aw_device *aw_dev =
		container_of(cali_desc, struct aw_device, cali_desc);
	uint16_t re_lbits, re_hbits;
	u32 cali_re;
	int ret;

	if ((aw_dev->cali_desc.cali_re >= AW88166_CALI_RE_MAX) ||
			(aw_dev->cali_desc.cali_re <= AW88166_CALI_RE_MIN))
		return -EINVAL;

	cali_re = AW88166_SHOW_RE_TO_DSP_RE((aw_dev->cali_desc.cali_re +
				aw_dev->cali_desc.ra), AW88166_DSP_RE_SHIFT);

	re_hbits = (cali_re & (~AW88166_CALI_RE_HBITS_MASK)) >> AW88166_CALI_RE_HBITS_SHIFT;
	re_lbits = (cali_re & (~AW88166_CALI_RE_LBITS_MASK)) >> AW88166_CALI_RE_LBITS_SHIFT;

	ret = regmap_write(aw_dev->regmap, AW88166_ACR1_REG, re_hbits);
	if (ret) {
		dev_err(aw_dev->dev, "set cali re error");
		return ret;
	}

	ret = regmap_write(aw_dev->regmap, AW88166_ACR2_REG, re_lbits);
	if (ret)
		dev_err(aw_dev->dev, "set cali re error");

	return ret;
}

static int aw_dev_fw_crc_check(struct aw_device *aw_dev)
{
	uint16_t check_val, fw_len_val;
	unsigned int reg_val;
	int ret;

	/* calculate fw_end_addr */
	fw_len_val = ((aw_dev->dsp_fw_len / AW_FW_ADDR_LEN) - 1) + AW88166_CRC_FW_BASE_ADDR;

	/* write fw_end_addr to crc_end_addr */
	ret = regmap_update_bits(aw_dev->regmap, AW88166_CRCCTRL_REG,
					~AW88166_CRC_END_ADDR_MASK, fw_len_val);
	if (ret)
		return ret;
	/* enable fw crc check */
	ret = regmap_update_bits(aw_dev->regmap, AW88166_CRCCTRL_REG,
		~AW88166_CRC_CODE_EN_MASK, AW88166_CRC_CODE_EN_ENABLE_VALUE);

	usleep_range(AW_2000_US, AW_2000_US + 10);

	/* read crc check result */
	regmap_read(aw_dev->regmap, AW88166_HAGCST_REG, &reg_val);
	if (ret)
		return ret;

	check_val = (reg_val & (~AW88166_CRC_CHECK_BITS_MASK)) >> AW88166_CRC_CHECK_START_BIT;

	/* disable fw crc check */
	ret = regmap_update_bits(aw_dev->regmap, AW88166_CRCCTRL_REG,
		~AW88166_CRC_CODE_EN_MASK, AW88166_CRC_CODE_EN_DISABLE_VALUE);
	if (ret)
		return ret;

	if (check_val != AW88166_CRC_CHECK_PASS_VAL) {
		dev_err(aw_dev->dev, "%s failed, check_val 0x%x != 0x%x\n",
				__func__, check_val, AW88166_CRC_CHECK_PASS_VAL);
		ret = -EINVAL;
	}

	return ret;
}

static int aw_dev_cfg_crc_check(struct aw_device *aw_dev)
{
	uint16_t check_val, cfg_len_val;
	unsigned int reg_val;
	int ret;

	/* calculate cfg end addr */
	cfg_len_val = ((aw_dev->dsp_cfg_len / AW_FW_ADDR_LEN) - 1) + AW88166_CRC_CFG_BASE_ADDR;

	/* write cfg_end_addr to crc_end_addr */
	ret = regmap_update_bits(aw_dev->regmap, AW88166_CRCCTRL_REG,
				~AW88166_CRC_END_ADDR_MASK, cfg_len_val);
	if (ret)
		return ret;

	/* enable cfg crc check */
	ret = regmap_update_bits(aw_dev->regmap, AW88166_CRCCTRL_REG,
			~AW88166_CRC_CFG_EN_MASK, AW88166_CRC_CFG_EN_ENABLE_VALUE);
	if (ret)
		return ret;

	usleep_range(AW_1000_US, AW_1000_US + 10);

	/* read crc check result */
	ret = regmap_read(aw_dev->regmap, AW88166_HAGCST_REG, &reg_val);
	if (ret)
		return ret;

	check_val = (reg_val & (~AW88166_CRC_CHECK_BITS_MASK)) >> AW88166_CRC_CHECK_START_BIT;

	/* disable cfg crc check */
	ret = regmap_update_bits(aw_dev->regmap, AW88166_CRCCTRL_REG,
			~AW88166_CRC_CFG_EN_MASK, AW88166_CRC_CFG_EN_DISABLE_VALUE);
	if (ret)
		return ret;

	if (check_val != AW88166_CRC_CHECK_PASS_VAL) {
		dev_err(aw_dev->dev, "crc_check failed, check val 0x%x != 0x%x\n",
						check_val, AW88166_CRC_CHECK_PASS_VAL);
		ret = -EINVAL;
	}

	return ret;
}

static int aw_dev_hw_crc_check(struct aw88166 *aw88166)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	int ret;

	ret = regmap_update_bits(aw_dev->regmap, AW88166_I2SCFG1_REG,
		~AW88166_RAM_CG_BYP_MASK, AW88166_RAM_CG_BYP_BYPASS_VALUE);
	if (ret)
		return ret;

	ret = aw_dev_fw_crc_check(aw_dev);
	if (ret) {
		dev_err(aw_dev->dev, "fw_crc_check failed\n");
		goto crc_check_failed;
	}

	ret = aw_dev_cfg_crc_check(aw_dev);
	if (ret) {
		dev_err(aw_dev->dev, "cfg_crc_check failed\n");
		goto crc_check_failed;
	}

	ret = regmap_write(aw_dev->regmap, AW88166_CRCCTRL_REG, aw88166->crc_init_val);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw_dev->regmap, AW88166_I2SCFG1_REG,
		~AW88166_RAM_CG_BYP_MASK, AW88166_RAM_CG_BYP_WORK_VALUE);

	return ret;

crc_check_failed:
	regmap_update_bits(aw_dev->regmap, AW88166_I2SCFG1_REG,
		~AW88166_RAM_CG_BYP_MASK, AW88166_RAM_CG_BYP_WORK_VALUE);
	return ret;
}

static int aw_dev_dsp_check(struct aw_device *aw_dev)
{
	int ret, i;


	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
					~AW88166_DSPBY_MASK, AW88166_DSPBY_BYPASS_VALUE);
	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
					~AW88166_DSPBY_MASK, AW88166_DSPBY_WORKING_VALUE);
	usleep_range(AW_1000_US, AW_1000_US + 10);
	for (i = 0; i < AW88166_DEV_DSP_CHECK_MAX; i++) {
		ret = aw_dev_get_dsp_status(aw_dev);
		if (ret) {
			dev_err(aw_dev->dev, "dsp wdt status error=%d", ret);
			usleep_range(AW_2000_US, AW_2000_US + 10);
		}
	}

	return ret;
}

static void aw88166_set_volume(struct aw_device *aw_dev, unsigned int value)
{
	struct aw_volume_desc *vol_desc = &aw_dev->volume_desc;
	unsigned int reg_value;
	u16 real_value;

	real_value = min((value + vol_desc->init_volume), (unsigned int)AW88166_MUTE_VOL);

	regmap_read(aw_dev->regmap, AW88166_SYSCTRL2_REG, &reg_value);

	dev_dbg(aw_dev->dev, "value 0x%x , reg:0x%x", value, real_value);

	real_value = (real_value << AW88166_VOL_START_BIT) | (reg_value & AW88166_VOL_MASK);

	regmap_write(aw_dev->regmap, AW88166_SYSCTRL2_REG, real_value);
}

static void aw88166_dev_mute(struct aw_device *aw_dev, bool is_mute)
{
	if (is_mute) {
		aw_dev_fade_out(aw_dev, aw88166_set_volume);
		regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
				~AW88166_HMUTE_MASK, AW88166_HMUTE_ENABLE_VALUE);
	} else {
		regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
				~AW88166_HMUTE_MASK, AW88166_HMUTE_DISABLE_VALUE);
		aw_dev_fade_in(aw_dev, aw88166_set_volume);
	}
}

static int aw88166_dev_start(struct aw88166 *aw88166)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	int ret;

	if (aw_dev->status == AW_DEV_PW_ON) {
		dev_dbg(aw_dev->dev, "already power on");
		return 0;
	}

	regmap_update_bits(aw_dev->regmap, AW88166_DBGCTRL_REG,
				~AW88166_DITHER_EN_MASK, AW88166_DITHER_EN_DISABLE_VALUE);

	/* power on */
	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
				~AW88166_PWDN_MASK, AW88166_PWDN_WORKING_VALUE);
	usleep_range(AW_2000_US, AW_2000_US + 10);

	ret = aw_dev_check_syspll(aw_dev);
	if (ret) {
		dev_err(aw_dev->dev, "pll check failed cannot start\n");
		goto pll_check_fail;
	}

	/* amppd on */
	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
				~AW88166_AMPPD_MASK, AW88166_AMPPD_WORKING_VALUE);
	usleep_range(AW_1000_US, AW_1000_US + 50);

	/* check i2s status */
	ret = aw_dev_check_sysst(aw_dev);
	if (ret) {
		dev_err(aw_dev->dev, "sysst check failed\n");
		goto sysst_check_fail;
	}

	if (aw_dev->dsp_cfg == AW_DEV_DSP_WORK) {
		aw_dev_backup_sec_recovery(aw88166);
		ret = aw_dev_hw_crc_check(aw88166);
		if (ret) {
			dev_err(aw_dev->dev, "dsp crc check failed\n");
			goto crc_check_fail;
		}
		regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
					~AW88166_DSPBY_MASK, AW88166_DSPBY_BYPASS_VALUE);
		aw88166_dev_set_vcalb(aw88166);
		aw_dev_update_cali_re(&aw_dev->cali_desc);
		ret = aw_dev_dsp_check(aw_dev);
		if (ret) {
			dev_err(aw_dev->dev, "dsp status check failed\n");
			goto dsp_check_fail;
		}
	} else {
		dev_dbg(aw_dev->dev, "start pa with dsp bypass");
	}

	/* enable tx feedback */
	regmap_update_bits(aw_dev->regmap, AW88166_I2SCTRL3_REG,
			~AW88166_I2STXEN_MASK, AW88166_I2STXEN_ENABLE_VALUE);

	if (aw88166->dither_st == AW88166_DITHER_EN_ENABLE_VALUE)
		regmap_update_bits(aw_dev->regmap, AW88166_DBGCTRL_REG,
				~AW88166_DITHER_EN_MASK, AW88166_DITHER_EN_ENABLE_VALUE);

	/* close mute */
	aw88166_dev_mute(aw_dev, false);
	/* clear inturrupt */
	aw_dev_clear_int_status(aw_dev);
	aw_dev->status = AW_DEV_PW_ON;

	return 0;

dsp_check_fail:
crc_check_fail:
	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
				~AW88166_DSPBY_MASK, AW88166_DSPBY_BYPASS_VALUE);
sysst_check_fail:
	aw_dev_clear_int_status(aw_dev);
	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
				~AW88166_AMPPD_MASK, AW88166_AMPPD_POWER_DOWN_VALUE);
pll_check_fail:
	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
				~AW88166_PWDN_MASK, AW88166_PWDN_POWER_DOWN_VALUE);
	aw_dev->status = AW_DEV_PW_OFF;

	return ret;
}

static int aw_dev_get_ra(struct aw_device *aw_dev)
{
	struct aw_cali_desc *cali_desc = &aw_dev->cali_desc;
	u32 dsp_ra;
	int ret;

	ret = aw_dev_dsp_read(aw_dev, AW88166_DSP_REG_CFG_ADPZ_RA,
				&dsp_ra, AW_DSP_32_DATA);
	if (ret) {
		dev_err(aw_dev->dev, "read ra error\n");
		return ret;
	}

	cali_desc->ra = AW88166_DSP_RE_TO_SHOW_RE(dsp_ra,
					AW88166_DSP_RE_SHIFT);

	return 0;
}

static int aw88166_check_sram(struct aw_device *aw_dev)
{
	unsigned int reg_val;

	/* read dsp_rom_check_reg */
	aw_dev_dsp_read(aw_dev, AW88166_DSP_ROM_CHECK_ADDR, &reg_val, AW_DSP_16_DATA);
	if (reg_val != AW88166_DSP_ROM_CHECK_DATA) {
		dev_err(aw_dev->dev, "check dsp rom failed, read[0x%x] != check[0x%x]\n",
						reg_val, AW88166_DSP_ROM_CHECK_DATA);
		goto error;
	}

	/* check dsp_cfg_base_addr */
	aw_dev_dsp_write(aw_dev, AW88166_DSP_CFG_ADDR,
					AW88166_DSP_ODD_NUM_BIT_TEST, AW_DSP_16_DATA);
	aw_dev_dsp_read(aw_dev, AW88166_DSP_CFG_ADDR, &reg_val, AW_DSP_16_DATA);
	if (reg_val != AW88166_DSP_ODD_NUM_BIT_TEST) {
		dev_err(aw_dev->dev, "check dsp cfg failed, read[0x%x] != write[0x%x]\n",
						reg_val, AW88166_DSP_ODD_NUM_BIT_TEST);
		goto error;
	}

	return 0;
error:
	return -EPERM;
}

static int aw88166_reg_update(struct aw88166 *aw88166,
				unsigned char *data, unsigned int len)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	struct aw_volume_desc *vol_desc = &aw_dev->volume_desc;
	u16 read_vol, reg_val;
	int data_len, i, ret;
	int16_t *reg_data;
	u8 reg_addr;

	reg_data = (int16_t *)data;
	data_len = len >> 1;

	if (data_len & 0x1) {
		dev_err(aw_dev->dev, "data len:%d unsupported\n", data_len);
		return -EINVAL;
	}

	for (i = 0; i < data_len; i += 2) {
		reg_addr = reg_data[i];
		reg_val = reg_data[i + 1];

		if (reg_addr == AW88166_DSPVCALB_REG) {
			aw88166->vcalb_init_val = reg_val;
			continue;
		}

		if (reg_addr == AW88166_SYSCTRL_REG) {
			if (reg_val & (~AW88166_DSPBY_MASK))
				aw_dev->dsp_cfg = AW_DEV_DSP_BYPASS;
			else
				aw_dev->dsp_cfg = AW_DEV_DSP_WORK;

			reg_val &= (AW88166_HMUTE_MASK | AW88166_PWDN_MASK |
						AW88166_DSPBY_MASK);
			reg_val |= (AW88166_HMUTE_ENABLE_VALUE | AW88166_PWDN_POWER_DOWN_VALUE |
						AW88166_DSPBY_BYPASS_VALUE);
		}

		if (reg_addr == AW88166_I2SCTRL3_REG) {
			reg_val &= AW88166_I2STXEN_MASK;
			reg_val |= AW88166_I2STXEN_DISABLE_VALUE;
		}

		if (reg_addr == AW88166_SYSCTRL2_REG) {
			read_vol = (reg_val & (~AW88166_VOL_MASK)) >>
				AW88166_VOL_START_BIT;
			aw_dev->volume_desc.init_volume = read_vol;
		}

		if (reg_addr == AW88166_DBGCTRL_REG) {
			if ((reg_val & (~AW88166_EF_DBMD_MASK)) == AW88166_EF_DBMD_OR_VALUE)
				aw88166->check_val = AW_EF_OR_CHECK;
			else
				aw88166->check_val = AW_EF_AND_CHECK;

			aw88166->dither_st = reg_val & (~AW88166_DITHER_EN_MASK);
		}

		if (reg_addr == AW88166_ACR1_REG) {
			aw88166->re_init_val |= (uint32_t)reg_val << 16;
			continue;
		}

		if (reg_addr == AW88166_ACR2_REG) {
			aw88166->re_init_val |= (uint32_t)reg_val;
			continue;
		}

		if (reg_addr == AW88166_CRCCTRL_REG)
			aw88166->crc_init_val = reg_val;

		ret = regmap_write(aw_dev->regmap, reg_addr, reg_val);
		if (ret)
			return ret;
	}

	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
				~AW88166_PWDN_MASK, AW88166_PWDN_WORKING_VALUE);
	usleep_range(AW_1000_US, AW_1000_US + 10);

	if (aw_dev->prof_cur != aw_dev->prof_index)
		vol_desc->ctl_volume = 0;
	else
		aw88166_set_volume(aw_dev, vol_desc->ctl_volume);

	return 0;
}

static int aw88166_dev_fw_update(struct aw88166 *aw88166, bool up_dsp_fw_en, bool force_up_en)
{
	struct aw_device *aw_dev = aw88166->aw_pa;
	struct aw_prof_desc *prof_index_desc;
	struct aw_sec_data_desc *sec_desc;
	char *prof_name;
	int ret;

	if ((aw_dev->prof_cur == aw_dev->prof_index) &&
			(force_up_en == AW_FORCE_UPDATE_OFF)) {
		dev_dbg(aw_dev->dev, "scene no change, not update");
		return 0;
	}

	if (aw_dev->fw_status == AW_DEV_FW_FAILED) {
		dev_err(aw_dev->dev, "fw status[%d] error\n", aw_dev->fw_status);
		return -EPERM;
	}

	ret = aw_dev_get_prof_name(aw_dev, aw_dev->prof_index, &prof_name);
	if (ret)
		return ret;

	dev_dbg(aw_dev->dev, "start update %s", prof_name);

	ret = aw_dev_get_prof_data(aw_dev, aw_dev->prof_index, &prof_index_desc);
	if (ret)
		return ret;

	/* update reg */
	sec_desc = prof_index_desc->sec_desc;
	ret = aw88166_reg_update(aw88166, sec_desc[AW_DATA_TYPE_REG].data,
					sec_desc[AW_DATA_TYPE_REG].len);
	if (ret) {
		dev_err(aw_dev->dev, "update reg failed\n");
		return ret;
	}

	regmap_update_bits(aw_dev->regmap, AW88166_DBGCTRL_REG, ~AW88166_MEM_CLKSEL_MASK,
					AW88166_MEM_CLKSEL_OSCCLK_VALUE);

	aw_dev_backup_sec_recovery(aw88166);

	if (up_dsp_fw_en) {
		ret = aw88166_check_sram(aw_dev);
		if (ret) {
			dev_err(aw_dev->dev, "check sram failed\n");
			goto error;
		}

		dev_dbg(aw_dev->dev, "fw_ver: [%x]", prof_index_desc->fw_ver);
		ret = aw_dev_dsp_update_fw(aw_dev, sec_desc[AW_DATA_TYPE_DSP_FW].data,
					sec_desc[AW_DATA_TYPE_DSP_FW].len,
					sec_desc[AW_DATA_TYPE_DSP_FW].addr);
		if (ret) {
			dev_err(aw_dev->dev, "update dsp fw failed\n");
			goto error;
		}
	}

	/* update dsp config */
	ret = aw_dev_dsp_update_cfg(aw_dev, sec_desc[AW_DATA_TYPE_DSP_CFG].data,
					sec_desc[AW_DATA_TYPE_DSP_CFG].len,
					sec_desc[AW_DATA_TYPE_DSP_CFG].addr);
	if (ret) {
		dev_err(aw_dev->dev, "update dsp cfg failed\n");
		goto error;
	}

	aw_dev_get_ra(aw_dev);
	aw_dev_backup_sec_record(aw88166);

	regmap_update_bits(aw_dev->regmap, AW88166_DBGCTRL_REG, ~AW88166_MEM_CLKSEL_MASK,
					AW88166_MEM_CLKSEL_DAPHCLK_VALUE);

	aw_dev->prof_cur = aw_dev->prof_index;

	return 0;

error:
	regmap_update_bits(aw_dev->regmap, AW88166_DBGCTRL_REG, ~AW88166_MEM_CLKSEL_MASK,
					AW88166_MEM_CLKSEL_DAPHCLK_VALUE);
	return ret;
}

static void aw88166_start_pa(struct aw88166 *aw88166)
{
	int ret, i;

	for (i = 0; i < AW88166_START_RETRIES; i++) {
		ret = aw88166_dev_start(aw88166);
		if (ret) {
			dev_err(aw88166->aw_pa->dev, "aw88166 device start failed. retry = %d", i);
			ret = aw88166_dev_fw_update(aw88166, AW_DSP_FW_UPDATE_ON, true);
			if (ret) {
				dev_err(aw88166->aw_pa->dev, "fw update failed");
				continue;
			}
		} else {
			dev_dbg(aw88166->aw_pa->dev, "start success\n");
			break;
		}
	}
}

static void aw88166_startup_work(struct work_struct *work)
{
	struct aw88166 *aw88166 =
		container_of(work, struct aw88166, start_work.work);

	mutex_lock(&aw88166->lock);
	aw88166_start_pa(aw88166);
	mutex_unlock(&aw88166->lock);
}

static void aw88166_start(struct aw88166 *aw88166, bool sync_start)
{
	int ret;

	if (aw88166->aw_pa->fw_status != AW_DEV_FW_OK)
		return;

	if (aw88166->aw_pa->status == AW_DEV_PW_ON)
		return;

	ret = aw88166_dev_fw_update(aw88166, AW_DSP_FW_UPDATE_OFF, aw88166->phase_sync);
	if (ret) {
		dev_err(aw88166->aw_pa->dev, "fw update failed\n");
		return;
	}

	if (sync_start == AW_SYNC_START)
		aw88166_start_pa(aw88166);
	else
		queue_delayed_work(system_wq,
			&aw88166->start_work,
			AW88166_START_WORK_DELAY_MS);
}

static int aw_dev_check_sysint(struct aw_device *aw_dev)
{
	u16 reg_val;

	aw_dev_get_int_status(aw_dev, &reg_val);
	if (reg_val & AW88166_BIT_SYSINT_CHECK) {
		dev_err(aw_dev->dev, "pa stop check fail:0x%04x\n", reg_val);
		return -EINVAL;
	}

	return 0;
}

static int aw88166_stop(struct aw_device *aw_dev)
{
	struct aw_sec_data_desc *dsp_cfg =
		&aw_dev->prof_info.prof_desc[aw_dev->prof_cur].sec_desc[AW_DATA_TYPE_DSP_CFG];
	struct aw_sec_data_desc *dsp_fw =
		&aw_dev->prof_info.prof_desc[aw_dev->prof_cur].sec_desc[AW_DATA_TYPE_DSP_FW];
	int int_st;

	if (aw_dev->status == AW_DEV_PW_OFF) {
		dev_dbg(aw_dev->dev, "already power off");
		return 0;
	}

	aw_dev->status = AW_DEV_PW_OFF;

	aw88166_dev_mute(aw_dev, true);
	usleep_range(AW_4000_US, AW_4000_US + 100);

	regmap_update_bits(aw_dev->regmap, AW88166_I2SCTRL3_REG,
			~AW88166_I2STXEN_MASK, AW88166_I2STXEN_DISABLE_VALUE);
	usleep_range(AW_1000_US, AW_1000_US + 100);

	int_st = aw_dev_check_sysint(aw_dev);

	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
			~AW88166_DSPBY_MASK, AW88166_DSPBY_BYPASS_VALUE);

	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
			~AW88166_AMPPD_MASK, AW88166_AMPPD_POWER_DOWN_VALUE);

	if (int_st) {
		regmap_update_bits(aw_dev->regmap, AW88166_DBGCTRL_REG, ~AW88166_MEM_CLKSEL_MASK,
					AW88166_MEM_CLKSEL_OSCCLK_VALUE);
		aw_dev_dsp_update_fw(aw_dev, dsp_fw->data, dsp_fw->len, dsp_fw->addr);
		aw_dev_dsp_update_cfg(aw_dev, dsp_cfg->data, dsp_cfg->len, dsp_cfg->addr);
		regmap_update_bits(aw_dev->regmap, AW88166_DBGCTRL_REG, ~AW88166_MEM_CLKSEL_MASK,
					AW88166_MEM_CLKSEL_DAPHCLK_VALUE);
	}

	regmap_update_bits(aw_dev->regmap, AW88166_SYSCTRL_REG,
			~AW88166_PWDN_MASK, AW88166_PWDN_POWER_DOWN_VALUE);

	return 0;
}

static struct snd_soc_dai_driver aw88166_dai[] = {
	{
		.name = "aw88166-aif",
		.id = 1,
		.playback = {
			.stream_name = "Speaker_Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = AW88166_RATES,
			.formats = AW88166_FORMATS,
		},
		.capture = {
			.stream_name = "Speaker_Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = AW88166_RATES,
			.formats = AW88166_FORMATS,
		},
	},
};

static int aw88166_get_fade_in_time(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(component);
	struct aw_device *aw_dev = aw88166->aw_pa;

	ucontrol->value.integer.value[0] = aw_dev->fade_in_time;

	return 0;
}

static int aw88166_set_fade_in_time(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct aw_device *aw_dev = aw88166->aw_pa;
	int time;

	time = ucontrol->value.integer.value[0];

	if (time < mc->min || time > mc->max)
		return -EINVAL;

	if (time != aw_dev->fade_in_time) {
		aw_dev->fade_in_time = time;
		return 1;
	}

	return 0;
}

static int aw88166_get_fade_out_time(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(component);
	struct aw_device *aw_dev = aw88166->aw_pa;

	ucontrol->value.integer.value[0] = aw_dev->fade_out_time;

	return 0;
}

static int aw88166_set_fade_out_time(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct aw_device *aw_dev = aw88166->aw_pa;
	int time;

	time = ucontrol->value.integer.value[0];
	if (time < mc->min || time > mc->max)
		return -EINVAL;

	if (time != aw_dev->fade_out_time) {
		aw_dev->fade_out_time = time;
		return 1;
	}

	return 0;
}

static int aw88166_profile_info(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);
	char *prof_name;
	int count, ret;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;

	count = aw88166->aw_pa->prof_info.count;
	if (count <= 0) {
		uinfo->value.enumerated.items = 0;
		return 0;
	}

	uinfo->value.enumerated.items = count;

	if (uinfo->value.enumerated.item >= count)
		uinfo->value.enumerated.item = count - 1;

	count = uinfo->value.enumerated.item;

	ret = aw_dev_get_prof_name(aw88166->aw_pa, count, &prof_name);
	if (ret) {
		strscpy(uinfo->value.enumerated.name, "null");
		return 0;
	}

	strscpy(uinfo->value.enumerated.name, prof_name);

	return 0;
}

static int aw88166_profile_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);

	ucontrol->value.integer.value[0] = aw88166->aw_pa->prof_index;

	return 0;
}

static int aw88166_profile_set(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);
	int ret;

	mutex_lock(&aw88166->lock);
	ret = aw_dev_set_profile_index(aw88166->aw_pa, ucontrol->value.integer.value[0]);
	if (ret) {
		dev_dbg(codec->dev, "profile index does not change");
		mutex_unlock(&aw88166->lock);
		return 0;
	}

	if (aw88166->aw_pa->status) {
		aw88166_stop(aw88166->aw_pa);
		aw88166_start(aw88166, AW_SYNC_START);
	}

	mutex_unlock(&aw88166->lock);

	return 1;
}

static int aw88166_volume_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);
	struct aw_volume_desc *vol_desc = &aw88166->aw_pa->volume_desc;

	ucontrol->value.integer.value[0] = vol_desc->ctl_volume;

	return 0;
}

static int aw88166_volume_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);
	struct aw_volume_desc *vol_desc = &aw88166->aw_pa->volume_desc;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int value;

	value = ucontrol->value.integer.value[0];
	if (value < mc->min || value > mc->max)
		return -EINVAL;

	if (vol_desc->ctl_volume != value) {
		vol_desc->ctl_volume = value;
		aw88166_set_volume(aw88166->aw_pa, vol_desc->ctl_volume);

		return 1;
	}

	return 0;
}

static int aw88166_get_fade_step(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);

	ucontrol->value.integer.value[0] = aw88166->aw_pa->fade_step;

	return 0;
}

static int aw88166_set_fade_step(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int value;

	value = ucontrol->value.integer.value[0];
	if (value < mc->min || value > mc->max)
		return -EINVAL;

	if (aw88166->aw_pa->fade_step != value) {
		aw88166->aw_pa->fade_step = value;
		return 1;
	}

	return 0;
}

static int aw88166_re_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);
	struct aw_device *aw_dev = aw88166->aw_pa;

	ucontrol->value.integer.value[0] = aw_dev->cali_desc.cali_re;

	return 0;
}

static int aw88166_re_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_soc_kcontrol_component(kcontrol);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(codec);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct aw_device *aw_dev = aw88166->aw_pa;
	int value;

	value = ucontrol->value.integer.value[0];
	if (value < mc->min || value > mc->max)
		return -EINVAL;

	if (aw_dev->cali_desc.cali_re != value) {
		aw_dev->cali_desc.cali_re = value;
		return 1;
	}

	return 0;
}

static const struct snd_kcontrol_new aw88166_controls[] = {
	SOC_SINGLE_EXT("PCM Playback Volume", AW88166_SYSCTRL2_REG,
		6, AW88166_MUTE_VOL, 0, aw88166_volume_get,
		aw88166_volume_set),
	SOC_SINGLE_EXT("Fade Step", 0, 0, AW88166_MUTE_VOL, 0,
		aw88166_get_fade_step, aw88166_set_fade_step),
	SOC_SINGLE_EXT("Volume Ramp Up Step", 0, 0, FADE_TIME_MAX, FADE_TIME_MIN,
		aw88166_get_fade_in_time, aw88166_set_fade_in_time),
	SOC_SINGLE_EXT("Volume Ramp Down Step", 0, 0, FADE_TIME_MAX, FADE_TIME_MIN,
		aw88166_get_fade_out_time, aw88166_set_fade_out_time),
	SOC_SINGLE_EXT("Calib", 0, 0, AW88166_CALI_RE_MAX, 0,
		aw88166_re_get, aw88166_re_set),
	AW_PROFILE_EXT("AW88166 Profile Set", aw88166_profile_info,
		aw88166_profile_get, aw88166_profile_set),
};

static int aw88166_playback_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(component);

	mutex_lock(&aw88166->lock);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		aw88166_start(aw88166, AW_ASYNC_START);
		break;
	case SND_SOC_DAPM_POST_PMD:
		aw88166_stop(aw88166->aw_pa);
		break;
	default:
		break;
	}
	mutex_unlock(&aw88166->lock);

	return 0;
}

static const struct snd_soc_dapm_widget aw88166_dapm_widgets[] = {
	 /* playback */
	SND_SOC_DAPM_AIF_IN_E("AIF_RX", "Speaker_Playback", 0, SND_SOC_NOPM, 0, 0,
					aw88166_playback_event,
					SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("DAC Output"),

	/* capture */
	SND_SOC_DAPM_AIF_OUT("AIF_TX", "Speaker_Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_INPUT("ADC Input"),
};

static const struct snd_soc_dapm_route aw88166_audio_map[] = {
	{"DAC Output", NULL, "AIF_RX"},
	{"AIF_TX", NULL, "ADC Input"},
};

static int aw88166_codec_probe(struct snd_soc_component *component)
{
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(component);
	int ret;

	INIT_DELAYED_WORK(&aw88166->start_work, aw88166_startup_work);

	ret = aw_dev_request_firmware_file(aw88166->aw_pa, AW88166_ACF_FILE);
	if (ret) {
		dev_err(aw88166->aw_pa->dev, "%s failed\n", __func__);
		return ret;
	}

	ret = aw88166_dev_fw_update(aw88166, AW_FORCE_UPDATE_ON, AW_DSP_FW_UPDATE_ON);
	if (ret)
		dev_err(aw88166->aw_pa->dev, "fw update failed ret = %d\n", ret);

	return ret;
}

static void aw88166_codec_remove(struct snd_soc_component *aw_codec)
{
	struct aw88166 *aw88166 = snd_soc_component_get_drvdata(aw_codec);

	cancel_delayed_work_sync(&aw88166->start_work);
}

static const struct snd_soc_component_driver soc_codec_dev_aw88166 = {
	.probe = aw88166_codec_probe,
	.remove = aw88166_codec_remove,
	.dapm_widgets = aw88166_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(aw88166_dapm_widgets),
	.dapm_routes = aw88166_audio_map,
	.num_dapm_routes = ARRAY_SIZE(aw88166_audio_map),
	.controls = aw88166_controls,
	.num_controls = ARRAY_SIZE(aw88166_controls),
};

static int aw88166_i2c_probe(struct i2c_client *i2c)
{
	struct aw88166 *aw88166;
	int ret;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&i2c->dev, -ENXIO, "check_functionality failed\n");

	aw88166 = devm_kzalloc(&i2c->dev, sizeof(*aw88166), GFP_KERNEL);
	if (!aw88166)
		return -ENOMEM;

	mutex_init(&aw88166->lock);

	i2c_set_clientdata(i2c, aw88166);

	aw88166->aw_pa = devm_kzalloc(&i2c->dev, sizeof(struct aw_device), GFP_KERNEL);
	if (!aw88166->aw_pa)
		return -ENOMEM;

	aw88166->aw_pa->reset_gpio = devm_gpiod_get_optional(&i2c->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(aw88166->aw_pa->reset_gpio))
		return dev_err_probe(&i2c->dev, PTR_ERR(aw88166->reset_gpio),
							"reset gpio not defined\n");
	aw_hw_reset(aw88166->aw_pa);

	aw88166->regmap = devm_regmap_init_i2c(i2c, &aw88166_remap_config);
	if (IS_ERR(aw88166->regmap))
		return dev_err_probe(&i2c->dev, PTR_ERR(aw88166->regmap),
							"failed to init regmap\n");

	/* aw pa init */
	ret = aw_dev_init(aw88166->aw_pa, i2c, aw88166->regmap);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(&i2c->dev,
			&soc_codec_dev_aw88166,
			aw88166_dai, ARRAY_SIZE(aw88166_dai));
}

static const struct i2c_device_id aw88166_i2c_id[] = {
	{ AW88166_I2C_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw88166_i2c_id);

static struct i2c_driver aw88166_i2c_driver = {
	.driver = {
		.name = AW88166_I2C_NAME,
	},
	.probe = aw88166_i2c_probe,
	.id_table = aw88166_i2c_id,
};
module_i2c_driver(aw88166_i2c_driver);

MODULE_DESCRIPTION("ASoC AW88166 Smart PA Driver");
MODULE_LICENSE("GPL v2");
