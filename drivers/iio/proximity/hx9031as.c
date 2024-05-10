// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 NanjingTianyihexin Electronics Ltd.
 * http://www.tianyihexin.com
 *
 * Driver for NanjingTianyihexin HX9031AS & HX9023S Cap Sensor
 * Author: Yasin Lee <yasin.lee.x@gmail.com>
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/version.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/irq.h>
#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/debugfs.h>

#define HX9031AS_DRIVER_VER "iio-1.0"
#define ENTER \
dev_info(hx9031as_pdata.pdev, "[%04d][%s]\n", __LINE__, __func__)
#define PRINT_DBG(format, x...) \
dev_info(hx9031as_pdata.pdev, "[%04d][%s] " format, __LINE__, __func__, ## x)
#define PRINT_INF(format, x...) \
dev_info(hx9031as_pdata.pdev, "[%04d][%s] " format, __LINE__, __func__, ## x)
#define PRINT_ERR(format, x...) \
dev_err(hx9031as_pdata.pdev, "[%04d][%s] " format, __LINE__, __func__, ## x)

#define HX9031AS_TEST_CHS_EN 0             //test
#define HX9023S_ON_BOARD 0
#define HX9031AS_ON_BOARD 1
#define HX9031AS_DRIVER_NAME "hx9031as"    //i2c addr: HX9031AS=0x28
#define HX9031AS_CHIP_ID 0x1D
#define HX9031AS_CH_NUM 5
#define HX9031AS_CH_USED 0x1F
#define HX9031AS_DATA_LOCK 1
#define HX9031AS_DATA_UNLOCK 0
#define CH_DATA_2BYTES 2
#define CH_DATA_3BYTES 3
#define CH_DATA_BYTES_MAX CH_DATA_3BYTES
#define HX9031AS_ODR_MS 200
#define TYHX_DELAY_MS(x) msleep(x)
#define BUF_SIZE 512

#define RW_00_GLOBAL_CTRL0                   0x00
#define RW_01_GLOBAL_CTRL1                   0x01
#define RW_02_PRF_CFG                        0x02
#define RW_03_CH0_CFG_7_0                    0x03
#define RW_04_CH0_CFG_9_8                    0x04
#define RW_05_CH1_CFG_7_0                    0x05
#define RW_06_CH1_CFG_9_8                    0x06
#define RW_07_CH2_CFG_7_0                    0x07
#define RW_08_CH2_CFG_9_8                    0x08
#define RW_09_CH3_CFG_7_0                    0x09
#define RW_0A_CH3_CFG_9_8                    0x0A
#define RW_0B_CH4_CFG_7_0                    0x0B
#define RW_0C_CH4_CFG_9_8                    0x0C
#define RW_0D_RANGE_7_0                      0x0D
#define RW_0E_RANGE_9_8                      0x0E
#define RW_0F_RANGE_18_16                    0x0F
#define RW_10_AVG0_NOSR0_CFG                 0x10
#define RW_11_NOSR12_CFG                     0x11
#define RW_12_NOSR34_CFG                     0x12
#define RW_13_AVG12_CFG                      0x13
#define RW_14_AVG34_CFG                      0x14
#define RW_15_OFFSET_DAC0_7_0                0x15
#define RW_16_OFFSET_DAC0_9_8                0x16
#define RW_17_OFFSET_DAC1_7_0                0x17
#define RW_18_OFFSET_DAC1_9_8                0x18
#define RW_19_OFFSET_DAC2_7_0                0x19
#define RW_1A_OFFSET_DAC2_9_8                0x1A
#define RW_1B_OFFSET_DAC3_7_0                0x1B
#define RW_1C_OFFSET_DAC3_9_8                0x1C
#define RW_1D_OFFSET_DAC4_7_0                0x1D
#define RW_1E_OFFSET_DAC4_9_8                0x1E
#define RW_1F_SAMPLE_NUM_7_0                 0x1F
#define RW_20_SAMPLE_NUM_9_8                 0x20
#define RW_21_INTEGRATION_NUM_7_0            0x21
#define RW_22_INTEGRATION_NUM_9_8            0x22
#define RW_23_GLOBAL_CTRL2                   0x23
#define RW_24_CH_NUM_CFG                     0x24
#define RW_25_DAC_SWAP_CFG                   0x25
#define RW_28_MOD_RST_CFG                    0x28
#define RW_29_LP_ALP_4_CFG                   0x29
#define RW_2A_LP_ALP_1_0_CFG                 0x2A
#define RW_2B_LP_ALP_3_2_CFG                 0x2B
#define RW_2C_UP_ALP_1_0_CFG                 0x2C
#define RW_2D_UP_ALP_3_2_CFG                 0x2D
#define RW_2E_DN_UP_ALP_0_4_CFG              0x2E
#define RW_2F_DN_ALP_2_1_CFG                 0x2F
#define RW_30_DN_ALP_4_3_CFG                 0x30
#define RW_31_INT_CAP_CFG                    0x31
#define RW_33_NDL_DLY_4_CFG                  0x33
#define RW_35_FORCE_NO_UP_CFG                0x35
#define RW_38_RAW_BL_RD_CFG                  0x38
#define RW_39_INTERRUPT_CFG                  0x39
#define RW_3A_INTERRUPT_CFG1                 0x3A
#define RW_3B_CALI_DIFF_CFG                  0x3B
#define RW_3C_DITHER_CFG                     0x3C
#define RW_40_ANALOG_MEM0_WRDATA_7_0         0x40
#define RW_41_ANALOG_MEM0_WRDATA_15_8        0x41
#define RW_42_ANALOG_MEM0_WRDATA_23_16       0x42
#define RW_43_ANALOG_MEM0_WRDATA_31_24       0x43
#define RW_48_ANALOG_PWE_PULSE_CYCLE7_0      0x48
#define RW_49_ANALOG_PWE_PULSE_CYCLE12_8     0x49
#define RW_4A_ANALOG_MEM_GLOBAL_CTRL         0x4A
#define RO_4B_DEBUG_MEM_ADC_FSM              0x4B
#define RW_4C_ANALOG_MEM_GLOBAL_CTRL1        0x4C
#define RO_5F_VERION_ID                      0x5F
#define RO_60_DEVICE_ID                      0x60
#define RO_61_TC_FSM                         0x61
#define RO_66_FLAG_RD                        0x66
#define RO_6A_CONV_TIMEOUT_CNT               0x6A
#define RO_6B_PROX_STATUS                    0x6B
#define RW_6C_PROX_INT_HIGH_CFG              0x6C
#define RW_6D_PROX_INT_LOW_CFG               0x6D
#define RW_6E_CAP_INI_CFG                    0x6E
#define RW_6F_INT_WIDTH_CFG0                 0x6F
#define RW_70_INT_WIDTH_CFG1                 0x70
#define RO_71_INT_STATE_RD0                  0x71
#define RO_72_INT_STATE_RD1                  0x72
#define RO_73_INT_STATE_RD2                  0x73
#define RO_74_INT_STATE_RD3                  0x74
#define RW_80_PROX_HIGH_DIFF_CFG_CH0_0       0x80
#define RW_81_PROX_HIGH_DIFF_CFG_CH0_1       0x81
#define RW_82_PROX_HIGH_DIFF_CFG_CH1_0       0x82
#define RW_83_PROX_HIGH_DIFF_CFG_CH1_1       0x83
#define RW_84_PROX_HIGH_DIFF_CFG_CH2_0       0x84
#define RW_85_PROX_HIGH_DIFF_CFG_CH2_1       0x85
#define RW_86_PROX_HIGH_DIFF_CFG_CH3_0       0x86
#define RW_87_PROX_HIGH_DIFF_CFG_CH3_1       0x87
#define RW_88_PROX_LOW_DIFF_CFG_CH0_0        0x88
#define RW_89_PROX_LOW_DIFF_CFG_CH0_1        0x89
#define RW_8A_PROX_LOW_DIFF_CFG_CH1_0        0x8A
#define RW_8B_PROX_LOW_DIFF_CFG_CH1_1        0x8B
#define RW_8C_PROX_LOW_DIFF_CFG_CH2_0        0x8C
#define RW_8D_PROX_LOW_DIFF_CFG_CH2_1        0x8D
#define RW_8E_PROX_LOW_DIFF_CFG_CH3_0        0x8E
#define RW_8F_PROX_LOW_DIFF_CFG_CH3_1        0x8F
#define RW_9E_PROX_HIGH_DIFF_CFG_CH4_0       0x9E
#define RW_9F_PROX_HIGH_DIFF_CFG_CH4_1       0x9F
#define RW_A2_PROX_LOW_DIFF_CFG_CH4_0        0xA2
#define RW_A3_PROX_LOW_DIFF_CFG_CH4_1        0xA3
#define RW_91_DSP_CONFIG_CTRL4               0x91
#define RW_93_DSP_CONFIG_CTRL6               0x93
#define RW_94_DSP_CONFIG_CTRL7               0x94
#define RW_95_DSP_CONFIG_CTRL8               0x95
#define RW_96_DSP_CONFIG_CTRL9               0x96
#define RW_97_DSP_CONFIG_CTRL10              0x97
#define RW_98_DSP_CONFIG_CTRL11              0x98
#define RW_A0_LP_OUT_DELTA_THRES_CH1_CFG0    0xA0
#define RW_A1_LP_OUT_DELTA_THRES_CH1_CFG1    0xA1
#define RW_A4_LP_OUT_DELTA_THRES_CH3_CFG0    0xA4
#define RW_A5_LP_OUT_DELTA_THRES_CH3_CFG1    0xA5
#define RW_A6_LP_OUT_DELTA_THRES_CH4_CFG0    0xA6
#define RW_A7_LP_OUT_DELTA_THRES_CH4_CFG1    0xA7
#define RW_A8_PROX_THRES_SHIFT_CFG0          0xA8
#define RW_A9_PROX_THRES_SHIFT_CFG1          0xA9
#define RW_AA_PROX_THRES_SHIFT_CFG2          0xAA
#define RW_AB_PROX_THRES_SHIFT_CFG3          0xAB
#define RW_AC_PROX_THRES_SHIFT_CFG4          0xAC
#define RW_AD_BL_IN_NO_UP_NUM_SEL0           0xAD
#define RW_AE_BL_IN_NO_UP_NUM_SEL1           0xAE
#define RW_AF_BL_IN_NO_UP_NUM_SEL2           0xAF
#define RW_B2_BL_ALPHA_UP_DN_SEL             0xB2
#define RW_BF_CH0_SAMP_CFG                   0xBF
#define RW_C0_CH10_SCAN_FACTOR               0xC0
#define RW_C1_CH32_SCAN_FACTOR               0xC1
#define RW_C2_OFFSET_CALI_CTRL               0xC2
#define RW_90_OFFSET_CALI_CTRL1              0x90
#define RW_C3_DSP_CONFIG_CTRL0               0xC3
#define RW_92_DSP_CONFIG_CTRL5               0x92
#define RW_C4_CH10_DOZE_FACTOR               0xC4
#define RW_C5_CH32_DOZE_FACTOR               0xC5
#define RW_C6_CH10_PROX_FACTOR               0xC6
#define RW_C7_CH4_FACTOR_CTRL                0xC7
#define RW_C8_DSP_CONFIG_CTRL1               0xC8
#define RW_C9_DSP_CONFIG_CTRL2               0xC9
#define RW_CA_DSP_CONFIG_CTRL3               0xCA
#define RO_CB_DEC_DATA0                      0xCB
#define RO_CC_DEC_DATA1                      0xCC
#define RO_CD_DEC_DATA2                      0xCD
#define RO_CE_DEC_DATA3                      0xCE
#define RO_E0_CAP_INI_CH0_0                  0xE0
#define RO_E1_CAP_INI_CH0_1                  0xE1
#define RO_99_CAP_INI_CH0_2                  0x99
#define RO_E2_CAP_INI_CH1_0                  0xE2
#define RO_E3_CAP_INI_CH1_1                  0xE3
#define RO_9A_CAP_INI_CH1_2                  0x9A
#define RO_E4_CAP_INI_CH2_0                  0xE4
#define RO_E5_CAP_INI_CH2_1                  0xE5
#define RO_9B_CAP_INI_CH2_2                  0x9B
#define RO_E6_CAP_INI_CH3_0                  0xE6
#define RO_E7_CAP_INI_CH3_1                  0xE7
#define RO_9C_CAP_INI_CH3_2                  0x9C
#define RO_B3_CAP_INI_CH4_0                  0xB3
#define RO_B4_CAP_INI_CH4_1                  0xB4
#define RO_9D_CAP_INI_CH4_2                  0x9D
#define RO_E8_RAW_BL_CH0_0                   0xE8
#define RO_E9_RAW_BL_CH0_1                   0xE9
#define RO_EA_RAW_BL_CH0_2                   0xEA
#define RO_EB_RAW_BL_CH1_0                   0xEB
#define RO_EC_RAW_BL_CH1_1                   0xEC
#define RO_ED_RAW_BL_CH1_2                   0xED
#define RO_EE_RAW_BL_CH2_0                   0xEE
#define RO_EF_RAW_BL_CH2_1                   0xEF
#define RO_F0_RAW_BL_CH2_2                   0xF0
#define RO_F1_RAW_BL_CH3_0                   0xF1
#define RO_F2_RAW_BL_CH3_1                   0xF2
#define RO_F3_RAW_BL_CH3_2                   0xF3
#define RO_B5_RAW_BL_CH4_0                   0xB5
#define RO_B6_RAW_BL_CH4_1                   0xB6
#define RO_B7_RAW_BL_CH4_2                   0xB7
#define RO_F4_LP_DIFF_CH0_0                  0xF4
#define RO_F5_LP_DIFF_CH0_1                  0xF5
#define RO_F6_LP_DIFF_CH0_2                  0xF6
#define RO_F7_LP_DIFF_CH1_0                  0xF7
#define RO_F8_LP_DIFF_CH1_1                  0xF8
#define RO_F9_LP_DIFF_CH1_2                  0xF9
#define RO_FA_LP_DIFF_CH2_0                  0xFA
#define RO_FB_LP_DIFF_CH2_1                  0xFB
#define RO_FC_LP_DIFF_CH2_2                  0xFC
#define RO_FD_LP_DIFF_CH3_0                  0xFD
#define RO_FE_LP_DIFF_CH3_1                  0xFE
#define RO_FF_LP_DIFF_CH3_2                  0xFF
#define RO_B8_LP_DIFF_CH4_0                  0xB8
#define RO_B9_LP_DIFF_CH4_1                  0xB9
#define RO_BA_LP_DIFF_CH4_2                  0xBA
#define RW_50_REG_TO_ANA2                    0x50
#define RW_51_REG_TO_ANA3                    0x51
#define RW_52_REG_TO_ANA4                    0x52
#define RW_53_REG_TO_ANA5                    0x53
#define RW_82_REG_TO_ANA6                    0x82

struct hx9031as_threshold {
	int32_t near;
	int32_t far;
};

struct hx9031as_addr_val_pair {
	uint8_t addr;
	uint8_t val;
};

struct hx9031as_channel_info {
	char name[20];
	bool enabled;
	bool used;
	int state;
};

struct hx9031as_platform_data {
	struct i2c_client *i2c_client;
	struct hx9031as_data *iio_data;
	uint8_t chip_select;
	uint8_t ch_en_stat;
	int polling_period_ms;
	int32_t raw[HX9031AS_CH_NUM];
	int32_t diff[HX9031AS_CH_NUM];
	int32_t lp[HX9031AS_CH_NUM];
	int32_t bl[HX9031AS_CH_NUM];
	uint16_t dac[HX9031AS_CH_NUM];
	uint8_t accuracy;
	atomic_t polling_flag;
	atomic_t irq_en;
	struct hx9031as_threshold thres[HX9031AS_CH_NUM];

	struct device *pdev;
	struct delayed_work polling_work;
	struct hx9031as_channel_info *chs_info;
	uint32_t channel_used_flag;
	int irq;
	int irq_gpio;
	char irq_disabled;
	uint32_t prox_state_reg;
	bool sel_bl[HX9031AS_CH_NUM];
	bool sel_raw[HX9031AS_CH_NUM];
	bool sel_diff[HX9031AS_CH_NUM];
	bool sel_lp[HX9031AS_CH_NUM];

	uint8_t chs_en_flag;
	uint8_t cali_en_flag;
	uint8_t device_id;
	uint8_t version_id;

	struct dentry *debugfs_dir;
};

static struct hx9031as_addr_val_pair hx9031as_reg_init_list[] = {
	{RW_24_CH_NUM_CFG,                 0x00},
	{RW_00_GLOBAL_CTRL0,               0x00},
	{RW_23_GLOBAL_CTRL2,               0x00},

	{RW_02_PRF_CFG,                    0x17},
	{RW_0D_RANGE_7_0,                  0x11},
	{RW_0E_RANGE_9_8,                  0x02},
	{RW_0F_RANGE_18_16,                0x00},

	{RW_10_AVG0_NOSR0_CFG,             0x71},
	{RW_11_NOSR12_CFG,                 0x44},
	{RW_12_NOSR34_CFG,                 0x00},
	{RW_13_AVG12_CFG,                  0x33},
	{RW_14_AVG34_CFG,                  0x00},

	{RW_1F_SAMPLE_NUM_7_0,             0x65},
	{RW_21_INTEGRATION_NUM_7_0,        0x65},

	{RW_2A_LP_ALP_1_0_CFG,             0x22},
	{RW_2B_LP_ALP_3_2_CFG,             0x22},
	{RW_29_LP_ALP_4_CFG,               0x02},
	{RW_2C_UP_ALP_1_0_CFG,             0x88},
	{RW_2D_UP_ALP_3_2_CFG,             0x88},
	{RW_2E_DN_UP_ALP_0_4_CFG,          0x18},
	{RW_2F_DN_ALP_2_1_CFG,             0x11},
	{RW_30_DN_ALP_4_3_CFG,             0x11},

	{RW_38_RAW_BL_RD_CFG,              0xF0},
	{RW_39_INTERRUPT_CFG,              0xFF},
	{RW_3A_INTERRUPT_CFG1,             0x3B},
	{RW_3B_CALI_DIFF_CFG,              0x07},
	{RW_3C_DITHER_CFG,                 0x21},
	{RW_6C_PROX_INT_HIGH_CFG,          0x01},
	{RW_6D_PROX_INT_LOW_CFG,           0x01},

	{RW_80_PROX_HIGH_DIFF_CFG_CH0_0,   0x40},
	{RW_81_PROX_HIGH_DIFF_CFG_CH0_1,   0x00},
	{RW_82_PROX_HIGH_DIFF_CFG_CH1_0,   0x40},
	{RW_83_PROX_HIGH_DIFF_CFG_CH1_1,   0x00},
	{RW_84_PROX_HIGH_DIFF_CFG_CH2_0,   0x40},
	{RW_85_PROX_HIGH_DIFF_CFG_CH2_1,   0x00},
	{RW_86_PROX_HIGH_DIFF_CFG_CH3_0,   0x40},
	{RW_87_PROX_HIGH_DIFF_CFG_CH3_1,   0x00},
	{RW_9E_PROX_HIGH_DIFF_CFG_CH4_0,   0x40},
	{RW_9F_PROX_HIGH_DIFF_CFG_CH4_1,   0x00},
	{RW_88_PROX_LOW_DIFF_CFG_CH0_0,    0x20},
	{RW_89_PROX_LOW_DIFF_CFG_CH0_1,    0x00},
	{RW_8A_PROX_LOW_DIFF_CFG_CH1_0,    0x20},
	{RW_8B_PROX_LOW_DIFF_CFG_CH1_1,    0x00},
	{RW_8C_PROX_LOW_DIFF_CFG_CH2_0,    0x20},
	{RW_8D_PROX_LOW_DIFF_CFG_CH2_1,    0x00},
	{RW_8E_PROX_LOW_DIFF_CFG_CH3_0,    0x20},
	{RW_8F_PROX_LOW_DIFF_CFG_CH3_1,    0x00},
	{RW_A2_PROX_LOW_DIFF_CFG_CH4_0,    0x20},
	{RW_A3_PROX_LOW_DIFF_CFG_CH4_1,    0x00},

	{RW_A8_PROX_THRES_SHIFT_CFG0,      0x00},
	{RW_A9_PROX_THRES_SHIFT_CFG1,      0x00},
	{RW_AA_PROX_THRES_SHIFT_CFG2,      0x00},
	{RW_AB_PROX_THRES_SHIFT_CFG3,      0x00},
	{RW_AC_PROX_THRES_SHIFT_CFG4,      0x00},

	{RW_C0_CH10_SCAN_FACTOR,           0x00},
	{RW_C1_CH32_SCAN_FACTOR,           0x00},
	{RW_C4_CH10_DOZE_FACTOR,           0x00},
	{RW_C5_CH32_DOZE_FACTOR,           0x00},
	{RW_C7_CH4_FACTOR_CTRL,            0x00},
	{RW_C8_DSP_CONFIG_CTRL1,           0x00},
	{RW_CA_DSP_CONFIG_CTRL3,           0x00},
};

static struct hx9031as_platform_data hx9031as_pdata = {
	.i2c_client = NULL,
	.ch_en_stat = 0x00,
	.polling_period_ms = 0,
	.accuracy = 16,
	.polling_flag = ATOMIC_INIT(0),
	.irq_en = ATOMIC_INIT(0),
	.thres = {
		{.near = 320, .far = 320},
		{.near = 320, .far = 320},
		{.near = 640, .far = 640},
		{.near = 640, .far = 640},
		{.near = 960, .far = 960}
	}
};

static DEFINE_MUTEX(hx9031as_ch_en_mutex);
static DEFINE_MUTEX(hx9031as_cali_mutex);

struct hx9031as_data {
	struct mutex mutex;
	struct i2c_client *client;
	struct iio_trigger *trig;
	struct regmap *regmap;
	struct regulator_bulk_data supplies[1];
	unsigned long chan_prox_stat;
	bool trigger_enabled;
	struct {
		__be16 channels[HX9031AS_CH_NUM];

		s64 ts __aligned(8);

	} buffer;
	unsigned long chan_read;
	unsigned long chan_event;  //channel en bit
};

static const struct iio_event_spec hx9031as_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

#define HX9031AS_NAMED_CHANNEL(idx, name)                    \
{                                                            \
	.type = IIO_PROXIMITY,                                   \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),            \
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.indexed = 1,                                            \
	.channel = idx,                                          \
	.extend_name = name,                                     \
	.address = 0,                                            \
	.event_spec = hx9031as_events,                           \
	.num_event_specs = ARRAY_SIZE(hx9031as_events),          \
	.scan_index = idx,                                       \
	.scan_type = {                                           \
		.sign = 's',                                         \
		.realbits = 12,                                      \
		.storagebits = 16,                                   \
		.endianness = IIO_BE,                                \
	},                                                       \
}

static const struct iio_chan_spec hx9031as_channels[] = {
	HX9031AS_NAMED_CHANNEL(0, "ch0"),
	HX9031AS_NAMED_CHANNEL(1, "ch1"),
	HX9031AS_NAMED_CHANNEL(2, "ch2"),
	HX9031AS_NAMED_CHANNEL(3, "ch3"),
	HX9031AS_NAMED_CHANNEL(4, "ch4"),
	IIO_CHAN_SOFT_TIMESTAMP(5),
};

static const uint32_t hx9031as_samp_freq_table[] = {
	2, 2, 4, 6, 8, 10, 14, 18, 22, 26,
	30, 34, 38, 42, 46, 50, 56, 62, 68, 74,
	80, 90, 100, 200, 300, 400, 600, 800, 1000, 2000,
	3000, 4000
};

static const struct regmap_config hx9031as_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_NONE,
};

static int hx9031as_read(uint8_t addr, uint8_t *rxbuf, int count)
{
	return regmap_bulk_read(hx9031as_pdata.iio_data->regmap, addr, rxbuf, count);
}

static int hx9031as_write(uint8_t addr, uint8_t *txbuf, int count)
{
	return regmap_bulk_write(hx9031as_pdata.iio_data->regmap, addr, txbuf, count);
}

static void hx9031as_data_lock(uint8_t lock_flag)
{
	int ret = -1;
	uint8_t rx_buf[1] = {0};

	if (lock_flag == HX9031AS_DATA_LOCK) {
		ret = hx9031as_read(RW_C8_DSP_CONFIG_CTRL1, rx_buf, 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_read failed\n");

		rx_buf[0] = rx_buf[0] | 0x10;
		ret = hx9031as_write(RW_C8_DSP_CONFIG_CTRL1, rx_buf, 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	} else if (lock_flag == HX9031AS_DATA_UNLOCK) {
		ret = hx9031as_read(RW_C8_DSP_CONFIG_CTRL1, rx_buf, 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_read failed\n");

		rx_buf[0] = rx_buf[0] & 0xE7;
		ret = hx9031as_write(RW_C8_DSP_CONFIG_CTRL1, rx_buf, 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	} else {
		PRINT_ERR("ERROR!!! wrong para. now do data unlock!\n");
		ret = hx9031as_read(RW_C8_DSP_CONFIG_CTRL1, rx_buf, 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_read failed\n");

		rx_buf[0] = rx_buf[0] & 0xE7;
		ret = hx9031as_write(RW_C8_DSP_CONFIG_CTRL1, rx_buf, 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	}
}

static int hx9031as_id_check(void)
{
	int ret = -1;
	uint8_t rxbuf[1] = {0};

	ret = hx9031as_read(RO_60_DEVICE_ID, rxbuf, 1);
	if (ret < 0) {
		PRINT_ERR("hx9031as_read failed\n");
		return ret;
	}
	hx9031as_pdata.device_id = rxbuf[0];
	rxbuf[0] = 0;

	if (hx9031as_pdata.device_id == HX9031AS_CHIP_ID) {
		ret = hx9031as_read(RO_5F_VERION_ID, rxbuf, 1);
		if (ret < 0)
			PRINT_ERR("hx9031as_read failed\n");
		hx9031as_pdata.version_id = rxbuf[0];
		PRINT_INF("success! device_id=0x%02X(HX9031AS) version_id=0x%02X\n",
				hx9031as_pdata.device_id, hx9031as_pdata.version_id);
	} else {
		PRINT_ERR("failed! device_id=0x%02X(UNKNOW_CHIP_ID) version_id=0x%02X\n",
				hx9031as_pdata.device_id, hx9031as_pdata.version_id);
		return -1;
	}
	return 0;
}

static void hx9031as_ch_cfg(uint8_t chip_select)
{
	int ret = -1;
	int ii = 0;
	uint16_t ch_cfg = 0;
	uint8_t cfg[HX9031AS_CH_NUM * 2] = {0};

	uint8_t cs0 = 0;
	uint8_t cs1 = 0;
	uint8_t cs2 = 0;
	uint8_t cs3 = 0;
	uint8_t cs4 = 0;
	uint8_t na = 16;
	uint8_t ch0_pos = na;
	uint8_t ch0_neg = na;
	uint8_t ch1_pos = na;
	uint8_t ch1_neg = na;
	uint8_t ch2_pos = na;
	uint8_t ch2_neg = na;
	uint8_t ch3_pos = na;
	uint8_t ch3_neg = na;
	uint8_t ch4_pos = na;
	uint8_t ch4_neg = na;

	ENTER;
	if (chip_select == HX9023S_ON_BOARD) {
		cs0 = 0; //Lshift0
		cs1 = 2; //Lshift2
		cs2 = 4; //Lshift4
		cs3 = 6; //Lshift6
		cs4 = 8; //Lshift8
		na = 16; //Lshift16
		PRINT_INF("HX9023S_ON_BOARD\n");
	} else if (chip_select == HX9031AS_ON_BOARD) {
		cs0 = 4; //Lshift4
		cs1 = 2; //Lshift2
		cs2 = 6; //Lshift6
		cs3 = 0; //Lshift0
		cs4 = 8; //Lshift8
		na = 16; //Lshift16
		PRINT_INF("HX9031AS_ON_BOARD\n");
	}

	ch0_pos = cs0;
	ch0_neg = na;
	ch1_pos = cs1;
	ch1_neg = na;
	ch2_pos = cs2;
	ch2_neg = na;
	ch3_pos = cs3;
	ch3_neg = na;
	ch4_pos = cs4;
	ch4_neg = na;

	ch_cfg = (uint16_t)((0x03 << ch0_pos) + (0x02 << ch0_neg));
	cfg[ii++] = (uint8_t)(ch_cfg);
	cfg[ii++] = (uint8_t)(ch_cfg >> 8);

	ch_cfg = (uint16_t)((0x03 << ch1_pos) + (0x02 << ch1_neg));
	cfg[ii++] = (uint8_t)(ch_cfg);
	cfg[ii++] = (uint8_t)(ch_cfg >> 8);

	ch_cfg = (uint16_t)((0x03 << ch2_pos) + (0x02 << ch2_neg));
	cfg[ii++] = (uint8_t)(ch_cfg);
	cfg[ii++] = (uint8_t)(ch_cfg >> 8);

	ch_cfg = (uint16_t)((0x03 << ch3_pos) + (0x02 << ch3_neg));
	cfg[ii++] = (uint8_t)(ch_cfg);
	cfg[ii++] = (uint8_t)(ch_cfg >> 8);

	ch_cfg = (uint16_t)((0x03 << ch4_pos) + (0x02 << ch4_neg));
	cfg[ii++] = (uint8_t)(ch_cfg);
	cfg[ii++] = (uint8_t)(ch_cfg >> 8);

	ret = hx9031as_write(RW_03_CH0_CFG_7_0, cfg, HX9031AS_CH_NUM * 2);
	if (ret != 0)
		PRINT_ERR("hx9031as_write failed\n");
}

static void hx9031as_reg_init(void)
{
	int ii = 0;
	int ret = -1;

	while (ii < (int)ARRAY_SIZE(hx9031as_reg_init_list)) {
		ret = hx9031as_write(hx9031as_reg_init_list[ii].addr, &hx9031as_reg_init_list[ii].val, 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
		ii++;
	}
}

static void hx9031as_read_offset_dac(void)
{
	int ret = -1;
	int ii = 0;
	uint8_t bytes_per_channel = 0;
	uint8_t bytes_all_channels = 0;
	uint8_t rx_buf[HX9031AS_CH_NUM * CH_DATA_BYTES_MAX] = {0};
	uint32_t data = 0;

	hx9031as_data_lock(HX9031AS_DATA_LOCK);
	bytes_per_channel = CH_DATA_2BYTES;
	bytes_all_channels = HX9031AS_CH_NUM * bytes_per_channel;
	ret = hx9031as_read(RW_15_OFFSET_DAC0_7_0, rx_buf, bytes_all_channels);
	if (ret == 0) {
		for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
			data = ((rx_buf[ii * bytes_per_channel + 1] << 8) | (rx_buf[ii * bytes_per_channel]));
			data = data & 0xFFF;//12位
			hx9031as_pdata.dac[ii] = data;
		}
	}
	hx9031as_data_lock(HX9031AS_DATA_UNLOCK);

	PRINT_DBG("OFFSET_DAC, %-8d, %-8d, %-8d, %-8d, %-8d\n",
			hx9031as_pdata.dac[0], hx9031as_pdata.dac[1], hx9031as_pdata.dac[2],
			hx9031as_pdata.dac[3], hx9031as_pdata.dac[4]);
}

static void hx9031as_manual_offset_calibration(uint8_t ch_id)
{
	int ret = -1;
	uint8_t buf[2] = {0};

	mutex_lock(&hx9031as_cali_mutex);
	ret = hx9031as_read(RW_C2_OFFSET_CALI_CTRL, &buf[0], 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");
	ret = hx9031as_read(RW_90_OFFSET_CALI_CTRL1, &buf[1], 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");

	if (ch_id < 4) {
		buf[0] |= (1 << (ch_id + 4));
		ret = hx9031as_write(RW_C2_OFFSET_CALI_CTRL, &buf[0], 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	} else {
		buf[1] |= 0x10;
		ret = hx9031as_write(RW_90_OFFSET_CALI_CTRL1, &buf[1], 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	}

	PRINT_INF("ch_%d will calibrate in next convert cycle (ODR=%dms)\n", ch_id, HX9031AS_ODR_MS);
	TYHX_DELAY_MS(HX9031AS_ODR_MS);
	mutex_unlock(&hx9031as_cali_mutex);
}

static void hx9031as_manual_offset_calibration_all_chs(void)
{
	int ret = -1;
	uint8_t buf[2] = {0};

	mutex_lock(&hx9031as_cali_mutex);
	ret = hx9031as_read(RW_C2_OFFSET_CALI_CTRL, &buf[0], 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");
	ret = hx9031as_read(RW_90_OFFSET_CALI_CTRL1, &buf[1], 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");

	buf[0] |= 0xF0;
	buf[1] |= 0x10;

	ret = hx9031as_write(RW_C2_OFFSET_CALI_CTRL, &buf[0], 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_write failed\n");
	ret = hx9031as_write(RW_90_OFFSET_CALI_CTRL1, &buf[1], 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_write failed\n");

	PRINT_INF("channels will calibrate in next convert cycle (ODR=%dms)\n", HX9031AS_ODR_MS);
	TYHX_DELAY_MS(HX9031AS_ODR_MS);
	mutex_unlock(&hx9031as_cali_mutex);
}

static int32_t hx9031as_get_thres_near(uint8_t ch)
{
	int ret = -1;
	uint8_t buf[2] = {0};

	if (ch == 4) {
		ret = hx9031as_read(RW_9E_PROX_HIGH_DIFF_CFG_CH4_0, buf, 2);
		if (ret != 0)
			PRINT_ERR("hx9031as_read failed\n");
	} else {
		ret = hx9031as_read(RW_80_PROX_HIGH_DIFF_CFG_CH0_0 + (ch * CH_DATA_2BYTES), buf, 2);
		if (ret != 0)
			PRINT_ERR("hx9031as_read failed\n");
	}

	hx9031as_pdata.thres[ch].near = (buf[0] + ((buf[1] & 0x03) << 8)) * 32;
	PRINT_INF("hx9031as_pdata.thres[%d].near=%d\n", ch, hx9031as_pdata.thres[ch].near);
	return hx9031as_pdata.thres[ch].near;
}

static int32_t hx9031as_get_thres_far(uint8_t ch)
{
	int ret = -1;
	uint8_t buf[2] = {0};

	if (ch == 4) {
		ret = hx9031as_read(RW_A2_PROX_LOW_DIFF_CFG_CH4_0, buf, 2);
		if (ret != 0)
			PRINT_ERR("hx9031as_read failed\n");
	} else {
		ret = hx9031as_read(RW_88_PROX_LOW_DIFF_CFG_CH0_0 + (ch * CH_DATA_2BYTES), buf, 2);
		if (ret != 0)
			PRINT_ERR("hx9031as_read failed\n");
	}

	hx9031as_pdata.thres[ch].far = (buf[0] + ((buf[1] & 0x03) << 8)) * 32;
	PRINT_INF("hx9031as_pdata.thres[%d].far=%d\n", ch, hx9031as_pdata.thres[ch].far);
	return hx9031as_pdata.thres[ch].far;
}

static int32_t hx9031as_set_thres_near(uint8_t ch, int32_t val)
{
	int ret = -1;
	uint8_t buf[2];

	val /= 32;
	buf[0] = val & 0xFF;
	buf[1] = (val >> 8) & 0x03;
	hx9031as_pdata.thres[ch].near = (val & 0x03FF) * 32;

	if (ch == 4) {
		ret = hx9031as_write(RW_9E_PROX_HIGH_DIFF_CFG_CH4_0, buf, 2);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	} else {
		ret = hx9031as_write(RW_80_PROX_HIGH_DIFF_CFG_CH0_0 + (ch * CH_DATA_2BYTES), buf, 2);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	}

	PRINT_INF("hx9031as_pdata.thres[%d].near=%d\n", ch, hx9031as_pdata.thres[ch].near);
	return hx9031as_pdata.thres[ch].near;
}

static int32_t hx9031as_set_thres_far(uint8_t ch, int32_t val)
{
	int ret = -1;
	uint8_t buf[2];

	val /= 32;
	buf[0] = val & 0xFF;
	buf[1] = (val >> 8) & 0x03;
	hx9031as_pdata.thres[ch].far = (val & 0x03FF) * 32;

	if (ch == 4) {
		ret = hx9031as_write(RW_A2_PROX_LOW_DIFF_CFG_CH4_0, buf, 2);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	} else {
		ret = hx9031as_write(RW_88_PROX_LOW_DIFF_CFG_CH0_0 + (ch * CH_DATA_2BYTES), buf, 2);
		if (ret != 0)
			PRINT_ERR("hx9031as_write failed\n");
	}

	PRINT_INF("hx9031as_pdata.thres[%d].far=%d\n", ch, hx9031as_pdata.thres[ch].far);
	return hx9031as_pdata.thres[ch].far;
}

static void hx9031as_get_prox_state(void)
{
	int ret = -1;
	uint8_t buf[1] = {0};

	hx9031as_pdata.prox_state_reg = 0;
	ret = hx9031as_read(RO_6B_PROX_STATUS, buf, 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");
	hx9031as_pdata.prox_state_reg = buf[0];

	PRINT_INF("prox_state_reg=0x%02X\n", hx9031as_pdata.prox_state_reg);
}

static void hx9031as_data_select(void)
{
	int ret = -1;
	int ii = 0;
	uint8_t buf[1] = {0};

	ret = hx9031as_read(RW_38_RAW_BL_RD_CFG, buf, 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");

	for (ii = 0; ii < 4; ii++) { //ch0~sh3
		hx9031as_pdata.sel_diff[ii] = buf[0] & (0x01 << ii);
		hx9031as_pdata.sel_lp[ii] = !hx9031as_pdata.sel_diff[ii];
		hx9031as_pdata.sel_bl[ii] = buf[0] & (0x10 << ii);
		hx9031as_pdata.sel_raw[ii] = !hx9031as_pdata.sel_bl[ii];
	}

	ret = hx9031as_read(RW_3A_INTERRUPT_CFG1, buf, 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");

	//ch4
	hx9031as_pdata.sel_diff[4] = buf[0] & (0x01 << 2);
	hx9031as_pdata.sel_lp[4] = !hx9031as_pdata.sel_diff[4];
	hx9031as_pdata.sel_bl[4] = buf[0] & (0x01 << 3);
	hx9031as_pdata.sel_raw[4] = !hx9031as_pdata.sel_bl[4];
}

static void hx9031as_sample(void)
{
	int ret = -1;
	int ii = 0;
	uint8_t bytes_per_channel = 0;
	uint8_t bytes_all_channels = 0;
	uint8_t rx_buf[HX9031AS_CH_NUM * CH_DATA_BYTES_MAX] = {0};
	int32_t data = 0;

	hx9031as_data_lock(HX9031AS_DATA_LOCK);
	hx9031as_data_select();

	bytes_per_channel = CH_DATA_3BYTES;
	bytes_all_channels = HX9031AS_CH_NUM * bytes_per_channel;
	ret = hx9031as_read(RO_E8_RAW_BL_CH0_0, rx_buf, bytes_all_channels - bytes_per_channel);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");
	ret = hx9031as_read(RO_B5_RAW_BL_CH4_0, rx_buf + (bytes_all_channels - bytes_per_channel), bytes_per_channel);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");
	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		if (hx9031as_pdata.accuracy == 16) {
			data = ((rx_buf[ii * bytes_per_channel + 2] << 8) | (rx_buf[ii * bytes_per_channel + 1]));
			data = (data > 0x7FFF) ? (data - (0xFFFF + 1)) : data;
		} else {
			data = ((rx_buf[ii * bytes_per_channel + 2] << 16) | (rx_buf[ii * bytes_per_channel + 1] << 8)
					| (rx_buf[ii * bytes_per_channel]));
			data = (data > 0x7FFFFF) ? (data - (0xFFFFFF + 1)) : data;
		}
		hx9031as_pdata.raw[ii] = 0;
		hx9031as_pdata.bl[ii] = 0;
		if (true == hx9031as_pdata.sel_raw[ii])
			hx9031as_pdata.raw[ii] = data;
		if (true == hx9031as_pdata.sel_bl[ii])
			hx9031as_pdata.bl[ii] = data;
	}

	bytes_per_channel = CH_DATA_3BYTES;
	bytes_all_channels = HX9031AS_CH_NUM * bytes_per_channel;
	ret = hx9031as_read(RO_F4_LP_DIFF_CH0_0, rx_buf, bytes_all_channels - bytes_per_channel);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");
	ret = hx9031as_read(RO_B8_LP_DIFF_CH4_0, rx_buf + (bytes_all_channels - bytes_per_channel), bytes_per_channel);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");
	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		if (hx9031as_pdata.accuracy == 16) {
			data = ((rx_buf[ii * bytes_per_channel + 2] << 8) | (rx_buf[ii * bytes_per_channel + 1]));
			data = (data > 0x7FFF) ? (data - (0xFFFF + 1)) : data;
		} else {
			data = ((rx_buf[ii * bytes_per_channel + 2] << 16) | (rx_buf[ii * bytes_per_channel + 1] << 8)
					| (rx_buf[ii * bytes_per_channel]));
			data = (data > 0x7FFFFF) ? (data - (0xFFFFFF + 1)) : data;
		}
		hx9031as_pdata.lp[ii] = 0;
		hx9031as_pdata.diff[ii] = 0;
		if (true == hx9031as_pdata.sel_lp[ii])
			hx9031as_pdata.lp[ii] = data;
		if (true == hx9031as_pdata.sel_diff[ii])
			hx9031as_pdata.diff[ii] = data;
	}

	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		if (true == hx9031as_pdata.sel_lp[ii] && true == hx9031as_pdata.sel_bl[ii])
			hx9031as_pdata.diff[ii] = hx9031as_pdata.lp[ii] - hx9031as_pdata.bl[ii];
	}

	bytes_per_channel = CH_DATA_2BYTES;
	bytes_all_channels = HX9031AS_CH_NUM * bytes_per_channel;
	ret = hx9031as_read(RW_15_OFFSET_DAC0_7_0, rx_buf, bytes_all_channels);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");
	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		data = ((rx_buf[ii * bytes_per_channel + 1] << 8) | (rx_buf[ii * bytes_per_channel]));
		data = data & 0xFFF;//12位
		hx9031as_pdata.dac[ii] = data;
	}

	hx9031as_data_lock(HX9031AS_DATA_UNLOCK);

	PRINT_DBG("accuracy=%d\n", hx9031as_pdata.accuracy);
	PRINT_DBG("DIFF  , %-8d, %-8d, %-8d, %-8d, %-8d\n",
				hx9031as_pdata.diff[0], hx9031as_pdata.diff[1], hx9031as_pdata.diff[2],
				hx9031as_pdata.diff[3], hx9031as_pdata.diff[4]);
	PRINT_DBG("RAW   , %-8d, %-8d, %-8d, %-8d, %-8d\n",
				hx9031as_pdata.raw[0], hx9031as_pdata.raw[1], hx9031as_pdata.raw[2],
				hx9031as_pdata.raw[3], hx9031as_pdata.raw[4]);
	PRINT_DBG("OFFSET, %-8d, %-8d, %-8d, %-8d, %-8d\n",
				hx9031as_pdata.dac[0], hx9031as_pdata.dac[1], hx9031as_pdata.dac[2],
				hx9031as_pdata.dac[3], hx9031as_pdata.dac[4]);
	PRINT_DBG("BL    , %-8d, %-8d, %-8d, %-8d, %-8d\n",
				hx9031as_pdata.bl[0], hx9031as_pdata.bl[1], hx9031as_pdata.bl[2],
				hx9031as_pdata.bl[3], hx9031as_pdata.bl[4]);
	PRINT_DBG("LP    , %-8d, %-8d, %-8d, %-8d, %-8d\n",
				hx9031as_pdata.lp[0], hx9031as_pdata.lp[1], hx9031as_pdata.lp[2],
				hx9031as_pdata.lp[3], hx9031as_pdata.lp[4]);
}

static void hx9031as_disable_irq(unsigned int irq)
{
	if (irq == 0) {
		PRINT_ERR("wrong irq number!\n");
		return;
	}

	if (atomic_read(&hx9031as_pdata.irq_en) == 1) {
		disable_irq_nosync(hx9031as_pdata.irq);
		atomic_set(&hx9031as_pdata.irq_en, 0);
		PRINT_DBG("irq_%d is disabled!\n", irq);
	} else {
		PRINT_ERR("irq_%d is disabled already!\n", irq);
	}
}

static void hx9031as_enable_irq(unsigned int irq)
{
	if (irq == 0) {
		PRINT_ERR("wrong irq number!\n");
		return;
	}

	if (atomic_read(&hx9031as_pdata.irq_en) == 0) {
		enable_irq(hx9031as_pdata.irq);
		atomic_set(&hx9031as_pdata.irq_en, 1);
		PRINT_DBG("irq_%d is enabled!\n", irq);
	} else {
		PRINT_ERR("irq_%d is enabled already!\n", irq);
	}
}

#if HX9031AS_TEST_CHS_EN
static int hx9031as_ch_en(uint8_t ch_id, uint8_t en)
{
	int ret = -1;
	uint8_t tx_buf[1] = {0};

	en = !!en;
	if (ch_id >= HX9031AS_CH_NUM) {
		PRINT_ERR("channel index over range!!! hx9031as_pdata.ch_en_stat=0x%02X (ch_id=%d, en=%d)\n",
					hx9031as_pdata.ch_en_stat, ch_id, en);
		return -1;
	}

	if (en == 1) {
		if (hx9031as_pdata.ch_en_stat == 0) {
			hx9031as_pdata.prox_state_reg = 0;
			tx_buf[0] = hx9031as_pdata.channel_used_flag;
			ret = hx9031as_write(RW_24_CH_NUM_CFG, tx_buf, 1);
			if (ret != 0) {
				PRINT_ERR("hx9031as_write failed\n");
				return -1;
			}
		}
		hx9031as_pdata.ch_en_stat |= (1 << ch_id);
		PRINT_INF("hx9031as_pdata.ch_en_stat=0x%02X (ch_%d enabled)\n", hx9031as_pdata.ch_en_stat, ch_id);
	} else {
		hx9031as_pdata.ch_en_stat &= ~(1 << ch_id);
		if (hx9031as_pdata.ch_en_stat == 0) {
			tx_buf[0] = 0x00;
			ret = hx9031as_write(RW_24_CH_NUM_CFG, tx_buf, 1);
			if (ret != 0) {
				PRINT_ERR("hx9031as_write failed\n");
				return -1;
			}
		}
		PRINT_INF("hx9031as_pdata.ch_en_stat=0x%02X (ch_%d disabled)\n", hx9031as_pdata.ch_en_stat, ch_id);
	}
	return 0;
}

#else

static int hx9031as_ch_en(uint8_t ch_id, uint8_t en)
{
	int ret = -1;
	uint8_t rx_buf[1] = {0};
	uint8_t tx_buf[1] = {0};

	en = !!en;
	if (ch_id >= HX9031AS_CH_NUM) {
		PRINT_ERR("channel index over range!!! hx9031as_pdata.ch_en_stat=0x%02X (ch_id=%d, en=%d)\n",
					hx9031as_pdata.ch_en_stat, ch_id, en);
		return -1;
	}

	ret = hx9031as_read(RW_24_CH_NUM_CFG, rx_buf, 1);
	if (ret != 0) {
		PRINT_ERR("hx9031as_read failed\n");
		return -1;
	}
	hx9031as_pdata.ch_en_stat = rx_buf[0];

	if (en == 1) {
		if (hx9031as_pdata.ch_en_stat == 0)
			hx9031as_pdata.prox_state_reg = 0;
		hx9031as_pdata.ch_en_stat |= (1 << ch_id);
		tx_buf[0] = hx9031as_pdata.ch_en_stat;
		ret = hx9031as_write(RW_24_CH_NUM_CFG, tx_buf, 1);
		if (ret != 0) {
			PRINT_ERR("hx9031as_write failed\n");
			return -1;
		}
		PRINT_INF("hx9031as_pdata.ch_en_stat=0x%02X (ch_%d enabled)\n", hx9031as_pdata.ch_en_stat, ch_id);
		TYHX_DELAY_MS(10);
	} else {
		hx9031as_pdata.ch_en_stat &= ~(1 << ch_id);
		tx_buf[0] = hx9031as_pdata.ch_en_stat;
		ret = hx9031as_write(RW_24_CH_NUM_CFG, tx_buf, 1);
		if (ret != 0) {
			PRINT_ERR("hx9031as_write failed\n");
			return -1;
		}
		PRINT_INF("hx9031as_pdata.ch_en_stat=0x%02X (ch_%d disabled)\n", hx9031as_pdata.ch_en_stat, ch_id);
	}
	return 0;
}
#endif

static int hx9031as_ch_en_hal(uint8_t ch_id, uint8_t enable)
{
	int ret = -1;

	mutex_lock(&hx9031as_ch_en_mutex);
	if (enable == 1) {
		PRINT_INF("enable ch_%d(name:%s)\n", ch_id, hx9031as_pdata.chs_info[ch_id].name);
		ret = hx9031as_ch_en(ch_id, 1);
		if (ret != 0) {
			PRINT_ERR("hx9031as_ch_en failed\n");
			mutex_unlock(&hx9031as_ch_en_mutex);
			return -1;
		}
		hx9031as_pdata.chs_info[ch_id].state = 0;
		hx9031as_pdata.chs_info[ch_id].enabled = true;
	} else if (enable == 0) {
		PRINT_INF("disable ch_%d(name:%s)\n", ch_id, hx9031as_pdata.chs_info[ch_id].name);
		ret = hx9031as_ch_en(ch_id, 0);
		if (ret != 0) {
			PRINT_ERR("hx9031as_ch_en failed\n");
			mutex_unlock(&hx9031as_ch_en_mutex);
			return -1;
		}
		hx9031as_pdata.chs_info[ch_id].state = 0;
		hx9031as_pdata.chs_info[ch_id].enabled = false;
	} else {
		PRINT_ERR("unknown enable symbol\n");
	}
	mutex_unlock(&hx9031as_ch_en_mutex);

	return 0;
}

static void hx9031as_polling_work_func(struct work_struct *work)
{
	ENTER;
	mutex_lock(&hx9031as_ch_en_mutex);
	hx9031as_sample();
	hx9031as_get_prox_state();

	if (atomic_read(&hx9031as_pdata.polling_flag) == 1)
		schedule_delayed_work(&hx9031as_pdata.polling_work, msecs_to_jiffies(hx9031as_pdata.polling_period_ms));
	mutex_unlock(&hx9031as_ch_en_mutex);
}

static ssize_t hx9031as_raw_data_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[BUF_SIZE] = {0};
	char *p = buf;
	int ii = 0;

	hx9031as_sample();
	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		p += snprintf(p, PAGE_SIZE, "ch[%d]: DIFF=%-8d, RAW=%-8d, OFFSET=%-8d, BL=%-8d, LP=%-8d\n",
						ii, hx9031as_pdata.diff[ii], hx9031as_pdata.raw[ii], hx9031as_pdata.dac[ii],
						hx9031as_pdata.bl[ii], hx9031as_pdata.lp[ii]);
	}

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static const struct file_operations hx9031as_raw_data_fops = {
	.read = hx9031as_raw_data_show,
};

static ssize_t hx9031as_reg_write_store(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
	int ret = -1;
	unsigned int reg_address = 0;
	unsigned int val = 0;
	uint8_t addr = 0;
	uint8_t tx_buf[1] = {0};
	char buf[BUF_SIZE];

	ENTER;
	if (count > BUF_SIZE)
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (sscanf(buf, "%x,%x", &reg_address, &val) != 2) {
		PRINT_ERR("please input two HEX numbers: aa,bb (aa: reg_address, bb: value_to_be_set)\n");
		return -EINVAL;
	}

	addr = (uint8_t)reg_address;
	tx_buf[0] = (uint8_t)val;

	ret = hx9031as_write(addr, tx_buf, 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_write failed\n");

	PRINT_INF("WRITE:Reg0x%02X=0x%02X\n", addr, tx_buf[0]);
	return count;
}

static const struct file_operations hx9031as_reg_write_fops = {
	.write = hx9031as_reg_write_store,
};

static ssize_t hx9031as_reg_read_store(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
	int ret = -1;
	unsigned long reg_address = 0;
	uint8_t addr = 0;
	uint8_t rx_buf[1] = {0};
	char buf[BUF_SIZE];

	ENTER;
	if (count > BUF_SIZE)
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (kstrtoul(buf, 16, &reg_address)) {
		PRINT_ERR("please input a HEX number\n");
		return -EINVAL;
	}
	addr = (uint8_t)reg_address;

	ret = hx9031as_read(addr, rx_buf, 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");

	PRINT_INF("READ:Reg0x%02X=0x%02X\n", addr, rx_buf[0]);
	return count;
}

static const struct file_operations hx9031as_reg_read_fops = {
	.write = hx9031as_reg_read_store,
};

static ssize_t hx9031as_channel_en_store(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
	int ch_id = 0;
	int en = 0;
	char buf[BUF_SIZE];

	ENTER;
	if (count > BUF_SIZE)
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (sscanf(buf, "%d,%d", &ch_id, &en) != 2) {
		PRINT_ERR("please input two DEC numbers: ch_id,en (ch_id: channel number, en: 1=enable, 0=disable)\n");
		return -EINVAL;
	}

	if ((ch_id >= HX9031AS_CH_NUM) || (ch_id < 0)) {
		PRINT_ERR("channel number out of range, the effective number is 0~%d\n", HX9031AS_CH_NUM - 1);
		return -EINVAL;
	}

	if ((hx9031as_pdata.channel_used_flag >> ch_id) & 0x01)
		hx9031as_ch_en_hal(ch_id, (en > 0) ? 1 : 0);
	else
		PRINT_ERR("ch_%d is unused, you can not enable or disable an unused channel\n", ch_id);

	return count;
}

static ssize_t hx9031as_channel_en_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[BUF_SIZE] = {0};
	char *p = buf;
	int ii = 0;

	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		if ((hx9031as_pdata.channel_used_flag >> ii) & 0x1) {
			PRINT_INF("hx9031as_pdata.chs_info[%d].enabled=%d\n",
						ii, hx9031as_pdata.chs_info[ii].enabled);
			p += snprintf(p, PAGE_SIZE, "hx9031as_pdata.chs_info[%d].enabled=%d\n",
						ii, hx9031as_pdata.chs_info[ii].enabled);
		}
	}

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static const struct file_operations hx9031as_channel_en_fops = {
	.write = hx9031as_channel_en_store,
	.read = hx9031as_channel_en_show,
};

static ssize_t hx9031as_manual_offset_calibration_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[BUF_SIZE] = {0};

	hx9031as_read_offset_dac();
	snprintf(buf, sizeof(buf), "OFFSET_DAC, %-8d, %-8d, %-8d, %-8d, %-8d\n",
				hx9031as_pdata.dac[0], hx9031as_pdata.dac[1], hx9031as_pdata.dac[2],
				hx9031as_pdata.dac[3], hx9031as_pdata.dac[4]);

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static ssize_t hx9031as_manual_offset_calibration_store(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
	unsigned long val;
	uint8_t ch_id = 0;
	char buf[BUF_SIZE];

	ENTER;
	if (count > BUF_SIZE)
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (kstrtoul(buf, 10, &val)) {
		PRINT_ERR("Invalid Argument\n");
		return -EINVAL;
	}
	ch_id = (uint8_t)val;

	if (ch_id == 99) {
		PRINT_INF("you are enter the calibration test mode, all channels will be calibrated\n");
		hx9031as_manual_offset_calibration_all_chs();
		return count;
	}

	if (ch_id < HX9031AS_CH_NUM)
		hx9031as_manual_offset_calibration(ch_id);
	else
		PRINT_ERR(" \"echo ch_id > calibrate\" to do a manual calibrate(ch_id is a channel num (0~%d)\n", HX9031AS_CH_NUM);

	return count;
}

static const struct file_operations hx9031as_manual_offset_calibration_fops = {
	.write = hx9031as_manual_offset_calibration_store,
	.read = hx9031as_manual_offset_calibration_show,
};

static ssize_t hx9031as_prox_state_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[BUF_SIZE] = {0};

	hx9031as_get_prox_state();
	snprintf(buf, sizeof(buf), "prox_state_reg=0x%02X\n", hx9031as_pdata.prox_state_reg);

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static const struct file_operations hx9031as_prox_state_fops = {
	.read = hx9031as_prox_state_show,
};

static ssize_t hx9031as_polling_store(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
	int value = 0;
	int ret = -1;
	char buf[BUF_SIZE];

	ENTER;
	if (count > BUF_SIZE)
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	ret = kstrtoint(buf, 10, &value);
	if (ret != 0) {
		PRINT_ERR("kstrtoint failed\n");
		goto exit;
	}

	if (value >= 10) {
		hx9031as_pdata.polling_period_ms = value;
		if (atomic_read(&hx9031as_pdata.polling_flag) == 1) {
			PRINT_INF("polling is already enabled!, no need to do enable again!, just update the polling period\n");
			goto exit;
		}

		atomic_set(&hx9031as_pdata.polling_flag, 1);
		hx9031as_disable_irq(hx9031as_pdata.irq);

		PRINT_INF("polling started! period=%dms\n", hx9031as_pdata.polling_period_ms);
		schedule_delayed_work(&hx9031as_pdata.polling_work, msecs_to_jiffies(hx9031as_pdata.polling_period_ms));
	} else {
		if (atomic_read(&hx9031as_pdata.polling_flag) == 0) {
			PRINT_INF("polling is already disabled!, no need to do again!\n");
			goto exit;
		}
		hx9031as_pdata.polling_period_ms = 0;
		atomic_set(&hx9031as_pdata.polling_flag, 0);
		PRINT_INF("polling stoped!\n");

		cancel_delayed_work(&hx9031as_pdata.polling_work);
		hx9031as_enable_irq(hx9031as_pdata.irq);
	}

exit:
	return count;
}

static ssize_t hx9031as_polling_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[BUF_SIZE] = {0};

	PRINT_INF("hx9031as_pdata.polling_flag=%d hx9031as_pdata.polling_period_ms=%d\n",
				atomic_read(&hx9031as_pdata.polling_flag), hx9031as_pdata.polling_period_ms);
	snprintf(buf, sizeof(buf), "hx9031as_pdata.polling_flag=%d hx9031as_pdata.polling_period_ms=%d\n",
				atomic_read(&hx9031as_pdata.polling_flag), hx9031as_pdata.polling_period_ms);

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static const struct file_operations hx9031as_polling_fops = {
	.write = hx9031as_polling_store,
	.read = hx9031as_polling_show,
};

static ssize_t hx9031as_accuracy_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[BUF_SIZE] = {0};

	PRINT_INF("hx9031as_pdata.accuracy=%d\n", hx9031as_pdata.accuracy);
	snprintf(buf, sizeof(buf), "hx9031as_pdata.accuracy=%d\n", hx9031as_pdata.accuracy);

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static ssize_t hx9031as_accuracy_store(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
	int ret = -1;
	int value = 0;
	char buf[BUF_SIZE];

	ENTER;
	if (count > BUF_SIZE)
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	ret = kstrtoint(buf, 10, &value);
	if (ret != 0) {
		PRINT_ERR("kstrtoint failed\n");
		return count;
	}

	hx9031as_pdata.accuracy = (value == 24) ? 24 : 16;
	PRINT_INF("set hx9031as_pdata.accuracy=%d\n", hx9031as_pdata.accuracy);
	return count;
}

static const struct file_operations hx9031as_accuracy_fops = {
	.write = hx9031as_accuracy_store,
	.read = hx9031as_accuracy_show,
};

static ssize_t hx9031as_threshold_store(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos)
{
	unsigned int ch = 0;
	unsigned int near = 0;
	unsigned int far = 0;
	char buf[BUF_SIZE];

	ENTER;
	if (count > BUF_SIZE)
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (sscanf(buf, "%d,%d,%d", &ch, &near, &far) != 3) {
		PRINT_ERR("please input 3 numbers in DEC: ch,near,far (eg: 0,500,300)\n");
		return -EINVAL;
	}

	if (ch >= HX9031AS_CH_NUM || near > (0x03FF * 32) || far > near) {
		PRINT_ERR("input value over range! (valid value: ch=%d, near=%d, far=%d)\n", ch, near, far);
		return -EINVAL;
	}

	near = (near / 32) * 32;
	far = (far / 32) * 32;

	PRINT_INF("set threshold: ch=%d, near=%d, far=%d\n", ch, near, far);
	hx9031as_set_thres_near(ch, near);
	hx9031as_set_thres_far(ch, far);

	return count;
}

static ssize_t hx9031as_threshold_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[BUF_SIZE] = {0};
	char *p = buf;
	int ii = 0;

	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		hx9031as_get_thres_near(ii);
		hx9031as_get_thres_far(ii);
		PRINT_INF("ch_%d threshold: near=%-8d, far=%-8d\n",
					ii, hx9031as_pdata.thres[ii].near, hx9031as_pdata.thres[ii].far);
		p += snprintf(p, PAGE_SIZE, "ch_%d threshold: near=%-8d, far=%-8d\n",
						ii, hx9031as_pdata.thres[ii].near, hx9031as_pdata.thres[ii].far);
	}

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static const struct file_operations hx9031as_threshold_fops = {
	.write = hx9031as_threshold_store,
	.read = hx9031as_threshold_show,
};

static ssize_t hx9031as_dump_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	int ret = -1;
	uint8_t rx_buf[1] = {0};
	char buf[BUF_SIZE * 2] = {0};
	char *p = buf;
	int ii = 0;

	for (ii = 0; ii < ARRAY_SIZE(hx9031as_reg_init_list); ii++) {
		ret = hx9031as_read(hx9031as_reg_init_list[ii].addr, rx_buf, 1);
		if (ret != 0)
			PRINT_ERR("hx9031as_read failed\n");
		PRINT_INF("0x%02X=0x%02X\n", hx9031as_reg_init_list[ii].addr, rx_buf[0]);
		p += snprintf(p, PAGE_SIZE, "0x%02X=0x%02X\n", hx9031as_reg_init_list[ii].addr, rx_buf[0]);
	}

	p += snprintf(p, PAGE_SIZE, "driver version:%s\n", HX9031AS_DRIVER_VER);

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static const struct file_operations hx9031as_dump_fops = {
	.read = hx9031as_dump_show,
};

static ssize_t hx9031as_offset_dac_show(struct file *file, char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[BUF_SIZE] = {0};
	char *p = buf;
	int ii = 0;

	hx9031as_read_offset_dac();

	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		PRINT_INF("hx9031as_pdata.dac[%d]=%dpF\n", ii, hx9031as_pdata.dac[ii] * 58 / 1000);
		p += snprintf(p, PAGE_SIZE, "ch[%d]=%dpF ", ii, hx9031as_pdata.dac[ii] * 58 / 1000);
	}
	p += snprintf(p, PAGE_SIZE, "\n");

	return simple_read_from_buffer(user_buf, count, ppos, buf, strlen(buf));
}

static const struct file_operations hx9031as_offset_dac_fops = {
	.read = hx9031as_offset_dac_show,
};

static int hx9031as_debug_for_iio(struct i2c_client *client)
{
	int ret = 0;
	int ii = 0;
	struct dentry *debugfs_file;

	PRINT_INF("i2c address:0x%02X\n", client->addr);
	hx9031as_pdata.i2c_client = client;
	hx9031as_pdata.pdev = &client->dev;
	hx9031as_pdata.irq = client->irq;
	hx9031as_pdata.channel_used_flag = 0x1F;
	hx9031as_pdata.chip_select = HX9023S_ON_BOARD;

	hx9031as_pdata.chs_info = devm_kzalloc(&client->dev,
											sizeof(struct hx9031as_channel_info) * HX9031AS_CH_NUM,
											GFP_KERNEL);
	if (hx9031as_pdata.chs_info == NULL) {
		PRINT_ERR("devm_kzalloc failed\n");
		ret = -ENOMEM;
		goto failed_devm_kzalloc;
	}

	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		snprintf(hx9031as_pdata.chs_info[ii].name,
					sizeof(hx9031as_pdata.chs_info[ii].name),
					"hx9031as_ch%d",
					ii);
		PRINT_DBG("name of ch_%d:\"%s\"\n", ii, hx9031as_pdata.chs_info[ii].name);
		hx9031as_pdata.chs_info[ii].used = false;
		hx9031as_pdata.chs_info[ii].enabled = false;
		if ((hx9031as_pdata.channel_used_flag >> ii) & 0x1) {
			hx9031as_pdata.chs_info[ii].used = true;
			hx9031as_pdata.chs_info[ii].state = 0;
		}
	}

	INIT_DELAYED_WORK(&hx9031as_pdata.polling_work, hx9031as_polling_work_func);

	hx9031as_pdata.debugfs_dir = debugfs_create_dir(HX9031AS_DRIVER_NAME, NULL);
	if (hx9031as_pdata.debugfs_dir != NULL) {
		debugfs_file = debugfs_create_file("raw_data", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_raw_data_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("reg_write", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_reg_write_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("reg_read", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_reg_read_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("channel_en", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_channel_en_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("calibrate", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_manual_offset_calibration_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("prox_state", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_prox_state_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("polling_period", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_polling_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("threshold", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_threshold_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("accuracy", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_accuracy_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("dump", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_dump_fops);
		if (!debugfs_file)
			goto failed_create_file;
		debugfs_file = debugfs_create_file("offset_dac", 0644, hx9031as_pdata.debugfs_dir, NULL, &hx9031as_offset_dac_fops);
		if (!debugfs_file)
			goto failed_create_file;
		PRINT_INF("debugfs_create_dir success\n");
	} else {
		PRINT_INF("debugfs_create_dir failed\n");
		//return -ENODEV;
	}

	return 0;

failed_create_file:
	PRINT_INF("debugfs_create_file failed\n");
	debugfs_remove_recursive(hx9031as_pdata.debugfs_dir);
	return 0;

failed_devm_kzalloc:
	PRINT_ERR("debug init failed\n");
	return ret;
}

static ssize_t hx9031as_show_samp_freq_avail(struct device *dev,
														struct device_attribute *attr,
														char *buf)
{
	size_t len = 0;
	int ii = 0;

	ENTER;
	for (ii = 0; ii < ARRAY_SIZE(hx9031as_samp_freq_table); ii++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "0x%02X:%4dms,",
						ii, hx9031as_samp_freq_table[ii]);
	buf[len - 1] = '\n';
	return len;
}
static IIO_DEV_ATTR_SAMP_FREQ_AVAIL(hx9031as_show_samp_freq_avail);

static int hx9031as_update_chan_en(struct hx9031as_data *data,
											unsigned long chan_read,
											unsigned long chan_event)
{
	int ii = 0;
	unsigned long channels = chan_read | chan_event;

	ENTER;
	if ((data->chan_read | data->chan_event) != channels) {
		for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
			if ((hx9031as_pdata.channel_used_flag >> ii) & 0x1) {
				if ((channels >> ii) & 0x1)
					hx9031as_ch_en_hal(ii, 1);
				else
					hx9031as_ch_en_hal(ii, 0);
			}
		}
	}

	data->chan_read = chan_read;
	data->chan_event = chan_event;
	return 0;
}

/*
 * static int hx9031as_get_read_channel(struct hx9031as_data *data, int channel)
 * {
 * return hx9031as_update_chan_en(data, data->chan_read | BIT(channel), data->chan_event);
 * }
 *
 * static int hx9031as_put_read_channel(struct hx9031as_data *data, int channel)
 * {
 * return hx9031as_update_chan_en(data, data->chan_read & ~BIT(channel), data->chan_event);
 * }
 *
 * static int hx9031as_get_event_channel(struct hx9031as_data *data, int channel)
 * {
 * return hx9031as_update_chan_en(data, data->chan_read, data->chan_event | BIT(channel));
 * }
 *
 * static int hx9031as_put_event_channel(struct hx9031as_data *data, int channel)
 * {
 * return hx9031as_update_chan_en(data, data->chan_read, data->chan_event & ~BIT(channel));
 * }
 */

static int hx9031as_get_proximity(struct hx9031as_data *data,
											const struct iio_chan_spec *chan, int *val)
{
	ENTER;
	hx9031as_sample();
	hx9031as_get_prox_state();
	*val = hx9031as_pdata.diff[chan->channel];
	return IIO_VAL_INT;
}

static int hx9031as_get_samp_freq(struct hx9031as_data *data, int *val, int *val2)
{
	int ret = -1;
	uint32_t odr;
	uint8_t buf[1] = {0};

	ENTER;
	ret = hx9031as_read(RW_02_PRF_CFG, buf, 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");

	odr = hx9031as_samp_freq_table[buf[0]];
	*val = 1000 / odr;
	*val2 = ((1000 % odr) * 1000000ULL) / odr;
	PRINT_INF("Period=%dms, Freq=%d.%dHz\n", odr, *val, *val2);

	return IIO_VAL_INT_PLUS_MICRO;
}

static int hx9031as_read_raw(struct iio_dev *indio_dev,
									const struct iio_chan_spec *chan, int *val,
									int *val2, long mask)
{
	struct hx9031as_data *data = iio_priv(indio_dev);
	int ret;

	ENTER;
	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;

		ret = hx9031as_get_proximity(data, chan, val);
		iio_device_release_direct_mode(indio_dev);
		return ret;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return hx9031as_get_samp_freq(data, val, val2);
	default:
		return -EINVAL;
	}
}

static int hx9031as_set_samp_freq(struct hx9031as_data *data, int val, int val2)
{
	int ii = 0;
	int ret = -1;
	int period_ms = 0;
	uint8_t buf[1] = {0};

	period_ms = 1000000000ULL / (val * 1000000ULL + val2);
	PRINT_INF("Freq=%d.%dHz, Period=%dms\n", val, val2, period_ms);

	for (ii = 0; ii < ARRAY_SIZE(hx9031as_samp_freq_table); ii++) {
		if (period_ms == hx9031as_samp_freq_table[ii]) {
			PRINT_INF("Peroid:%dms found! index=%d\n", period_ms, ii);
			break;
		}
	}
	if (ii == ARRAY_SIZE(hx9031as_samp_freq_table)) {
		PRINT_ERR("Peroid:%dms NOT found!\n", period_ms);
		return -EINVAL;
	}

	buf[0] = ii;
	ret = hx9031as_write(RW_02_PRF_CFG, buf, 1);
	if (ret != 0)
		PRINT_ERR("hx9031as_read failed\n");

	return ret;
}

static int hx9031as_write_raw(struct iio_dev *indio_dev,
									const struct iio_chan_spec *chan,
									int val, int val2, long mask)
{
	struct hx9031as_data *data = iio_priv(indio_dev);

	ENTER;
	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	if (mask != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;

	return hx9031as_set_samp_freq(data, val, val2);
}

static irqreturn_t hx9031as_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct hx9031as_data *data = iio_priv(indio_dev);

	ENTER;
	if (data->trigger_enabled)
		iio_trigger_poll(data->trig);

	return IRQ_WAKE_THREAD;
}

static void hx9031as_push_events(struct iio_dev *indio_dev)
{
	struct hx9031as_data *data = iio_priv(indio_dev);
	s64 timestamp = iio_get_time_ns(indio_dev);
	unsigned long prox_changed;
	unsigned int chan = 0;

	ENTER;
	hx9031as_sample();
	hx9031as_get_prox_state();

	prox_changed = (data->chan_prox_stat ^ hx9031as_pdata.prox_state_reg) & data->chan_event;

	for_each_set_bit(chan, &prox_changed, HX9031AS_CH_NUM) {
		int dir;
		u64 ev;

		dir = (hx9031as_pdata.prox_state_reg & BIT(chan)) ? IIO_EV_DIR_FALLING : IIO_EV_DIR_RISING;
		ev = IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, chan, IIO_EV_TYPE_THRESH, dir);

		iio_push_event(indio_dev, ev, timestamp);
		PRINT_INF("chan=%d, dir=%d, prox_changed=0x%08lX, ev=0x%016llX\n",
					chan, dir, prox_changed, ev);
	}
	data->chan_prox_stat = hx9031as_pdata.prox_state_reg;
}

static irqreturn_t hx9031as_irq_thread_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct hx9031as_data *data = iio_priv(indio_dev);

	ENTER;
	mutex_lock(&data->mutex);
	hx9031as_push_events(indio_dev);
	mutex_unlock(&data->mutex);

	return IRQ_HANDLED;
}

static int hx9031as_read_event_config(struct iio_dev *indio_dev,
										const struct iio_chan_spec *chan,
										enum iio_event_type type,
										enum iio_event_direction dir)
{
	struct hx9031as_data *data = iio_priv(indio_dev);
	int en_state = 0;

	en_state = !!(data->chan_event & BIT(chan->channel));
	PRINT_INF("chan_event=0x%016lX, ch%d=%d, en_state=%d\n",
				data->chan_event,
				chan->channel,
				hx9031as_pdata.chs_info[chan->channel].enabled,
				en_state);
	return en_state;
}

static int hx9031as_write_event_config(struct iio_dev *indio_dev,
										const struct iio_chan_spec *chan,
										enum iio_event_type type,
										enum iio_event_direction dir, int state)
{
	struct hx9031as_data *data = iio_priv(indio_dev);

	PRINT_INF("befor:chan_event=0x%016lX\n", data->chan_event);
	if ((hx9031as_pdata.channel_used_flag >> chan->channel) & 0x1) {
		hx9031as_ch_en_hal(chan->channel, !!state);
		if (hx9031as_pdata.chs_info[chan->channel].enabled == 1)
			data->chan_event = (data->chan_event | BIT(chan->channel));
		else
			data->chan_event = (data->chan_event & ~BIT(chan->channel));
	}
	PRINT_INF("after:chan_event=0x%016lX(ch%d updated)\n", data->chan_event, chan->channel);
	return 0;
}

static struct attribute *hx9031as_attributes[] = {
	&iio_dev_attr_sampling_frequency_available.dev_attr.attr,
	NULL
};

static const struct attribute_group hx9031as_attribute_group = {
	.attrs = hx9031as_attributes,
};

static const struct iio_info hx9031as_info = {
	.attrs = &hx9031as_attribute_group,
	.read_raw = hx9031as_read_raw,
	.write_raw = hx9031as_write_raw,
	.read_event_config = hx9031as_read_event_config,//get ch en flag
	.write_event_config = hx9031as_write_event_config,//set ch en flag
};

static int hx9031as_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct hx9031as_data *data = iio_priv(indio_dev);

	ENTER;
	mutex_lock(&data->mutex);
	if (state)
		hx9031as_enable_irq(hx9031as_pdata.irq);
	else if (!data->chan_read)
		hx9031as_disable_irq(hx9031as_pdata.irq);
	data->trigger_enabled = state;
	mutex_unlock(&data->mutex);
	return 0;
}

static const struct iio_trigger_ops hx9031as_trigger_ops = {
	.set_trigger_state = hx9031as_set_trigger_state,
};

static irqreturn_t hx9031as_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct hx9031as_data *data = iio_priv(indio_dev);
	__be16 val;
	int bit = 0;
	int ii = 0;

	ENTER;
	mutex_lock(&data->mutex);

	hx9031as_sample();
	hx9031as_get_prox_state();

	for_each_set_bit(bit, indio_dev->active_scan_mask, indio_dev->masklength) {
		val = hx9031as_pdata.diff[indio_dev->channels[bit].channel];
		data->buffer.channels[ii++] = val;
		PRINT_INF("bit=%d, ii=%d, val=%d\n", bit, ii - 1, val);
	}

	iio_push_to_buffers_with_timestamp(indio_dev, &data->buffer, pf->timestamp);
	mutex_unlock(&data->mutex);

	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int hx9031as_buffer_preenable(struct iio_dev *indio_dev)
{
	struct hx9031as_data *data = iio_priv(indio_dev);
	unsigned long channels = 0;
	int bit = 0;

	ENTER;
	mutex_lock(&data->mutex);
	for_each_set_bit(bit, indio_dev->active_scan_mask, indio_dev->masklength) {
		__set_bit(indio_dev->channels[bit].channel, &channels);
	}

	hx9031as_update_chan_en(data, channels, data->chan_event);
	mutex_unlock(&data->mutex);
	return 0;
}

static int hx9031as_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct hx9031as_data *data = iio_priv(indio_dev);

	ENTER;
	mutex_lock(&data->mutex);
	hx9031as_update_chan_en(data, 0, data->chan_event);
	mutex_unlock(&data->mutex);
	return 0;
}

static const struct iio_buffer_setup_ops hx9031as_buffer_setup_ops = {
	.preenable = hx9031as_buffer_preenable,
	.postdisable = hx9031as_buffer_postdisable,
};

static int hx9031as_init_device(struct iio_dev *indio_dev)
{
	//struct hx9031as_data *data = iio_priv(indio_dev);
	int ii = 0;

	ENTER;
	hx9031as_reg_init();
	hx9031as_ch_cfg(hx9031as_pdata.chip_select);

	for (ii = 0; ii < HX9031AS_CH_NUM; ii++) {
		hx9031as_set_thres_near(ii, hx9031as_pdata.thres[ii].near);
		hx9031as_set_thres_far(ii, hx9031as_pdata.thres[ii].far);
	}

	return 0;
}

static void hx9031as_regulator_disable(void *_data)
{
	struct hx9031as_data *data = _data;

	ENTER;
	regulator_bulk_disable(ARRAY_SIZE(data->supplies), data->supplies);
}

static int hx9031as_probe(struct i2c_client *client)
{
	int ret;
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct hx9031as_data *data;

	PRINT_INF("driver version:%s\n", HX9031AS_DRIVER_VER);
	PRINT_INF("client->name=%s, client->addr=0x%02X, client->irq=%d\n",
				client->name, client->addr, client->irq);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	data->supplies[0].supply = "vdd";
	mutex_init(&data->mutex);

	data->regmap = devm_regmap_init_i2c(client, &hx9031as_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);
	hx9031as_pdata.iio_data = data;

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(data->supplies), data->supplies);
	if (ret) {
		PRINT_ERR("regulator bulk get failed\n");
		return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(data->supplies), data->supplies);
	if (ret) {
		PRINT_ERR("regulator bulk enable failed\n");
		return ret;
	}

	/* Must wait for Tpor time after initial power up */
	usleep_range(1000, 1100);

	ret = devm_add_action_or_reset(dev, hx9031as_regulator_disable, data);
	if (ret)
		return ret;

	hx9031as_debug_for_iio(client);

	ret = hx9031as_id_check();
	if (ret != 0) {
		PRINT_ERR("hx9031as_id_check failed\n");
		return ret;
	}

	indio_dev->channels = hx9031as_channels;
	indio_dev->num_channels = ARRAY_SIZE(hx9031as_channels);
	indio_dev->info = &hx9031as_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->name = HX9031AS_DRIVER_NAME;
	i2c_set_clientdata(client, indio_dev);

	ret = hx9031as_init_device(indio_dev);
	if (ret)
		return ret;

	if (client->irq) {
		ret = devm_request_threaded_irq(dev, client->irq,
										hx9031as_irq_handler,
										hx9031as_irq_thread_handler,
										IRQF_ONESHOT,
										"hx9031as_event", indio_dev);
		if (ret)
			return ret;
		atomic_set(&hx9031as_pdata.irq_en, 1);

		data->trig = devm_iio_trigger_alloc(dev, "%s-dev%d",
											indio_dev->name,
											iio_device_id(indio_dev));
		if (!data->trig)
			return -ENOMEM;

		data->trig->dev.parent = dev;
		data->trig->ops = &hx9031as_trigger_ops;
		iio_trigger_set_drvdata(data->trig, indio_dev);

		ret = devm_iio_trigger_register(dev, data->trig);
		if (ret)
			return ret;
	}

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
										iio_pollfunc_store_time,
										hx9031as_trigger_handler,
										&hx9031as_buffer_setup_ops);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static int __maybe_unused hx9031as_suspend(struct device *dev)
{
	//struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	//struct hx9031as_data *data = iio_priv(indio_dev);

	ENTER;
	hx9031as_disable_irq(hx9031as_pdata.irq);
	return 0;
}

static int __maybe_unused hx9031as_resume(struct device *dev)
{
	//struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	//struct hx9031as_data *data = iio_priv(indio_dev);

	ENTER;
	hx9031as_enable_irq(hx9031as_pdata.irq);
	return 0;
}

static SIMPLE_DEV_PM_OPS(hx9031as_pm_ops, hx9031as_suspend, hx9031as_resume);

static const struct acpi_device_id hx9031as_acpi_match[] = {
	{ HX9031AS_DRIVER_NAME, HX9031AS_CHIP_ID },
	{}
};
MODULE_DEVICE_TABLE(acpi, hx9031as_acpi_match);

static const struct of_device_id hx9031as_of_match[] = {
	{ .compatible = "tyhx,hx9031as", (void *)HX9031AS_CHIP_ID },
	{}
};
MODULE_DEVICE_TABLE(of, hx9031as_of_match);

static const struct i2c_device_id hx9031as_id[] = {
	{ HX9031AS_DRIVER_NAME, HX9031AS_CHIP_ID },
	{}
};
MODULE_DEVICE_TABLE(i2c, hx9031as_id);

static struct i2c_driver hx9031as_driver = {
	.driver = {
		.name = HX9031AS_DRIVER_NAME,
		.acpi_match_table = hx9031as_acpi_match,
		.of_match_table = hx9031as_of_match,
		.pm = &hx9031as_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = hx9031as_probe,
	.id_table = hx9031as_id,
};
module_i2c_driver(hx9031as_driver);

MODULE_AUTHOR("Yasin Lee <yasin.lee.x@gmail.com>");
MODULE_DESCRIPTION("Driver for TYHX HX9031AS/HX9023S SAR sensor");
MODULE_LICENSE("GPL v2");
