// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Synopsys DesignWare Multimedia Card Interface driver with CMDQ support
 *  (Based on Synopsys DesignWare Multimedia Card Interface driver)
 *
 * Copyright (c) 2023 Realtek Semiconductor Corp
 */

#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/stat.h>

#include "dw_mmc_cqe.h"
#include "cqhci.h"

#define DW_MCI_FREQ_MAX	200000000	/* unit: HZ */
#define DW_MCI_FREQ_MIN	100000		/* unit: HZ */
#define DW_MCI_CMDQ_DISABLED	0x30f0001
#define DW_MCI_CMDQ_ENABLED	0x30f0101
#define DW_MCI_POWEROFF		0x3220301
#define DW_MCI_DESC_LEN		0x100000
#define DW_MCI_MAX_SCRIPT_BLK	128
#define DW_MCI_TIMEOUT_Ms	200
#define DW_MCI_TIMEOUT		200000
#define TUNING_ERR		531
#define DW_MCI_NOT_READY	9999

DECLARE_COMPLETION(dw_mci_wait);

#if defined(CONFIG_DEBUG_FS)
static int dw_mci_cqe_req_show(struct seq_file *s, void *v)
{
	struct dw_mci_slot *slot = s->private;
	struct mmc_request *mrq;
	struct mmc_command *cmd;
	struct mmc_command *stop;
	struct mmc_data	*data;

	/* Make sure we get a consistent snapshot */
	spin_lock_bh(&slot->host->lock);
	mrq = slot->mrq;

	if (mrq) {
		cmd = mrq->cmd;
		data = mrq->data;
		stop = mrq->stop;

		if (cmd)
			seq_printf(s,
				   "CMD%u(0x%x) flg %x rsp %x %x %x %x err %d\n",
				   cmd->opcode, cmd->arg, cmd->flags,
				   cmd->resp[0], cmd->resp[1], cmd->resp[2],
				   cmd->resp[2], cmd->error);
		if (data)
			seq_printf(s, "DATA %u / %u * %u flg %x err %d\n",
				   data->bytes_xfered, data->blocks,
				   data->blksz, data->flags, data->error);
		if (stop)
			seq_printf(s,
				   "CMD%u(0x%x) flg %x rsp %x %x %x %x err %d\n",
				   stop->opcode, stop->arg, stop->flags,
				   stop->resp[0], stop->resp[1], stop->resp[2],
				   stop->resp[2], stop->error);
	}

	spin_unlock_bh(&slot->host->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dw_mci_cqe_req);
#endif /* defined(CONFIG_DEBUG_FS) */

static int dw_mci_cqe_regs_show(struct dw_mci *host,
				struct mmc_command *cmd, u32 cmd_flags)
{
	dev_err(host->dev, "opcode = %d, arg = 0x%x, cmdflags = 0x%x\n",
				cmd->opcode, cmd->arg, cmd_flags);
	dev_err(host->dev, "status_int = 0x%x\n", host->normal_interrupt);
	dev_err(host->dev, "error_int = 0x%x\n", host->error_interrupt);
	dev_err(host->dev, "auto_error_int = 0x%x\n", host->auto_error_interrupt);

	return 0;
}

static void dw_mci_cqe_dumpregs(struct mmc_host *mmc)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;

	dev_info(host->dev, "%s: cmd idx 0x%08x\n", __func__, mcq_readw(host, CMD_R));
}

static void dw_mci_cqe_setup_tran_desc(struct mmc_data *data,
				      struct cqhci_host *cq_host,
				      u8 *desc,
				      int sg_count)
{
	struct scatterlist *sg;
	u32 cur_blk_cnt, remain_blk_cnt;
	unsigned int begin, end;
	int i, len;
	bool last = false;
	bool dma64 = cq_host->dma64;
	dma_addr_t addr;

	for_each_sg(data->sg, sg, sg_count, i) {
		addr = sg_dma_address(sg);
		len = sg_dma_len(sg);
		remain_blk_cnt  = len >> 9;

		while (remain_blk_cnt) {
			/*DW_MCI_MAX_SCRIPT_BLK is tha max for each descriptor record*/
			if (remain_blk_cnt > DW_MCI_MAX_SCRIPT_BLK)
				cur_blk_cnt = DW_MCI_MAX_SCRIPT_BLK;
			else
				cur_blk_cnt = remain_blk_cnt;

			/* In Synopsys DesignWare Databook Page 84,
			 * They mentioned the DMA 128MB restriction
			 */
			begin = addr / SZ_128M;
			end = (addr + cur_blk_cnt * SZ_512) / SZ_128M;

			if (begin != end)
				cur_blk_cnt = (end * SZ_128M - addr) / SZ_512;

			if ((i+1) == sg_count && (remain_blk_cnt == cur_blk_cnt))
				last = true;

			cqhci_set_tran_desc(desc, addr,
					(cur_blk_cnt << 9), last, dma64);

			addr = addr + (cur_blk_cnt << 9);
			remain_blk_cnt -= cur_blk_cnt;
			desc += cq_host->trans_desc_len;
		}
	}
}

static void dw_mci_cqe_enable(struct mmc_host *mmc)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;

	/*clear data path SW_RST_R.SW_RST_DAT = 1*/
	mcq_writeb(host, SW_RST_R, SDMMC_RST_DAT);
	/*0x9801200c*/
	mcq_writew(host, XFER_MODE_R,
		((1 << SDMMC_MULTI_BLK_SEL) | SDMMC_BLOCK_COUNT_ENABLE | SDMMC_DMA_ENABLE));

	/*Set DMA_SEL to ADMA2 only mode in the HOST_CTRL1_R*/
	mcq_writeb(host, HOST_CTRL1_R,
		(mcq_readb(host, HOST_CTRL1_R) & 0xe7) | (SDMMC_ADMA2_32 << SDMMC_DMA_SEL));
	mcq_writew(host, BLOCKSIZE_R, 0x200);
	mcq_writew(host, BLOCKCOUNT_R, 0);

	/*Set SDMASA_R (while using 32 bits) to 0*/
	mcq_writel(host, SDMASA_R, 0);
	/*we set this register additionally to enhance the IO perofrmance*/

	cqhci_writel(host->cqe, 0x10, CQHCI_SSC1);
	cqhci_writel(host->cqe, 0, CQHCI_CTL);

	if (cqhci_readl(host->cqe, CQHCI_CTL) && CQHCI_HALT) {
		dev_err(host->dev, "%s: cqhci: CQE failed to exit halt state\n",
			mmc_hostname(mmc));
	}

	/*cmdq interrupt mode*/
	dw_mci_clr_signal_int(host);
	dw_mci_en_cqe_int(host);
}

static void dw_mci_cqe_pre_enable(struct mmc_host *mmc)
{
	struct cqhci_host *cq_host = mmc->cqe_private;
	u32 reg;

	reg = cqhci_readl(cq_host, CQHCI_CFG);
	reg |= CQHCI_ENABLE;
	cqhci_writel(cq_host, reg, CQHCI_CFG);
}

static void dw_mci_cqe_post_disable(struct mmc_host *mmc)
{
	struct cqhci_host *cq_host = mmc->cqe_private;
	u32 reg;

	reg = cqhci_readl(cq_host, CQHCI_CFG);
	reg &= ~CQHCI_ENABLE;
	cqhci_writel(cq_host, reg, CQHCI_CFG);
}

