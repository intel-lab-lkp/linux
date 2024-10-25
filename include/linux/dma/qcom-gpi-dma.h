/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020, Linaro Limited
 */

#ifndef QCOM_GPI_DMA_H
#define QCOM_GPI_DMA_H

/**
 * enum spi_transfer_cmd - spi transfer commands
 */
enum spi_transfer_cmd {
	SPI_TX = 1,
	SPI_RX,
	SPI_DUPLEX,
};

#define QCOM_GPI_BLOCK_EVENT_IRQ	BIT(0)

#define QCOM_GPI_MAX_NUM_MSGS		16
#define NUM_MSGS_PER_IRQ		8
#define MIN_NUM_OF_MSGS_MULTI_DESC	4

/**
 * struct gpi_spi_config - spi config for peripheral
 *
 * @loopback_en: spi loopback enable when set
 * @clock_pol_high: clock polarity
 * @data_pol_high: data polarity
 * @pack_en: process tx/rx buffers as packed
 * @word_len: spi word length
 * @clk_div: source clock divider
 * @clk_src: serial clock
 * @cmd: spi cmd
 * @fragmentation: keep CS asserted at end of sequence
 * @cs: chip select toggle
 * @set_config: set peripheral config
 * @rx_len: receive length for buffer
 */
struct gpi_spi_config {
	u8 set_config;
	u8 loopback_en;
	u8 clock_pol_high;
	u8 data_pol_high;
	u8 pack_en;
	u8 word_len;
	u8 fragmentation;
	u8 cs;
	u32 clk_div;
	u32 clk_src;
	enum spi_transfer_cmd cmd;
	u32 rx_len;
};

enum i2c_op {
	I2C_WRITE = 1,
	I2C_READ,
};

/**
 * struct gpi_multi_xfer - Used for multi transfer support
 *
 * @msg_idx_cnt: message index for the transfer
 * @buf_idx: dma buffer index
 * @unmap_msg_cnt: unampped transfer index
 * @freed_msg_cnt: freed transfer index
 * @irq_cnt: received interrupt count
 * @irq_msg_cnt: transfer message count for the received irqs
 * @dma_buf: virtual address of the buffer
 * @dma_addr: dma address of the buffer
 */
struct gpi_multi_xfer {
	u32 msg_idx_cnt;
	u32 buf_idx;
	u32 unmap_msg_cnt;
	u32 freed_msg_cnt;
	u32 irq_cnt;
	u32 irq_msg_cnt;
	void *dma_buf[QCOM_GPI_MAX_NUM_MSGS];
	dma_addr_t dma_addr[QCOM_GPI_MAX_NUM_MSGS];
};

/**
 * struct gpi_i2c_config - i2c config for peripheral
 *
 * @pack_enable: process tx/rx buffers as packed
 * @cycle_count: clock cycles to be sent
 * @high_count: high period of clock
 * @low_count: low period of clock
 * @clk_div: source clock divider
 * @addr: i2c bus address
 * @stretch: stretch the clock at eot
 * @set_config: set peripheral config
 * @rx_len: receive length for buffer
 * @op: i2c cmd
 * @muli-msg: is part of multi i2c r-w msgs
 * @flags: true for block event interrupt support
 * @multi_xfer: indicates transfer has multi messages
 */
struct gpi_i2c_config {
	u8 set_config;
	u8 pack_enable;
	u8 cycle_count;
	u8 high_count;
	u8 low_count;
	u8 addr;
	u8 stretch;
	u16 clk_div;
	u32 rx_len;
	enum i2c_op op;
	bool multi_msg;
	u8 flags;
	struct gpi_multi_xfer multi_xfer;
};

int gpi_multi_desc_process(struct device *dev, struct gpi_multi_xfer *multi_xfer,
			   u32 num_xfers, u32 tranfer_timeout_msecs,
			   struct completion *transfer_comp);

#endif /* QCOM_GPI_DMA_H */
