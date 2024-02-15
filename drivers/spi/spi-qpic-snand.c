// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Authors:
 *	Md Sadre Alam <quic_mdalam@quicinc.com>
 *	Sricharan R <quic_srichara@quicinc.com>
 *	Varadarajan Narayanan <quic_varada@quicinc.com>
 */

#include <linux/mtd/spinand.h>
#include <linux/mtd/nand-qpic-common.h>

/* QSPI NAND config reg bits */
#define LOAD_CLK_CNTR_INIT_EN   BIT(28)
#define CLK_CNTR_INIT_VAL_VEC   0x924
#define FEA_STATUS_DEV_ADDR     0xc0
#define SPI_CFG			BIT(0)
#define SPI_NUM_ADDR		0xDA4DB
#define SPI_WAIT_CNT		0x10
#define QPIC_QSPI_NUM_CS	1
#define SPI_TRANSFER_MODE_x1	BIT(29)
#define SPI_TRANSFER_MODE_x4	(3 << 29)
#define SPI_WP			BIT(28)
#define SPI_HOLD		BIT(27)
#define QPIC_SET_FEATURE	BIT(31)

#define SPINAND_RESET		0xff
#define SPINAND_READID		0x9f
#define SPINAND_GET_FEATURE	0x0f
#define SPINAND_SET_FEATURE	0x1f
#define SPINAND_READ		0x13
#define SPINAND_ERASE		0xd8
#define SPINAND_WRITE_EN	0x06
#define SPINAND_PROGRAM_EXECUTE	0x10
#define SPINAND_PROGRAM_LOAD	0x84

#define snandc_set_read_loc_first(snandc, reg, cw_offset, read_size, is_last_read_loc)	\
snandc_set_reg(snandc, reg,			\
	      ((cw_offset) << READ_LOCATION_OFFSET) |		\
	      ((read_size) << READ_LOCATION_SIZE) |			\
	      ((is_last_read_loc) << READ_LOCATION_LAST))

#define snandc_set_read_loc_last(snandc, reg, cw_offset, read_size, is_last_read_loc)	\
snandc_set_reg(snandc, reg,			\
	      ((cw_offset) << READ_LOCATION_OFFSET) |		\
	      ((read_size) << READ_LOCATION_SIZE) |			\
	      ((is_last_read_loc) << READ_LOCATION_LAST))

struct qpic_snand_op {
	u32 cmd_reg;
	u32 addr1_reg;
	u32 addr2_reg;
};

struct snandc_read_status {
	__le32 snandc_flash;
	__le32 snandc_buffer;
	__le32 snandc_erased_cw;
};

void snandc_set_reg(struct qcom_nand_controller *snandc, int offset, u32 val)
{
	struct nandc_regs *regs = snandc->regs;
	__le32 *reg;

	reg = offset_to_nandc_reg(regs, offset);

	if (reg)
		*reg = cpu_to_le32(val);
}

static struct qcom_nand_controller *nand_to_qcom_snand(struct nand_device *nand)
{
	struct nand_ecc_engine *eng = nand->ecc.engine;

	return container_of(eng, struct qcom_nand_controller, ecc_eng);
}

static int qcom_snand_init(struct qcom_nand_controller *snandc)
{
	u32 snand_cfg_val = 0x0;
	int ret;

	snand_cfg_val |= (LOAD_CLK_CNTR_INIT_EN | (CLK_CNTR_INIT_VAL_VEC << 16)
			| (FEA_STATUS_DEV_ADDR << 8) | SPI_CFG);

	snandc_set_reg(snandc, NAND_FLASH_SPI_CFG, 0);
	snandc_set_reg(snandc, NAND_FLASH_SPI_CFG, snand_cfg_val);
	snandc_set_reg(snandc, NAND_NUM_ADDR_CYCLES, SPI_NUM_ADDR);
	snandc_set_reg(snandc, NAND_BUSY_CHECK_WAIT_CNT, SPI_WAIT_CNT);

	write_reg_dma(snandc, NAND_FLASH_SPI_CFG, 1, 0);
	write_reg_dma(snandc, NAND_FLASH_SPI_CFG, 1, 0);

	snand_cfg_val &= ~LOAD_CLK_CNTR_INIT_EN;
	snandc_set_reg(snandc, NAND_FLASH_SPI_CFG, snand_cfg_val);

	write_reg_dma(snandc, NAND_FLASH_SPI_CFG, 1, 0);

	write_reg_dma(snandc, NAND_NUM_ADDR_CYCLES, 1, 0);
	write_reg_dma(snandc, NAND_BUSY_CHECK_WAIT_CNT, 1, NAND_BAM_NEXT_SGL);

	ret = submit_descs(snandc);
	if (ret)
		dev_err(snandc->dev, "failure in sbumitting spiinit descriptor\n");

	return 0;
}

static int qcom_snand_ooblayout_ecc(struct mtd_info *mtd, int section,
				    struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct qcom_nand_controller *snandc = nand_to_qcom_snand(nand);
	struct qpic_ecc *qecc = snandc->ecc;

	if (section > 1)
		return -ERANGE;

	if (!section) {
		oobregion->length = (qecc->bytes * (qecc->steps - 1)) + qecc->bbm_size;
		oobregion->offset = 0;
	} else {
		oobregion->length = qecc->ecc_bytes_hw + qecc->spare_bytes;
		oobregion->offset = mtd->oobsize - oobregion->length;
	}

	return 0;
}

