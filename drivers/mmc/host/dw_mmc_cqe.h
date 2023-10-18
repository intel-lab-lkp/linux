/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2023 Realtek Semiconductors, All Rights Reserved.
 *
 */

#ifndef __DW_MMC_CQE_H
#define __DW_MMC_CQE_H

#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/mmc/core.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>

struct dw_mci {
	spinlock_t              lock;
	spinlock_t              irq_lock;
	struct tasklet_struct   tasklet;
	struct rw_semaphore     cr_rw_sem;

	void __iomem            *regs;
	resource_size_t         phy_regs;

	struct mmc_request      *mrq;
	struct mmc_command      *cmd;
	struct mmc_command	stop_abort;
	struct mmc_command	stat_ready;
	struct mmc_data         *data;

	struct clk              *biu_clk;
	struct clk              *ciu_clk;
	struct dw_mci_slot      *slot;
	struct timer_list       timer;
	struct completion	*int_waiting;

	unsigned int		*desc_vaddr;
	unsigned int            *sg_cpu;
	dma_addr_t              sg_dma;
	int                     use_dma;

	struct platform_device  *pdev;
	struct device           *dev;
	struct dw_mci_board     *pdata;
	const struct dw_mci_drv_data    *drv_data;
	void                    *priv;

	u32			opcode;
	u32			arg;
	u16                     normal_interrupt;
	u16                     error_interrupt;
	u16                     auto_error_interrupt;

	u32                     bus_hz;
	u32			current_speed;
	u32			stop_cmdr;
	bool			is_sbc;
	int			dma_64bit_address;
	int			using_dma;

	unsigned long           irq_flags; /* IRQ flags */
	int                     irq;
	int                     sdio_id0;

	struct scatterlist	*sg;
	u32			dma_nents;

	u8			tuning;
	struct cqhci_host       *cqe;
};

enum {
	TRANS_MODE_PIO = 0,
	TRANS_MODE_DMA,
};

enum dw_mci_cookie {
	COOKIE_UNMAPPED,
	COOKIE_PRE_MAPPED,	/* mapped by pre_req() of dwmmc */
	COOKIE_MAPPED,		/* mapped by prepare_data() of dwmmc */
};
/* eMMC control register definition */
#define SDMMC_SDMASA_R				0x000
#define SDMMC_BLOCKSIZE_R			0x004
#define SDMMC_BLOCKCOUNT_R			0x006
#define SDMMC_ARGUMENT_R			0x008
#define SDMMC_XFER_MODE_R			0x00c
#define SDMMC_CMD_R				0x00e
#define SDMMC_RESP01_R				0x010
#define SDMMC_RESP23_R				0x014
#define SDMMC_RESP45_R				0x018
#define SDMMC_RESP67_R				0x01c
#define SDMMC_BUF_DATA_R			0x020
#define SDMMC_PSTATE_REG			0x024
#define SDMMC_HOST_CTRL1_R			0x028
#define SDMMC_PWR_CTRL_R			0x029
#define SDMMC_BGAP_CTRL_R			0x02a
#define SDMMC_CLK_CTRL_R			0x02c
#define SDMMC_TOUT_CTRL_R			0x02e
#define SDMMC_SW_RST_R				0x02f
#define SDMMC_NORMAL_INT_STAT_R			0x030
#define SDMMC_ERROR_INT_STAT_R			0x032
#define SDMMC_NORMAL_INT_STAT_EN_R		0x034
#define SDMMC_ERROR_INT_STAT_EN_R		0x036
#define SDMMC_NORMAL_INT_SIGNAL_EN_R		0x038
#define SDMMC_ERROR_INT_SIGNAL_EN_R		0x03a
#define SDMMC_AUTO_CMD_STAT_R			0x03c
#define SDMMC_HOST_CTRL2_R			0x03e
#define SDMMC_ADMA_ERR_STAT_R			0x054
#define SDMMC_ADMA_SA_LOW_R			0x058