static const struct cqhci_host_ops dw_mci_cqhci_host_ops = {
	.enable = dw_mci_cqe_enable,
	.dumpregs = dw_mci_cqe_dumpregs,
	.pre_enable = dw_mci_cqe_pre_enable,
	.post_disable = dw_mci_cqe_post_disable,
	.setup_tran_desc = dw_mci_cqe_setup_tran_desc,
};

static void dw_mci_cqe_reset(struct dw_mci *host)
{
	int ret;
	u32 status;
	/*check the cmd line*/
	if (mcq_readw(host, ERROR_INT_STAT_R) & SDMMC_CMD_ERR) {
		/*Perform a software reset*/
		mcq_writeb(host, SW_RST_R, SDMMC_RST_CMD);

		ret = readl_poll_timeout(host->regs + SDMMC_CLK_CTRL_R, status,
			(status & SW_RST_CMD_DONE) == 0x0, 10, DW_MCI_TIMEOUT);
		if (ret)
			dev_err(host->dev, "Timeout resetting CMD line\n");
	}
	/*check data line*/
	if (mcq_readw(host, ERROR_INT_STAT_R) & SDMMC_DATA_ERR) {
		mcq_writeb(host, SW_RST_R, SDMMC_RST_DAT);

		ret = readl_poll_timeout(host->regs + SDMMC_CLK_CTRL_R, status,
			(status & SW_RST_DATA_DONE) == 0x0, 10, DW_MCI_TIMEOUT);
		if (ret)
			dev_err(host->dev, "Timeout resetting DATA line\n");
	}
}

static void dw_mci_cqe_read_rsp(struct dw_mci *host, struct mmc_command *cmd, u32 *rsp)
{
	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136) {
			/*R2 long response*/
			u32 rsp_tmp[4];

			rsp_tmp[3] = mcq_readl(host, RESP01_R);
			rsp_tmp[2] = mcq_readl(host, RESP23_R);
			rsp_tmp[1] = mcq_readl(host, RESP45_R);
			rsp_tmp[0] = mcq_readl(host, RESP67_R);

			/* dw_mmc_databook shift Response field to 08 - 139 bits*/
			rsp[3] = (rsp_tmp[3] & 0x00ffffff) << 8;
			rsp[2] = ((rsp_tmp[2] & 0x00ffffff) << 8)
					| ((rsp_tmp[3] & 0xff000000) >> 24);
			rsp[1] = ((rsp_tmp[1] & 0x00ffffff) << 8)
					| ((rsp_tmp[2] & 0xff000000) >> 24);
			rsp[0] = ((rsp_tmp[0] & 0x00ffffff) << 8)
					| ((rsp_tmp[1] & 0xff000000) >> 24);
		} else {
			/*Short response*/
			rsp[0] = rsp[1] = rsp[2] = rsp[3] = 0;
			rsp[0] = mcq_readl(host, RESP01_R);
		}
	}
}

static u32 dw_mci_cqe_prepare_command(struct mmc_host *mmc, struct mmc_command *cmd)
{
	u32 cmdr;

	cmd->error = -EINPROGRESS;
	cmdr = (cmd->opcode << 8);

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			cmdr |= SDMMC_RESP_LEN_136;
		else {
			if (cmd->flags & MMC_RSP_BUSY)
				cmdr |= SDMMC_RESP_LEN_48B;
			else
				cmdr |= SDMMC_RESP_LEN_48;
		}
	}

	cmdr |= SDMMC_CMD_CHK_RESP_CRC;
	if (cmd->opcode == MMC_GO_IDLE_STATE ||
	   cmd->opcode == MMC_SEND_OP_COND ||
	   (cmd->opcode == MMC_SELECT_CARD && cmd->flags == (MMC_RSP_NONE | MMC_CMD_AC)))
		cmdr &= ~SDMMC_CMD_CHK_RESP_CRC;

	cmdr |= SDMMC_CMD_IDX_CHK_ENABLE;
	if (cmd->opcode == MMC_GO_IDLE_STATE ||
	   cmd->opcode == MMC_SEND_OP_COND ||
	   cmd->opcode == MMC_SEND_CSD ||
	   cmd->opcode == MMC_SEND_CID ||
	   cmd->opcode == MMC_ALL_SEND_CID ||
	   (cmd->opcode == MMC_SELECT_CARD && cmd->flags == (MMC_RSP_NONE | MMC_CMD_AC)))
		cmdr &= ~SDMMC_CMD_IDX_CHK_ENABLE;

	if (cmd->data)
		cmdr |= SDMMC_DATA;

	if (cmd->opcode == MMC_STOP_TRANSMISSION)
		cmdr |= (SDMMC_ABORT_CMD << 6);

	return cmdr;
}

static int dw_mci_cqe_start_command(struct dw_mci *host,
				 struct mmc_command *cmd, u32 cmd_flags)
{
	int err = 0;
	unsigned long end = 0;
	unsigned long flags;
	bool xfer_flag = false;
	int ret;
	u32 status;

	host->cmd = cmd;

	switch (cmd->opcode) {
	case MMC_READ_SINGLE_BLOCK:
	case MMC_READ_MULTIPLE_BLOCK:
	case MMC_WRITE_BLOCK:
	case MMC_WRITE_MULTIPLE_BLOCK:
	case MMC_SEND_EXT_CSD:
	case MMC_GEN_CMD:
	case MMC_SLEEP_AWAKE:
	case MMC_SWITCH:
	case MMC_SET_WRITE_PROT:
	case MMC_CLR_WRITE_PROT:
	case MMC_SEND_WRITE_PROT:
	case MMC_ERASE:
	case MMC_SEND_TUNING_BLOCK_HS200:
		xfer_flag = true;
		break;
	default:
		xfer_flag = false;
	}

	host->int_waiting = &dw_mci_wait;
	end = jiffies + msecs_to_jiffies(DW_MCI_TIMEOUT_Ms);
	mod_timer(&host->timer, end);

	if (host->int_waiting) {
		dw_mci_clr_signal_int(host);
		dw_mci_clr_int(host);

		/*command with data, r1b case*/
		if (xfer_flag == 1)
			dw_mci_en_xfer_int(host);
		else
			dw_mci_en_cd_int(host);

		/*If we use cmd23, we cannot send auto stop command*/
		if (cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK ||
		    cmd->opcode == MMC_READ_MULTIPLE_BLOCK) {
			if (host->is_sbc) {
				mcq_writew(host, XFER_MODE_R,
					mcq_readw(host, XFER_MODE_R) & ~BIT(SDMMC_AUTO_CMD_ENABLE));
					host->is_sbc = 0;
			}
		}

		host->opcode = cmd->opcode;
		host->arg = cmd->arg;

		spin_lock_irqsave(&host->irq_lock, flags);
		mcq_writew(host, CMD_R, cmd_flags);
		spin_unlock_irqrestore(&host->irq_lock, flags);

		wait_for_completion(host->int_waiting);

		if (xfer_flag == 1) {
			ret = readl_poll_timeout(host->regs + SDMMC_NORMAL_INT_STAT_R, status,
				(status & SDMMC_XFER_COMPLETE) == SDMMC_XFER_COMPLETE
				, 10, DW_MCI_TIMEOUT);
			if (ret) {
				/*error interrupt detected*/
				if ((mcq_readw(host, NORMAL_INT_STAT_R) & SDMMC_ERR_INTERRUPT)
					&& (host->tuning == 1))
					dev_info(host->dev, "Tuning error ... keep tuning\n");
				else
					dev_err(host->dev, "Timeout waiting xfer complete, status = 0x%x\n",
						(status & 0xffff));
			}
		} else {
			ret = readl_poll_timeout(host->regs + SDMMC_NORMAL_INT_STAT_R, status,
				(status & SDMMC_CMD_COMPLETE) == SDMMC_CMD_COMPLETE,
				10, DW_MCI_TIMEOUT);
			if (ret)
				dev_err(host->dev, "Timeout waiting cmd request complete\n");
		}

		if (host->normal_interrupt & SDMMC_ERR_INTERRUPT) {
			if (host->tuning != 1)
				dw_mci_cqe_regs_show(host, cmd, cmd_flags);
			err = -1;
		}
	}

