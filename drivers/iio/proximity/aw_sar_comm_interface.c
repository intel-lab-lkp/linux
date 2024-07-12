// SPDX-License-Identifier: GPL-2.0
#include "aw_sar_comm_interface.h"

#define AW_I2C_RW_RETRY_TIME_MIN		(2000)
#define AW_I2C_RW_RETRY_TIME_MAX		(3000)
#define AW_RETRIES				(5)

static int awinic_i2c_write(struct i2c_client *i2c, unsigned char *tr_data, unsigned short len)
{
	struct i2c_msg msg;

	msg.addr = i2c->addr;
	msg.flags = 0;
	msg.len = len;
	msg.buf = tr_data;

	return i2c_transfer(i2c->adapter, &msg, 1);
}

static int awinic_i2c_read(struct i2c_client *i2c, unsigned char *addr,
				unsigned char addr_len, unsigned char *data, unsigned short data_len)
{
	struct i2c_msg msg[2];

	msg[0].addr = i2c->addr;
	msg[0].flags = 0;
	msg[0].len = addr_len;
	msg[0].buf = addr;

	msg[1].addr = i2c->addr;
	msg[1].flags = 1;
	msg[1].len = data_len;
	msg[1].buf = data;

	return i2c_transfer(i2c->adapter, msg, 2);
}

int aw_sar_i2c_read(struct i2c_client *i2c, unsigned short reg_addr16, unsigned int *reg_data32)
{
	unsigned char r_buf[6] = { 0 };
	unsigned char cnt = 5;
	int ret;

	if (!i2c)
		return -EINVAL;

	r_buf[0] = (unsigned char)(reg_addr16 >> OFFSET_BIT_8);
	r_buf[1] = (unsigned char)(reg_addr16);

	do {
		ret = awinic_i2c_read(i2c, r_buf, 2, &r_buf[2], 4);
		if (ret < 0)
			dev_err(&i2c->dev, "i2c read error reg: 0x%04x, ret= %d cnt= %d",
					reg_addr16, ret, cnt);
		else
			break;
		usleep_range(2000, 3000);
	} while (cnt--);

	if (cnt < 0) {
		dev_err(&i2c->dev, "i2c read error!");
		return ret;
	}

	*reg_data32 = ((unsigned int)r_buf[5] << OFFSET_BIT_0) | ((unsigned int)r_buf[4] << OFFSET_BIT_8) |
		((unsigned int)r_buf[3] << OFFSET_BIT_16) | ((unsigned int)r_buf[2] << OFFSET_BIT_24);

	return 0;
}

int aw_sar_i2c_write(struct i2c_client *i2c, unsigned short reg_addr16, unsigned int reg_data32)
{
	unsigned char w_buf[6] = { 0 };
	unsigned char cnt = 5;
	int ret;

	if (!i2c)
		return -EINVAL;

	/* reg_addr */
	w_buf[0] = (unsigned char)(reg_addr16 >> OFFSET_BIT_8);
	w_buf[1] = (unsigned char)(reg_addr16);
	/* data */
	w_buf[2] = (unsigned char)(reg_data32 >> OFFSET_BIT_24);
	w_buf[3] = (unsigned char)(reg_data32 >> OFFSET_BIT_16);
	w_buf[4] = (unsigned char)(reg_data32 >> OFFSET_BIT_8);
	w_buf[5] = (unsigned char)(reg_data32);

	do {
		ret = awinic_i2c_write(i2c, w_buf, ARRAY_SIZE(w_buf));
		if (ret < 0) {
			dev_err(&i2c->dev,
					"i2c write error reg: 0x%04x data: 0x%08x, ret= %d cnt= %d",
					reg_addr16, reg_data32, ret, cnt);
		} else {
			break;
		}
	} while (cnt--);

	if (cnt < 0) {
		dev_err(&i2c->dev, "i2c write error!");
		return ret;
	}

	return 0;
}

int
aw_sar_i2c_write_bits(struct i2c_client *i2c, unsigned short reg_addr16, unsigned int mask, unsigned int val)
{
	unsigned int reg_val;

	aw_sar_i2c_read(i2c, reg_addr16, &reg_val);
	reg_val &= mask;
	reg_val |= (val & (~mask));
	aw_sar_i2c_write(i2c, reg_addr16, reg_val);

	return 0;
}