#define SDMMC_MSHC_CTRL_R			0x208
#define SDMMC_CTRL_R				0x22c

#define SDMMC_CMD_CONFLICT_CHECK		BIT(0)
#define CMD_IDX_MASK(x)				((x >> 8)&0x3f)

/*0xc*/
#define SDMMC_MULTI_BLK_SEL			5
#define SDMMC_DATA_XFER_DIR			4
#define SDMMC_BLOCK_COUNT_ENABLE		BIT(1)
#define SDMMC_DMA_ENABLE			BIT(0)
#define SDMMC_AUTO_CMD_ENABLE			2
#define SDMMC_AUTO_CMD_DISABLED			0x0
#define SDMMC_AUTO_CMD12_ENABLED		0x1
#define SDMMC_AUTO_CMD23_ENABLED		0x2
#define SDMMC_AUTO_CMD_SEL			0x3

/*0xe*/
#define SDMMC_RESP_TYPE_SELECT			0
#define SDMMC_CMD_TYPE				6
#define SDMMC_NO_RESP				0x0
#define SDMMC_RESP_LEN_136			0x1
#define SDMMC_RESP_LEN_48			0x2
#define SDMMC_RESP_LEN_48B			0x3
#define SDMMC_CMD_CHK_RESP_CRC			BIT(3)
#define SDMMC_CMD_IDX_CHK_ENABLE		BIT(4)
#define SDMMC_DATA				BIT(5)
#define SDMMC_ABORT_CMD				0x3
#define SDMMC_RESUME_CMD			0x2
#define SDMMC_SUSPEND_CMD			0x1
#define SDMMC_NORMAL_CMD			0x0

/*0x24 PSTATE*/
#define SDMMC_CMD_INHIBIT			BIT(0)
#define SDMMC_CMD_INHIBIT_DAT			BIT(1)
#define SDMMC_DAT_3_0				(0xf << 20)
#define SDMMC_DAT_7_4				(0xf << 4)

/*0x28*/
#define SDMMC_DMA_SEL				3
#define SDMMC_SDMA				(0x0)
#define SDMMC_ADMA2_32				(0x2)
#define SDMMC_ADMA2_64				(0x3)
#define SDMMC_EXT_DAT_XFER			BIT(5)
#define SDMMC_EXT_DAT_XFER_MASK			(~SDMMC_EXT_DAT_XFER & 0xff)
#define SDMMC_HIGH_SPEED_EN			BIT(2)
#define SDMMC_HIGH_SPEED_MASK			((~BIT(2)) & 0xff)
#define SDMMC_UHS_MODE_SEL_MASK			((~(BIT(0)|BIT(1)|BIT(2))) & 0xffff)
#define SDMMC_DAT_XFER_WIDTH			BIT(1)
#define SDMMC_DAT_XFER_WIDTH_MASK		(~SDMMC_DAT_XFER_WIDTH & 0xff)
#define SDMMC_BUS_WIDTH_8			SDMMC_EXT_DAT_XFER
#define SDMMC_BUS_WIDTH_4			SDMMC_DAT_XFER_WIDTH
#define SDMMC_BUS_WIDTH_1			0
#define SDMMC_DMA_SEL_CLR			(0xff & (~(0x3<<SDMMC_DMA_SEL)))
#define SDMMC_DATA_XFER_CLR			((0xff & (~SDMMC_EXT_DAT_XFER)) \
							& (~SDMMC_DAT_XFER_WIDTH))

/*0x2a*/
#define SDMMC_STOP_BG_REQ			BIT(0)

/*0x2f SW_RST_R*/
#define SDMMC_RST_DAT			BIT(2)
#define SDMMC_RST_CMD			BIT(1)
#define SDMMC_RST_ALL			BIT(0)

