// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NAND Controller Driver for Loongson-1 SoC
 *
 * Copyright (C) 2015-2024 Keguang Zhang <keguang.zhang@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>

/* Loongson-1 NAND Controller Registers */
#define LS1X_NAND_CMD		0x0
#define LS1X_NAND_ADDR1		0x4
#define LS1X_NAND_ADDR2		0x8
#define LS1X_NAND_TIMING	0xc
#define LS1X_NAND_IDL		0x10
#define LS1X_NAND_IDH_STATUS	0x14
#define LS1X_NAND_PARAM		0x18
#define LS1X_NAND_OP_NUM	0x1c

/* NAND Command Register Bits */
#define LS1X_NAND_CMD_OP_DONE		BIT(10)
#define LS1X_NAND_CMD_OP_SPARE		BIT(9)
#define LS1X_NAND_CMD_OP_MAIN		BIT(8)
#define LS1X_NAND_CMD_STATUS		BIT(7)
#define LS1X_NAND_CMD_RESET		BIT(6)
#define LS1X_NAND_CMD_READID		BIT(5)
#define LS1X_NAND_CMD_BLOCKS_ERASE	BIT(4)
#define LS1X_NAND_CMD_ERASE		BIT(3)
#define LS1X_NAND_CMD_WRITE		BIT(2)
#define LS1X_NAND_CMD_READ		BIT(1)
#define LS1X_NAND_CMD_VALID		BIT(0)

#define LS1X_NAND_CMD_OP_AREA_MASK	GENMASK(9, 8)
#define LS1X_NAND_WAIT_CYCLE_MASK	GENMASK(7, 0)
#define LS1X_NAND_HOLD_CYCLE_MASK	GENMASK(15, 8)
#define LS1X_NAND_CELL_SIZE_MASK	GENMASK(11, 8)

#define LS1X_NAND_MAX_ADDR_CYC		5U
#define LS1X_NAND_DMA_ADDR		0x1fe78040

#define BITS_PER_WORD		(4 * BITS_PER_BYTE)

struct ls1x_nfc_op {
	char addrs[LS1X_NAND_MAX_ADDR_CYC];
	unsigned int naddrs;
	unsigned int addrs_offset;
	unsigned int addr1_reg;
	unsigned int addr2_reg;
	unsigned int aligned_offset;
	unsigned int row_shift;
	unsigned int cmd_reg;
	unsigned int rdy_timeout_ms;
	unsigned int len;
	size_t dma_len;
	bool restore_row;
	bool is_write;
	char *buf;
};

struct ls1x_nfc_data {
	unsigned int status_field;
	unsigned int op_scope_field;
	unsigned int hold_cycle;
	unsigned int wait_cycle;
	void (*parse_address)(struct ls1x_nfc_op *op);
};

struct ls1x_nfc {
	struct device *dev;
	struct nand_chip chip;
	struct nand_controller controller;
	const struct ls1x_nfc_data *data;
	void __iomem *reg_base;
	struct regmap *regmap;
	/* DMA Engine stuff */
	struct dma_chan *dma_chan;
	dma_cookie_t dma_cookie;
	struct completion dma_complete;
};

static const struct regmap_config ls1x_nand_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int ls1x_nand_op_cmd_mapping(struct nand_chip *chip,
				    struct ls1x_nfc_op *op, u8 opcode)
{
	struct ls1x_nfc *nfc = nand_get_controller_data(chip);
	int ret = 0;

	op->row_shift = chip->page_shift + 1;