	return err;
}

static void dw_mci_cqe_prep_stop_abort(struct dw_mci *host, struct mmc_command *cmd)
{

	struct mmc_command stop;
	u32 cmdr;
	/*Stop command only use after data command*/
	if (!cmd->data)
		return;

	memset(&stop, 0, sizeof(struct mmc_command));

	if (cmd->opcode == MMC_READ_SINGLE_BLOCK ||
	    cmd->opcode == MMC_READ_MULTIPLE_BLOCK ||
	    cmd->opcode == MMC_WRITE_BLOCK ||
	    cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK ||
	    cmd->opcode == MMC_SEND_TUNING_BLOCK ||
	    cmd->opcode == MMC_SEND_TUNING_BLOCK_HS200) {
		stop.opcode = MMC_STOP_TRANSMISSION;
		stop.arg = 0;
		stop.flags = MMC_RSP_R1 | MMC_CMD_AC;
	} else if (cmd->opcode == SD_IO_RW_EXTENDED) {
		stop.opcode = SD_IO_RW_DIRECT;
		stop.arg |= (1 << 31) | (0 << 28) | (SDIO_CCCR_ABORT << 9) |
			    ((cmd->arg >> 28) & 0x7);
		stop.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_AC;
	} else {
		return;
	}

	cmdr = (stop.opcode << 8) | SDMMC_RESP_LEN_48 |
		SDMMC_CMD_CHK_RESP_CRC | SDMMC_CMD_IDX_CHK_ENABLE;
	cmdr |= (SDMMC_ABORT_CMD << 6);
	mcq_writew(host, XFER_MODE_R, 0);
	mcq_writel(host, ARGUMENT_R, stop.arg);
	dw_mci_cqe_start_command(host, &stop, cmdr);
}

static int dw_mci_cqe_wait_status(struct dw_mci *host, struct mmc_command *cmd, u32 *status)
{
	struct mmc_command wait;
	u32 cmdr;
	u32 cur_state;
	unsigned long timeend;
	int err = 0;

	/* According to Synopsys userguide, we need to send wait command after
	 * stop cmd to check current status
	 */

	wait = host->stat_ready;
	memset(&wait, 0, sizeof(struct mmc_command));

	timeend = jiffies + msecs_to_jiffies(500);
	do {
		wait.opcode = MMC_SEND_STATUS;
		wait.arg = 1 << 16;
		wait.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_AC;
		wait.data = NULL;
		cmdr = (wait.opcode << 8) | SDMMC_RESP_LEN_48 |
			SDMMC_CMD_CHK_RESP_CRC | SDMMC_CMD_IDX_CHK_ENABLE;

		mcq_writew(host, XFER_MODE_R, 0);
		mcq_writel(host, ARGUMENT_R, wait.arg);
		err = dw_mci_cqe_start_command(host, &wait, cmdr);
		if (err) {
			dw_mci_cqe_reset(host);
			break;
		}

		dw_mci_cqe_read_rsp(host, &wait, wait.resp);
		*status = wait.resp[0];
		cur_state = R1_CURRENT_STATE(wait.resp[0]);
		err = -DW_MCI_NOT_READY;
		if (cur_state == R1_STATE_TRAN) {
			if (wait.resp[0] & R1_READY_FOR_DATA) {
				err = 0;
				break;
			}
		}
	} while (time_before(jiffies, timeend));

	return err;
}

static void dw_mci_cqe_stop_dma(struct dw_mci *host, struct mmc_data *data)
{
	u32 dir = 0;

	if (data->flags & MMC_DATA_READ)
		dir = DMA_FROM_DEVICE;
	else
		dir = DMA_TO_DEVICE;

	dma_unmap_sg(mmc_dev(host->slot->mmc), data->sg, data->sg_len, dir);
	host->sg = NULL;
}

static void dw_mci_cqe_prepare_desc64(struct dw_mci *host, struct mmc_data *data,
					struct scatterlist *sg)
{
	dev_info(host->dev, "Currently, the 64bit DMA mode is not implemented yet.\n");
}


static void dw_mci_cqe_prepare_desc32(struct dw_mci *host, struct mmc_data *data,
					struct scatterlist *sg)
{
	u32  blk_cnt, cur_blk_cnt, remain_blk_cnt;
	u32  tmp_val;
	u32 *desc_base = host->sg_cpu;
	u32  dma_len = 0;
	u32  dma_addr;
	u32  i;
	unsigned int begin, end;

	for (i = 0; i < host->dma_nents; i++, sg++) {
		dma_len = sg_dma_len(sg);

		/*blk_cnt must be the multiple of 512(0x200)*/
		if (dma_len < SZ_512)
			blk_cnt = 1;
		else
			blk_cnt  = dma_len >> 9;

		remain_blk_cnt  = blk_cnt;
		dma_addr = sg_dma_address(sg);

		while (remain_blk_cnt) {
			/*DW_MCI_MAX_SCRIPT_BLK is the max
			 * for each descriptor record
			 */
			if (remain_blk_cnt > DW_MCI_MAX_SCRIPT_BLK)
				cur_blk_cnt = DW_MCI_MAX_SCRIPT_BLK;
			else
				cur_blk_cnt = remain_blk_cnt;

			/* In Synopsys DesignWare Databook Page 84,
			 * They mentioned the DMA 128MB restriction
			 */
			begin = dma_addr / SZ_128M;
			end = (dma_addr + cur_blk_cnt * SZ_512) / SZ_128M;

			/*If begin and end in the different 128MB memory zone*/
			if (begin != end)
				cur_blk_cnt = (end * SZ_128M - dma_addr) / SZ_512;

			if (dma_len < SZ_512)
				tmp_val = ((dma_len) << 16) | VALID(0x1) | ACT(0x4);
			else
				tmp_val = ((cur_blk_cnt & 0x7f) << 25) | VALID(0x1) | ACT(0x4);

			/*Last descriptor*/
			if (i == host->dma_nents - 1 && remain_blk_cnt == cur_blk_cnt)
				tmp_val |= END(0x1);

			desc_base[0] =  tmp_val;
			desc_base[1] =  dma_addr;

			dma_addr = dma_addr + (cur_blk_cnt << 9);
			remain_blk_cnt -= cur_blk_cnt;
			desc_base += 2;
		}
	}
}