/*0x30 status bitmap*/
#define SDMMC_STATUS_ALL			0xffff
#define SDMMC_ERR_INTERRUPT			BIT(15)
#define SDMMC_CQE_EVENT				BIT(14)
#define SDMMC_FX_EVENT				BIT(13)
#define SDMMC_RE_TUNE_EVENT			BIT(12)
#define SDMMC_INT_C				BIT(11)
#define SDMMC_INT_B				BIT(10)
#define SDMMC_INT_A				BIT(9)
#define SDMMC_CARD_INTERRUPT			BIT(8)
#define SDMMC_CARD_REMOVAL			BIT(7)
#define SDMMC_CARD_INSERTION			BIT(6)
#define SDMMC_BUF_RD_READY			BIT(5)
#define SDMMC_BUF_WR_READY			BIT(4)
#define SDMMC_DMA_INTERRPT			BIT(3)
#define SDMMC_BGAP_EVENT			BIT(2)
#define SDMMC_XFER_COMPLETE			BIT(1)
#define SDMMC_CMD_COMPLETE			BIT(0)

/*0x32 error bitmap*/
#define SDMMC_VENDOR_ERR3			BIT(15)
#define SDMMC_VENDOR_ERR2			BIT(14)
#define SDMMC_VENDOR_ERR1			BIT(13)
#define SDMMC_BOOT_ACK_ERR			BIT(12)
#define SDMMC_RESP_ERR				BIT(11)
#define SDMMC_TUNING_ERR			BIT(10)
#define SDMMC_ADMA_ERR				BIT(9)
#define SDMMC_AUTO_CMD_ERR			BIT(8)
#define SDMMC_CUR_LMT_ERR			BIT(7)
#define SDMMC_DATA_END_BIT_ERR			BIT(6)
#define SDMMC_DATA_CRC_ERR			BIT(5)
#define SDMMC_DATA_TOUT_ERR			BIT(4)
#define SDMMC_CMD_IDX_ERR			BIT(3)
#define SDMMC_CMD_END_BIT_ERR			BIT(2)
#define SDMMC_CMD_CRC_ERR			BIT(1)
#define SDMMC_CMD_TOUT_ERR			BIT(0)
#define SDMMC_CMD_ERR                           (SDMMC_AUTO_CMD_ERR|SDMMC_CMD_IDX_ERR| \
						SDMMC_CMD_END_BIT_ERR|SDMMC_CMD_CRC_ERR| \
						SDMMC_CMD_TOUT_ERR)
#define SDMMC_DATA_ERR				(SDMMC_ADMA_ERR|SDMMC_DATA_END_BIT_ERR| \
						SDMMC_DATA_CRC_ERR|SDMMC_DATA_TOUT_ERR)

/*0x34 status enable bitmap*/
#define SDMMC_CQE_EVENT_STAT_EN			BIT(14)
#define SDMMC_FX_EVENT_STAT_EN			BIT(13)
#define SDMMC_RE_TUNE_EVENT_STAT_EN		BIT(12)
#define SDMMC_INT_C_STAT_EN			BIT(11)
#define SDMMC_INT_B_STAT_EN			BIT(10)
#define SDMMC_INT_A_STAT_EN			BIT(9)
#define SDMMC_CARD_INTERRUPT_STAT_EN		BIT(8)
#define SDMMC_CARD_REMOVAL_STAT_EN		BIT(7)
#define SDMMC_CARD_INSERTION_STAT_EN		BIT(6)
#define SDMMC_BUF_RD_READY_STAT_EN		BIT(5)
#define SDMMC_BUF_WR_READY_STAT_EN		BIT(4)
#define SDMMC_DMA_INTERRPT_STAT_EN		BIT(3)
#define SDMMC_BGAP_EVENT_STAT_EN		BIT(2)
#define SDMMC_XFER_COMPLETE_STAT_EN		BIT(1)
#define SDMMC_CMD_COMPLETE_STAT_EN		BIT(0)