static int qcom_snand_ooblayout_free(struct mtd_info *mtd, int section,
				     struct mtd_oob_region *oobregion)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);
	struct qcom_nand_controller *snandc = nand_to_qcom_snand(nand);
	struct qpic_ecc *qecc = snandc->ecc;

	if (section)
		return -ERANGE;

	oobregion->length = qecc->steps * 4;
	oobregion->offset = ((qecc->steps - 1) * qecc->bytes) + qecc->bbm_size;

	return 0;
}

static const struct mtd_ooblayout_ops qcom_snand_ooblayout = {
	.ecc = qcom_snand_ooblayout_ecc,
	.free = qcom_snand_ooblayout_free,
};

static int qpic_snand_ecc_init_ctx_pipelined(struct nand_device *nand)
{
	struct qcom_nand_controller *snandc = nand_to_qcom_snand(nand);
	struct nand_ecc_props *conf = &nand->ecc.ctx.conf;
	struct nand_ecc_props *reqs = &nand->ecc.requirements;
	struct nand_ecc_props *user = &nand->ecc.user_conf;
	struct mtd_info *mtd = nanddev_to_mtd(nand);
	int step_size = 0, strength = 0, desired_correction = 0, steps;
	bool ecc_user = false;
	int cwperpage, bad_block_byte;
	struct qpic_ecc *ecc_cfg;

	cwperpage = mtd->writesize / NANDC_STEP_SIZE;

	ecc_cfg = kzalloc(sizeof(*ecc_cfg), GFP_KERNEL);
	if (!ecc_cfg)
		return -ENOMEM;

	nand->ecc.ctx.priv = ecc_cfg;

	if (user->step_size && user->strength) {
		step_size = user->step_size;
		strength = user->strength;
		ecc_user = true;
	} else if (reqs->step_size && reqs->strength) {
		step_size = reqs->step_size;
		strength = reqs->strength;
	}

	if (step_size && strength) {
		steps = mtd->writesize / step_size;
		desired_correction = steps * strength;
	}

	ecc_cfg->ecc_bytes_hw = 7;
	ecc_cfg->spare_bytes = 4;
	ecc_cfg->bbm_size = 1;
	ecc_cfg->bch_enabled = true;
	ecc_cfg->bytes = ecc_cfg->ecc_bytes_hw + ecc_cfg->spare_bytes + ecc_cfg->bbm_size;

	ecc_cfg->steps = 4;
	ecc_cfg->strength = 4;
	ecc_cfg->step_size = 512;

	mtd_set_ooblayout(mtd, &qcom_snand_ooblayout);

	ecc_cfg->cw_data = 516;
	ecc_cfg->cw_size = ecc_cfg->cw_data + ecc_cfg->bytes;
	bad_block_byte = mtd->writesize - ecc_cfg->cw_size * (cwperpage - 1) + 1;

	ecc_cfg->cfg0 = (cwperpage - 1) << CW_PER_PAGE
				| ecc_cfg->cw_data << UD_SIZE_BYTES
				| 1 << DISABLE_STATUS_AFTER_WRITE
				| 3 << NUM_ADDR_CYCLES
				| ecc_cfg->ecc_bytes_hw << ECC_PARITY_SIZE_BYTES_RS
				| 0 << STATUS_BFR_READ
				| 1 << SET_RD_MODE_AFTER_STATUS
				| ecc_cfg->spare_bytes << SPARE_SIZE_BYTES;

	ecc_cfg->cfg1 = 0 << NAND_RECOVERY_CYCLES
				| 0 <<  CS_ACTIVE_BSY
				| bad_block_byte << BAD_BLOCK_BYTE_NUM
				| 0 << BAD_BLOCK_IN_SPARE_AREA
				| 20 << WR_RD_BSY_GAP
				| 0 << WIDE_FLASH
				| ecc_cfg->bch_enabled << ENABLE_BCH_ECC;

	ecc_cfg->cfg0_raw = (cwperpage - 1) << CW_PER_PAGE
				| ecc_cfg->cw_size << UD_SIZE_BYTES
				| 3 << NUM_ADDR_CYCLES
				| 0 << SPARE_SIZE_BYTES;

	ecc_cfg->cfg1_raw = 0 << NAND_RECOVERY_CYCLES
				| 0 << CS_ACTIVE_BSY
				| 17 << BAD_BLOCK_BYTE_NUM
				| 1 << BAD_BLOCK_IN_SPARE_AREA
				| 20 << WR_RD_BSY_GAP
				| 0 << WIDE_FLASH
				| 1 << DEV0_CFG1_ECC_DISABLE;

	ecc_cfg->ecc_bch_cfg = !ecc_cfg->bch_enabled << ECC_CFG_ECC_DISABLE
				| 0 << ECC_SW_RESET
				| ecc_cfg->cw_data << ECC_NUM_DATA_BYTES
				| 1 << ECC_FORCE_CLK_OPEN
				| 0 << ECC_MODE
				| ecc_cfg->ecc_bytes_hw << ECC_PARITY_SIZE_BYTES_BCH;

	ecc_cfg->ecc_buf_cfg = 0x203 << NUM_STEPS;
	ecc_cfg->clrflashstatus = FS_READY_BSY_N;
	ecc_cfg->clrreadstatus = 0xc0;

	conf->step_size = ecc_cfg->step_size;
	conf->strength = ecc_cfg->strength;

	if (ecc_cfg->strength < strength)
		dev_warn(snandc->dev, "Unable to fulfill ECC requirements of %u bits.\n", strength);

	dev_info(snandc->dev, "ECC strength: %u bits per %u bytes\n",
		 ecc_cfg->strength, ecc_cfg->step_size);

	return 0;
}