int aw_sar_i2c_write_seq(struct i2c_client *i2c, unsigned char *tr_data, unsigned short len)
{
	unsigned char cnt = AW_RETRIES;
	int ret;

	do {
		ret = awinic_i2c_write(i2c, tr_data, len);
		if (ret < 0)
			dev_err(&i2c->dev, "awinic i2c write seq error %d", ret);
		else
			break;
		usleep_range(AW_I2C_RW_RETRY_TIME_MIN, AW_I2C_RW_RETRY_TIME_MAX);
	} while (cnt--);

	if (cnt < 0) {
		dev_err(&i2c->dev, "awinic i2c write error!");
		return ret;
	}

	return 0;
}

int aw_sar_i2c_read_seq(struct i2c_client *i2c, unsigned char *addr,
				unsigned char addr_len, unsigned char *data, unsigned short data_len)
{
	unsigned char cnt = AW_RETRIES;
	int ret;

	do {
		ret = awinic_i2c_read(i2c, addr, addr_len, data, data_len);
		if (ret < 0)
			dev_err(&i2c->dev, "awinic sar i2c write error %d", ret);
		else
			break;
		usleep_range(AW_I2C_RW_RETRY_TIME_MIN, AW_I2C_RW_RETRY_TIME_MAX);
	} while (cnt--);

	if (cnt < 0) {
		dev_err(&i2c->dev, "awinic sar i2c read error!");
		return ret;
	}

	return 0;
}


enum bin_header_version_enum {
	HEADER_VERSION_1_0_0 = 0x01000000,
};

enum data_type_enum {
	DATA_TYPE_REGISTER = 0x00000000,
	DATA_TYPE_DSP_REG = 0x00000010,
	DATA_TYPE_DSP_CFG = 0x00000011,
	DATA_TYPE_SOC_REG = 0x00000020,
	DATA_TYPE_SOC_APP = 0x00000021,
	DATA_TYPE_MULTI_BINS = 0x00002000,
};

#define BigLittleSwap16(A)	((((unsigned short int)(A) & 0xff00) >> 8) | \
				(((unsigned short int)(A) & 0x00ff) << 8))

#define BigLittleSwap32(A)	((((unsigned long)(A) & 0xff000000) >> 24) | \
				(((unsigned long)(A) & 0x00ff0000) >> 8) | \
				(((unsigned long)(A) & 0x0000ff00) << 8) | \
				(((unsigned long)(A) & 0x000000ff) << 24))

static enum aw_bin_err_val aw_parse_bin_header_1_0_0(struct aw_bin *bin);

static enum aw_bin_err_val aw_check_sum(struct aw_bin *bin, int bin_num)
{
	unsigned char *p_check_sum;
	unsigned int sum_data = 0;
	unsigned int check_sum;
	unsigned int i;

	p_check_sum = &(bin->info.data[(bin->header_info[bin_num].valid_data_addr -
			bin->header_info[bin_num].header_len)]);
	check_sum = AW_SAR_GET_32_DATA(*(p_check_sum + 3), *(p_check_sum + 2),
				*(p_check_sum + 1), *(p_check_sum));

	for (i = 4; i < bin->header_info[bin_num].bin_data_len +
			bin->header_info[bin_num].header_len; i++)
		sum_data += *(p_check_sum + i);

	if (sum_data != check_sum) {
		p_check_sum = NULL;
		return AW_BIN_ERROR_SUM_OR_DATA_LEN;
	}
	p_check_sum = NULL;

	return AW_BIN_ERROR_NONE;
}

static enum aw_bin_err_val aw_check_register_num_v1(struct aw_bin *bin, int bin_num)
{
	unsigned int check_register_num;
	unsigned int parse_register_num;
	char *p_check_sum;

	p_check_sum =
		&(bin->info.data[(bin->header_info[bin_num].valid_data_addr)]);
	parse_register_num = AW_SAR_GET_32_DATA(*(p_check_sum + 3), *(p_check_sum + 2),
					*(p_check_sum + 1), *(p_check_sum));
	check_register_num = (bin->header_info[bin_num].bin_data_len - 4) /
				(bin->header_info[bin_num].reg_byte_len +
				bin->header_info[bin_num].data_byte_len);
	if (parse_register_num != check_register_num) {
		p_check_sum = NULL;
		return AW_BIN_ERROR_REGISTER_NUM;
	}
	bin->header_info[bin_num].reg_num = parse_register_num;
	bin->header_info[bin_num].valid_data_len = bin->header_info[bin_num].bin_data_len - 4;
	p_check_sum = NULL;
	bin->header_info[bin_num].valid_data_addr =
		bin->header_info[bin_num].valid_data_addr + 4;