/*0x36 error status enable bitmap*/
#define SDMMC_VENDOR_ERR_STAT_EN3		BIT(15)
#define SDMMC_VENDOR_ERR_STAT_EN2		BIT(14)
#define SDMMC_VENDOR_ERR_STAT_EN1		BIT(13)
#define SDMMC_BOOT_ACK_ERR_STAT_EN		BIT(12)
#define SDMMC_RESP_ERR_STAT_EN			BIT(11)
#define SDMMC_TUNING_ERR_STAT_EN		BIT(10)
#define SDMMC_ADMA_ERR_STAT_EN			BIT(9)
#define SDMMC_AUTO_CMD_ERR_STAT_EN		BIT(8)
#define SDMMC_CUR_LMT_ERR_STAT_EN		BIT(7)
#define SDMMC_DATA_END_BIT_ERR_STAT_EN		BIT(6)
#define SDMMC_DATA_CRC_ERR_STAT_EN		BIT(5)
#define SDMMC_DATA_TOUT_ERR_STAT_EN		BIT(4)
#define SDMMC_CMD_IDX_ERR_STAT_EN		BIT(3)
#define SDMMC_CMD_END_BIT_ERR_STAT_EN		BIT(2)
#define SDMMC_CMD_CRC_ERR_STAT_EN		BIT(1)
#define SDMMC_CMD_TOUT_ERR_STAT_EN		BIT(0)

/*0x38 signal interrupt enable*/
#define SDMMC_CQE_EVENT_SIGNAL_EN		BIT(14)
#define SDMMC_FX_EVENT_SIGNAL_EN		BIT(13)
#define SDMMC_RE_TUNE_EVENT_SIGNAL_EN		BIT(12)
#define SDMMC_INT_C_SIGNAL_EN			BIT(11)
#define SDMMC_INT_B_SIGNAL_EN			BIT(10)
#define SDMMC_INT_A_SIGNAL_EN			BIT(9)
#define SDMMC_CARD_INTERRUPT_SIGNAL_EN		BIT(8)
#define SDMMC_CARD_REMOVAL_SIGNAL_EN		BIT(7)
#define SDMMC_CARD_INSERTION_SIGNAL_EN		BIT(6)
#define SDMMC_BUF_RD_READY_SIGNAL_EN		BIT(5)
#define SDMMC_BUF_WR_READY_SIGNAL_EN		BIT(4)
#define SDMMC_DMA_INTERRPT_SIGNAL_EN		BIT(3)
#define SDMMC_BGAP_EVENT_SIGNAL_EN		BIT(2)
#define SDMMC_XFER_COMPLETE_SIGNAL_EN		BIT(1)
#define SDMMC_CMD_COMPLETE_SIGNAL_EN		BIT(0)
#define SDMMC_NORMAL_INT_SIGNAL_CMD_EN_R	(~(BIT(6) | BIT(7) | BIT(8) | BIT(1)) & 0xffff)
#define SDMMC_NORMAL_INT_SIGNAL_DAT_EN_R	(~(BIT(6) | BIT(7) | BIT(8) | BIT(0)) & 0xffff)
#define SDMMC_NORMAL_INT_SIGNAL_CQE_EN_R	(~(BIT(6) | BIT(7) | BIT(8) | BIT(1) \
						| BIT(0)) & 0xffff)

/*0x3a error ssignal enable bitmap*/
#define SDMMC_VENDOR_ERR_SIGNAL_EN3		BIT(15)
#define SDMMC_VENDOR_ERR_SIGNAL_EN2		BIT(14)
#define SDMMC_VENDOR_ERR_SIGNAL_EN1		BIT(13)
#define SDMMC_BOOT_ACK_ERR_SIGNAL_EN		BIT(12)
#define SDMMC_RESP_ERR_SIGNAL_EN		BIT(11)
#define SDMMC_TUNING_ERR_SIGNAL_EN		BIT(10)
#define SDMMC_ADMA_ERR_SIGNAL_EN		BIT(9)
#define SDMMC_AUTO_CMD_ERR_SIGNAL_EN		BIT(8)
#define SDMMC_CUR_LMT_ERR_SIGNAL_EN		BIT(7)
#define SDMMC_DATA_END_BIT_ERR_SIGNAL_EN	BIT(6)
#define SDMMC_DATA_CRC_ERR_SIGNAL_EN		BIT(5)
#define SDMMC_DATA_TOUT_ERR_SIGNAL_EN		BIT(4)
#define SDMMC_CMD_IDX_ERR_SIGNAL_EN		BIT(3)
#define SDMMC_CMD_END_BIT_ERR_SIGNAL_EN		BIT(2)
#define SDMMC_CMD_CRC_ERR_SIGNAL_EN		BIT(1)
#define SDMMC_CMD_TOUT_ERR_STAT_EN		BIT(0)