static void qpic_snand_ecc_cleanup_ctx_pipelined(struct nand_device *nand)
{
	struct qpic_ecc *ecc_cfg = nand_to_ecc_ctx(nand);

	kfree(ecc_cfg);
}

static int qpic_snand_ecc_prepare_io_req_pipelined(struct nand_device *nand,
						   struct nand_page_io_req *req)
{
	struct qcom_nand_controller *snandc = nand_to_qcom_snand(nand);
	struct qpic_ecc *ecc_cfg = nand_to_ecc_ctx(nand);

	snandc->ecc = ecc_cfg;
	snandc->raw = false;
	snandc->oob_read = false;

	if (req->mode == MTD_OPS_RAW) {
		if (req->ooblen)
			snandc->oob_read = true;
		snandc->raw = true;
	}

	return 0;
}

static int qpic_snand_ecc_finish_io_req_pipelined(struct nand_device *nand,
						  struct nand_page_io_req *req)
{
	struct qcom_nand_controller *snandc = nand_to_qcom_snand(nand);
	struct mtd_info *mtd = nanddev_to_mtd(nand);

	if (req->mode == MTD_OPS_RAW || req->type != NAND_PAGE_READ)
		return 0;

	if (snandc->ecc_stats.failed)
		mtd->ecc_stats.failed += snandc->ecc_stats.failed;
	mtd->ecc_stats.corrected += snandc->ecc_stats.corrected;

	return snandc->ecc_stats.failed ? -EBADMSG : snandc->ecc_stats.bitflips;
}

static struct nand_ecc_engine_ops qcom_snand_ecc_engine_ops_pipelined = {
	.init_ctx = qpic_snand_ecc_init_ctx_pipelined,
	.cleanup_ctx = qpic_snand_ecc_cleanup_ctx_pipelined,
	.prepare_io_req = qpic_snand_ecc_prepare_io_req_pipelined,
	.finish_io_req = qpic_snand_ecc_finish_io_req_pipelined,
};

/* helper to configure location register values */
static void snandc_set_read_loc(struct qcom_nand_controller *snandc, int cw, int reg,
				int cw_offset, int read_size, int is_last_read_loc)
{
	int reg_base = NAND_READ_LOCATION_0;

	if (cw == 3)
		reg_base = NAND_READ_LOCATION_LAST_CW_0;

	reg_base += reg * 4;

	if (cw == 3)
		return snandc_set_read_loc_last(snandc, reg_base, cw_offset,
				read_size, is_last_read_loc);
	else
		return snandc_set_read_loc_first(snandc, reg_base, cw_offset,
				read_size, is_last_read_loc);
}

static void
snandc_config_cw_read(struct qcom_nand_controller *snandc, bool use_ecc, int cw)
{
	int reg = NAND_READ_LOCATION_0;

	if (cw == 3)
		reg = NAND_READ_LOCATION_LAST_CW_0;

	if (snandc->props->is_bam)
		write_reg_dma(snandc, reg, 4, NAND_BAM_NEXT_SGL);

	write_reg_dma(snandc, NAND_FLASH_CMD, 1, NAND_BAM_NEXT_SGL);
	write_reg_dma(snandc, NAND_EXEC_CMD, 1, NAND_BAM_NEXT_SGL);

	read_reg_dma(snandc, NAND_FLASH_STATUS, 2, 0);
	read_reg_dma(snandc, NAND_ERASED_CW_DETECT_STATUS, 1,
		     NAND_BAM_NEXT_SGL);
}

static int qpic_snand_block_erase(struct qcom_nand_controller *snandc)
{
	struct qpic_ecc *ecc_cfg = snandc->ecc;
	int ret;

	snandc->buf_count = 0;
	snandc->buf_start = 0;
	clear_read_regs(snandc);
	clear_bam_transaction(snandc);

	snandc_set_reg(snandc, NAND_FLASH_CMD, snandc->cmd);
	snandc_set_reg(snandc, NAND_ADDR0, snandc->addr1);
	snandc_set_reg(snandc, NAND_ADDR1, snandc->addr2);
	snandc_set_reg(snandc, NAND_DEV0_CFG0, ecc_cfg->cfg0_raw & ~(7 << CW_PER_PAGE));
	snandc_set_reg(snandc, NAND_DEV0_CFG1, ecc_cfg->cfg1_raw);
	snandc_set_reg(snandc, NAND_EXEC_CMD, 1);

	write_reg_dma(snandc, NAND_FLASH_CMD, 3, NAND_BAM_NEXT_SGL);
	write_reg_dma(snandc, NAND_DEV0_CFG0, 2, NAND_BAM_NEXT_SGL);
	write_reg_dma(snandc, NAND_EXEC_CMD, 1, NAND_BAM_NEXT_SGL);

	ret = submit_descs(snandc);
	if (ret) {
		dev_err(snandc->dev, "failure to erase block\n");
		return ret;
	}

	return 0;
}