	/* The controller abstracts the following NAND operations. */
	switch (opcode) {
	case NAND_CMD_RESET:
		op->cmd_reg = LS1X_NAND_CMD_RESET;
		break;
	case NAND_CMD_READID:
		op->cmd_reg = LS1X_NAND_CMD_READID;
		break;
	case NAND_CMD_ERASE1:
	case NAND_CMD_ERASE2:
		op->cmd_reg = LS1X_NAND_CMD_ERASE;
		op->addrs_offset = 2;
		op->row_shift = chip->page_shift;
		break;
	case NAND_CMD_STATUS:
		op->cmd_reg = LS1X_NAND_CMD_STATUS;
		break;
	case NAND_CMD_SEQIN:
	case NAND_CMD_PAGEPROG:
		op->cmd_reg = LS1X_NAND_CMD_WRITE;
		op->is_write = true;
		break;
	case NAND_CMD_RNDOUT:
	case NAND_CMD_RNDOUTSTART:
		op->restore_row = true;
		fallthrough;
	case NAND_CMD_READ0:
	case NAND_CMD_READSTART:
		op->cmd_reg = LS1X_NAND_CMD_READ;
		break;
	default:
		dev_err(nfc->dev, "Opcode not supported: %u\n", opcode);
		return -EOPNOTSUPP;
	}

	return ret;
}

static int ls1x_nand_parse_instructions(struct nand_chip *chip,
					const struct nand_subop *subop,
					struct ls1x_nfc_op *op)
{
	unsigned int op_id;
	int ret;

	for (op_id = 0; op_id < subop->ninstrs; op_id++) {
		const struct nand_op_instr *instr = &subop->instrs[op_id];
		unsigned int offset, naddrs;
		const u8 *addrs;

		switch (instr->type) {
		case NAND_OP_CMD_INSTR:
			ret = ls1x_nand_op_cmd_mapping(chip, op,
						       instr->ctx.cmd.opcode);
			if (ret < 0)
				return ret;
			break;
		case NAND_OP_ADDR_INSTR:
			naddrs = nand_subop_get_num_addr_cyc(subop, op_id);
			if (naddrs > LS1X_NAND_MAX_ADDR_CYC)
				return -EOPNOTSUPP;
			op->naddrs = naddrs;
			offset = nand_subop_get_addr_start_off(subop, op_id);
			addrs = &instr->ctx.addr.addrs[offset];
			memcpy(op->addrs + op->addrs_offset, addrs, naddrs);
			break;
		case NAND_OP_DATA_IN_INSTR:
		case NAND_OP_DATA_OUT_INSTR:
			offset = nand_subop_get_data_start_off(subop, op_id);
			op->len = nand_subop_get_data_len(subop, op_id);
			if (instr->type == NAND_OP_DATA_IN_INSTR)
				op->buf = instr->ctx.data.buf.in + offset;
			else if (instr->type == NAND_OP_DATA_OUT_INSTR)
				op->buf =
				    (void *)instr->ctx.data.buf.out + offset;

			break;
		case NAND_OP_WAITRDY_INSTR:
			op->rdy_timeout_ms = instr->ctx.waitrdy.timeout_ms;
			break;
		default:
			break;
		}
	}

	return 0;
}

static void ls1b_nand_parse_address(struct ls1x_nfc_op *op)
{
	int i;

	for (i = 0; i < LS1X_NAND_MAX_ADDR_CYC; i++) {
		if (i < 2)
			op->addr1_reg |= (u32)op->addrs[i] << i * BITS_PER_BYTE;
		else if (i < 4)
			op->addr1_reg |=
			    (u32)op->addrs[i] << (op->row_shift +
						  (i - 2) * BITS_PER_BYTE);
		else
			op->addr2_reg |=
			    (u32)op->addrs[i] >> (BITS_PER_WORD -
						  op->row_shift - (i - 4) *
						  BITS_PER_BYTE);
	}
}

static void ls1c_nand_parse_address(struct ls1x_nfc_op *op)
{
	int i;

	for (i = 0; i < LS1X_NAND_MAX_ADDR_CYC; i++) {
		if (i < 2)
			op->addr1_reg |= (u32)op->addrs[i] << i * BITS_PER_BYTE;
		else
			op->addr2_reg |=
			    (u32)op->addrs[i] << (i - 2) * BITS_PER_BYTE;
	}
}