static int dw_mci_cqe_get_cd(struct mmc_host *mmc)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;
	int gpio_cd = mmc_gpio_get_cd(mmc);
	int present = -1;

	/* Use platform get_cd function, else try onboard card detect */
	if (((mmc->caps & MMC_CAP_NEEDS_POLL)
		|| !mmc_card_is_removable(mmc))) {
		present = 1;

		if (!test_bit(DW_MMC_CARD_PRESENT, &slot->flags)) {
			if (mmc->caps & MMC_CAP_NEEDS_POLL) {
				dev_info(&mmc->class_dev,
					"card is polling.\n");
			} else {
				dev_info(&mmc->class_dev,
					"card is non-removable.\n");
			}
			set_bit(DW_MMC_CARD_PRESENT, &slot->flags);
		}

		return present;
	} else if (gpio_cd >= 0) {
		present = gpio_cd;
	} else {
		/*SD card detect using IP regs is todo*/
		dev_err(&mmc->class_dev, "SD card detect using IP regs is ToDo.\n");
	}

	spin_lock_bh(&host->lock);

	if (present && !test_and_set_bit(DW_MMC_CARD_PRESENT, &slot->flags))
		dev_dbg(&mmc->class_dev, "card is present\n");
	else if (!present &&
		!test_and_clear_bit(DW_MMC_CARD_PRESENT, &slot->flags))
		dev_dbg(&mmc->class_dev, "card is not present\n");

	spin_unlock_bh(&host->lock);

	return present;
}

static void dw_mci_cqe_submit_data_dma(struct dw_mci *host)
{
	if (host->dma_64bit_address == 1)
		dw_mci_cqe_prepare_desc64(host, host->data, host->sg);
	else
		dw_mci_cqe_prepare_desc32(host, host->data, host->sg);

}

static void dw_mci_cqe_submit_data(struct dw_mci *host, struct mmc_data *data)
{
	u32 dir = 0;

	host->sg = NULL;
	host->data = data;

	if (data->flags & MMC_DATA_READ)
		dir = DMA_FROM_DEVICE;
	else
		dir = DMA_TO_DEVICE;

	host->dma_nents = dma_map_sg(mmc_dev(host->slot->mmc),
					data->sg, data->sg_len, dir);
	host->sg = data->sg;

	host->using_dma = 1;

	dw_mci_cqe_submit_data_dma(host);
}

static void dw_mci_cqe_setup_bus(struct dw_mci_slot *slot, bool force_clkinit)
{
	struct dw_mci *host = slot->host;
	const struct dw_mci_drv_data *drv_data = host->drv_data;
	unsigned int clock = slot->clock;
	u32 div = 0;

	slot->mmc->actual_clock = 0;

	if (clock != host->current_speed || force_clkinit) {
		div = host->bus_hz / clock;
		if (host->bus_hz % clock)
			div += 1;

		if (clock != slot->__clk_old) {
			/* Silent the verbose log if calling from PM context */
			dev_info(&slot->mmc->class_dev,
				"Bus speed (slot %d) = %dHz (slot req %dHz, actual %dHZ div = %d)\n",
				slot->id, host->bus_hz, clock, host->bus_hz / div, div);
		}

		slot->__clk_old = clock;
		slot->mmc->actual_clock = host->bus_hz / div;

		if (drv_data && drv_data->set_ios)
			drv_data->set_ios(slot, &slot->mmc->ios);
	}
}


static void dw_mci_cqe_err_handle(struct dw_mci *host, struct mmc_command *cmd)
{
	u32 status = 0;
	int pstat_rty = 0;
	int ret;
	int err = 0;
	int rty_cnt = 0;

	do {
		mcq_writew(host, ERROR_INT_STAT_R,
			mcq_readw(host, ERROR_INT_STAT_R) & 0xffff);
		/*synchronous abort: stop host dma*/
		mcq_writeb(host, BGAP_CTRL_R, SDMMC_STOP_BG_REQ);

		ret = readl_poll_timeout(host->regs + SDMMC_NORMAL_INT_STAT_R, status,
			(status & SDMMC_XFER_COMPLETE) == SDMMC_XFER_COMPLETE, 10, DW_MCI_TIMEOUT);
		if (ret) {
			if ((mcq_readw(host, NORMAL_INT_STAT_R) & SDMMC_ERR_INTERRUPT) != 0)
				dev_info(host->dev, "status = 0x%x\n", (status & 0xffff));
			else
				dev_err(host->dev, "Timeout waiting err_handle xfer complete\n");
		}

		mcq_writew(host, NORMAL_INT_STAT_R, SDMMC_XFER_COMPLETE);

		if (cmd->opcode != MMC_SEND_TUNING_BLOCK_HS200) {
			dw_mci_cqe_prep_stop_abort(host, cmd);
			mdelay(1);

			err = dw_mci_cqe_wait_status(host, cmd, &status);
				rty_cnt++;
				if (rty_cnt > 100) {
					if (err == -DW_MCI_NOT_READY) {
						dev_err(host->dev, "status check failed, err = %d, status = 0x%x\n",
							err, status);
						break;
					}
				}
		} else {
			break;
		}

		mcq_writeb(host, SW_RST_R, SDMMC_RST_CMD | SDMMC_RST_DAT);

		ret = readl_poll_timeout(host->regs + SDMMC_CLK_CTRL_R, status,
				(status & SW_RST_BOTH_DONE) == 0x0, 10, DW_MCI_TIMEOUT);
		ret = readl_poll_timeout(host->regs + SDMMC_PSTATE_REG, status,
				(status & 0x3) == 0x0, 10, DW_MCI_TIMEOUT);
		if (ret)
			dev_err(host->dev, "Waiting error handling done timeout\n");

		udelay(40);

		pstat_rty++;
		if (pstat_rty > 5000) {
			dev_err(host->dev, "wait pstate register data line ready timeout\n");
			break;
		}
	} while ((mcq_readl(host, PSTATE_REG) & 0xf00000) != 0xf00000 ||
		(mcq_readl(host, PSTATE_REG) & 0xf0) != 0xf0);
}

static void dw_mci_cqe_send_stop_abort(struct dw_mci *host,
			      struct dw_mci_slot *slot,
			      struct mmc_command *cmd)
{
	dw_mci_cqe_reset(host);

	if (cmd->data)
		dw_mci_cqe_err_handle(host, cmd);
	else
		return;
}

static u32 dw_mci_cqe_prepare_data_flags(struct mmc_command *cmd)
{
	u32 dataflags;
	int read_flag = 1;
	int mul_blk_flag = 0;
	int auto_stop_flag = 0;

	if (cmd->opcode == MMC_WRITE_BLOCK ||
	   cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK ||
	   cmd->opcode == MMC_LOCK_UNLOCK ||
	   (cmd->opcode == MMC_GEN_CMD && cmd->arg == 0))
		read_flag = 0;

	if (cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK ||
	   cmd->opcode == MMC_READ_MULTIPLE_BLOCK) {
		mul_blk_flag = 1;
		auto_stop_flag = 1;
	}

	dataflags = (mul_blk_flag << SDMMC_MULTI_BLK_SEL) |
		    (read_flag << SDMMC_DATA_XFER_DIR) |
		    (auto_stop_flag << SDMMC_AUTO_CMD_ENABLE) |
		    (SDMMC_BLOCK_COUNT_ENABLE) |
		    (SDMMC_DMA_ENABLE);

	return dataflags;
}

