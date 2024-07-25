/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AW9610X_H_
#define _AW9610X_H_

#define AW9610X_CHANNEL_USED_MASK		0x1F
#define AW_VCC_MIN_UV				1700000
#define AW_VCC_MAX_UV				3600000
#define AW_DATA_PROCESS_FACTOR			1024
#define AW9610X_CPU_WORK_MASK			1
#define AW9610X_CHIP_MIN_VOLTAGE		1600000
#define AW_CHIP_AW9610XA			0x03000b00
#define AW_READ_CHIPID_RETRIES			3
#define AW_I2C_RETRIES				5
#define USB_POWER_SUPPLY_NAME			"charger"
#define AW9610X_AOT_MASK			0x3f
#define AW9610X_AOT_BIT				8
#define AW9610X_CHIP_ID				0xa961
#define AW_CHIP_AW9610XA			0x03000b00
#define AW9610X_CPU_WORK_MASK			1
#define AW9610X_AOT_MASK			0x3f
#define AW9610X_AOT_BIT				8
#define REG_EEDA0				0x0408
#define REG_EEDA1				0x040C
#define AW9610X_BIN_VALID_DATA_OFFSET		64
#define AW9610X_BIN_DATA_LEN_OFFSET		16
#define AW9610X_BIN_DATA_REG_NUM_SIZE		4
#define AW9610X_BIN_CHIP_TYPE_SIZE		8
#define AW9610X_BIN_CHIP_TYPE_OFFSET		24
#define AW9610X_BLFILT_CH_STEP			0x3C
#define AW9610X_BLRSTRNG_MASK			0x3F
#define AW9610X_CHIPID_MASK			GENMASK(31, 16)
#define AW9610X_BLERRTRIG_MASK			BIT(25)

#define AFE_BASE_ADDR				0x0000
#define DSP_BASE_ADDR				0x0000
#define STAT_BASE_ADDR				0x0000
#define SFR_BASE_ADDR				0x0000
#define DATA_BASE_ADDR				0x0000
#define REG_SCANCTRL0				(0x0000 + AFE_BASE_ADDR)
#define REG_AFECFG1_CH0				(0x0014 + AFE_BASE_ADDR)
#define REG_FWVER				(0x0088 + STAT_BASE_ADDR)
#define REG_WST					(0x008C + STAT_BASE_ADDR)
#define REG_STAT0				(0x0090 + STAT_BASE_ADDR)
#define REG_STAT1				(0x0094 + STAT_BASE_ADDR)
#define REG_CHINTEN				(0x009C + STAT_BASE_ADDR)
#define REG_BLFILT_CH0				(0x00A8 + DSP_BASE_ADDR)
#define REG_BLRSTRNG_CH0			(0x00B4 + DSP_BASE_ADDR)
#define REG_BLFILT_CH1				(0x00E4 + DSP_BASE_ADDR)
#define REG_COMP_CH0				(0x0210 + DATA_BASE_ADDR)
#define REG_BASELINE_CH0			(0x0228 + DATA_BASE_ADDR)
#define REG_DIFF_CH0				(0x0240 + DATA_BASE_ADDR)
#define REG_FWVER2				(0x0410 + DATA_BASE_ADDR)
#define REG_CMD					(0xF008 + SFR_BASE_ADDR)
#define REG_IRQSRC				(0xF080 + SFR_BASE_ADDR)
#define REG_IRQEN				(0xF084 + SFR_BASE_ADDR)
#define REG_OSCEN				(0xFF00 + SFR_BASE_ADDR)
#define REG_RESET				(0xFF0C + SFR_BASE_ADDR)
#define REG_CHIPID				(0xFF10 + SFR_BASE_ADDR)

#define REG_NONE_ACCESS				0
#define REG_RD_ACCESS				(1 << 0)
#define REG_WR_ACCESS				(1 << 1)
struct aw_reg_data {
	unsigned char rw;
	unsigned short reg;
};

/**
 * struct aw_bin -
 * @chip_type: Frame header information-chip type
 * @valid_data_len: Length of valid data obtained after parsing
 * @valid_data_addr: The offset address of the valid data obtained
 *		after parsing relative to info
 * @len: The size of the bin file obtained from the firmware
 * @data: Store the bin file obtained from the firmware
 */
struct aw_bin {
	unsigned char chip_type[8];
	unsigned int valid_data_len;
	unsigned int valid_data_addr;
	unsigned int len;
	unsigned char data[];
};

enum aw9610x_sar_vers {
	AW9610X = 2,
	AW9610XA = 6,
	AW9610XB = 0xa,
};

enum aw9610x_operation_mode {
	AW9610X_ACTIVE_MODE = 1,
	AW9610X_SLEEP_MODE,
	AW9610X_DEEPSLEEP_MODE,
	AW9610XB_DEEPSLEEP_MODE,
};

enum aw9610x_channel {
	AW_CHANNEL0,
	AW_CHANNEL1,
	AW_CHANNEL2,
	AW_CHANNEL3,
	AW_CHANNEL4,
	AW_CHANNEL5,
	AW_CHANNEL_MAX,
};

enum aw9610x_irq_trigger_position {
	FAR,
	TRIGGER_TH0,
	TRIGGER_TH1 = 0x03,
	TRIGGER_TH2 = 0x07,
	TRIGGER_TH3 = 0x0f,
};

struct aw_channels_info {
	bool used;
	unsigned int last_channel_info;
};

struct aw9610x {
	struct iio_dev *aw_iio_dev;
	unsigned char vers;
	unsigned int irq_status;
	unsigned int hostirqen;
	struct work_struct ps_notify_work;
	struct notifier_block ps_notif;
	bool ps_is_present;
	struct delayed_work cfg_work;
	struct i2c_client *i2c;
	struct regmap *regmap;
	struct device *dev;
	struct aw_bin *aw_bin;
	struct regulator *vcc;
	struct aw_channels_info *channels_arr;
	unsigned char chip_type[9];
};

#endif