static void ls1x_nand_trigger_op(struct ls1x_nfc *nfc, struct ls1x_nfc_op *op)
{
	struct nand_chip *chip = &nfc->chip;
	struct mtd_info *mtd = nand_to_mtd(chip);
	int col0 = op->addrs[0];
	short col;

	/* restore row address for column change */
	if (op->restore_row) {
		op->addr2_reg = readl(nfc->reg_base + LS1X_NAND_ADDR2);
		op->addr1_reg = readl(nfc->reg_base + LS1X_NAND_ADDR1);
		op->addr1_reg &= ~(mtd->writesize - 1);
	}

	if (!IS_ALIGNED(col0, chip->buf_align)) {
		col0 = ALIGN_DOWN(op->addrs[0], chip->buf_align);
		op->aligned_offset = op->addrs[0] - col0;
		op->addrs[0] = col0;
	}

	if (nfc->data->parse_address)
		nfc->data->parse_address(op);

	/* set address */
	writel(op->addr1_reg, nfc->reg_base + LS1X_NAND_ADDR1);
	writel(op->addr2_reg, nfc->reg_base + LS1X_NAND_ADDR2);

	/* set data length */
	op->dma_len = ALIGN(op->len + op->aligned_offset, chip->buf_align);
	if (op->cmd_reg & LS1X_NAND_CMD_ERASE)
		writel(1, nfc->reg_base + LS1X_NAND_OP_NUM);
	else
		writel(op->dma_len, nfc->reg_base + LS1X_NAND_OP_NUM);

	/* set operation area */
	col = op->addrs[1] << BITS_PER_BYTE | op->addrs[0];
	if (op->len) {
		if (col < mtd->writesize)
			op->cmd_reg |= LS1X_NAND_CMD_OP_MAIN;

		op->cmd_reg |= LS1X_NAND_CMD_OP_SPARE;
	}

	/* set operation scope */
	if (nfc->data->op_scope_field) {
		int op_area = op->cmd_reg & LS1X_NAND_CMD_OP_AREA_MASK;
		unsigned int op_scope;

		switch (op_area) {
		case LS1X_NAND_CMD_OP_MAIN:
			op_scope = mtd->writesize;
			break;
		case LS1X_NAND_CMD_OP_SPARE:
			op_scope = mtd->oobsize;
			break;
		case LS1X_NAND_CMD_OP_AREA_MASK:
			op_scope = mtd->writesize + mtd->oobsize;
			break;
		default:
			op_scope = 0;
			break;
		}

		op_scope <<= __ffs(nfc->data->op_scope_field);
		regmap_update_bits(nfc->regmap, LS1X_NAND_PARAM,
				   nfc->data->op_scope_field, op_scope);
	}

	/* set command */
	writel(op->cmd_reg, nfc->reg_base + LS1X_NAND_CMD);

	/* trigger operation */
	regmap_write_bits(nfc->regmap, LS1X_NAND_CMD,
			  LS1X_NAND_CMD_VALID, LS1X_NAND_CMD_VALID);
}

static int ls1x_nand_wait_for_op_done(struct ls1x_nfc *nfc,
				      struct ls1x_nfc_op *op)
{
	unsigned int val;
	int ret = 0;

	if (op->rdy_timeout_ms) {
		ret = regmap_read_poll_timeout(nfc->regmap, LS1X_NAND_CMD,
					       val, val & LS1X_NAND_CMD_OP_DONE,
					       0, op->rdy_timeout_ms * 1000);
		if (ret)
			dev_err(nfc->dev, "operation failed\n");
	}

	return ret;
}

static void ls1x_nand_dma_callback(void *data)
{
	struct ls1x_nfc *nfc = (struct ls1x_nfc *)data;
	struct dma_chan *chan = nfc->dma_chan;
	struct device *dev = chan->device->dev;
	enum dma_status status;

	status = dmaengine_tx_status(chan, nfc->dma_cookie, NULL);
	if (likely(status == DMA_COMPLETE))
		dev_dbg(dev, "DMA complete with cookie=%d\n", nfc->dma_cookie);
	else
		dev_err(dev, "DMA error with cookie=%d\n", nfc->dma_cookie);

	complete(&nfc->dma_complete);
}