static int dw_mci_cqe_command_complete(struct dw_mci *host, u16 interrupt,
					int *cmd_error)
{
	if (interrupt & (SDMMC_CMD_IDX_ERR | SDMMC_CMD_END_BIT_ERR
		| SDMMC_CMD_CRC_ERR)) {
		if (host->tuning)
			*cmd_error = -TUNING_ERR;
		else
			*cmd_error = -EILSEQ;
	} else if (interrupt & SDMMC_CMD_TOUT_ERR) {
		if (host->tuning)
			*cmd_error = -TUNING_ERR;
		else
			*cmd_error = -ETIMEDOUT;
	} else {
		*cmd_error = 0;
	}

	return *cmd_error;
}

static int dw_mci_cqe_data_complete(struct dw_mci *host, u16 interrupt,
					int *data_error)
{
	if (interrupt & (SDMMC_DATA_END_BIT_ERR | SDMMC_DATA_CRC_ERR)) {
		if (host->tuning)
			*data_error = -TUNING_ERR;
		else
			*data_error = -EILSEQ;
	} else if (interrupt & SDMMC_DATA_TOUT_ERR) {
		if (host->tuning)
			*data_error = -TUNING_ERR;
		else
			*data_error = -ETIMEDOUT;
	} else if (interrupt & SDMMC_ADMA_ERR) {
		*data_error = -EIO;
	} else {
		*data_error = 0;
	}

	return *data_error;
}

static void __dw_mci_cqe_start_request(struct dw_mci *host,
				   struct dw_mci_slot *slot,
				   struct mmc_command *cmd)
{
	struct mmc_data *data;
	u32 cmdflags;
	u32 dataflags;
	int ret = 0;

	data = cmd->data;

	if (data) {
		mcq_writew(host, BLOCKCOUNT_R, data->blocks);
		mcq_writel(host, BLOCKSIZE_R, data->blksz);
		mcq_writel(host, ADMA_SA_LOW_R, host->sg_dma);

		dataflags = dw_mci_cqe_prepare_data_flags(cmd);

		mcq_writew(host, XFER_MODE_R, dataflags);
	} else {
		if (cmd->opcode == MMC_SET_BLOCK_COUNT)
			host->is_sbc = 1;
		else
			host->is_sbc = 0;

		mcq_writew(host, XFER_MODE_R, 0);
	}

	mcq_writel(host, ARGUMENT_R, cmd->arg);

	cmdflags = dw_mci_cqe_prepare_command(slot->mmc, cmd);

	if (data) {
		data->bytes_xfered = 0;
		if (host->use_dma == TRANS_MODE_DMA) {
			dw_mci_cqe_submit_data(host, data);
			wmb(); /* drain writebuffer */
		} else {
			/*Using PIO mode*/
			dev_err(host->dev, "pio mode is not supported currently\n");
		}
	}

	ret = dw_mci_cqe_start_command(host, cmd, cmdflags);

	if (ret == 0) {
		dw_mci_cqe_read_rsp(host, cmd, cmd->resp);

		if (data)
			data->bytes_xfered += (data->blocks * data->blksz);
	}

	dw_mci_cqe_command_complete(host, host->error_interrupt, &cmd->error);
	if (data) {
		dw_mci_cqe_data_complete(host, host->error_interrupt, &data->error);
		if (host->use_dma == TRANS_MODE_DMA)
			dw_mci_cqe_stop_dma(host, data);
		else {
			/*Using PIO mode*/
			dev_err(host->dev, "pio mode is not supported currently\n");
		}
	}

	if (ret != 0)
		dw_mci_cqe_send_stop_abort(host, slot, cmd);

	if (cmd->opcode == SD_SWITCH_VOLTAGE) {
		/*
		 * If cmd11 needs to be dealt with specially, put in here.
		 */
	}
}

static void dw_mci_cqe_start_request(struct dw_mci *host,
				 struct dw_mci_slot *slot)
{
	struct mmc_request *mrq = slot->mrq;

	if (mrq->sbc)
		__dw_mci_cqe_start_request(host, slot, mrq->sbc);

	if (mrq->cmd)
		__dw_mci_cqe_start_request(host, slot, mrq->cmd);
}

static void dw_mci_cqe_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;

	WARN_ON(slot->mrq);

	/*
	 * The check for card presence and queueing of the request must be
	 * atomic, otherwise the card could be removed in between and the
	 * request wouldn't fail until another card was inserted.
	 */

	if (!dw_mci_cqe_get_cd(mmc)) {
		mrq->cmd->error = -ENOMEDIUM;
		mmc_request_done(mmc, mrq);
		return;
	}

	down_write(&host->cr_rw_sem);

	slot->mrq = mrq;
	host->mrq = mrq;

	dw_mci_cqe_start_request(host, slot);

	tasklet_schedule(&host->tasklet);

	up_write(&host->cr_rw_sem);
}

static void dw_mci_cqe_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;
	const struct dw_mci_drv_data *drv_data = slot->host->drv_data;

	switch (ios->timing) {
	case MMC_TIMING_MMC_HS400:
		mcq_writew(host, HOST_CTRL2_R,
			(mcq_readw(host, HOST_CTRL2_R)
				& SDMMC_UHS_MODE_SEL_MASK) | SDMMC_HS400);
		break;
	case MMC_TIMING_MMC_HS200:
		mcq_writew(host, HOST_CTRL2_R,
			(mcq_readw(host, HOST_CTRL2_R)
				& SDMMC_UHS_MODE_SEL_MASK) | SDMMC_HS200);
		break;
	case MMC_TIMING_MMC_HS:
		mcq_writew(host, HOST_CTRL2_R,
			(mcq_readw(host, HOST_CTRL2_R)
				& SDMMC_UHS_MODE_SEL_MASK) | SDMMC_SDR);
		break;
	default:
		/*MMC_TIMING_LEGACY case*/
		mcq_writew(host, HOST_CTRL2_R,
			(mcq_readw(host, HOST_CTRL2_R)
				& SDMMC_UHS_MODE_SEL_MASK) | SDMMC_LEGACY);
	}

	slot->clock = ios->clock;

	if (drv_data && drv_data->set_ios)
		drv_data->set_ios(slot, ios);

	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_4:
		mcq_writeb(host, HOST_CTRL1_R,
			(mcq_readb(host, HOST_CTRL1_R) &
			(SDMMC_EXT_DAT_XFER_MASK & SDMMC_DAT_XFER_WIDTH_MASK))
				|SDMMC_BUS_WIDTH_4);
		break;
	case MMC_BUS_WIDTH_8:
		mcq_writeb(host, HOST_CTRL1_R,
			(mcq_readb(host, HOST_CTRL1_R) &
				SDMMC_EXT_DAT_XFER_MASK) | SDMMC_BUS_WIDTH_8);
		break;
	default:
		/* set default 1 bit mode */
		mcq_writeb(host, HOST_CTRL1_R,
			(mcq_readb(host, HOST_CTRL1_R) &
				(SDMMC_EXT_DAT_XFER_MASK &
				SDMMC_DAT_XFER_WIDTH_MASK)) | SDMMC_BUS_WIDTH_1);
	}
}