	return AW_BIN_ERROR_NONE;
}

static enum aw_bin_err_val aw_check_dsp_reg_num_v1(struct aw_bin *bin, int bin_num)
{
	unsigned int check_dsp_reg_num;
	unsigned int parse_dsp_reg_num;
	char *p_check_sum;

	p_check_sum =
		&(bin->info.data[(bin->header_info[bin_num].valid_data_addr)]);
	parse_dsp_reg_num = AW_SAR_GET_32_DATA(*(p_check_sum + 7),
					*(p_check_sum + 6),
					*(p_check_sum + 5),
					*(p_check_sum + 4));
	bin->header_info[bin_num].reg_data_byte_len =
		AW_SAR_GET_32_DATA(*(p_check_sum + 11), *(p_check_sum + 10),
			*(p_check_sum + 9), *(p_check_sum + 8));
	check_dsp_reg_num = (bin->header_info[bin_num].bin_data_len -
				12) / bin->header_info[bin_num].reg_data_byte_len;
	if (parse_dsp_reg_num != check_dsp_reg_num) {
		p_check_sum = NULL;
		return AW_BIN_ERROR_DSP_REG_NUM;
	}
	bin->header_info[bin_num].download_addr =
		AW_SAR_GET_32_DATA(*(p_check_sum + 3), *(p_check_sum + 2),
			*(p_check_sum + 1), *(p_check_sum));
	bin->header_info[bin_num].reg_num = parse_dsp_reg_num;
	bin->header_info[bin_num].valid_data_len = bin->header_info[bin_num].bin_data_len - 12;
	p_check_sum = NULL;
	bin->header_info[bin_num].valid_data_addr =
		bin->header_info[bin_num].valid_data_addr + 12;

	return AW_BIN_ERROR_NONE;
}

static enum aw_bin_err_val aw_check_soc_app_num_v1(struct aw_bin *bin, int bin_num)
{
	unsigned int check_soc_app_num;
	unsigned int parse_soc_app_num;
	char *p_check_sum;

	p_check_sum = &(bin->info.data[(bin->header_info[bin_num].valid_data_addr)]);
	bin->header_info[bin_num].app_version = AW_SAR_GET_32_DATA(*(p_check_sum + 3),
			*(p_check_sum + 2), *(p_check_sum + 1), *(p_check_sum));
	parse_soc_app_num = AW_SAR_GET_32_DATA(*(p_check_sum + 11), *(p_check_sum + 10),
					*(p_check_sum + 9), *(p_check_sum + 8));
	check_soc_app_num = bin->header_info[bin_num].bin_data_len - 12;
	if (parse_soc_app_num != check_soc_app_num) {
		p_check_sum = NULL;
		return AW_BIN_ERROR_SOC_APP_NUM;
	}
	bin->header_info[bin_num].reg_num = parse_soc_app_num;
	bin->header_info[bin_num].download_addr =
		AW_SAR_GET_32_DATA(*(p_check_sum + 7), *(p_check_sum + 6),
				*(p_check_sum + 5), *(p_check_sum + 4));
	bin->header_info[bin_num].valid_data_len =
		bin->header_info[bin_num].bin_data_len - 12;
	p_check_sum = NULL;
	bin->header_info[bin_num].valid_data_addr =
		bin->header_info[bin_num].valid_data_addr + 12;

	return AW_BIN_ERROR_NONE;
}