static int ls1x_nand_dma_transfer(struct ls1x_nfc *nfc,
				  struct ls1x_nfc_op *op)
{
	struct nand_chip *chip = &nfc->chip;
	struct dma_chan *chan = nfc->dma_chan;
	struct device *dev = chan->device->dev;
	struct dma_async_tx_descriptor *desc;
	enum dma_data_direction data_dir =
	    op->is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	enum dma_transfer_direction xfer_dir =
	    op->is_write ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM;
	void *buf = op->buf;
	char *dma_buf = NULL;
	dma_addr_t dma_addr;
	int ret;

	if (IS_ALIGNED((uintptr_t)buf, chip->buf_align) &&
	    IS_ALIGNED(op->len, chip->buf_align)) {
		dma_addr = dma_map_single(dev, buf, op->len, data_dir);
		if (dma_mapping_error(dev, dma_addr)) {
			dev_err(dev, "failed to map DMA buffer\n");
			return -ENXIO;
		}
	} else if (!op->is_write) {
		dma_buf = dma_alloc_coherent(dev, op->dma_len, &dma_addr,
					     GFP_KERNEL);
		if (!dma_buf)
			return -ENOMEM;
	} else {
		dev_err(dev, "subpage writing not supported\n");
		return -EOPNOTSUPP;
	}

	desc = dmaengine_prep_slave_single(chan, dma_addr, op->dma_len,
					   xfer_dir, DMA_PREP_INTERRUPT);
	if (!desc) {
		dev_err(dev, "failed to prepare DMA descriptor\n");
		ret = PTR_ERR(desc);
		goto err;
	}
	desc->callback = ls1x_nand_dma_callback;
	desc->callback_param = nfc;

	nfc->dma_cookie = dmaengine_submit(desc);
	ret = dma_submit_error(nfc->dma_cookie);
	if (ret) {
		dev_err(dev, "failed to submit DMA descriptor\n");
		goto err;
	}

	dev_dbg(dev, "issue DMA with cookie=%d\n", nfc->dma_cookie);
	dma_async_issue_pending(chan);

	ret = wait_for_completion_timeout(&nfc->dma_complete,
					  msecs_to_jiffies(2000));
	if (!ret) {
		dmaengine_terminate_sync(chan);
		reinit_completion(&nfc->dma_complete);
		ret = -ETIMEDOUT;
		goto err;
	}
	ret = 0;

	if (dma_buf)
		memcpy(buf, dma_buf + op->aligned_offset, op->len);
err:
	if (dma_buf)
		dma_free_coherent(dev, op->dma_len, dma_buf, dma_addr);
	else
		dma_unmap_single(dev, dma_addr, op->len, data_dir);

	return ret;
}

static int ls1x_nand_data_type_exec(struct nand_chip *chip,
				    const struct nand_subop *subop)
{
	struct ls1x_nfc *nfc = nand_get_controller_data(chip);
	struct ls1x_nfc_op op = { };
	int ret;

	ret = ls1x_nand_parse_instructions(chip, subop, &op);
	if (ret)
		return ret;

	ls1x_nand_trigger_op(nfc, &op);

	ret = ls1x_nand_dma_transfer(nfc, &op);
	if (ret)
		return ret;

	return ls1x_nand_wait_for_op_done(nfc, &op);
}

static int ls1x_nand_misc_type_exec(struct nand_chip *chip,
				    const struct nand_subop *subop,
				    struct ls1x_nfc_op *op)
{
	struct ls1x_nfc *nfc = nand_get_controller_data(chip);
	int ret;

	ret = ls1x_nand_parse_instructions(chip, subop, op);
	if (ret)
		return ret;

	ls1x_nand_trigger_op(nfc, op);

	return ls1x_nand_wait_for_op_done(nfc, op);
}

static int ls1x_nand_zerolen_type_exec(struct nand_chip *chip,
				       const struct nand_subop *subop)
{
	struct ls1x_nfc_op op = { };

	return ls1x_nand_misc_type_exec(chip, subop, &op);
}

