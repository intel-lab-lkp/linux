/* SPDX-License-Identifier: GPL-2.0 */

#ifndef LOONGSON2_MMC_H
#define LOONGSON2_MMC_H

#define LOONGSON2_MMC_REG_CTL		0x00 /* Control Register */
#define LOONGSON2_MMC_REG_PRE		0x04 /* Prescaler Register */
#define LOONGSON2_MMC_REG_CARG		0x08 /* Command Register */
#define LOONGSON2_MMC_REG_CCTL		0x0c /* Command Control Register */
#define LOONGSON2_MMC_REG_CSTS		0x10 /* Command Status Register */
#define LOONGSON2_MMC_REG_RSP0		0x14 /* Command Response Register 0 */
#define LOONGSON2_MMC_REG_RSP1		0x18 /* Command Response Register 1 */
#define LOONGSON2_MMC_REG_RSP2		0x1c /* Command Response Register 2 */
#define LOONGSON2_MMC_REG_RSP3		0x20 /* Command Response Register 3 */
#define LOONGSON2_MMC_REG_TIMER		0x24 /* Data Timeout Register */
#define LOONGSON2_MMC_REG_BSIZE		0x28 /* Block Size Register */
#define LOONGSON2_MMC_REG_DCTL		0x2c /* Data Control Register */
#define LOONGSON2_MMC_REG_DCNT		0x30 /* Data Counter Register */
#define LOONGSON2_MMC_REG_DSTS		0x34 /* Data Status Register */
#define LOONGSON2_MMC_REG_FSTS		0x38 /* FIFO Status Register */
#define LOONGSON2_MMC_REG_INT		0x3c /* Interrupt Register */
#define LOONGSON2_MMC_REG_DATA		0x40 /* Data Register */
#define LOONGSON2_MMC_REG_IEN		0x64 /* Interrupt Enable Register */

/* Bitfields of control register */
#define LOONGSON2_MMC_CTL_ENCLK		BIT(0)
#define LOONGSON2_MMC_CTL_RESET		BIT(8)

/* Bitfields of prescaler register */
#define LOONGSON2_MMC_PRE		GENMASK(9, 0)
#define LOONGSON2_MMC_PRE_EN		BIT(31)

/* Bitfields of command control register */
#define LOONGSON2_MMC_CCTL_INDEX		GENMASK(5, 0)
#define LOONGSON2_MMC_CCTL_HOST		BIT(6)
#define LOONGSON2_MMC_CCTL_START		BIT(8)
#define LOONGSON2_MMC_CCTL_WAIT_RSP	BIT(9)
#define LOONGSON2_MMC_CCTL_LONG_RSP	BIT(10)
#define LOONGSON2_MMC_CCTL_ABORT		BIT(12)
#define LOONGSON2_MMC_CCTL_CHECK		BIT(13)
#define LOONGSON2_MMC_CCTL_SDIO		BIT(14)
#define LOONGSON2_MMC_CCTL_CMD6		BIT(18)

/* Bitfields of command status register */
#define LOONGSON2_MMC_CSTS_INDEX		GENMASK(7, 0)
#define LOONGSON2_MMC_CSTS_ON		BIT(8)
#define LOONGSON2_MMC_CSTS_RSP		BIT(9)
#define LOONGSON2_MMC_CSTS_TIMEOUT	BIT(10)
#define LOONGSON2_MMC_CSTS_END		BIT(11)
#define LOONGSON2_MMC_CSTS_CRC_ERR	BIT(12)
#define LOONGSON2_MMC_CSTS_AUTO_STOP	BIT(13)
#define LOONGSON2_MMC_CSTS_FIN		BIT(14)

/* Bitfields of data timeout register */
#define LOONGSON2_MMC_DTIMR		GENMASK(23, 0)

/* Bitfields of block size register */
#define LOONGSON2_MMC_BSIZE		GENMASK(11, 0)

/* Bitfields of data control register */
#define LOONGSON2_MMC_DCTL_BNUM		GENMASK(11, 0)
#define LOONGSON2_MMC_DCTL_START		BIT(14)
#define LOONGSON2_MMC_DCTL_ENDMA		BIT(15)
#define LOONGSON2_MMC_DCTL_WIDE		BIT(16)
#define LOONGSON2_MMC_DCTL_RWAIT		BIT(17)
#define LOONGSON2_MMC_DCTL_IO_SUSPEND	BIT(18)
#define LOONGSON2_MMC_DCTL_IO_RESUME	BIT(19)
#define LOONGSON2_MMC_DCTL_RW_RESUME	BIT(20)
#define LOONGSON2_MMC_DCTL_8BIT_BUS      BIT(26)

/* Bitfields of sata counter register */
#define LOONGSON2_MMC_DCNT_BNUM		GENMASK(11, 0)
#define LOONGSON2_MMC_DCNT_BYTE		GENMASK(23, 12)