static void config_snand_single_cw_page_read(struct qcom_nand_controller *snandc,
					     bool use_ecc, int cw)
{
	int reg;

	write_reg_dma(snandc, NAND_ADDR0, 2, 0);
	write_reg_dma(snandc, NAND_DEV0_CFG0, 3, 0);
	write_reg_dma(snandc, NAND_ERASED_CW_DETECT_CFG, 1, 0);
	write_reg_dma(snandc, NAND_ERASED_CW_DETECT_CFG, 1,
		      NAND_ERASED_CW_SET | NAND_BAM_NEXT_SGL);

	reg = NAND_READ_LOCATION_0;
	if (cw == 3)
		reg = NAND_READ_LOCATION_LAST_CW_0;
	write_reg_dma(snandc, reg, 4, NAND_BAM_NEXT_SGL);
	write_reg_dma(snandc, NAND_FLASH_CMD, 1, NAND_BAM_NEXT_SGL);
	write_reg_dma(snandc, NAND_EXEC_CMD, 1, NAND_BAM_NEXT_SGL);

	read_reg_dma(snandc, NAND_FLASH_STATUS, 2, 0);
	read_reg_dma(snandc, NAND_ERASED_CW_DETECT_STATUS, 1, NAND_BAM_NEXT_SGL);
}

static int qpic_snand_read_oob(struct qcom_nand_controller *snandc,
			       const struct spi_mem_op *op)
{
	struct qpic_ecc *ecc_cfg = snandc->ecc;
	u8 *oob_buf;
	int size, ret;
	int col, num_cw = 4, bbpos;
	u32 cfg0, cfg1, ecc_bch_cfg;

	oob_buf = op->data.buf.in;

	clear_bam_transaction(snandc);
	clear_read_regs(snandc);

	size = ecc_cfg->cw_size;
	col = ecc_cfg->cw_size * (num_cw - 1);

	/* prepare a clean read buffer */
	memset(snandc->data_buffer, 0xff, size);
	snandc_set_reg(snandc, NAND_ADDR0, (snandc->addr1 | col));
	snandc_set_reg(snandc, NAND_ADDR1, snandc->addr2);

	cfg0 = (ecc_cfg->cfg0_raw & ~(7U << CW_PER_PAGE)) |
		0 << CW_PER_PAGE;
	cfg1 = ecc_cfg->cfg1_raw;
	ecc_bch_cfg = 1 << ECC_CFG_ECC_DISABLE;

	snandc_set_reg(snandc, NAND_FLASH_CMD, snandc->cmd);
	snandc_set_reg(snandc, NAND_DEV0_CFG0, cfg0);
	snandc_set_reg(snandc, NAND_DEV0_CFG1, cfg1);
	snandc_set_reg(snandc, NAND_DEV0_ECC_CFG, ecc_bch_cfg);
	snandc_set_reg(snandc, NAND_EXEC_CMD, 1);

	config_snand_single_cw_page_read(snandc, false, num_cw - 1);

	read_data_dma(snandc, FLASH_BUF_ACC, snandc->data_buffer, size, 0);

	ret = submit_descs(snandc);
	if (ret)
		dev_err(snandc->dev, "failed to read oob\n");

	nandc_read_buffer_sync(snandc, true);
	u32 flash = le32_to_cpu(snandc->reg_read_buf[0]);

	if (flash & (FS_OP_ERR | FS_MPU_ERR))
		return -EIO;

	bbpos = 2048 - ecc_cfg->cw_size * (num_cw - 1);
	memcpy(op->data.buf.in, snandc->data_buffer + bbpos, op->data.nbytes);

	return ret;
}

static int snandc_check_error(struct qcom_nand_controller *snandc)
{
	struct snandc_read_status *buf;
	int i, num_cw = 4;
	bool serial_op_err = false, erased;

	nandc_read_buffer_sync(snandc, true);
	buf = (struct snandc_read_status *)snandc->reg_read_buf;

	for (i = 0; i < num_cw; i++, buf++) {
		u32 flash, buffer, erased_cw;

		flash = le32_to_cpu(buf->snandc_flash);
		buffer = le32_to_cpu(buf->snandc_buffer);
		erased_cw = le32_to_cpu(buf->snandc_erased_cw);

		if ((flash & FS_OP_ERR) && (buffer & BS_UNCORRECTABLE_BIT)) {
			erased = (erased_cw & ERASED_CW) == ERASED_CW ?
				true : false;
		} else if (flash & (FS_OP_ERR | FS_MPU_ERR)) {
			serial_op_err = true;
		}
	}

	if (serial_op_err)
		return -EIO;

	return 0;
}