static int dw_mci_cqe_switch_voltage(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;
	const struct dw_mci_drv_data *drv_data = host->drv_data;

	if (drv_data && drv_data->switch_voltage)
		return drv_data->switch_voltage(mmc, ios);

	return 0;
}

static int dw_mci_cqe_get_ro(struct mmc_host *mmc)
{
	int read_only = 0;
	int gpio_ro;

	gpio_ro = mmc_gpio_get_ro(mmc);

	/* Use platform get_ro function, else try on board write protect */
	if (gpio_ro >= 0)
		read_only = gpio_ro;
	else
		/*Need to read the IP register to judge if ro*/
		dev_err(&mmc->class_dev, "IP get_ro feature is not implemented currently.\n");

	dev_dbg(&mmc->class_dev, "card is %s\n",
		read_only ? "read-only" : "read-write");

	return read_only;
}

static int dw_mci_cqe_execute_tuning(struct mmc_host *mmc, u32 opcode)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;
	const struct dw_mci_drv_data *drv_data = host->drv_data;
	int err = -EINVAL;

	if (drv_data && drv_data->execute_tuning)
		err = drv_data->execute_tuning(slot, opcode);
	return err;

}

static int dw_mci_cqe_prepare_hs400_tuning(struct mmc_host *mmc,
				       struct mmc_ios *ios)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;
	const struct dw_mci_drv_data *drv_data = host->drv_data;

	if (drv_data && drv_data->prepare_hs400_tuning)
		return drv_data->prepare_hs400_tuning(host, ios);

	return 0;
}

static void dw_mci_cqe_hs400_complete(struct mmc_host *mmc)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;
	const struct dw_mci_drv_data *drv_data = host->drv_data;

	if (drv_data && drv_data->hs400_complete)
		drv_data->hs400_complete(mmc);
}

static void dw_mci_cqe_init_card(struct mmc_host *mmc, struct mmc_card *card)
{
	struct dw_mci_slot *slot = mmc_priv(mmc);
	struct dw_mci *host = slot->host;
	const struct dw_mci_drv_data *drv_data = host->drv_data;

	/*
	 * Add any quirks for this synopsys IP here or
	 * deal with something special for some specific
	 * vendors' SOC platform by calling drv_data->init_card().
	 */
	if (drv_data && drv_data->init_card)
		drv_data->init_card(mmc, card);
}

static const struct mmc_host_ops dw_mci_ops = {
	.request		= dw_mci_cqe_request,
	.set_ios		= dw_mci_cqe_set_ios,
	.get_ro			= dw_mci_cqe_get_ro,
	.get_cd			= dw_mci_cqe_get_cd,
	.execute_tuning		= dw_mci_cqe_execute_tuning,
	.start_signal_voltage_switch = dw_mci_cqe_switch_voltage,
	.init_card		= dw_mci_cqe_init_card,
	.prepare_hs400_tuning	= dw_mci_cqe_prepare_hs400_tuning,
	.hs400_complete         = dw_mci_cqe_hs400_complete,
};

static void dw_mci_cqe_tasklet_func(unsigned long priv)
{
	struct dw_mci *host = (struct dw_mci *)priv;
	struct mmc_host *prev_mmc = host->slot->mmc;
	struct mmc_request *mrq;
	unsigned long flags;

	spin_lock_irqsave(&host->irq_lock, flags);

	host->cmd = NULL;
	host->data = NULL;
	mrq = host->mrq;
	host->slot->mrq = NULL;
	host->mrq = NULL;

	spin_unlock_irqrestore(&host->irq_lock, flags);

	mmc_request_done(prev_mmc, mrq);
}

static irqreturn_t dw_mci_cqe_interrupt(int irq, void *dev_id)
{
	struct dw_mci *host = dev_id;
	struct mmc_host *mmc = host->slot->mmc;
	struct cqhci_host *cq_host = NULL;
	int cmd_error = 0, data_error = 0;

	if (host->pdata && (host->pdata->caps2 & MMC_CAP2_CQE))
		cq_host = mmc->cqe_private;

	dw_mci_get_int(host);

	if (host->pdata && (host->pdata->caps2 & MMC_CAP2_CQE)) {
		if (mmc->cqe_on == false && cq_host->activated == false)
			dw_mci_clr_signal_int(host);
	} else {
		dw_mci_clr_signal_int(host);
	}
	/*if run the cmdq mode*/
	if (host->pdata && (host->pdata->caps2 & MMC_CAP2_CQE) &&
		mmc->cqe_on == true && cq_host->activated == true) {
		if (host->normal_interrupt & SDMMC_ERR_INTERRUPT) {
			dev_err(host->dev, "cmdq error: interrupt status=%08x, error interrupt=0x%08x, CQIS=0x%x, CQTCN=0x%x\n",
				host->normal_interrupt, host->error_interrupt,
				readl(host->cqe->mmio + CQHCI_IS),
				readl(host->cqe->mmio + CQHCI_TCN));

			dw_mci_cqe_command_complete(host, host->error_interrupt, &cmd_error);
			dw_mci_cqe_data_complete(host, host->error_interrupt, &data_error);
		}
		cqhci_irq(mmc, (u32)(host->normal_interrupt), cmd_error, data_error);
		dw_mci_clr_int(host);

		return IRQ_HANDLED;
	}

	if (host->int_waiting) {
		del_timer(&host->timer);
		complete(host->int_waiting);
	}

	return IRQ_HANDLED;

}

static void dw_mci_cqe_setup(struct dw_mci *host)
{
	mcq_writeb(host, SW_RST_R, (SDMMC_RST_ALL|SDMMC_RST_CMD|SDMMC_RST_DAT));
	mcq_writeb(host, TOUT_CTRL_R, 0xe);
	mcq_writew(host, HOST_CTRL2_R, SDMMC_HOST_VER4_ENABLE|SDMMC_SIGNALING_EN);
	mcq_writew(host, NORMAL_INT_STAT_EN_R, 0xffff);
	mcq_writew(host, ERROR_INT_STAT_EN_R, SDMMC_ALL_ERR_STAT_EN);
	/*Card detect will be enabled in the last*/
	mcq_writew(host, NORMAL_INT_SIGNAL_EN_R, (~(SDMMC_CARD_INSERTION_SIGNAL_EN |
		SDMMC_CARD_REMOVAL_SIGNAL_EN | SDMMC_CARD_INTERRUPT_SIGNAL_EN) & 0xffff));
	mcq_writew(host, ERROR_INT_SIGNAL_EN_R, SDMMC_ALL_ERR_SIGNAL_EN);
	mcq_writeb(host, CTRL_R, SDMMC_RST_N_OE|SDMMC_RST_N|SDMMC_CARD_IS_EMMC);
	mcq_writeb(host, HOST_CTRL1_R,
		(mcq_readb(host, HOST_CTRL1_R)&0xe7) | (SDMMC_ADMA2_32 << SDMMC_DMA_SEL));
	mcq_writeb(host, MSHC_CTRL_R, mcq_readb(host, MSHC_CTRL_R) & (~SDMMC_CMD_CONFLICT_CHECK));
	mcq_writew(host, CLK_CTRL_R, mcq_readw(host, CLK_CTRL_R)|SDMMC_INTERNAL_CLK_EN);
}

