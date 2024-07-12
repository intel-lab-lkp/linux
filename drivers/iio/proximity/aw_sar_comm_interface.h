/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AW_SAR_PLAT_HW_INTERFACE_H_
#define _AW_SAR_PLAT_HW_INTERFACE_H_
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/of_gpio.h>
#include <linux/power_supply.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

enum aw_sar_chip_list_t {
	AW_SAR_NONE_CHECK_CHIP,

	SAR_AW9610X = 1 << 1,
	SAR_AW9610XA = 1 << 2,

	SAR_AW96303 = 1 << 6,
	SAR_AW96305 = 1 << 7,
	SAR_AW96308 = 1 << 8,
	SAR_AW96310 = 1 << 9,
	SAR_AW96312 = 1 << 10,
};

#ifndef AW_TRUE
#define AW_TRUE					(1)
#endif

#ifndef AW_FALSE
#define AW_FALSE				(0)
#endif

#define AW_ERR_IRQ_INIT_OVER			(0xAA)

enum aw_sar_rst_val {
	AW_OK,
	AW_BIN_PARA_INVALID,
	AW_PROT_UPDATE_ERR,
	AW_REG_LOAD_ERR,
};

#ifndef OFFSET_BIT_0
#define OFFSET_BIT_0				(0)
#endif

#ifndef OFFSET_BIT_8
#define OFFSET_BIT_8				(8)
#endif

#ifndef OFFSET_BIT_16
#define OFFSET_BIT_16				(16)
#endif

#ifndef OFFSET_BIT_24
#define OFFSET_BIT_24				(24)
#endif

#define AW_SAR_GET_32_DATA(w, x, y, z)	((unsigned int)(((w) << 24) | ((x) << 16) | ((y) << 8) | (z)))

enum AW_SAR_HOST_IRQ_STAT {
	IRQ_ENABLE,
	IRQ_DISABLE,
};

#define AW_SAR_BIN_NUM_MAX	100

enum aw_bin_err_val {
	AW_BIN_ERROR_NONE = 0,
	AW_BIN_ERROR_HEADER_VERSION = -1,
	AW_BIN_ERROR_DATA_TYPE = -2,
	AW_BIN_ERROR_SUM_OR_DATA_LEN = -3,
	AW_BIN_ERROR_DATA_VERSION = -4,
	AW_BIN_ERROR_REGISTER_NUM = -5,
	AW_BIN_ERROR_DSP_REG_NUM = -6,
	AW_BIN_ERROR_SOC_APP_NUM = -7,
	AW_BIN_ERROR_NULL_POINT = -8,
};

/**
 * struct bin_header_info -
 * @header_len: Frame header length
 * @check_sum: Frame header information-Checksum
 * @header_ver: Frame header information-Frame header version
 * @bin_data_type: Frame header information-Data type
 * @bin_data_ver: Frame header information-Data version
 * @bin_data_len: Frame header information-Data length
 * @ui_ver: Frame header information-ui version
 * @chip_type: Frame header information-chip type
 * @reg_byte_len: Frame header information-reg byte len
 * @data_byte_len: Frame header information-data byte len
 * @device_addr: Frame header information-device addr
 * @valid_data_len: Length of valid data obtained after parsing
 * @valid_data_addr: The offset address of the valid data obtained
 *		after parsing relative to info
 * @reg_num: The number of registers obtained after parsing
 * @reg_data_byte_len: The byte length of the register obtained after parsing
 * @download_addr: The starting address or download address obtained after parsing
 * @app_version: The software version number obtained after parsing
 */
struct bin_header_info {
	unsigned int header_len;
	unsigned int check_sum;
	unsigned int header_ver;
	unsigned int bin_data_type;
	unsigned int bin_data_ver;
	unsigned int bin_data_len;
	unsigned int ui_ver;
	unsigned char chip_type[8];
	unsigned int reg_byte_len;
	unsigned int data_byte_len;
	unsigned int device_addr;
	unsigned int valid_data_len;
	unsigned int valid_data_addr;
	unsigned int reg_num;
	unsigned int reg_data_byte_len;
	unsigned int download_addr;
	unsigned int app_version;
};

/**
 * struct bin_container -
 * @len: The size of the bin file obtained from the firmware
 * @data: Store the bin file obtained from the firmware
 */
struct bin_container {
	unsigned int len;
	unsigned char data[];
};

/**
 * struct aw_bin -
 * @p_addr: Offset pointer (backward offset pointer to obtain frame header information and
 *		important information)
 * @all_bin_parse_num: The number of all bin files
 * @multi_bin_parse_num: The number of single bin files
 * @single_bin_parse_num: The number of multiple bin files
 * @header_info: Frame header information and other important data obtained after parsing
 * @info: Obtained bin file data that needs to be parsed
 */
struct aw_bin {
	char *p_addr;
	unsigned int all_bin_parse_num;
	unsigned int multi_bin_parse_num;
	unsigned int single_bin_parse_num;
	struct bin_header_info header_info[AW_SAR_BIN_NUM_MAX];
	struct bin_container info;
};

/* I2C communication API */
int aw_sar_i2c_read(struct i2c_client *i2c, unsigned short reg_addr16, unsigned int *reg_data32);
int aw_sar_i2c_write(struct i2c_client *i2c, unsigned short reg_addr16, unsigned int reg_data32);
int aw_sar_i2c_write_bits(struct i2c_client *i2c, unsigned short reg_addr16,
		unsigned int mask, unsigned int val);
int aw_sar_i2c_write_seq(struct i2c_client *i2c, unsigned char *tr_data, unsigned short len);
int aw_sar_i2c_read_seq(struct i2c_client *i2c, unsigned char *addr,
		unsigned char addr_len, unsigned char *data, unsigned short data_len);

enum aw_bin_err_val aw_sar_parsing_bin_file(struct aw_bin *bin);
unsigned int aw_sar_pow2(unsigned int cnt);
int aw_sar_load_reg(struct aw_bin *aw_bin, struct i2c_client *i2c);

#endif