static int qpic_snand_read_page_cache(struct qcom_nand_controller *snandc,
				      const struct spi_mem_op *op)
{
	struct qpic_ecc *ecc_cfg = snandc->ecc;
	u8 *data_buf;
	int ret, i;
	u32 cfg0, cfg1, ecc_bch_cfg, num_cw = 4;

	data_buf = op->data.buf.in;

	if (snandc->oob_read) {
		return qpic_snand_read_oob(snandc, op);
		snandc->oob_read = false;
	}

	snandc->buf_count = 0;
	snandc->buf_start = 0;
	clear_read_regs(snandc);

	cfg0 = (ecc_cfg->cfg0 & ~(7U << CW_PER_PAGE)) |
				(num_cw - 1) << CW_PER_PAGE;
	cfg1 = ecc_cfg->cfg1;
	ecc_bch_cfg = ecc_cfg->ecc_bch_cfg;

	snandc_set_reg(snandc, NAND_ADDR0, snandc->addr1);
	snandc_set_reg(snandc, NAND_ADDR1, snandc->addr2);
	snandc_set_reg(snandc, NAND_FLASH_CMD, snandc->cmd);
	snandc_set_reg(snandc, NAND_DEV0_CFG0, cfg0);
	snandc_set_reg(snandc, NAND_DEV0_CFG1, cfg1);
	snandc_set_reg(snandc, NAND_DEV0_ECC_CFG, ecc_bch_cfg);
	snandc_set_reg(snandc, NAND_FLASH_STATUS, ecc_cfg->clrflashstatus);
	snandc_set_reg(snandc, NAND_READ_STATUS, ecc_cfg->clrreadstatus);
	snandc_set_reg(snandc, NAND_EXEC_CMD, 1);
	snandc_set_read_loc(snandc, 0, 0, 0, ecc_cfg->cw_data, 1);

	clear_bam_transaction(snandc);

	write_reg_dma(snandc, NAND_ADDR0, 2, 0);
	write_reg_dma(snandc, NAND_DEV0_CFG0, 3, 0);
	write_reg_dma(snandc, NAND_ERASED_CW_DETECT_CFG, 1, 0);
	write_reg_dma(snandc, NAND_ERASED_CW_DETECT_CFG, 1,
		      NAND_ERASED_CW_SET | NAND_BAM_NEXT_SGL);

	for (i = 0; i < num_cw; i++) {
		int data_size;

		if (i == (num_cw - 1))
			data_size = 512 - ((num_cw - 1) << 2);
		else
			data_size = ecc_cfg->cw_data;

		if (data_buf)
			snandc_set_read_loc(snandc, i, 0, 0, data_size, 1);

		snandc_config_cw_read(snandc, true, i);

		if (data_buf)
			read_data_dma(snandc, FLASH_BUF_ACC, data_buf,
				      data_size, 0);

		if (data_buf)
			data_buf += data_size;
	}

	ret = submit_descs(snandc);
	if (ret) {
		dev_err(snandc->dev, "failure to read page/oob\n");
		return ret;
	}

	return snandc_check_error(snandc);
}

static void config_snand_page_write(struct qcom_nand_controller *snandc)
{
	write_reg_dma(snandc, NAND_ADDR0, 2, 0);
	write_reg_dma(snandc, NAND_DEV0_CFG0, 3, 0);
	write_reg_dma(snandc, NAND_EBI2_ECC_BUF_CFG, 1, NAND_BAM_NEXT_SGL);
}

static void config_snand_cw_write(struct qcom_nand_controller *snandc)
{
	write_reg_dma(snandc, NAND_FLASH_CMD, 1, NAND_BAM_NEXT_SGL);
	write_reg_dma(snandc, NAND_EXEC_CMD, 1, NAND_BAM_NEXT_SGL);
}

static int qpic_snand_program_execute(struct qcom_nand_controller *snandc,
				      const struct spi_mem_op *op)
{
	struct qpic_ecc *ecc_cfg = snandc->ecc;
	u8 *data_buf;
	int i, ret;
	int num_cw = 4;
	u32 cfg0, cfg1, ecc_bch_cfg, ecc_buf_cfg;

	cfg0 = (ecc_cfg->cfg0 & ~(7U << CW_PER_PAGE)) |
				(num_cw - 1) << CW_PER_PAGE;
	cfg1 = ecc_cfg->cfg1;
	ecc_bch_cfg = ecc_cfg->ecc_bch_cfg;
	ecc_buf_cfg = ecc_cfg->ecc_buf_cfg;

	data_buf = (u8 *)snandc->wbuf;

	snandc->buf_count = 0;
	snandc->buf_start = 0;
	clear_read_regs(snandc);
	clear_bam_transaction(snandc);

	snandc_set_reg(snandc, NAND_ADDR0, snandc->addr1);
	snandc_set_reg(snandc, NAND_ADDR1, snandc->addr2);
	snandc_set_reg(snandc, NAND_FLASH_CMD, snandc->cmd);

	snandc_set_reg(snandc, NAND_DEV0_CFG0, cfg0);
	snandc_set_reg(snandc, NAND_DEV0_CFG1, cfg1);
	snandc_set_reg(snandc, NAND_DEV0_ECC_CFG, ecc_bch_cfg);

	snandc_set_reg(snandc, NAND_EBI2_ECC_BUF_CFG, ecc_buf_cfg);

	snandc_set_reg(snandc, NAND_EXEC_CMD, 1);

	config_snand_page_write(snandc);

	for (i = 0; i < num_cw; i++) {
		int data_size;

		if (i == (num_cw - 1))
			data_size = NANDC_STEP_SIZE - ((num_cw - 1) << 2);
		else
			data_size = ecc_cfg->cw_data;

		write_data_dma(snandc, FLASH_BUF_ACC, data_buf, data_size,
			       i == (num_cw - 1) ? NAND_BAM_NO_EOT : 0);

		config_snand_cw_write(snandc);
		if (data_buf)
			data_buf += data_size;
	}

	ret = submit_descs(snandc);
	if (ret) {
		dev_err(snandc->dev, "failure to write page\n");
		return ret;
	}

	return 0;
}