static int dw_mci_cqe_init_slot_caps(struct dw_mci_slot *slot)
{
	struct dw_mci *host = slot->host;
	const struct dw_mci_drv_data *drv_data = host->drv_data;
	struct mmc_host *mmc = slot->mmc;
	int ctrl_id;

	if (host->pdata->caps)
		mmc->caps = host->pdata->caps;

	if (host->pdata->pm_caps)
		mmc->pm_caps = host->pdata->pm_caps;

	if (host->dev->of_node) {
		ctrl_id = of_alias_get_id(host->dev->of_node, "mshc");
		if (ctrl_id < 0)
			ctrl_id = 0;
	} else {
		ctrl_id = to_platform_device(host->dev)->id;
	}

	if (drv_data && drv_data->caps) {
		if (ctrl_id >= drv_data->num_caps) {
			dev_err(host->dev, "invalid controller id %d\n",
				ctrl_id);
			return -EINVAL;
		}
		mmc->caps |= drv_data->caps[ctrl_id];
	}

	if (host->pdata->caps2)
		mmc->caps2 = host->pdata->caps2;

	mmc->f_min = DW_MCI_FREQ_MIN;
	if (!mmc->f_max)
		mmc->f_max = DW_MCI_FREQ_MAX;

	/* Process SDIO IRQs through the sdio_irq_work. */
	if (mmc->caps & MMC_CAP_SDIO_IRQ)
		mmc->caps2 |= MMC_CAP2_SDIO_IRQ_NOTHREAD;

	return 0;
}

static int dw_mci_cqe_init_slot(struct dw_mci *host)
{
	struct mmc_host *mmc;
	struct dw_mci_slot *slot;
	int ret;

	mmc = mmc_alloc_host(sizeof(struct dw_mci_slot), host->dev);
	if (!mmc)
		return -ENOMEM;

	slot = mmc_priv(mmc);
	slot->id = 0;
	slot->sdio_id = host->sdio_id0 + slot->id;
	slot->mmc = mmc;
	slot->host = host;
	host->slot = slot;

	mmc->ops = &dw_mci_ops;

	/*if there are external regulators, get them*/
	ret = mmc_regulator_get_supply(mmc);
	if (ret)
		goto err_host_allocated;

	if (!mmc->ocr_avail)
		mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	dev_info(host->dev, "regulator support volage ocr_avail=0x%x\n",
			mmc->ocr_avail);

	ret = mmc_of_parse(mmc);
	if (ret)
		goto err_host_allocated;

	ret = dw_mci_cqe_init_slot_caps(slot);
	if (ret)
		goto err_host_allocated;

	/* Useful defaults if platform data is unset. */
	if (host->use_dma == TRANS_MODE_DMA) {
		mmc->max_segs = 256;
		mmc->max_blk_size = 512;
		mmc->max_seg_size = 0x1000;
		mmc->max_req_size = mmc->max_seg_size * mmc->max_segs;
		mmc->max_blk_count = mmc->max_req_size / 512;
	} else {
		dev_info(host->dev, "dw-mmc-cqe pio mode is ToDo.\n");
		/* To DO, TRANS_MODE_PIO */
	}

	dw_mci_cqe_get_cd(mmc);

	ret = mmc_add_host(mmc);
	if (ret)
		goto err_host_allocated;

	return 0;

err_host_allocated:
	mmc_free_host(mmc);
	return ret;
}

static void dw_mci_cqe_cleanup_slot(struct dw_mci_slot *slot)
{
	/* Debugfs stuff is cleaned up by mmc core */
	mmc_remove_host(slot->mmc);
	slot->host->slot = NULL;
	mmc_free_host(slot->mmc);
}

static void dw_mci_cqe_init_dma(struct dw_mci *host)
{
	host->use_dma = TRANS_MODE_DMA;

	/* Determine which DMA interface to use */
	/* using 32bit DMA by default,
	 * user can modify this setting by drv_data->init()
	 */
	if (host->use_dma == TRANS_MODE_DMA) {
		host->dma_64bit_address = 0;
		dev_info(host->dev, "IDMAC supports 32-bit address mode.\n");
	}

	/* Alloc memory for sg translation */
	host->sg_cpu = dma_alloc_coherent(host->dev,
						DW_MCI_DESC_LEN,
						&host->sg_dma, GFP_KERNEL);
	if (!host->sg_cpu) {
		dev_err(host->dev,
			"%s: could not alloc DMA memory\n",
			__func__);
		goto no_dma;
	}

	return;

no_dma:
	dev_info(host->dev, "Using PIO mode.\n");
	host->use_dma = TRANS_MODE_PIO;
}

#ifdef CONFIG_OF
static struct dw_mci_board *dw_mci_cqe_parse_dt(struct dw_mci *host)
{
	struct dw_mci_board *pdata;
	struct device *dev = host->dev;
	const struct dw_mci_drv_data *drv_data = host->drv_data;
	int ret;
	u32 clock_frequency;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	/* find reset controller when exist */
	pdata->rstc = devm_reset_control_get_optional(dev, "reset");
	if (IS_ERR(pdata->rstc)) {
		if (PTR_ERR(pdata->rstc) == -EPROBE_DEFER)
			return ERR_PTR(-EPROBE_DEFER);
	}

	device_property_read_u32(dev, "card-detect-delay",
		&pdata->detect_delay_ms);

	if (!device_property_read_u32(dev, "clock-frequency", &clock_frequency))
		pdata->bus_hz = clock_frequency;

	if (drv_data && drv_data->parse_dt) {
		ret = drv_data->parse_dt(host);
		if (ret)
			return ERR_PTR(ret);
	}

	return pdata;
}

#else /* CONFIG_OF */
static struct dw_mci_board *dw_mci_cqe_parse_dt(struct dw_mci *host)
{
	return ERR_PTR(-EINVAL);
}
#endif /* CONFIG_OF */

static void dw_mci_cqe_cto_timer(struct timer_list *t)
{
	struct dw_mci *host = from_timer(host, t, timer);

	if (host->int_waiting) {
		dev_err(host->dev, "fired, opcode=%d, arg=0x%x, irq status=0x%x, err irq=0x%x, auto err irq=0x%x\n",
				host->opcode, host->arg,
			host->normal_interrupt, host->error_interrupt,
			host->auto_error_interrupt);

		dw_mci_clr_signal_int(host);
		dw_mci_get_int(host);

		complete(host->int_waiting);
	}
}

static void dw_mci_cqhci_init(struct dw_mci *host)
{
	if (host->pdata && (host->pdata->caps2 & MMC_CAP2_CQE)) {
		host->cqe = cqhci_pltfm_init(host->pdev);
		if (PTR_ERR(host->cqe) == -EINVAL ||
		   PTR_ERR(host->cqe) == -ENOMEM ||
		   PTR_ERR(host->cqe) == -EBUSY) {
			dev_err(host->dev, "Unable to get the cmdq related attribute,err = %ld\n",
				PTR_ERR(host->cqe));
			host->cqe = 0;
			host->pdata->caps2 &= ~(MMC_CAP2_CQE|MMC_CAP2_CQE_DCMD);
		} else {
			host->cqe->ops = &dw_mci_cqhci_host_ops;
			cqhci_init(host->cqe, host->slot->mmc, 0);
		}
	}
}