static int ls1x_nand_read_id_type_exec(struct nand_chip *chip,
				       const struct nand_subop *subop)
{
	struct ls1x_nfc *nfc = nand_get_controller_data(chip);
	struct ls1x_nfc_op op = { };
	int i, ret;
	union {
		char ids[5];
		struct {
			int idl;
			char idh;
		};
	} nand_id;

	ret = ls1x_nand_misc_type_exec(chip, subop, &op);
	if (ret) {
		dev_err(nfc->dev, "failed to read ID! %d\n", ret);
		return ret;
	}

	nand_id.idl = readl(nfc->reg_base + LS1X_NAND_IDL);
	nand_id.idh = readb(nfc->reg_base + LS1X_NAND_IDH_STATUS);

	for (i = 0; i < min(sizeof(nand_id.ids), op.len); i++)
		op.buf[i] = nand_id.ids[sizeof(nand_id.ids) - 1 - i];

	return ret;
}

static int ls1x_nand_read_status_type_exec(struct nand_chip *chip,
					   const struct nand_subop *subop)
{
	struct ls1x_nfc *nfc = nand_get_controller_data(chip);
	struct ls1x_nfc_op op = { };
	int val, ret;

	ret = ls1x_nand_misc_type_exec(chip, subop, &op);
	if (ret) {
		dev_err(nfc->dev, "failed to read status! %d\n", ret);
		return ret;
	}

	val = readl(nfc->reg_base +
		    LS1X_NAND_IDH_STATUS) & ~nfc->data->status_field;
	op.buf[0] = val << ffs(nfc->data->status_field);

	return ret;
}

static const struct nand_op_parser ls1x_nand_op_parser = NAND_OP_PARSER(
	NAND_OP_PARSER_PATTERN(
		ls1x_nand_read_id_type_exec,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_ADDR_ELEM(false, LS1X_NAND_MAX_ADDR_CYC),
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(false, 8)),
	NAND_OP_PARSER_PATTERN(
		ls1x_nand_read_status_type_exec,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(false, 1)),
	NAND_OP_PARSER_PATTERN(
		ls1x_nand_zerolen_type_exec,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(false)),
	NAND_OP_PARSER_PATTERN(
		ls1x_nand_zerolen_type_exec,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_ADDR_ELEM(false, LS1X_NAND_MAX_ADDR_CYC),
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(false)),
	NAND_OP_PARSER_PATTERN(
		ls1x_nand_data_type_exec,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_ADDR_ELEM(false, LS1X_NAND_MAX_ADDR_CYC),
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(true),
		NAND_OP_PARSER_PAT_DATA_IN_ELEM(false, 0)),
	NAND_OP_PARSER_PATTERN(
		ls1x_nand_data_type_exec,
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_ADDR_ELEM(false, LS1X_NAND_MAX_ADDR_CYC),
		NAND_OP_PARSER_PAT_DATA_OUT_ELEM(false, 0),
		NAND_OP_PARSER_PAT_CMD_ELEM(false),
		NAND_OP_PARSER_PAT_WAITRDY_ELEM(true)),
	);

static int ls1x_nand_check_op(struct nand_chip *chip,
			      const struct nand_operation *op)
{
	const struct nand_op_instr *instr;
	int op_id;

	for (op_id = 0; op_id < op->ninstrs; op_id++) {
		instr = &op->instrs[op_id];

		switch (instr->type) {
		case NAND_OP_CMD_INSTR:
			if (instr->ctx.cmd.opcode != NAND_CMD_RESET &&
			    instr->ctx.cmd.opcode != NAND_CMD_READID &&
			    instr->ctx.cmd.opcode != NAND_CMD_ERASE1 &&
			    instr->ctx.cmd.opcode != NAND_CMD_ERASE2 &&
			    instr->ctx.cmd.opcode != NAND_CMD_STATUS &&
			    instr->ctx.cmd.opcode != NAND_CMD_SEQIN &&
			    instr->ctx.cmd.opcode != NAND_CMD_PAGEPROG &&
			    instr->ctx.cmd.opcode != NAND_CMD_RNDOUT &&
			    instr->ctx.cmd.opcode != NAND_CMD_RNDOUTSTART &&
			    instr->ctx.cmd.opcode != NAND_CMD_READ0 &&
			    instr->ctx.cmd.opcode != NAND_CMD_READSTART)
				return -EOPNOTSUPP;
			break;
		case NAND_OP_ADDR_INSTR:
			if (instr->ctx.addr.naddrs > LS1X_NAND_MAX_ADDR_CYC)
				return -EOPNOTSUPP;
			break;
		default:
			break;
		}
	}

	return 0;
}