static void aw_get_single_bin_header_1_0_0(struct aw_bin *bin)
{
	int i;

	bin->header_info[bin->all_bin_parse_num].header_len = 60;
	bin->header_info[bin->all_bin_parse_num].check_sum =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 3), *(bin->p_addr + 2),
				*(bin->p_addr + 1), *(bin->p_addr));
	bin->header_info[bin->all_bin_parse_num].header_ver =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 7), *(bin->p_addr + 6),
				*(bin->p_addr + 5), *(bin->p_addr + 4));
	bin->header_info[bin->all_bin_parse_num].bin_data_type =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 11), *(bin->p_addr + 10),
				*(bin->p_addr + 9), *(bin->p_addr + 8));
	bin->header_info[bin->all_bin_parse_num].bin_data_ver =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 15), *(bin->p_addr + 14),
				*(bin->p_addr + 13), *(bin->p_addr + 12));
	bin->header_info[bin->all_bin_parse_num].bin_data_len =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 19), *(bin->p_addr + 18),
				*(bin->p_addr + 17), *(bin->p_addr + 16));
	bin->header_info[bin->all_bin_parse_num].ui_ver =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 23), *(bin->p_addr + 22),
				*(bin->p_addr + 21), *(bin->p_addr + 20));
	bin->header_info[bin->all_bin_parse_num].reg_byte_len =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 35), *(bin->p_addr + 34),
				*(bin->p_addr + 33), *(bin->p_addr + 32));
	bin->header_info[bin->all_bin_parse_num].data_byte_len =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 39), *(bin->p_addr + 38),
				*(bin->p_addr + 37), *(bin->p_addr + 36));
	bin->header_info[bin->all_bin_parse_num].device_addr =
		AW_SAR_GET_32_DATA(*(bin->p_addr + 43), *(bin->p_addr + 42),
				*(bin->p_addr + 41), *(bin->p_addr + 40));
	for (i = 0; i < 8; i++) {
		bin->header_info[bin->all_bin_parse_num].chip_type[i] =
			*(bin->p_addr + 24 + i);
	}

	bin->header_info[bin->all_bin_parse_num].reg_num = 0x00000000;
	bin->header_info[bin->all_bin_parse_num].reg_data_byte_len = 0x00000000;
	bin->header_info[bin->all_bin_parse_num].download_addr = 0x00000000;
	bin->header_info[bin->all_bin_parse_num].app_version = 0x00000000;
	bin->header_info[bin->all_bin_parse_num].valid_data_len = 0x00000000;
	bin->all_bin_parse_num += 1;
}

static enum aw_bin_err_val aw_parse_each_of_multi_bins_1_0_0(unsigned int bin_num,
		int bin_serial_num, struct aw_bin *bin)
{
	unsigned int bin_start_addr;
	unsigned int valid_data_len;
	enum aw_bin_err_val ret;

	if (!bin_serial_num) {
		bin_start_addr = AW_SAR_GET_32_DATA(*(bin->p_addr + 67), *(bin->p_addr + 66),
				*(bin->p_addr + 65), *(bin->p_addr + 64));
		bin->p_addr += (60 + bin_start_addr);
		bin->header_info[bin->all_bin_parse_num].valid_data_addr =
			bin->header_info[bin->all_bin_parse_num - 1].valid_data_addr +
			4 + 8 * bin_num + 60;
	} else {
		valid_data_len =
			bin->header_info[bin->all_bin_parse_num - 1].bin_data_len;
		bin->p_addr += (60 + valid_data_len);
		bin->header_info[bin->all_bin_parse_num].valid_data_addr =
			bin->header_info[bin->all_bin_parse_num - 1].valid_data_addr +
			bin->header_info[bin->all_bin_parse_num - 1].bin_data_len + 60;
	}

	ret = aw_parse_bin_header_1_0_0(bin);
	return ret;
}

/* Get the number of bins in multi bins, and set a for loop, loop processing each bin data */
static enum aw_bin_err_val aw_get_multi_bin_header_1_0_0(struct aw_bin *bin)
{
	unsigned int bin_num;
	enum aw_bin_err_val ret;
	int i;

	bin_num = AW_SAR_GET_32_DATA(*(bin->p_addr + 63), *(bin->p_addr + 62),
			*(bin->p_addr + 61), *(bin->p_addr + 60));
	if (bin->multi_bin_parse_num == 1)
		bin->header_info[bin->all_bin_parse_num].valid_data_addr = 60;
	aw_get_single_bin_header_1_0_0(bin);

	for (i = 0; i < bin_num; i++) {
		ret = aw_parse_each_of_multi_bins_1_0_0(bin_num, i, bin);
		if (ret < 0)
			return ret;
	}
	return AW_BIN_ERROR_NONE;
}

/*
 * If the bin framework header version is 1.0.0,
 * determine the data type of bin, and then perform different processing
 * according to the data type
 * If it is a single bin data type, write the data directly into the structure array
 * If it is a multi-bin data type, first obtain the number of bins,
 * and then recursively call the bin frame header processing function
 * according to the bin number to process the frame header information of each bin separately
 */
static enum aw_bin_err_val aw_parse_bin_header_1_0_0(struct aw_bin *bin)
{
	unsigned int bin_data_type;
	enum aw_bin_err_val ret;