#define SDMMC_ALL_NORMAL_STAT_EN		(0xfeff)
#define SDMMC_ALL_ERR_STAT_EN			(0xffff)
#define SDMMC_ALL_SIGNAL_STAT_EN                (0xfeff)
#define SDMMC_ALL_ERR_SIGNAL_EN			(0xffff)

/*0x3e*/
#define SDMMC_LEGACY				0x0
#define SDMMC_SDR				0x1
#define SDMMC_HS200				0x3
#define SDMMC_DDR				0x4
#define SDMMC_HS400				0x7
#define SDMMC_HOST_VER4_ENABLE			BIT(12)
#define SDMMC_SIGNALING_EN			BIT(3)

/*0x22c*/
#define SDMMC_RST_N_OE				BIT(3)
#define SDMMC_RST_N				BIT(2)
#define SDMMC_CARD_IS_EMMC			BIT(0)

#define SDMMC_INTERNAL_CLK_EN			BIT(0)
#define SDMMC_PLL_USABLE			BIT(0)

#define VALID(x)			((x & 1) << 0)
#define END(x)				((x & 1) << 1)
#define INT(x)				((x & 1) << 2)
#define ACT(x)				((x & 0x7) << 3)
#define DAT_LENGTH(x)			((x & 0xFFFF) << 16)

#define SW_RST_CMD_DONE				0x2000000
#define SW_RST_DATA_DONE			0x4000000
#define SW_RST_BOTH_DONE                        0x6000000