static u32 qpic_snand_cmd_mapping(struct qcom_nand_controller *snandc, u32 opcode)
{
	u32 cmd = 0x0;

	switch (opcode) {
	case SPINAND_RESET:
		cmd = (SPI_WP | SPI_HOLD | SPI_TRANSFER_MODE_x1 | OP_RESET_DEVICE);
		break;
	case SPINAND_READID:
		cmd = (SPI_WP | SPI_HOLD | SPI_TRANSFER_MODE_x1 | OP_FETCH_ID);
		break;
	case SPINAND_GET_FEATURE:
		cmd = (SPI_TRANSFER_MODE_x1 | SPI_WP | SPI_HOLD | ACC_FEATURE);
		break;
	case SPINAND_SET_FEATURE:
		cmd = (SPI_TRANSFER_MODE_x1 | SPI_WP | SPI_HOLD | ACC_FEATURE |
			QPIC_SET_FEATURE);
		break;
	case SPINAND_READ:
		if (snandc->raw)
			cmd = (PAGE_ACC | LAST_PAGE | SPI_TRANSFER_MODE_x1 |
					SPI_WP | SPI_HOLD | OP_PAGE_READ);
		else
			cmd = (PAGE_ACC | LAST_PAGE | SPI_TRANSFER_MODE_x1 |
					SPI_WP | SPI_HOLD | OP_PAGE_READ_WITH_ECC);
		break;
	case SPINAND_ERASE:
		cmd = OP_BLOCK_ERASE | PAGE_ACC | LAST_PAGE | SPI_WP |
			SPI_HOLD | SPI_TRANSFER_MODE_x1;
		break;
	case SPINAND_WRITE_EN:
		cmd = SPINAND_WRITE_EN;
		break;
	case SPINAND_PROGRAM_EXECUTE:
		cmd = (PAGE_ACC | LAST_PAGE | SPI_TRANSFER_MODE_x1 |
				SPI_WP | SPI_HOLD | OP_PROGRAM_PAGE);
		break;
	case SPINAND_PROGRAM_LOAD:
		cmd = SPINAND_PROGRAM_LOAD;
		break;
	default:
		break;
	}

	return cmd;
}

static int qpic_snand_write_page_cache(struct qcom_nand_controller *snandc,
				       const struct spi_mem_op *op)
{
	struct qpic_snand_op s_op = {};
	u32 cmd;

	cmd = qpic_snand_cmd_mapping(snandc, op->cmd.opcode);
	s_op.cmd_reg = cmd;

	if (op->cmd.opcode == SPINAND_PROGRAM_LOAD) {
		snandc->wbuf = op->data.buf.out;
		snandc->wlen = op->data.nbytes;
	}

	return 0;
}

static int qpic_snand_send_cmdaddr(struct qcom_nand_controller *snandc,
				   const struct spi_mem_op *op)
{
	struct qpic_snand_op s_op = {};
	u32 cmd;
	int ret;

	cmd = qpic_snand_cmd_mapping(snandc, op->cmd.opcode);

	s_op.cmd_reg = cmd;
	s_op.addr1_reg = op->addr.val;
	s_op.addr2_reg = 0;

	if (op->cmd.opcode == SPINAND_WRITE_EN)
		return 0;

	if (op->cmd.opcode == SPINAND_PROGRAM_EXECUTE) {
		s_op.addr1_reg = op->addr.val << 16;
		s_op.addr2_reg = op->addr.val >> 16 & 0xff;
		snandc->addr1 = s_op.addr1_reg;
		snandc->addr2 = s_op.addr2_reg;
		snandc->cmd = cmd;
		return qpic_snand_program_execute(snandc, op);
	}

	if (op->cmd.opcode == SPINAND_READ) {
		s_op.addr1_reg = (op->addr.val << 16);
		s_op.addr2_reg = op->addr.val >> 16 & 0xff;
		snandc->addr1 = s_op.addr1_reg;
		snandc->addr2 = s_op.addr2_reg;
		snandc->cmd = cmd;
		return 0;
	}

	if (op->cmd.opcode == SPINAND_ERASE) {
		s_op.addr2_reg = (op->addr.val >> 16) & 0xffff;
		s_op.addr1_reg = op->addr.val;
		snandc->addr1 = s_op.addr1_reg;
		snandc->addr1 <<= 16;
		snandc->addr2 = s_op.addr2_reg;
		snandc->cmd = cmd;
		qpic_snand_block_erase(snandc);
		return 0;
	}

	snandc->buf_count = 0;
	snandc->buf_start = 0;
	clear_read_regs(snandc);
	clear_bam_transaction(snandc);

	snandc_set_reg(snandc, NAND_FLASH_CMD, s_op.cmd_reg);
	snandc_set_reg(snandc, NAND_EXEC_CMD, 0x1);
	snandc_set_reg(snandc, NAND_ADDR0, s_op.addr1_reg);
	snandc_set_reg(snandc, NAND_ADDR1, s_op.addr2_reg);

	write_reg_dma(snandc, NAND_FLASH_CMD, 3, NAND_BAM_NEXT_SGL);
	write_reg_dma(snandc, NAND_EXEC_CMD, 1, NAND_BAM_NEXT_SGL);

	ret = submit_descs(snandc);
	if (ret)
		dev_err(snandc->dev, "failure in sbumitting cmd descriptor\n");

	return ret;
}