	bin_data_type = AW_SAR_GET_32_DATA(*(bin->p_addr + 11), *(bin->p_addr + 10),
			*(bin->p_addr + 9), *(bin->p_addr + 8));
	switch (bin_data_type) {
	case DATA_TYPE_REGISTER:
	case DATA_TYPE_DSP_REG:
	case DATA_TYPE_SOC_APP:
		/*
		 * Divided into two processing methods,
		 * one is single bin processing,
		 * and the other is single bin processing in multi bin
		 */
		bin->single_bin_parse_num += 1;
		if (!bin->multi_bin_parse_num)
			bin->header_info[bin->all_bin_parse_num].valid_data_addr = 60;
		aw_get_single_bin_header_1_0_0(bin);
		break;
	case DATA_TYPE_MULTI_BINS:
		/* Get the number of times to enter multi bins */
		bin->multi_bin_parse_num += 1;
		ret = aw_get_multi_bin_header_1_0_0(bin);
		if (ret < 0)
			return ret;
		break;
	default:
		return AW_BIN_ERROR_DATA_TYPE;
	}
	return AW_BIN_ERROR_NONE;
}

/* get the bin's header version */
static enum aw_bin_err_val aw_check_bin_header_version(struct aw_bin *bin)
{
	unsigned int header_version;
	enum aw_bin_err_val ret;

	header_version = AW_SAR_GET_32_DATA(*(bin->p_addr + 7), *(bin->p_addr + 6),
			*(bin->p_addr + 5), *(bin->p_addr + 4));


	/*
	 * Write data to the corresponding structure array
	 * according to different formats of the bin frame header version
	 */
	switch (header_version) {
	case HEADER_VERSION_1_0_0:
		ret = aw_parse_bin_header_1_0_0(bin);
		return ret;
	default:
		return AW_BIN_ERROR_HEADER_VERSION;
	}
}

enum aw_bin_err_val aw_sar_parsing_bin_file(struct aw_bin *bin)
{
	enum aw_bin_err_val ret;
	int i;

	if (!bin)
		return AW_BIN_ERROR_NULL_POINT;
	bin->p_addr = bin->info.data;
	bin->all_bin_parse_num = 0;
	bin->multi_bin_parse_num = 0;
	bin->single_bin_parse_num = 0;

	/* filling bins header info */
	ret = aw_check_bin_header_version(bin);
	if (ret < 0)
		return ret;
	bin->p_addr = NULL;

	/* check bin header info */
	for (i = 0; i < bin->all_bin_parse_num; i++) {
		/* check sum */
		ret = aw_check_sum(bin, i);
		if (ret < 0)
			return ret;

		/* check register num */
		if (bin->header_info[i].bin_data_type == DATA_TYPE_REGISTER) {
			ret = aw_check_register_num_v1(bin, i);
			if (ret < 0)
				return ret;
			/* check dsp reg num */
		} else if (bin->header_info[i].bin_data_type == DATA_TYPE_DSP_REG) {
			ret = aw_check_dsp_reg_num_v1(bin, i);
			if (ret < 0)
				return ret;
			/* check soc app num */
		} else if (bin->header_info[i].bin_data_type == DATA_TYPE_SOC_APP) {
			ret = aw_check_soc_app_num_v1(bin, i);
			if (ret < 0)
				return ret;
		} else {
			bin->header_info[i].valid_data_len = bin->header_info[i].bin_data_len;
		}
	}

	return AW_BIN_ERROR_NONE;
}

unsigned int aw_sar_pow2(unsigned int cnt)
{
	unsigned int sum = 1;
	unsigned int i;

	if (cnt == 0) {
		sum = 1;
	} else {
		for (i = 0; i < cnt; i++)
			sum *= 2;
	}

	return sum;
}

int aw_sar_load_reg(struct aw_bin *aw_bin, struct i2c_client *i2c)
{
	unsigned int start_addr = aw_bin->header_info[0].valid_data_addr;
	unsigned short reg_addr;
	unsigned int reg_data;
	int ret;
	unsigned int i;

	for (i = 0; i < aw_bin->header_info[0].valid_data_len; i += 6, start_addr += 6) {
		reg_addr = (aw_bin->info.data[start_addr]) |
				aw_bin->info.data[start_addr + 1] << OFFSET_BIT_8;
		reg_data = aw_bin->info.data[start_addr + 2] |
			(aw_bin->info.data[start_addr + 3] << OFFSET_BIT_8) |
			(aw_bin->info.data[start_addr + 4] << OFFSET_BIT_16) |
			(aw_bin->info.data[start_addr + 5] << OFFSET_BIT_24);

		ret = aw_sar_i2c_write(i2c, reg_addr, reg_data);
		if (ret < 0) {
			dev_err(&i2c->dev, "i2c write err");
			return ret;
		}
	}

	return 0;
}

