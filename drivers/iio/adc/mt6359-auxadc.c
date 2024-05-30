// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6359 PMIC AUXADC IIO driver
 *
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2024 Collabora Ltd
 * Author: AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/mfd/mt6397/core.h>

#include <dt-bindings/iio/adc/mediatek,mt6357-auxadc.h>
#include <dt-bindings/iio/adc/mediatek,mt6358-auxadc.h>
#include <dt-bindings/iio/adc/mediatek,mt6359-auxadc.h>

#define AUXADC_AVG_TIME_US		10
#define AUXADC_POLL_DELAY_US		100
#define AUXADC_TIMEOUT_US		32000
#define AUXADC_VOLT_FULL		1800
#define IMP_STOP_DELAY_US		150
#define IMP_POLL_DELAY_US		1000

#define PMIC_RG_RESET_VAL		(BIT(0) | BIT(3))
#define PMIC_AUXADC_RDY_BIT		BIT(15)
#define PMIC_AUXADC_ADCx(x)		((x) << 1)
#define MT6357_IMP_ADC_NUM		30
#define MT6358_IMP_ADC_NUM		28

#define MT6358_DCM_CK_SW_EN		GENMASK(1, 0)
#define MT6358_IMP0_CLEAR		(BIT(14) | BIT(7))
#define MT6358_IMP0_IRQ_RDY		BIT(8)
#define MT6358_IMP1_AUTOREPEAT_EN	BIT(15)

#define MT6359_IMP0_CONV_EN		BIT(0)
#define MT6359_IMP1_IRQ_RDY		BIT(15)

enum mtk_pmic_auxadc_regs {
	PMIC_AUXADC_ADC0,
	PMIC_AUXADC_DCM_CON,
	PMIC_AUXADC_IMP0,
	PMIC_AUXADC_IMP1,
	PMIC_AUXADC_IMP3,
	PMIC_AUXADC_RQST0,
	PMIC_AUXADC_RQST1,
	PMIC_HK_TOP_WKEY,
	PMIC_HK_TOP_RST_CON0,
	PMIC_FGADC_R_CON0,
	PMIC_AUXADC_REGS_MAX
};

enum mtk_pmic_auxadc_channels {
	PMIC_AUXADC_CHAN_BATADC,
	PMIC_AUXADC_CHAN_ISENSE,
	PMIC_AUXADC_CHAN_VCDT,
	PMIC_AUXADC_CHAN_BAT_TEMP,
	PMIC_AUXADC_CHAN_BATID,
	PMIC_AUXADC_CHAN_CHIP_TEMP,
	PMIC_AUXADC_CHAN_VCORE_TEMP,
	PMIC_AUXADC_CHAN_VPROC_TEMP,
	PMIC_AUXADC_CHAN_VGPU_TEMP,
	PMIC_AUXADC_CHAN_ACCDET,
	PMIC_AUXADC_CHAN_VDCXO,
	PMIC_AUXADC_CHAN_TSX_TEMP,
	PMIC_AUXADC_CHAN_HPOFS_CAL,
	PMIC_AUXADC_CHAN_DCXO_TEMP,
	PMIC_AUXADC_CHAN_VBIF,
	PMIC_AUXADC_CHAN_IBAT,
	PMIC_AUXADC_CHAN_VBAT,
	PMIC_AUXADC_CHAN_MAX
};

/**
 * struct mt6359_auxadc - Main driver structure
 * @dev:           Device pointer
 * @regmap:        Regmap from SoC PMIC Wrapper
 * @pdata:         PMIC specific platform data
 * @lock:          Mutex lock for AUXADC reads
 * @timed_out:     Signals whether the last read timed out
 */
struct mt6359_auxadc {
	struct device *dev;
	struct regmap *regmap;
	const struct mtk_pmic_auxadc_pdata *pdata;
	struct mutex lock;
	bool timed_out;
};

/**
 * struct mtk_pmic_auxadc_chan - PMIC AUXADC channel data
 * @req_idx:       Request register number
 * @req_mask:      Bitmask to activate a channel
 * @num_samples:   Number of AUXADC samples for averaging
 * @r_numerator:   Resistance ratio numerator
 * @r_denominator: Resistance ratio denominator
 */
