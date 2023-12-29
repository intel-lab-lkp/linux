/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __I2C_VIAI2C_COMMON_H_
#define __I2C_VIAI2C_COMMON_H_

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

/* REG_CR Bit fields */
#define WMTI2C_REG_CR		0x00
#define WMTI2C_CR_TX_NEXT_ACK		0x0000
#define WMTI2C_CR_ENABLE		0x0001
#define WMTI2C_CR_TX_NEXT_NO_ACK	0x0002
#define WMTI2C_CR_TX_END		0x0004
#define WMTI2C_CR_CPU_RDY		0x0008

/* REG_TCR Bit fields */
#define WMTI2C_REG_TCR		0x02
#define WMTI2C_TCR_STANDARD_MODE	0x0000
#define WMTI2C_TCR_MASTER_WRITE		0x0000
#define WMTI2C_TCR_HS_MODE		0x2000
#define WMTI2C_TCR_MASTER_READ		0x4000
#define WMTI2C_TCR_FAST_MODE		0x8000
#define WMTI2C_TCR_SLAVE_ADDR_MASK	0x007F

/* REG_CSR Bit fields */
#define WMTI2C_REG_CSR		0x04
#define WMTI2C_CSR_RCV_NOT_ACK		0x0001
#define WMTI2C_CSR_RCV_ACK_MASK		0x0001
#define WMTI2C_CSR_READY_MASK		0x0002

/* REG_ISR Bit fields */
#define WMTI2C_REG_ISR		0x06
#define WMTI2C_ISR_NACK_ADDR		0x0001
#define WMTI2C_ISR_BYTE_END		0x0002
#define WMTI2C_ISR_SCL_TIMEOUT		0x0004
#define WMTI2C_ISR_WRITE_ALL		0x0007

/* REG_IMR Bit fields */
#define WMTI2C_REG_IMR		0x08
#define WMTI2C_IMR_ENABLE_ALL		0x0007

#define WMTI2C_REG_CDR		0x0A
#define WMTI2C_REG_TR		0x0C
#define WMTI2C_REG_MCR		0x0E

struct wmt_i2c {
	struct i2c_adapter	adapter;
	struct completion	complete;
	struct device		*dev;
	void __iomem		*base;
	struct clk		*clk;
	u16			tcr;
	int			irq;
	u16			cmd_status;
};

int wmt_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num);
int wmt_i2c_init(struct platform_device *pdev, struct wmt_i2c **pi2c);

#endif