static int ls1x_nand_exec_op(struct nand_chip *chip,
			     const struct nand_operation *op, bool check_only)
{
	if (check_only)
		return ls1x_nand_check_op(chip, op);

	return nand_op_parser_exec_op(chip, &ls1x_nand_op_parser, op,
				      check_only);
}

static int ls1x_nand_attach_chip(struct nand_chip *chip)
{
	struct ls1x_nfc *nfc = nand_get_controller_data(chip);
	u64 chipsize = nanddev_target_size(&chip->base);
	int cell_size = 0;

	switch (chipsize) {
	case SZ_128M:
		cell_size = 0x0;
		break;
	case SZ_256M:
		cell_size = 0x1;
		break;
	case SZ_512M:
		cell_size = 0x2;
		break;
	case SZ_1G:
		cell_size = 0x3;
		break;
	case SZ_2G:
		cell_size = 0x4;
		break;
	case SZ_4G:
		cell_size = 0x5;
		break;
	case SZ_8G:
		cell_size = 0x6;
		break;
	case SZ_16G:
		cell_size = 0x7;
		break;
	default:
		dev_err(nfc->dev, "unsupported chip size: %llu MB\n", chipsize);
		return -EOPNOTSUPP;
	}

	/* set cell size */
	regmap_update_bits(nfc->regmap, LS1X_NAND_PARAM,
			   LS1X_NAND_CELL_SIZE_MASK,
			   FIELD_PREP(LS1X_NAND_CELL_SIZE_MASK, cell_size));

	regmap_update_bits(nfc->regmap, LS1X_NAND_TIMING,
			   LS1X_NAND_HOLD_CYCLE_MASK,
			   FIELD_PREP(LS1X_NAND_HOLD_CYCLE_MASK,
				      nfc->data->hold_cycle));
	regmap_update_bits(nfc->regmap, LS1X_NAND_TIMING,
			   LS1X_NAND_WAIT_CYCLE_MASK,
			   FIELD_PREP(LS1X_NAND_WAIT_CYCLE_MASK,
				      nfc->data->wait_cycle));

	chip->ecc.read_page_raw = nand_monolithic_read_page_raw;
	chip->ecc.write_page_raw = nand_monolithic_write_page_raw;

	return 0;
}

static const struct nand_controller_ops ls1x_nfc_ops = {
	.exec_op = ls1x_nand_exec_op,
	.attach_chip = ls1x_nand_attach_chip,
};

static void ls1x_nand_controller_cleanup(struct ls1x_nfc *nfc)
{
	if (nfc->dma_chan)
		dma_release_channel(nfc->dma_chan);
}

static int ls1x_nand_controller_init(struct ls1x_nfc *nfc)
{
	struct device *dev = nfc->dev;
	struct dma_chan *chan;
	struct dma_slave_config cfg = { };
	int ret;

	nfc->regmap = devm_regmap_init_mmio(dev, nfc->reg_base,
					    &ls1x_nand_regmap_config);
	if (IS_ERR(nfc->regmap))
		return dev_err_probe(dev, PTR_ERR(nfc->regmap),
				     "failed to init regmap\n");

	chan = dma_request_chan(dev, "rxtx");
	if (IS_ERR(chan))
		return dev_err_probe(dev, PTR_ERR(chan),
				     "failed to request DMA channel\n");
	nfc->dma_chan = chan;

	cfg.src_addr = LS1X_NAND_DMA_ADDR;
	cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_addr = LS1X_NAND_DMA_ADDR;
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	ret = dmaengine_slave_config(nfc->dma_chan, &cfg);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to config DMA channel\n");

	init_completion(&nfc->dma_complete);

	dev_dbg(dev, "got %s for %s access\n",
		dma_chan_name(nfc->dma_chan), dev_name(dev));

	return 0;
}