struct mtk_pmic_auxadc_chan {
	u8 req_idx;
	u16 req_mask;
	u16 num_samples;
	u8 r_numerator;
	u8 r_denominator;
};

/**
 * struct mtk_pmic_auxadc_pdata - PMIC specific platform data
 * @channels:       IIO specification of ADC channels
 * @num_channels:   Number of ADC channels
 * @desc:           PMIC AUXADC channel data
 * @regs:           List of PMIC specific registers
 * @sec_unlock_key: Security unlock key for HK_TOP writes
 * @imp_adc_num:    ADC channel for IMP readings
 * @read_imp:       Callback to read PMIC IMP channels
 */
struct mtk_pmic_auxadc_pdata {
	const struct iio_chan_spec *channels;
	int num_channels;
	const struct mtk_pmic_auxadc_chan *desc;
	const u16 *regs;
	u16 sec_unlock_key;
	u8 imp_adc_num;
	int (*read_imp)(struct mt6359_auxadc *adc_dev, int *vbat, int *ibat);
};

#define MTK_PMIC_ADC_CHAN(_ch_idx, _req_idx, _req_bit, _samples, _rnum, _rdiv)	\
	[PMIC_AUXADC_CHAN_##_ch_idx] = {					\
		.req_idx = _req_idx,						\
		.req_mask = BIT(_req_bit),					\
		.num_samples = _samples,					\
		.r_numerator = _rnum,						\
		.r_denominator = _rdiv,						\
	}

#define MTK_PMIC_IIO_CHAN(_model, _name, _ch_idx, _adc_idx, _nbits, _ch_type)	\
{										\
	.type = _ch_type,							\
	.channel = _model##_AUXADC_##_ch_idx,					\
	.address = _adc_idx,							\
	.scan_index = PMIC_AUXADC_CHAN_##_ch_idx,				\
	.datasheet_name = __stringify(_name),					\
	.scan_type =  {								\
		.sign = 'u',							\
		.realbits = _nbits,						\
		.storagebits = 16,						\
		.endianness = IIO_CPU						\
	},									\
	.indexed = 1,								\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE)	\
}