/* Bitfields of command status register */
#define LOONGSON2_MMC_DSTS_RXON		BIT(0)
#define LOONGSON2_MMC_DSTS_TXON		BIT(1)
#define LOONGSON2_MMC_DSTS_SBITERR	BIT(2)
#define LOONGSON2_MMC_DSTS_BUSYFIN	BIT(3)
#define LOONGSON2_MMC_DSTS_XFERFIN	BIT(4)
#define LOONGSON2_MMC_DSTS_DTIMEOUT	BIT(5)
#define LOONGSON2_MMC_DSTS_RXCRC		BIT(6)
#define LOONGSON2_MMC_DSTS_TXCRC		BIT(7)
#define LOONGSON2_MMC_DSTS_IRQ		BIT(8)
#define LOONGSON2_MMC_DSTS_START		BIT(13)
#define LOONGSON2_MMC_DSTS_RESUME	BIT(15)
#define LOONGSON2_MMC_DSTS_SUSPEND	BIT(16)

/* Bitfields of interrupt register */
#define LOONGSON2_MMC_INT_DFIN		BIT(0)
#define LOONGSON2_MMC_INT_DTIMEOUT	BIT(1)
#define LOONGSON2_MMC_INT_RXCRC		BIT(2)
#define LOONGSON2_MMC_INT_TXCRC		BIT(3)
#define LOONGSON2_MMC_INT_PROGERR	BIT(4)
#define LOONGSON2_MMC_INT_SDIOIRQ	BIT(5)
#define LOONGSON2_MMC_INT_CSENT		BIT(6)
#define LOONGSON2_MMC_INT_CTIMEOUT	BIT(7)
#define LOONGSON2_MMC_INT_RESPCRC	BIT(8)
#define LOONGSON2_MMC_INT_BUSYEND	BIT(9)

/* Bitfields of interrupt enable register */
#define LOONGSON2_MMC_IEN_DFIN		BIT(0)
#define LOONGSON2_MMC_IEN_DTIMEOUT	BIT(1)
#define LOONGSON2_MMC_IEN_RXCRC		BIT(2)
#define LOONGSON2_MMC_IEN_TXCRC		BIT(3)
#define LOONGSON2_MMC_IEN_PROGERR	BIT(4)
#define LOONGSON2_MMC_IEN_SDIOIRQ	BIT(5)
#define LOONGSON2_MMC_IEN_CSENT		BIT(6)
#define LOONGSON2_MMC_IEN_CTIMEOUT	BIT(7)
#define LOONGSON2_MMC_IEN_RESPCRC	BIT(8)
#define LOONGSON2_MMC_IEN_BUSYEND	BIT(9)

#define LOONGSON2_MMC_IEN_ALL		GENMASK(9, 0)
#define LOONGSON2_MMC_INT_CLEAR		GENMASK(9, 0)

/* Loongson-2K1000 SDIO2 DMA routing register */
#define LS2K1000_SDIO_DMA_MASK		GENMASK(17, 15)
#define LS2K1000_DMA0_CONF		0x0
#define LS2K1000_DMA1_CONF		0x1
#define LS2K1000_DMA2_CONF		0x2
#define LS2K1000_DMA3_CONF		0x3
#define LS2K1000_DMA4_CONF		0x4

/* Loongson-2K0500 SDIO2 DMA routing register */
#define LS2K0500_SDIO_DMA_MASK		GENMASK(15, 14)
#define LS2K0500_DMA0_CONF		0x1
#define LS2K0500_DMA1_CONF		0x2
#define LS2K0500_DMA2_CONF		0x3

enum loongson2_mmc_state {
	STATE_NONE,
	STATE_FINALIZE,
	STATE_CMDSENT,
	STATE_RSPFIN,
	STATE_XFERFINISH,
	STATE_XFERFINISH_RSPFIN,
};

struct loongson2_dma_desc {
	u32 ndesc_addr;
	u32 mem_addr;
	u32 apb_addr;
	u32 len;
	u32 step_len;
	u32 step_times;
	u32 cmd;
	u32 stats;
	u32 high_ndesc_addr;
	u32 high_mem_addr;
	u32 reserved[2];
} __packed;

struct loongson2_mmc_host {
	struct device *dev;
	struct mmc_host *mmc;
	struct mmc_request *mrq;
	struct regmap *regmap;
	struct resource *res;
	struct clk *clk;
	u64 rate;
	int dma_complete;
	struct dma_chan *chan;
	int cmd_is_stop;
	int bus_width;
	spinlock_t lock;
	enum loongson2_mmc_state state;
	const struct loongson2_mmc_pdata *pdata;
};

struct loongson2_mmc_pdata {
	const struct regmap_config regmap_config;
	void (*reorder_cmd_data)(struct loongson2_mmc_host *host, struct mmc_command *cmd);
	int (*setting_dma)(struct loongson2_mmc_host *host, struct platform_device *pdev);
	int (*prepare_dma)(struct loongson2_mmc_host *host, struct mmc_data *data);
	void (*release_dma)(struct loongson2_mmc_host *host, struct device *dev);
};
#endif
