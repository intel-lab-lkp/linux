/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AW96103_H_
#define _AW96103_H_

#define AW_DATA_PROCESS_FACTOR			1024
#define AW_CHIP_AW96103A			0x03000b00
#define AW_READ_CHIPID_RETRIES			3
#define AW96103_CHIP_ID				0xa961
#define AW96103_BIN_VALID_DATA_OFFSET		64
#define AW96103_BIN_DATA_LEN_OFFSET		16
#define AW96103_BIN_DATA_REG_NUM_SIZE		4
#define AW96103_BIN_CHIP_TYPE_SIZE		8
#define AW96103_BIN_CHIP_TYPE_OFFSET		24

#define AW96103_REG_SCANCTRL0			0x0000
#define AW96103_REG_STAT0			0x0090
#define AW96103_REG_BLFILT_CH0			0x00A8
#define AW96103_REG_BLRSTRNG_CH0		0x00B4
#define AW96103_REG_DIFF_CH0			0x0240
#define AW96103_REG_FWVER2			0x0410
#define AW96103_REG_CMD				0xF008
#define AW96103_REG_IRQSRC			0xF080
#define AW96103_REG_IRQEN			0xF084
#define AW96103_REG_RESET			0xFF0C
#define AW96103_REG_CHIPID			0xFF10
#define AW96103_REG_EEDA0			0x0408
#define AW96103_REG_EEDA1			0x040C
#define AW96103_REG_PROXCTRL_CH0		0x00B0
#define AW96103_REG_PROXTH0_CH0			0x00B8
#define AW96103_PROXTH_CH_STEP			0x3C
#define AW96103_THHYST_MASK			GENMASK(13, 12)
#define AW96103_INDEB_MASK			GENMASK(11, 10)
#define AW96103_OUTDEB_MASK			GENMASK(9, 8)
#define AW96103_INITOVERIRQ_MASK		BIT(0)
#define AW96103_BLFILT_CH_STEP			0x3C
#define AW96103_BLRSTRNG_MASK			GENMASK(5, 0)
#define AW96103_CHIPID_MASK			GENMASK(31, 16)
#define AW96103_BLERRTRIG_MASK			BIT(25)
#define AW96103_CHAN_EN_MASK			GENMASK(5, 0)

/**
 * struct aw_bin - Store the data obtained from parsing the configuration file.
 * @chip_type: Frame header information-chip type
 * @valid_data_len: Length of valid data obtained after parsing
 * @valid_data_addr: The offset address of the valid data obtained
 *		     after parsing relative to info
 * @len: The size of the bin file obtained from the firmware
 * @data: Store the bin file obtained from the firmware
 */
struct aw_bin {
	unsigned char chip_type[8];
	unsigned int valid_data_len;
	unsigned int valid_data_addr;
	unsigned int len;
	unsigned char data[] __counted_by(len);
};

enum aw96103_sar_vers {
	AW96103 = 2,
	AW96103A = 6,
	AW96103B = 0xa,
};

enum aw96103_operation_mode {
	AW96103_ACTIVE_MODE = 1,
	AW96103_SLEEP_MODE,
	AW96103_DEEPSLEEP_MODE,
	AW96103B_DEEPSLEEP_MODE,
};

enum aw96103_channel {
	AW_CHANNEL0,
	AW_CHANNEL1,
	AW_CHANNEL2,
	AW_CHANNEL3,
	AW_CHANNEL4,
	AW_CHANNEL5,
	AW_CHANNEL_MAX
};

enum aw96103_irq_trigger_position {
	FAR,
	TRIGGER_TH0,
	TRIGGER_TH1 = 0x03,
	TRIGGER_TH2 = 0x07,
	TRIGGER_TH3 = 0x0f,
};

enum aw96103_sensor_type {
	AW96103_VAL,
	AW96105_VAL,
};

struct aw_channels_info {
	bool used;
	unsigned int last_channel_info;
};

struct aw96103 {
	unsigned char vers;
	unsigned int irq_status;
	unsigned int hostirqen;
	struct delayed_work cfg_work;
	struct i2c_client *i2c;
	struct regmap *regmap;
	struct device *dev;
	struct aw_bin *aw_bin;
	struct aw_channels_info *channels_arr;
	unsigned char chip_type[9];
	unsigned int max_channels;
	unsigned int chan_en;
	struct mutex mutex;
};

#endif

