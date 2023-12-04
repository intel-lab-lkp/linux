/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tas25xx_dsp_loader.h - Texas Instruments TAS25xx Mono Audio Amplifier
 *
 * Copyright (C) 2020 Texas Instruments Incorporated -  http://www.ti.com
 *
 * Author: Dan Murphy <dmurphy@ti.com>
 */

#ifndef _TAS25XX_DSP_LOADER_H
#define _TAS25XX_DSP_LOADER_H

#define TAS25XX_NAME_SIZE	64
#define TAS25XX_PROG_SIZE	5
#define TAS25XX_CONFIG_SIZE	10

#define TAS25XX_CMD_SING_W	0x1
#define TAS25XX_CMD_BURST	0x2
#define TAS25XX_CMD_DELAY	0x3

struct tas25xx_cmd_reg {
	u8 book;
	u8 page;
	u8 offset;
	u8 data;
};

struct tas25xx_cmd_hdr {
	u16 cmd_type;
	u16 length;
};

struct tas25xx_cmd {
	struct tas25xx_cmd_hdr hdr;
	struct tas25xx_cmd_reg reg;
};

struct tas25xx_block_data {
	u32 block_type;
	u16 pram_checksum;
	u16 yram_checksum;
	u32 block_size;
	u32 num_subblocks;
};

struct tas25xx_program_info {
	char name[TAS25XX_NAME_SIZE];
	u8 app_mode;
	u8 pdm_i2s_mode;
	u8 isns_pd;
	u8 vsns_pd;
	u8 reserved_1;
	u16 reserved_2;
	u8 ldg_power_up;
	struct tas25xx_block_data blk_data;
};

struct tas25xx_config_info {
	char name[TAS25XX_NAME_SIZE];
	u16 devices;
	u16 program;
	u32 samp_rate;
	u16 clk_src;
	u16 sbclk_fs_ratio;
	u32 clk_freq;
	u32 num_blocks;
	struct tas25xx_block_data blk_data;
};

struct tas25xx_fw_hdr {
	u32 magic_num;
	u32 image_size;
	u32 checksum;
	u32 version_num;
	u32 dsp_version;
	u32 drv_fw_version;
	u32 timestamp;
	char firmware_name[TAS25XX_NAME_SIZE];
	u16 dev_family;
	u16 dev_subfamily;
	u32 num_programs;
	u32 prog_size[TAS25XX_PROG_SIZE];
	u32 num_configs;
	u32 config_size[TAS25XX_CONFIG_SIZE];
};

struct tas25xx_fw_data {
	struct tas25xx_fw_hdr *hdr;
	u8 *config_data;
	u8 *prog_data;
};

struct tas25xx_fw_data *tas25xx_parse_fw(struct device *dev,
	const struct firmware *fw);
int tas25xx_write_config(struct device *dev, struct regmap *regmap,
	struct tas25xx_fw_data *fw_data, int config_num);
int tas25xx_write_program(struct device *dev, struct regmap *regmap,
	struct tas25xx_fw_data *fw_data, int prog_num);

#endif /* _TAS25XX_DSP_LOADER_H */
