// SPDX-License-Identifier: GPL-2.0-only
//
// aw-dev-common.h --  awinic amp common driver interface
//
// Copyright (c) 2025 AWINIC Technology CO., LTD
//
// Author: Weidong Wang <wangweidong.a@awinic.com>
//

#ifndef __AW_COMMON_DEVICE_H__
#define __AW_COMMON_DEVICE_H__

#define AW_ID_REG		(0x00)
#define AW_SYSINT_REG		(0x02)
#define AW_DSPMADD_REG		(0x40)
#define AW_DSPMDAT_REG		(0x41)
#define AW_WDT_REG		(0x42)
#define AW_DSP_16_DATA_MASK	(0x0000ffff)
#define AW_WDT_CNT_START_BIT	(0)
#define AW_WDT_CNT_BITS_LEN	(8)
#define AW_WDT_CNT_MASK		\
	(~(((1<<AW_WDT_CNT_BITS_LEN)-1) << AW_WDT_CNT_START_BIT))

#define AW_VOLUME_STEP_DB		(6 * 8)
#define AW_VOL_DEFAULT_VALUE		(0)
#define AW_DSP_I2C_WRITES
#define AW_MAX_RAM_WRITE_BYTE_SIZE	(128)
#define AW_DATA_TYPE_NUM		(3)
#define FADE_TIME_MAX			100000
#define FADE_TIME_MIN			0
#define AW_PROFILE_NUM_MAX		10

#define AW_PROFILE_EXT(xname, profile_info, profile_get, profile_set) \
{ \
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	.name = xname, \
	.info = profile_info, \
	.get = profile_get, \
	.put = profile_set, \
}

enum {
	AW_DSP_FW_UPDATE_OFF = 0,
	AW_DSP_FW_UPDATE_ON = 1,
};

enum {
	AW_FORCE_UPDATE_OFF = 0,
	AW_FORCE_UPDATE_ON = 1,
};

enum {
	AW_1000_US = 1000,
	AW_2000_US = 2000,
	AW_3000_US = 3000,
	AW_4000_US = 4000,
	AW_5000_US = 5000,
	AW_10000_US = 10000,
	AW_100000_US = 100000,
};

enum AW_DEV_PW_STATUS {
	AW_DEV_PW_OFF = 0,
	AW_DEV_PW_ON,
};

enum AW_DEV_FW_STATUS {
	AW_DEV_FW_FAILED = 0,
	AW_DEV_FW_OK,
};

enum AW_DEV_DSP_CFG {
	AW_DEV_DSP_WORK = 0,
	AW_DEV_DSP_BYPASS = 1,
};

enum {
	AW_DSP_16_DATA = 0,
	AW_DSP_32_DATA = 1,
};

enum {
	CALI_RESULT_NORMAL,
	CALI_RESULT_ERROR,
};

enum {
	AW_SYNC_START = 0,
	AW_ASYNC_START,
};

#define AW_CALI_CFG_NUM (4)
struct cali_cfg {
	uint32_t data[AW_CALI_CFG_NUM];
};

struct aw_cali_backup_desc {
	unsigned int dsp_ng_cfg;
	unsigned int dsp_lp_cfg;
};

struct aw_cali_desc {
	u32 cali_re;
	u32 ra;
	bool cali_switch;
	bool cali_running;
	uint16_t cali_result;
	uint16_t store_vol;
	struct cali_cfg cali_cfg;
	struct aw_cali_backup_desc backup_info;
};

struct aw_sec_data_desc {
	u32 addr;
	u32 len;
	u8 *data;
};

struct aw_prof_desc {
	u32 id;
	u32 prof_st;
	char *prf_str;
	u32 fw_ver;
	struct aw_sec_data_desc sec_desc[AW_DATA_TYPE_NUM];
};

struct aw_all_prof_info {
	struct aw_prof_desc prof_desc[AW_PROFILE_NUM_MAX];
};

struct aw_prof_info {
	int count;
	int prof_type;
	char **prof_name_list;
	struct aw_prof_desc *prof_desc;
};

struct aw_volume_desc {
	unsigned int init_volume;
	unsigned int mute_volume;
	unsigned int ctl_volume;
	unsigned int max_volume;
};

struct aw_device {
	int status;
	struct mutex dsp_lock;

	unsigned char prof_cur;
	unsigned char prof_index;
	unsigned char dsp_crc_st;
	unsigned char dsp_cfg;
	u16 chip_id;

	unsigned int channel;
	unsigned int fade_step;
	unsigned int prof_data_type;

	struct i2c_client *i2c;
	struct gpio_desc *reset_gpio;
	struct device *dev;
	struct regmap *regmap;
	char *acf;

	u32 dsp_fw_len;
	u32 dsp_cfg_len;
	u32 fw_ver;
	u8 fw_status;
	bool phase_sync;

	unsigned int fade_in_time;
	unsigned int fade_out_time;

	struct aw_container *aw_cfg;
	struct aw_prof_info prof_info;
	struct aw_sec_data_desc crc_dsp_cfg;
	struct aw_volume_desc volume_desc;

	struct aw_cali_desc cali_desc;
};

int aw_dev_init(struct aw_device *aw_dev, struct i2c_client *i2c, struct regmap *regmap);
int aw_dev_request_firmware_file(struct aw_device *aw_dev, char *acf_name);
int aw_dev_get_prof_data(struct aw_device *aw_dev, int index, struct aw_prof_desc **prof_desc);
int aw_dev_get_prof_name(struct aw_device *aw_dev, int index, char **prof_name);
int aw_dev_set_profile_index(struct aw_device *aw_dev, int index);
int aw_dev_dsp_update_fw(struct aw_device *aw_dev, unsigned char *data,
		unsigned int len, unsigned int addr);
int aw_dev_dsp_update_cfg(struct aw_device *aw_dev,
		unsigned char *data, unsigned int len, unsigned int addr);
int aw_dev_dsp_read_16bit(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int *dsp_data);
int aw_dev_get_dsp_status(struct aw_device *aw_dev);
int aw_dev_dsp_read(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int *dsp_data, unsigned char data_type);
int aw_dev_dsp_write(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int dsp_data, unsigned char data_type);
void aw_dev_get_int_status(struct aw_device *aw_dev, unsigned short *int_status);
void aw_dev_clear_int_status(struct aw_device *aw_dev);
void aw_dev_fade_out(struct aw_device *aw_dev,
	void (*set_volume)(struct aw_device *aw_dev, unsigned int value));
void aw_dev_fade_in(struct aw_device *aw_dev,
	void (*set_volume)(struct aw_device *aw_dev, unsigned int value));
void aw_hw_reset(struct aw_device *aw_dev);

#endif
