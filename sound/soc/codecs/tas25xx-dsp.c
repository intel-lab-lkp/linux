// SPDX-License-Identifier: GPL-2.0
//
// Firmware loader for the Texas Instruments TAS25XX DSP
// Copyright (C) 2020 Texas Instruments Inc.

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <sound/tas2562.h>
#include <sound/tas25xx-dsp.h>


static void tas25xx_process_fw_delay(struct tas25xx_cmd *cmd)
{
	mdelay(cpu_to_be16(cmd->hdr.length));
}

static int tas25xx_process_fw_single(struct regmap *regmap,
	struct tas25xx_cmd *cmd)
{
	int ret;
	int num_writes = cpu_to_be16(cmd->hdr.length);
	struct tas25xx_cmd_reg *reg = &cmd->reg;

	for (int i = 0; i < num_writes; i++, reg++) {
		/* Reset Page to 0 to access BOOK_CTRL */
		ret = regmap_write(regmap, TAS2562_PAGE_CTRL, 0);
		if (ret)
			return ret;

		ret = regmap_write(regmap, TAS2562_BOOK_CTRL, reg->book);
		if (ret)
			return ret;

		ret = regmap_write(regmap, TAS2562_REG(reg->page, reg->offset),
			reg->data);
		if (ret)
			return ret;
	}

	return 0;
}

static int tas25xx_process_fw_burst(struct regmap *regmap,
	struct tas25xx_cmd *cmd)
{
	int ret;

	/* Reset Page to 0 to access BOOK_CTRL */
	ret = regmap_write(regmap, TAS2562_PAGE_CTRL, 0);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TAS2562_BOOK_CTRL, cmd->reg.book);
	if (ret)
		return ret;

	ret = regmap_bulk_write(regmap, TAS2562_REG(cmd->reg.page, cmd->reg.offset), &cmd[1],
				cpu_to_be16(cmd->hdr.length));
	if (ret)
		return ret;

	return 0;
}

static int tas25xx_process_block(struct device *dev, struct regmap *regmap,
	struct tas25xx_cmd *cmd, int max_block_size)
{
	int ret;
	int block_read;

	const int hdr_size = sizeof(struct tas25xx_cmd_hdr);
	const int reg_size = sizeof(struct tas25xx_cmd_reg);
	const int cmd_size = sizeof(struct tas25xx_cmd);

	switch (cpu_to_be16(cmd->hdr.cmd_type)) {
	case TAS25XX_CMD_SING_W:
		block_read = cpu_to_be16(cmd->hdr.length) * reg_size + hdr_size;
		break;
	case TAS25XX_CMD_BURST:
		block_read = cpu_to_be16(cmd->hdr.length) + cmd_size;
		break;
	case TAS25XX_CMD_DELAY:
		block_read = 4;
		break;
	default:
		block_read = 0;
	}

	if (block_read > max_block_size) {
		dev_err(dev,
			"Corrupt firmware: block_read > max_block_size %d %d\n",
			block_read, max_block_size);
		return -EINVAL;
	}

	switch (cpu_to_be16(cmd->hdr.cmd_type)) {
	case TAS25XX_CMD_SING_W:
		ret = tas25xx_process_fw_single(regmap, cmd);
		if (ret) {
			dev_err(dev, "Failed to process single write %d\n",
				ret);
			return ret;
		}
		break;
	case TAS25XX_CMD_BURST:
		ret = tas25xx_process_fw_burst(regmap, cmd);
		if (ret) {
			dev_err(dev, "Failed to process burst write %d\n", ret);
			return ret;
		}
		break;
	case TAS25XX_CMD_DELAY:
		tas25xx_process_fw_delay(cmd);
		break;
	default:
		dev_warn(dev, "Unknown cmd type %d\n",
			cpu_to_be16(cmd->hdr.cmd_type));
		break;
	};

	return block_read;
}


int tas25xx_write_program(struct device *dev, struct regmap *regmap,
	struct tas25xx_fw_data *fw_data, int prog_num)
{
	int offset = 0;
	int length = 0;
	struct tas25xx_program_info *prog_info;
	struct tas25xx_fw_hdr *hdr = fw_data->hdr;
	struct tas25xx_cmd *cmd;

	if (prog_num > cpu_to_be32(hdr->num_programs))
		return -EINVAL;

	for (int i = 0; i < prog_num; i++)
		offset += cpu_to_be32(hdr->prog_size[i]);

	prog_info = (struct tas25xx_program_info *)&fw_data->prog_data[offset];
	dev_info(dev, "Write program %d: %s\n", prog_num, prog_info->name);

	int max_offset = offset + cpu_to_be32(hdr->prog_size[prog_num]);
	int num_subblocks = cpu_to_be32(prog_info->blk_data.num_subblocks);