static int qpic_snand_io_op(struct qcom_nand_controller *snandc, const struct spi_mem_op *op)
{
	int ret, val;

	ret = qpic_snand_send_cmdaddr(snandc, op);
	if (ret)
		return ret;

	snandc->buf_count = 0;
	snandc->buf_start = 0;
	clear_read_regs(snandc);
	clear_bam_transaction(snandc);

	if (op->cmd.opcode == SPINAND_READID) {
		snandc->buf_count = 4;
		read_reg_dma(snandc, NAND_READ_ID, 1, NAND_BAM_NEXT_SGL);

		ret = submit_descs(snandc);
		if (ret)
			dev_err(snandc->dev, "failure in submitting descriptor for readid\n");

		nandc_read_buffer_sync(snandc, true);
		memcpy(op->data.buf.in, snandc->reg_read_buf, snandc->buf_count);

		return ret;
	}

	if (op->cmd.opcode == SPINAND_GET_FEATURE) {
		snandc->buf_count = 4;
		read_reg_dma(snandc, NAND_FLASH_FEATURES, 1, NAND_BAM_NEXT_SGL);

		ret = submit_descs(snandc);
		if (ret)
			dev_err(snandc->dev, "failure in submitting descriptor for get feature\n");

		nandc_read_buffer_sync(snandc, true);

		val = le32_to_cpu(*(__le32 *)snandc->reg_read_buf);
		val >>= 8;
		memcpy(op->data.buf.in, &val, snandc->buf_count);
	}

	if (op->cmd.opcode == SPINAND_SET_FEATURE) {
		snandc_set_reg(snandc, NAND_FLASH_FEATURES, *(u32 *)op->data.buf.out);
		write_reg_dma(snandc, NAND_FLASH_FEATURES, 1, NAND_BAM_NEXT_SGL);
		ret = submit_descs(snandc);
		if (ret)
			dev_err(snandc->dev, "failure in submitting descriptor for set feature\n");
	}

	return ret;
}

static bool qpic_snand_is_page_op(const struct spi_mem_op *op)
{
	if (op->addr.buswidth != 1 && op->addr.buswidth != 2 && op->addr.buswidth != 4)
		return false;

	if (op->data.dir == SPI_MEM_DATA_IN) {
		if (op->addr.buswidth == 4 && op->data.buswidth == 4)
			return true;

		if (op->addr.nbytes == 2 && op->addr.buswidth == 1)
			return true;

	} else if (op->data.dir == SPI_MEM_DATA_OUT) {
		if (op->data.buswidth == 4)
			return true;
		if (op->addr.nbytes == 2 && op->addr.buswidth == 1)
			return true;
	}

	return false;
}

static bool qpic_snand_supports_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	if (!spi_mem_default_supports_op(mem, op))
		return false;

	if (op->cmd.nbytes != 1 || op->cmd.buswidth != 1)
		return false;

	if (qpic_snand_is_page_op(op))
		return true;

	return ((op->addr.nbytes == 0 || op->addr.buswidth == 1) &&
		(op->dummy.nbytes == 0 || op->dummy.buswidth == 1) &&
		(op->data.nbytes == 0 || op->data.buswidth == 1));
}

static int qpic_snand_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct qcom_nand_controller *snandc = spi_controller_get_devdata(mem->spi->controller);

	dev_dbg(snandc->dev, "OP %02x ADDR %08llX@%d:%u DATA %d:%u", op->cmd.opcode,
		op->addr.val, op->addr.buswidth, op->addr.nbytes,
		op->data.buswidth, op->data.nbytes);

	if (qpic_snand_is_page_op(op)) {
		if (op->data.dir == SPI_MEM_DATA_IN)
			return qpic_snand_read_page_cache(snandc, op);
		if (op->data.dir == SPI_MEM_DATA_OUT)
			return qpic_snand_write_page_cache(snandc, op);
	} else {
		return qpic_snand_io_op(snandc, op);
	}

	return 0;
}

static const struct spi_controller_mem_ops qcom_spi_mem_ops = {
	.supports_op = qpic_snand_supports_op,
	.exec_op = qpic_snand_exec_op,
};