int dw_mci_cqe_probe(struct dw_mci *host)
{
	const struct dw_mci_drv_data *drv_data = host->drv_data;
	int ret = 0;

	if (!host->pdata) {
		host->pdata = dw_mci_cqe_parse_dt(host);
		if (PTR_ERR(host->pdata) == -EPROBE_DEFER) {
			return -EPROBE_DEFER;
		} else if (IS_ERR(host->pdata)) {
			dev_err(host->dev, "platform data not available\n");
			return -EINVAL;
		}
	}

	host->biu_clk = devm_clk_get(host->dev, "biu");
	if (IS_ERR(host->biu_clk)) {
		dev_dbg(host->dev, "biu clock not available\n");
	} else {
		ret = clk_prepare_enable(host->biu_clk);
		if (ret) {
			dev_err(host->dev, "failed to enable biu clock\n");
			return ret;
		}
	}

	host->ciu_clk = devm_clk_get(host->dev, "ciu");
	if (IS_ERR(host->ciu_clk)) {
		dev_dbg(host->dev, "ciu clock not available\n");
		host->bus_hz = host->pdata->bus_hz;
	} else {
		ret = clk_prepare_enable(host->ciu_clk);
		if (ret) {
			dev_err(host->dev, "failed to enable ciu clock\n");
			goto err_clk_biu;
		}

		if (host->pdata->bus_hz) {
			ret = clk_set_rate(host->ciu_clk, host->pdata->bus_hz);
			if (ret)
				dev_warn(host->dev,
					"Unable to set bus rate to %uHz\n",
					 host->pdata->bus_hz);
		}
		host->bus_hz = clk_get_rate(host->ciu_clk);
	}

	if (!host->bus_hz) {
		dev_err(host->dev,
			"Platform data must supply bus speed\n");
		ret = -ENODEV;
		goto err_clk_ciu;
	}

	if (!IS_ERR(host->pdata->rstc)) {
		reset_control_assert(host->pdata->rstc);
		usleep_range(10, 50);
		reset_control_deassert(host->pdata->rstc);
	}

	timer_setup(&host->timer, dw_mci_cqe_cto_timer, 0);

	spin_lock_init(&host->lock);
	spin_lock_init(&host->irq_lock);
	init_rwsem(&host->cr_rw_sem);
	tasklet_init(&host->tasklet, dw_mci_cqe_tasklet_func, (unsigned long)host);

	/*pio mode's parameters should be initialized here*/

	/*Initialize the eMMC IP related attribute*/
	dw_mci_cqe_setup(host);

	dw_mci_cqe_init_dma(host);

	/* This flag will be set 1 when doing tuning,
	 * we add this flag because
	 * some vendors might use other cmd instead of 21
	 * to tune phase under high speed interface.
	 * we use this flag to recognize if the system is under tuning stage.
	 */
	host->tuning = 0;

	/*Timing_setting is to avoid sending command
	 *before setting phase in hs200, hs400
	 */
	host->current_speed = 0;

	/*Do the rest of init for specific*/
	if (drv_data && drv_data->init) {
		ret = drv_data->init(host);
		if (ret) {
			dev_err(host->dev,
				"implementation specific init failed\n");
			goto err_dmaunmap;
		}
	}

	ret = dw_mci_cqe_init_slot(host);
	if (ret) {
		dev_err(host->dev, "slot 0 init failed\n");
		goto err_dmaunmap;
	}

	ret = devm_request_irq(host->dev, host->irq, dw_mci_cqe_interrupt,
				host->irq_flags, "dw-mci-cqe", host);
	if (ret)
		goto err_dmaunmap;

	/*After the slot initialization,
	 *now we have mmc data and can initialize cmdq if user enabled
	 */
	dw_mci_cqhci_init(host);

	return 0;

err_dmaunmap:
	if (!IS_ERR(host->pdata->rstc))
		reset_control_assert(host->pdata->rstc);
err_clk_ciu:
	clk_disable_unprepare(host->ciu_clk);

err_clk_biu:
	clk_disable_unprepare(host->biu_clk);

	return ret;
}
EXPORT_SYMBOL(dw_mci_cqe_probe);

void dw_mci_cqe_remove(struct dw_mci *host)
{
	dev_dbg(host->dev, "remove slot\n");
	if (host->slot)
		dw_mci_cqe_cleanup_slot(host->slot);

	if (!IS_ERR(host->pdata->rstc))
		reset_control_assert(host->pdata->rstc);

	clk_disable_unprepare(host->ciu_clk);
	clk_disable_unprepare(host->biu_clk);

}
EXPORT_SYMBOL(dw_mci_cqe_remove);

#ifdef CONFIG_PM
int dw_mci_cqe_runtime_suspend(struct device *dev)
{
	struct dw_mci *host = dev_get_drvdata(dev);
	int ret = 0;

	if (host->pdata && (host->pdata->caps2 & MMC_CAP2_CQE)) {
		if (host->slot) {
			dev_info(host->dev, "cqe suspend\n");
			ret = cqhci_suspend(host->slot->mmc);
			if (ret) {
				dev_err(host->dev, "cqe suspend failed\n");
				return ret;
			}
		}
	}

	clk_disable_unprepare(host->ciu_clk);

	return ret;
}
EXPORT_SYMBOL(dw_mci_cqe_runtime_suspend);

int dw_mci_cqe_runtime_resume(struct device *dev)
{
	struct dw_mci *host = dev_get_drvdata(dev);
	const struct dw_mci_drv_data *drv_data = host->drv_data;
	int ret = 0;

	clk_prepare_enable(host->ciu_clk);

	dw_mci_cqe_setup(host);
	if (drv_data && drv_data->init) {
		ret = drv_data->init(host);
		if (ret)
			dev_err(host->dev, "implementation specific init failed\n");
	}

	init_completion(host->int_waiting);

	if (host->pdata && (host->pdata->caps2 & MMC_CAP2_CQE)) {
		if (host->slot) {
			dev_info(host->dev, "cqe resume\n");
			ret = cqhci_resume(host->slot->mmc);
			if (ret)
				dev_err(host->dev, "cqe resume failed\n");
		}
	}

	dw_mci_cqe_setup_bus(host->slot, true);

	return ret;
}
EXPORT_SYMBOL(dw_mci_cqe_runtime_resume);
#endif /* CONFIG_PM */

static int __init dw_mci_cqe_init(void)
{
	pr_info("Synopsys Designware Multimedia Card Interface Driver\n");
	return 0;
}

static void __exit dw_mci_cqe_exit(void)
{
}

module_init(dw_mci_cqe_init);
module_exit(dw_mci_cqe_exit);

MODULE_DESCRIPTION("DW Multimedia Card CMDQ Interface driver");
MODULE_AUTHOR("<jyanchou@realtek.com>");
MODULE_LICENSE("GPL");