/* Register access macros */
#define mcq_readl(dev, reg)                     \
	readl_relaxed((dev)->regs + SDMMC_##reg)
#define mcq_writel(dev, reg, value)                     \
	writel_relaxed((value), (dev)->regs + SDMMC_##reg)

#define mcq_readw(dev, reg)                     \
	readw_relaxed((dev)->regs + SDMMC_##reg)
#define mcq_writew(dev, reg, value)                     \
	writew_relaxed((value), (dev)->regs + SDMMC_##reg)

#define mcq_readb(dev, reg)                     \
	readb_relaxed((dev)->regs + SDMMC_##reg)
#define mcq_writeb(dev, reg, value)                     \
	writeb_relaxed((value), (dev)->regs + SDMMC_##reg)

#define dw_mci_get_int(dev)    \
	do {    \
		dev->normal_interrupt = mcq_readw(dev, NORMAL_INT_STAT_R);   \
		dev->error_interrupt = mcq_readw(dev, ERROR_INT_STAT_R);   \
		dev->auto_error_interrupt = mcq_readw(dev, AUTO_CMD_STAT_R);     \
	} while (0)

/*clear status register, we always keep the card interrupt*/
#define dw_mci_clr_int(dev)                                             \
	do {                                                            \
		mcq_writew(dev, ERROR_INT_STAT_R, mcq_readw(dev, ERROR_INT_STAT_R) & 0xffff); \
		mcq_writew(dev, NORMAL_INT_STAT_R, mcq_readw(dev, NORMAL_INT_STAT_R) & 0xffff); \
	} while (0)

/*mask all emmc interrupts*/
#define dw_mci_clr_signal_int(dev)    \
	do {      \
		mcq_writew(dev, NORMAL_INT_SIGNAL_EN_R, mcq_readw(dev, NORMAL_INT_SIGNAL_EN_R) \
			& (BIT(6)|BIT(7))); \
		mcq_writew(dev, ERROR_INT_SIGNAL_EN_R, 0); \
	} while (0)

/*for cmdq, we do not need cmd and xfer done, only cqe event*/
#define dw_mci_en_cqe_int(dev)  \
	do { \
		mcq_writew(dev, NORMAL_INT_SIGNAL_EN_R, \
				mcq_readw(dev, NORMAL_INT_SIGNAL_EN_R) | \
				SDMMC_NORMAL_INT_SIGNAL_CQE_EN_R); \
		mcq_writew(dev, ERROR_INT_SIGNAL_EN_R, SDMMC_ALL_ERR_SIGNAL_EN); \
	} while (0)

/*used for data, r1b case, we mask cmd done interrupt*/
#define dw_mci_en_xfer_int(dev)  \
	do {  \
		mcq_writew(dev, NORMAL_INT_SIGNAL_EN_R, \
			mcq_readw(dev, NORMAL_INT_SIGNAL_EN_R)|SDMMC_NORMAL_INT_SIGNAL_DAT_EN_R); \
		mcq_writew(dev, ERROR_INT_SIGNAL_EN_R, SDMMC_ALL_ERR_SIGNAL_EN); \
	} while (0)

/*used for none-stream case (cmd w/wo/ resp)*/
#define dw_mci_en_cd_int(dev)  \
	do {    \
		mcq_writew(dev, NORMAL_INT_SIGNAL_EN_R, \
			mcq_readw(dev, NORMAL_INT_SIGNAL_EN_R)|SDMMC_NORMAL_INT_SIGNAL_CMD_EN_R); \
		mcq_writew(dev, ERROR_INT_SIGNAL_EN_R, SDMMC_ALL_ERR_SIGNAL_EN); \
	} while (0)

extern int dw_mci_cqe_probe(struct dw_mci *host);
extern void dw_mci_cqe_remove(struct dw_mci *host);
#ifdef CONFIG_PM
extern int dw_mci_cqe_runtime_suspend(struct device *device);
extern int dw_mci_cqe_runtime_resume(struct device *device);
#endif
irqreturn_t cqhci_irq(struct mmc_host *mmc, u32 intmask, int cmd_error, int data_error);
void cqhci_set_tran_desc(u8 *desc, dma_addr_t addr, int len, bool end, bool dma64);

/* Board platform data */
struct dw_mci_board {
	unsigned int bus_hz; /* Clock speed at the cclk_in pad */
	u32 caps;       /* Capabilities */
	u32 caps2;      /* More capabilities */
	u32 pm_caps;    /* PM capabilities */

	/* delay in mS before detecting cards after interrupt */
	u32 detect_delay_ms;

	struct reset_control *rstc;
};

struct dw_mci_slot {
	struct mmc_host         *mmc;
	struct dw_mci           *host;

	struct mmc_request      *mrq;

	unsigned int            clock;
	unsigned int            __clk_old;

	unsigned long           flags;
#define DW_MMC_CARD_PRESENT     1
	int                     id;
	int                     sdio_id;
	u8                      switch_partition;
};

struct dw_mci_drv_data {
	unsigned long   *caps;
	u32             num_caps;
	int             (*init)(struct dw_mci *host);
	void            (*set_ios)(struct dw_mci_slot *slot, struct mmc_ios *ios);
	int             (*parse_dt)(struct dw_mci *host);
	int             (*execute_tuning)(struct dw_mci_slot *slot, u32 opcode);
	int             (*prepare_hs400_tuning)(struct dw_mci *host,
					struct mmc_ios *ios);
	int             (*switch_voltage)(struct mmc_host *mmc,
					struct mmc_ios *ios);
	void            (*hs400_complete)(struct mmc_host *mmc);
	void		(*init_card)(struct mmc_host *host,
					struct mmc_card *card);
	void            (*shift_rsp)(struct dw_mci *host, struct mmc_command *cmd, u32 *rsp);
};

void dw_mci_cqe_wait_done(struct dw_mci *host, u32 *addr, u32 mask, u32 value);
#endif