static const struct spi_controller_mem_caps qcom_snand_mem_caps = {
	.ecc = true,
};

static int qcom_snand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct qcom_nand_controller *snandc;
	struct resource *res;
	const void *dev_data;
	struct qpic_ecc *ecc;
	int ret;

	ecc = devm_kzalloc(dev, sizeof(*ecc), GFP_KERNEL);
	if (!ecc)
		return -ENOMEM;

	ctlr = devm_spi_alloc_master(dev, sizeof(*snandc));
	if (!ctlr)
		return -ENOMEM;

	platform_set_drvdata(pdev, ctlr);

	snandc = spi_controller_get_devdata(ctlr);

	snandc->ctlr = ctlr;
	snandc->dev = dev;
	snandc->ecc = ecc;

	dev_data = of_device_get_match_data(dev);
	if (!dev_data) {
		dev_err(&pdev->dev, "failed to get device data\n");
		return -ENODEV;
	}

	snandc->props = dev_data;
	snandc->dev = &pdev->dev;

	snandc->core_clk = devm_clk_get(dev, "core");
	if (IS_ERR(snandc->core_clk))
		return PTR_ERR(snandc->core_clk);

	snandc->aon_clk = devm_clk_get(dev, "aon");
	if (IS_ERR(snandc->aon_clk))
		return PTR_ERR(snandc->aon_clk);

	snandc->iomacro_clk = devm_clk_get(dev, "iom");
	if (IS_ERR(snandc->iomacro_clk))
		return PTR_ERR(snandc->iomacro_clk);

	snandc->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(snandc->base))
		return PTR_ERR(snandc->base);

	snandc->base_phys = res->start;
	snandc->base_dma = dma_map_resource(dev, res->start, resource_size(res),
					    DMA_BIDIRECTIONAL, 0);
	if (dma_mapping_error(dev, snandc->base_dma))
		return -ENXIO;

	ret = clk_prepare_enable(snandc->core_clk);
	if (ret)
		goto err_core_clk;

	ret = clk_prepare_enable(snandc->aon_clk);
	if (ret)
		goto err_aon_clk;

	ret = clk_prepare_enable(snandc->iomacro_clk);
	if (ret)
		goto err_snandc_alloc;

	ret = qcom_nandc_alloc(snandc);
	if (ret)
		goto err_snandc_alloc;

	ret = qcom_snand_init(snandc);
	if (ret)
		goto err_init;

	/* setup ECC engine */
	snandc->ecc_eng.dev = &pdev->dev;
	snandc->ecc_eng.integration = NAND_ECC_ENGINE_INTEGRATION_PIPELINED;
	snandc->ecc_eng.ops = &qcom_snand_ecc_engine_ops_pipelined;
	snandc->ecc_eng.priv = snandc;

	ret = nand_ecc_register_on_host_hw_engine(&snandc->ecc_eng);
	if (ret) {
		dev_err(&pdev->dev, "failed to register ecc engine.\n");
		goto err_init;
	}

	ctlr->num_chipselect = QPIC_QSPI_NUM_CS;
	ctlr->mem_ops = &qcom_spi_mem_ops;
	ctlr->mem_caps = &qcom_snand_mem_caps;
	ctlr->dev.of_node = pdev->dev.of_node;
	ctlr->mode_bits = SPI_TX_DUAL | SPI_RX_DUAL |
			    SPI_TX_QUAD | SPI_RX_QUAD;

	ret = spi_register_controller(ctlr);
	if (ret) {
		dev_err(&pdev->dev, "spi_register_controller failed.\n");
		goto err_init;
	}

	return 0;

err_init:
	qcom_nandc_unalloc(snandc);
err_snandc_alloc:
	clk_disable_unprepare(snandc->aon_clk);
err_aon_clk:
	clk_disable_unprepare(snandc->core_clk);
err_core_clk:
	dma_unmap_resource(dev, res->start, resource_size(res),
			   DMA_BIDIRECTIONAL, 0);
	return ret;
}

static int qcom_snand_remove(struct platform_device *pdev)
{
	struct spi_controller *ctlr = platform_get_drvdata(pdev);

	spi_unregister_controller(ctlr);

	return 0;
}

static const struct qcom_nandc_props ipq9574_snandc_props = {
	.dev_cmd_reg_start = 0x7000,
	.is_bam = true,
};

static const struct of_device_id qcom_snandc_of_match[] = {
	{
		.compatible = "qcom,ipq9574-snand",
		.data = &ipq9574_snandc_props,
	},
	{}
}
MODULE_DEVICE_TABLE(of, qcom_snandc_of_match);

static struct platform_driver qcom_snand_driver = {
	.driver = {
		.name		= "qcom_snand",
		.of_match_table = qcom_snandc_of_match,
	},
	.probe = qcom_snand_probe,
	.remove = qcom_snand_remove,
};
module_platform_driver(qcom_snand_driver);

MODULE_DESCRIPTION("SPI driver for QPIC QSPI cores");
MODULE_AUTHOR("Md Sadre Alam <quic_mdalam@quicinc.com>");
MODULE_LICENSE("GPL");