static int ls1x_nand_chip_init(struct ls1x_nfc *nfc)
{
	struct device *dev = nfc->dev;
	int nchips = of_get_child_count(dev->of_node);
	struct device_node *chip_np;
	struct nand_chip *chip = &nfc->chip;
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret = 0;

	if (nchips != 1)
		return dev_err_probe(dev, -EINVAL,
				     "Currently one NAND chip supported\n");

	chip_np = of_get_next_child(dev->of_node, NULL);
	if (!chip_np)
		return dev_err_probe(dev, -ENODEV,
				     "failed to get child node for NAND chip\n");

	chip->controller = &nfc->controller;
	chip->options = NAND_NO_SUBPAGE_WRITE | NAND_USES_DMA | NAND_BROKEN_XD;
	chip->buf_align = 16;
	nand_set_controller_data(chip, nfc);
	nand_set_flash_node(chip, chip_np);

	mtd->dev.parent = dev;
	mtd->name = "ls1x-nand";
	mtd->owner = THIS_MODULE;

	ret = nand_scan(chip, 1);
	if (ret) {
		of_node_put(chip_np);
		return ret;
	}

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "failed to register MTD device! %d\n", ret);
		nand_cleanup(chip);
		of_node_put(chip_np);
	}

	return ret;
}

static int ls1x_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct ls1x_nfc_data *data;
	struct ls1x_nfc *nfc;
	int ret;

	data = of_device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	nfc = devm_kzalloc(dev, sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;

	nfc->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(nfc->reg_base))
		return PTR_ERR(nfc->reg_base);

	nand_controller_init(&nfc->controller);

	nfc->dev = dev;
	nfc->data = data;
	nfc->controller.ops = &ls1x_nfc_ops;

	ret = ls1x_nand_controller_init(nfc);
	if (ret)
		goto err;

	ret = ls1x_nand_chip_init(nfc);
	if (ret)
		goto err;

	platform_set_drvdata(pdev, nfc);

	return 0;
err:
	ls1x_nand_controller_cleanup(nfc);

	return ret;
}

static void ls1x_nand_remove(struct platform_device *pdev)
{
	struct ls1x_nfc *nfc = platform_get_drvdata(pdev);
	struct nand_chip *chip = &nfc->chip;
	int ret;

	ret = mtd_device_unregister(nand_to_mtd(chip));
	WARN_ON(ret);
	nand_cleanup(chip);
	ls1x_nand_controller_cleanup(nfc);
}

static const struct ls1x_nfc_data ls1b_nfc_data = {
	.status_field = GENMASK(15, 8),
	.hold_cycle = 0x2,
	.wait_cycle = 0xc,
	.parse_address = ls1b_nand_parse_address,
};

static const struct ls1x_nfc_data ls1c_nfc_data = {
	.status_field = GENMASK(23, 16),
	.op_scope_field = GENMASK(29, 16),
	.hold_cycle = 0x2,
	.wait_cycle = 0xc,
	.parse_address = ls1c_nand_parse_address,
};

static const struct of_device_id ls1x_nfc_match[] = {
	{
		.compatible = "loongson,ls1b-nand-controller",
		.data = &ls1b_nfc_data,
	},
	{
		.compatible = "loongson,ls1c-nand-controller",
		.data = &ls1c_nfc_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ls1x_nfc_match);

static struct platform_driver ls1x_nand_driver = {
	.probe = ls1x_nand_probe,
	.remove = ls1x_nand_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = ls1x_nfc_match,
	},
};

module_platform_driver(ls1x_nand_driver);

MODULE_AUTHOR("Keguang Zhang <keguang.zhang@gmail.com>");
MODULE_DESCRIPTION("Loongson-1 NAND Controller driver");
MODULE_LICENSE("GPL");