static const struct iio_chan_spec mt6357_auxadc_channels[] = {
	MTK_PMIC_IIO_CHAN(MT6357, bat_adc, BATADC, 0, 15, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6357, isense, ISENSE, 1, 12, IIO_CURRENT),
	MTK_PMIC_IIO_CHAN(MT6357, cdt_v, VCDT, 2, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6357, batt_temp, BAT_TEMP, 3, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6357, chip_temp, CHIP_TEMP, 4, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6357, acc_det, ACCDET, 5, 12, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6357, dcxo_v, VDCXO, 6, 12, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(MT6357, tsx_temp, TSX_TEMP, 7, 15, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6357, hp_ofs_cal, HPOFS_CAL, 9, 15, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6357, dcxo_temp, DCXO_TEMP, 36, 15, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6357, vcore_temp, VCORE_TEMP, 40, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6357, vproc_temp, VPROC_TEMP, 41, 12, IIO_TEMP),

	/* IMP channels */
	MTK_PMIC_IIO_CHAN(MT6357, batt_v, VBAT, 0, 15, IIO_VOLTAGE),
};

static const struct mtk_pmic_auxadc_chan mt6357_auxadc_ch_desc[] = {
	MTK_PMIC_ADC_CHAN(BATADC, PMIC_AUXADC_RQST0, 0, 128, 3, 1),
	MTK_PMIC_ADC_CHAN(ISENSE, PMIC_AUXADC_RQST0, 0, 128, 3, 1),
	MTK_PMIC_ADC_CHAN(VCDT, PMIC_AUXADC_RQST0, 0, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(BAT_TEMP, PMIC_AUXADC_RQST0, 3, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(CHIP_TEMP, PMIC_AUXADC_RQST0, 4, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(ACCDET, PMIC_AUXADC_RQST0, 5, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(TSX_TEMP, PMIC_AUXADC_RQST0, 7, 128, 1, 1),
	MTK_PMIC_ADC_CHAN(HPOFS_CAL, PMIC_AUXADC_RQST0, 9, 256, 1, 1),
	MTK_PMIC_ADC_CHAN(DCXO_TEMP, PMIC_AUXADC_RQST0, 10, 16, 1, 1),
	MTK_PMIC_ADC_CHAN(VBIF, PMIC_AUXADC_RQST0, 11, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(VCORE_TEMP, PMIC_AUXADC_RQST1, 5, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(VPROC_TEMP, PMIC_AUXADC_RQST1, 6, 8, 1, 1),

	/* IMP channels */
	MTK_PMIC_ADC_CHAN(VBAT, 0, 0, 128, 3, 1),
};

static const u16 mt6357_auxadc_regs[] = {
	[PMIC_HK_TOP_RST_CON0]	= 0xf90,
	[PMIC_AUXADC_DCM_CON]	= 0x122e,
	[PMIC_AUXADC_ADC0]	= 0x1088,
	[PMIC_AUXADC_IMP0]	= 0x119c,
	[PMIC_AUXADC_IMP1]	= 0x119e,
	[PMIC_AUXADC_RQST0]	= 0x110e,
	[PMIC_AUXADC_RQST1]	= 0x1114,
};

static const struct iio_chan_spec mt6358_auxadc_channels[] = {
	MTK_PMIC_IIO_CHAN(MT6358, bat_adc, BATADC, 0, 15, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6358, cdt_v, VCDT, 2, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6358, batt_temp, BAT_TEMP, 3, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6358, chip_temp, CHIP_TEMP, 4, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6358, acc_det, ACCDET, 5, 12, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6358, dcxo_v, VDCXO, 6, 12, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(MT6358, tsx_temp, TSX_TEMP, 7, 15, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6358, hp_ofs_cal, HPOFS_CAL, 9, 15, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6358, dcxo_temp, DCXO_TEMP, 10, 15, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6358, bif_v, VBIF, 11, 12, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(MT6358, vcore_temp, VCORE_TEMP, 38, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6358, vproc_temp, VPROC_TEMP, 39, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6358, vgpu_temp, VGPU_TEMP, 40, 12, IIO_TEMP),

	/* IMP channels */
	MTK_PMIC_IIO_CHAN(MT6358, batt_v, VBAT, 0, 15, IIO_VOLTAGE),
};

static const struct mtk_pmic_auxadc_chan mt6358_auxadc_ch_desc[] = {
	MTK_PMIC_ADC_CHAN(BATADC, PMIC_AUXADC_RQST0, 0, 128, 3, 1),
	MTK_PMIC_ADC_CHAN(VCDT, PMIC_AUXADC_RQST0, 0, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(BAT_TEMP, PMIC_AUXADC_RQST0, 3, 8, 2, 1),
	MTK_PMIC_ADC_CHAN(CHIP_TEMP, PMIC_AUXADC_RQST0, 4, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(ACCDET, PMIC_AUXADC_RQST0, 5, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(VDCXO, PMIC_AUXADC_RQST0, 6, 8, 3, 2),
	MTK_PMIC_ADC_CHAN(TSX_TEMP, PMIC_AUXADC_RQST0, 7, 128, 1, 1),
	MTK_PMIC_ADC_CHAN(HPOFS_CAL, PMIC_AUXADC_RQST0, 9, 256, 1, 1),
	MTK_PMIC_ADC_CHAN(DCXO_TEMP, PMIC_AUXADC_RQST0, 10, 16, 1, 1),
	MTK_PMIC_ADC_CHAN(VBIF, PMIC_AUXADC_RQST0, 11, 8, 2, 1),
	MTK_PMIC_ADC_CHAN(VCORE_TEMP, PMIC_AUXADC_RQST1, 8, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(VPROC_TEMP, PMIC_AUXADC_RQST1, 9, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(VGPU_TEMP, PMIC_AUXADC_RQST1, 10, 8, 1, 1),

	/* IMP channels */
	MTK_PMIC_ADC_CHAN(VBAT, 0, 0, 128, 7, 2),
};

static const u16 mt6358_auxadc_regs[] = {
	[PMIC_HK_TOP_RST_CON0]	= 0xf90,
	[PMIC_AUXADC_DCM_CON]	= 0x1260,
	[PMIC_AUXADC_ADC0]	= 0x1088,
	[PMIC_AUXADC_IMP0]	= 0x1208,
	[PMIC_AUXADC_IMP1]	= 0x120a,
	[PMIC_AUXADC_RQST0]	= 0x1108,
	[PMIC_AUXADC_RQST1]	= 0x110a,
};

static const struct iio_chan_spec mt6359_auxadc_channels[] = {
	MTK_PMIC_IIO_CHAN(MT6359, bat_adc, BATADC, 0, 15, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6359, batt_temp, BAT_TEMP, 3, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6359, chip_temp, CHIP_TEMP, 4, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6359, acc_det, ACCDET, 5, 12, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6359, dcxo_v, VDCXO, 6, 12, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(MT6359, tsx_temp, TSX_TEMP, 7, 15, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6359, hp_ofs_cal, HPOFS_CAL, 9, 15, IIO_RESISTANCE),
	MTK_PMIC_IIO_CHAN(MT6359, dcxo_temp, DCXO_TEMP, 10, 15, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6359, bif_v, VBIF, 11, 12, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(MT6359, vcore_temp, VCORE_TEMP, 30, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6359, vproc_temp, VPROC_TEMP, 31, 12, IIO_TEMP),
	MTK_PMIC_IIO_CHAN(MT6359, vgpu_temp, VGPU_TEMP, 32, 12, IIO_TEMP),

	/* IMP channels */
	MTK_PMIC_IIO_CHAN(MT6359, batt_v, VBAT, 0, 15, IIO_VOLTAGE),
	MTK_PMIC_IIO_CHAN(MT6359, batt_i, IBAT, 0, 15, IIO_CURRENT),
};

static const struct mtk_pmic_auxadc_chan mt6359_auxadc_ch_desc[] = {
	MTK_PMIC_ADC_CHAN(BATADC, PMIC_AUXADC_RQST0, 0, 128, 7, 2),
	MTK_PMIC_ADC_CHAN(BAT_TEMP, PMIC_AUXADC_RQST0, 3, 8, 5, 2),
	MTK_PMIC_ADC_CHAN(CHIP_TEMP, PMIC_AUXADC_RQST0, 4, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(ACCDET, PMIC_AUXADC_RQST0, 5, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(VDCXO, PMIC_AUXADC_RQST0, 6, 8, 3, 2),
	MTK_PMIC_ADC_CHAN(TSX_TEMP, PMIC_AUXADC_RQST0, 7, 128, 1, 1),
	MTK_PMIC_ADC_CHAN(HPOFS_CAL, PMIC_AUXADC_RQST0, 9, 256, 1, 1),
	MTK_PMIC_ADC_CHAN(DCXO_TEMP, PMIC_AUXADC_RQST0, 10, 16, 1, 1),
	MTK_PMIC_ADC_CHAN(VBIF, PMIC_AUXADC_RQST0, 11, 8, 5, 2),
	MTK_PMIC_ADC_CHAN(VCORE_TEMP, PMIC_AUXADC_RQST1, 8, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(VPROC_TEMP, PMIC_AUXADC_RQST1, 9, 8, 1, 1),
	MTK_PMIC_ADC_CHAN(VGPU_TEMP, PMIC_AUXADC_RQST1, 10, 8, 1, 1),

	/* IMP channels */
	MTK_PMIC_ADC_CHAN(VBAT, 0, 0, 128, 7, 2),
	MTK_PMIC_ADC_CHAN(IBAT, 0, 0, 128, 7, 2),
};

static const u16 mt6359_auxadc_regs[] = {
	[PMIC_FGADC_R_CON0]	= 0xd88,
	[PMIC_HK_TOP_WKEY]	= 0xfb4,
	[PMIC_HK_TOP_RST_CON0]	= 0xf90,
	[PMIC_AUXADC_RQST0]	= 0x1108,
	[PMIC_AUXADC_RQST1]	= 0x110a,
	[PMIC_AUXADC_ADC0]	= 0x1088,
	[PMIC_AUXADC_IMP0]	= 0x1208,
	[PMIC_AUXADC_IMP1]	= 0x120a,
	[PMIC_AUXADC_IMP3]	= 0x120e,
};

static void mt6358_stop_imp_conv(struct mt6359_auxadc *adc_dev)
{
	const struct mtk_pmic_auxadc_pdata *pdata = adc_dev->pdata;
	struct regmap *regmap = adc_dev->regmap;

	regmap_set_bits(regmap, pdata->regs[PMIC_AUXADC_IMP0], MT6358_IMP0_CLEAR);
	regmap_clear_bits(regmap, pdata->regs[PMIC_AUXADC_IMP0], MT6358_IMP0_CLEAR);
	regmap_clear_bits(regmap, pdata->regs[PMIC_AUXADC_IMP1], MT6358_IMP1_AUTOREPEAT_EN);
	regmap_clear_bits(regmap, pdata->regs[PMIC_AUXADC_DCM_CON], MT6358_DCM_CK_SW_EN);
}

static int mt6358_start_imp_conv(struct mt6359_auxadc *adc_dev)
{
	const struct mtk_pmic_auxadc_pdata *pdata = adc_dev->pdata;
	struct regmap *regmap = adc_dev->regmap;
	u32 val;
	int ret;

	regmap_set_bits(regmap, pdata->regs[PMIC_AUXADC_DCM_CON], MT6358_DCM_CK_SW_EN);
	regmap_set_bits(regmap, pdata->regs[PMIC_AUXADC_IMP1], MT6358_IMP1_AUTOREPEAT_EN);

	ret = regmap_read_poll_timeout(adc_dev->regmap, pdata->regs[PMIC_AUXADC_IMP0],
				       val, !!(val & MT6358_IMP0_IRQ_RDY),
				       IMP_POLL_DELAY_US, AUXADC_TIMEOUT_US);
	if (ret) {
		mt6358_stop_imp_conv(adc_dev);
		return ret;
	}

	return 0;
}

static int mt6358_read_imp(struct mt6359_auxadc *adc_dev, int *vbat, int *ibat)
{
	const struct mtk_pmic_auxadc_pdata *pdata = adc_dev->pdata;
	struct regmap *regmap = adc_dev->regmap;
	u16 reg_adc0 = pdata->regs[PMIC_AUXADC_ADC0];
	int val_v, ret;

	ret = mt6358_start_imp_conv(adc_dev);
	if (ret)
		return ret;

	/* Read the params before stopping */
	regmap_read(regmap, reg_adc0 + PMIC_AUXADC_ADCx(pdata->imp_adc_num), &val_v);

	mt6358_stop_imp_conv(adc_dev);

	*vbat = val_v;
	*ibat = 0;

	return 0;
}

static int mt6359_read_imp(struct mt6359_auxadc *adc_dev, int *vbat, int *ibat)
{
	const struct mtk_pmic_auxadc_pdata *pdata = adc_dev->pdata;
	struct regmap *regmap = adc_dev->regmap;
	int val_v, val_i, ret;
	u32 val;

	/* Start conversion */
	regmap_write(regmap, pdata->regs[PMIC_AUXADC_IMP0], MT6359_IMP0_CONV_EN);
	ret = regmap_read_poll_timeout(regmap, pdata->regs[PMIC_AUXADC_IMP1],
				       val, !!(val & MT6359_IMP1_IRQ_RDY),
				       IMP_POLL_DELAY_US, AUXADC_TIMEOUT_US);

	/* Stop conversion regardless of the result */
	regmap_write(regmap, pdata->regs[PMIC_AUXADC_IMP0], 0);
	if (ret)
		return ret;

	/* If it succeeded, wait for the registers to be populated */
	usleep_range(IMP_STOP_DELAY_US, IMP_STOP_DELAY_US + 50);

	ret = regmap_read(regmap, pdata->regs[PMIC_AUXADC_IMP3], &val_v);
	if (ret)
		return ret;

	ret = regmap_read(regmap, pdata->regs[PMIC_FGADC_R_CON0], &val_i);
	if (ret)
		return ret;

	*vbat = val_v;
	*ibat = val_i;

	return 0;
}

static const struct mtk_pmic_auxadc_pdata mt6357_pdata = {
	.channels = mt6357_auxadc_channels,
	.num_channels = ARRAY_SIZE(mt6357_auxadc_channels),
	.desc = mt6357_auxadc_ch_desc,
	.regs = mt6357_auxadc_regs,
	.imp_adc_num = MT6357_IMP_ADC_NUM,
	.read_imp = mt6358_read_imp,
};

static const struct mtk_pmic_auxadc_pdata mt6358_pdata = {
	.channels = mt6358_auxadc_channels,
	.num_channels = ARRAY_SIZE(mt6358_auxadc_channels),
	.desc = mt6358_auxadc_ch_desc,
	.regs = mt6358_auxadc_regs,
	.imp_adc_num = MT6358_IMP_ADC_NUM,
	.read_imp = mt6358_read_imp,
};

static const struct mtk_pmic_auxadc_pdata mt6359_pdata = {
	.channels = mt6359_auxadc_channels,
	.num_channels = ARRAY_SIZE(mt6359_auxadc_channels),
	.desc = mt6359_auxadc_ch_desc,
	.regs = mt6359_auxadc_regs,
	.sec_unlock_key = 0x6359,
	.read_imp = mt6359_read_imp,
};

static void mt6359_auxadc_reset(struct mt6359_auxadc *adc_dev)
{
	const struct mtk_pmic_auxadc_pdata *pdata = adc_dev->pdata;
	struct regmap *regmap = adc_dev->regmap;

	/* Unlock HK_TOP writes */
	if (pdata->sec_unlock_key)
		regmap_write(regmap, pdata->regs[PMIC_HK_TOP_WKEY], pdata->sec_unlock_key);

	/* Assert ADC reset */
	regmap_set_bits(regmap, pdata->regs[PMIC_HK_TOP_RST_CON0], PMIC_RG_RESET_VAL);

	/* De-assert ADC reset */
	regmap_clear_bits(regmap, pdata->regs[PMIC_HK_TOP_RST_CON0], PMIC_RG_RESET_VAL);

	/* Lock HK_TOP writes again */
	if (pdata->sec_unlock_key)
		regmap_write(regmap, pdata->regs[PMIC_HK_TOP_WKEY], 0);
}

static int mt6359_auxadc_read_adc(struct mt6359_auxadc *adc_dev,
				  const struct iio_chan_spec *chan, int *out)
{
	const struct mtk_pmic_auxadc_pdata *pdata = adc_dev->pdata;
	const struct mtk_pmic_auxadc_chan *desc = &pdata->desc[chan->scan_index];
	struct regmap *regmap = adc_dev->regmap;
	u32 val;
	int ret;

	/* Request to start sampling for ADC channel */
	ret = regmap_write(regmap, pdata->regs[desc->req_idx], desc->req_mask);
	if (ret)
		return ret;

	/* Wait until all samples are averaged */
	usleep_range(desc->num_samples * AUXADC_AVG_TIME_US,
		     (desc->num_samples + 1) * AUXADC_AVG_TIME_US);

	ret = regmap_read_poll_timeout(regmap,
				       (pdata->regs[PMIC_AUXADC_ADC0] +
					PMIC_AUXADC_ADCx(chan->address)),
				       val, (val & PMIC_AUXADC_RDY_BIT),
				       AUXADC_POLL_DELAY_US, AUXADC_TIMEOUT_US);
	if (ret)
		return ret;

	/* Stop sampling */
	regmap_write(regmap, pdata->regs[desc->req_idx], 0);

	*out = val & GENMASK(chan->scan_type.realbits - 1, 0);
	return 0;
}

static int mt6359_auxadc_read_label(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan, char *label)
{
	return sysfs_emit(label, "%s\n", chan->datasheet_name);
}

static int mt6359_auxadc_read_raw(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  int *val, int *val2, long mask)
{
	struct mt6359_auxadc *adc_dev = iio_priv(indio_dev);
	const struct mtk_pmic_auxadc_pdata *pdata = adc_dev->pdata;
	const struct mtk_pmic_auxadc_chan *desc = &pdata->desc[chan->scan_index];
	int ret;

	if (mask == IIO_CHAN_INFO_SCALE) {
		*val = desc->r_numerator * AUXADC_VOLT_FULL;

		if (desc->r_denominator > 1) {
			*val2 = desc->r_denominator;
			return IIO_VAL_FRACTIONAL;
		}

		return IIO_VAL_INT;
	}

	mutex_lock(&adc_dev->lock);

	switch (chan->scan_index) {
	case PMIC_AUXADC_CHAN_IBAT:
		ret = adc_dev->pdata->read_imp(adc_dev, val2, val);
		break;
	case PMIC_AUXADC_CHAN_VBAT:
		ret = adc_dev->pdata->read_imp(adc_dev, val, val2);
		break;
	default:
		ret = mt6359_auxadc_read_adc(adc_dev, chan, val);
		break;
	}

	mutex_unlock(&adc_dev->lock);

	if (ret) {
		/*
		 * If we get more than one timeout, it's possible that the
		 * AUXADC is stuck: perform a full reset to recover it.
		 */
		if (ret == -ETIMEDOUT) {
			if (adc_dev->timed_out) {
				dev_warn(adc_dev->dev, "Resetting stuck ADC!\r\n");
				mt6359_auxadc_reset(adc_dev);
			}
			adc_dev->timed_out = true;
		}
		return ret;
	}
	adc_dev->timed_out = false;

	return IIO_VAL_INT;
}

static const struct iio_info mt6359_auxadc_info = {
	.read_label = mt6359_auxadc_read_label,
	.read_raw = mt6359_auxadc_read_raw,
};

static int mt6359_auxadc_probe(struct platform_device *pdev)
{
	struct device *mt6397_mfd_dev = pdev->dev.parent;
	struct mt6359_auxadc *adc_dev;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	int ret;

	/* Regmap is from SoC PMIC Wrapper, parent of the mt6397 MFD */
	regmap = dev_get_regmap(mt6397_mfd_dev->parent, NULL);
	if (!regmap)
		return dev_err_probe(&pdev->dev, -ENODEV, "Failed to get regmap\n");

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*adc_dev));
	if (!indio_dev)
		return -ENOMEM;

	adc_dev = iio_priv(indio_dev);
	adc_dev->regmap = regmap;
	adc_dev->dev = &pdev->dev;

	adc_dev->pdata = device_get_match_data(&pdev->dev);
	if (!adc_dev->pdata)
		return -EINVAL;

	mutex_init(&adc_dev->lock);

	mt6359_auxadc_reset(adc_dev);

	indio_dev->dev.parent = &pdev->dev;
	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->info = &mt6359_auxadc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adc_dev->pdata->channels;
	indio_dev->num_channels = adc_dev->pdata->num_channels;

	ret = devm_iio_device_register(&pdev->dev, indio_dev);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to register iio device\n");

	return 0;
}

static const struct of_device_id mt6359_auxadc_of_match[] = {
	{ .compatible = "mediatek,mt6357-auxadc", .data = &mt6357_pdata },
	{ .compatible = "mediatek,mt6358-auxadc", .data = &mt6358_pdata },
	{ .compatible = "mediatek,mt6359-auxadc", .data = &mt6359_pdata },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6359_auxadc_of_match);

static struct platform_driver mt6359_auxadc_driver = {
	.driver = {
		.name = "mt6359-auxadc",
		.of_match_table = mt6359_auxadc_of_match,
	},
	.probe	= mt6359_auxadc_probe,
};
module_platform_driver(mt6359_auxadc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6359 PMIC AUXADC Driver");