	offset += sizeof(struct tas25xx_program_info);

	for (int i = 0; i < num_subblocks; i++) {
		cmd = (struct tas25xx_cmd *)&fw_data->prog_data[offset];
		length = tas25xx_process_block(dev, regmap, cmd,
			max_offset - offset);
		if (length < 0)
			return length;

		offset += length;
	}

	/* Reset Book to 0 */
	regmap_write(regmap, TAS2562_PAGE_CTRL, 0);
	regmap_write(regmap, TAS2562_BOOK_CTRL, 0);

	return 0;
}
EXPORT_SYMBOL_GPL(tas25xx_write_program);

int tas25xx_write_config(struct device *dev, struct regmap *regmap,
	struct tas25xx_fw_data *fw_data, int config_num)
{
	int offset = 0;
	int length = 0;
	struct tas25xx_config_info *cfg_info;
	struct tas25xx_fw_hdr *hdr = fw_data->hdr;
	struct tas25xx_cmd *cmd;

	if (config_num > cpu_to_be32(hdr->num_configs))
		return -EINVAL;

	for (int i = 0; i < config_num; i++)
		offset += cpu_to_be32(hdr->config_size[i]);

	cfg_info = (struct tas25xx_config_info *)&fw_data->config_data[offset];
	dev_info(dev, "Write config %d: %s\n", config_num, cfg_info->name);

	int max_offset = offset + cpu_to_be32(hdr->config_size[config_num]);
	int num_subblocks = cpu_to_be32(cfg_info->blk_data.num_subblocks);

	offset += sizeof(struct tas25xx_config_info);

	for (int i = 0; i < num_subblocks; i++) {
		cmd = (struct tas25xx_cmd *)&fw_data->config_data[offset];
		length = tas25xx_process_block(dev, regmap, cmd,
			max_offset - offset);
		if (length < 0)
			return length;

		offset += length;
	}

	/* Reset Book to 0 */
	regmap_write(regmap, TAS2562_PAGE_CTRL, 0);
	regmap_write(regmap, TAS2562_BOOK_CTRL, 0);

	return 0;
}
EXPORT_SYMBOL_GPL(tas25xx_write_config);


struct tas25xx_fw_data *tas25xx_parse_fw(struct device *dev,
	const struct firmware *fw)
{
	u32 total_prog_sz = 0;
	u32 total_config_sz = 0;
	u32 prog_num = 0;
	u32 config_num = 0;
	int hdr_size = sizeof(struct tas25xx_fw_hdr);
	struct tas25xx_fw_data *fw_data = NULL;

	fw_data = devm_kzalloc(dev, sizeof(struct tas25xx_fw_data), GFP_KERNEL);
	if (!fw_data)
		goto err_fw;

	if (fw->size < hdr_size)
		goto err_data;

	fw_data->hdr = devm_kzalloc(dev, hdr_size, GFP_KERNEL);
	if (!fw_data->hdr)
		goto err_data;

	memcpy(fw_data->hdr, &fw->data[0], hdr_size);

	for (int i = 0; i < cpu_to_be32(fw_data->hdr->num_programs); i++)
		total_prog_sz += cpu_to_be32(fw_data->hdr->prog_size[i]);

	for (int i = 0; i < cpu_to_be32(fw_data->hdr->num_configs); i++)
		total_config_sz += cpu_to_be32(fw_data->hdr->config_size[i]);

	if (fw->size < hdr_size + total_prog_sz + total_config_sz)
		goto err_hdr;

	fw_data->prog_data = devm_kzalloc(dev, total_prog_sz, GFP_KERNEL);
	if (!fw_data->prog_data)
		goto err_hdr;

	memcpy(fw_data->prog_data, &fw->data[hdr_size], total_prog_sz);

	fw_data->config_data = devm_kzalloc(dev, total_config_sz, GFP_KERNEL);
	if (!fw_data->config_data)
		goto err_prog;

	memcpy(fw_data->config_data, &fw->data[hdr_size + total_prog_sz],
		total_config_sz);

	prog_num = cpu_to_be32(fw_data->hdr->num_programs);
	config_num = cpu_to_be32(fw_data->hdr->num_configs);
	dev_info(dev, "Firmware loaded: programs %d, configs %d\n",
		prog_num, config_num);

	return fw_data;

err_prog:
	devm_kfree(dev, fw_data->prog_data);
err_hdr:
	devm_kfree(dev, fw_data->hdr);
err_data:
	devm_kfree(dev, fw_data);
err_fw:
	release_firmware(fw);

	return NULL;
}
EXPORT_SYMBOL_GPL(tas25xx_parse_fw);

MODULE_DESCRIPTION("TAS25xx DSP library");
MODULE_AUTHOR("Dan Murphy <dmurphy@ti.com>");
MODULE_LICENSE("GPL");
