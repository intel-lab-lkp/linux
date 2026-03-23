// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx XADC platform driver
 *
 * Copyright 2013-2014 Analog Devices Inc.
 *  Author: Lars-Peter Clausen <lars@metafoo.de>
 *
 * Documentation for the parts can be found at:
 *  - XADC hardmacro: Xilinx UG480
 *  - ZYNQ XADC interface: Xilinx UG585
 *  - AXI XADC interface: Xilinx PG019
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>

#include "xilinx-xadc.h"

static const unsigned int XADC_ZYNQ_UNMASK_TIMEOUT = 500;

/* ZYNQ register definitions */
#define XADC_ZYNQ_REG_CFG	0x00
#define XADC_ZYNQ_REG_INTSTS	0x04
#define XADC_ZYNQ_REG_INTMSK	0x08
#define XADC_ZYNQ_REG_STATUS	0x0c
#define XADC_ZYNQ_REG_CFIFO	0x10
#define XADC_ZYNQ_REG_DFIFO	0x14
#define XADC_ZYNQ_REG_CTL		0x18

#define XADC_ZYNQ_CFG_ENABLE		BIT(31)
#define XADC_ZYNQ_CFG_CFIFOTH_MASK	(0xf << 20)
#define XADC_ZYNQ_CFG_CFIFOTH_OFFSET	20
#define XADC_ZYNQ_CFG_DFIFOTH_MASK	(0xf << 16)
#define XADC_ZYNQ_CFG_DFIFOTH_OFFSET	16
#define XADC_ZYNQ_CFG_WEDGE		BIT(13)
#define XADC_ZYNQ_CFG_REDGE		BIT(12)
#define XADC_ZYNQ_CFG_TCKRATE_MASK	(0x3 << 8)
#define XADC_ZYNQ_CFG_TCKRATE_DIV2	(0x0 << 8)
#define XADC_ZYNQ_CFG_TCKRATE_DIV4	(0x1 << 8)
#define XADC_ZYNQ_CFG_TCKRATE_DIV8	(0x2 << 8)
#define XADC_ZYNQ_CFG_TCKRATE_DIV16	(0x3 << 8)
#define XADC_ZYNQ_CFG_IGAP_MASK		0x1f
#define XADC_ZYNQ_CFG_IGAP(x)		(x)

#define XADC_ZYNQ_INT_CFIFO_LTH		BIT(9)
#define XADC_ZYNQ_INT_DFIFO_GTH		BIT(8)
#define XADC_ZYNQ_INT_ALARM_MASK	0xff
#define XADC_ZYNQ_INT_ALARM_OFFSET	0

#define XADC_ZYNQ_STATUS_CFIFO_LVL_MASK	(0xf << 16)
#define XADC_ZYNQ_STATUS_CFIFO_LVL_OFFSET	16
#define XADC_ZYNQ_STATUS_DFIFO_LVL_MASK	(0xf << 12)
#define XADC_ZYNQ_STATUS_DFIFO_LVL_OFFSET	12
#define XADC_ZYNQ_STATUS_CFIFOF		BIT(11)
#define XADC_ZYNQ_STATUS_CFIFOE		BIT(10)
#define XADC_ZYNQ_STATUS_DFIFOF		BIT(9)
#define XADC_ZYNQ_STATUS_DFIFOE		BIT(8)
#define XADC_ZYNQ_STATUS_OT		BIT(7)
#define XADC_ZYNQ_STATUS_ALM(x)		BIT(x)

#define XADC_ZYNQ_CTL_RESET		BIT(4)

#define XADC_ZYNQ_CMD_NOP		0x00
#define XADC_ZYNQ_CMD_READ		0x01
#define XADC_ZYNQ_CMD_WRITE		0x02

#define XADC_ZYNQ_CMD(cmd, addr, data) (((cmd) << 26) | ((addr) << 16) | (data))

/* AXI register definitions */
#define XADC_AXI_REG_RESET		0x00
#define XADC_AXI_REG_STATUS		0x04
#define XADC_AXI_REG_ALARM_STATUS	0x08
#define XADC_AXI_REG_CONVST		0x0c
#define XADC_AXI_REG_XADC_RESET		0x10
#define XADC_AXI_REG_GIER		0x5c
#define XADC_AXI_REG_IPISR		0x60
#define XADC_AXI_REG_IPIER		0x68

/* 7 Series */
#define XADC_7S_AXI_ADC_REG_OFFSET	0x200
/* UltraScale */
#define XADC_US_AXI_ADC_REG_OFFSET	0x400
#define XADC_AXI_RESET_MAGIC		0xa
#define XADC_AXI_GIER_ENABLE		BIT(31)
#define XADC_AXI_INT_EOS		BIT(4)
#define XADC_AXI_INT_ALARM_MASK		0x3c0f

/*
 * The ZYNQ interface uses two asynchronous FIFOs for communication with the
 * XADC. Reads and writes to the XADC register are performed by submitting a
 * request to the command FIFO (CFIFO), once the request has been completed the
 * result can be read from the data FIFO (DFIFO). The method currently used in
 * this driver is to submit the request for a read/write operation, then go to
 * sleep and wait for an interrupt that signals that a response is available in
 * the data FIFO.
 */
static void xadc_zynq_write_fifo(struct xadc *xadc, uint32_t *cmd, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		xadc_write_reg(xadc, XADC_ZYNQ_REG_CFIFO, cmd[i]);
}

static void xadc_zynq_drain_fifo(struct xadc *xadc)
{
	u32 status, tmp;

	xadc_read_reg(xadc, XADC_ZYNQ_REG_STATUS, &status);

	while (!(status & XADC_ZYNQ_STATUS_DFIFOE)) {
		xadc_read_reg(xadc, XADC_ZYNQ_REG_DFIFO, &tmp);
		xadc_read_reg(xadc, XADC_ZYNQ_REG_STATUS, &status);
	}
}

static void xadc_zynq_update_intmsk(struct xadc *xadc, unsigned int mask, unsigned int val)
{
	xadc->zynq_intmask &= ~mask;
	xadc->zynq_intmask |= val;

	xadc_write_reg(xadc, XADC_ZYNQ_REG_INTMSK, xadc->zynq_intmask | xadc->zynq_masked_alarm);
}

static int xadc_zynq_write_adc_reg(struct xadc *xadc, unsigned int reg, uint16_t val)
{
	u32 cmd[1], tmp;
	int ret;

	spin_lock_irq(&xadc->lock);
	xadc_zynq_update_intmsk(xadc, XADC_ZYNQ_INT_DFIFO_GTH, XADC_ZYNQ_INT_DFIFO_GTH);

	reinit_completion(&xadc->completion);

	cmd[0] = XADC_ZYNQ_CMD(XADC_ZYNQ_CMD_WRITE, reg, val);
	xadc_zynq_write_fifo(xadc, cmd, ARRAY_SIZE(cmd));
	xadc_read_reg(xadc, XADC_ZYNQ_REG_CFG, &tmp);
	tmp &= ~XADC_ZYNQ_CFG_DFIFOTH_MASK;
	tmp |= 0 << XADC_ZYNQ_CFG_DFIFOTH_OFFSET;
	xadc_write_reg(xadc, XADC_ZYNQ_REG_CFG, tmp);

	xadc_zynq_update_intmsk(xadc, XADC_ZYNQ_INT_DFIFO_GTH, 0);
	spin_unlock_irq(&xadc->lock);

	ret = wait_for_completion_interruptible_timeout(&xadc->completion, HZ);
	if (ret == 0)
		ret = -EIO;
	else
		ret = 0;

	xadc_read_reg(xadc, XADC_ZYNQ_REG_DFIFO, &tmp);

	return ret;
}

static int xadc_zynq_read_adc_reg(struct xadc *xadc, unsigned int reg, uint16_t *val)
{
	u32 cmd[2], resp, tmp;
	int ret;

	cmd[0] = XADC_ZYNQ_CMD(XADC_ZYNQ_CMD_READ, reg, 0);
	cmd[1] = XADC_ZYNQ_CMD(XADC_ZYNQ_CMD_NOP, 0, 0);

	spin_lock_irq(&xadc->lock);
	xadc_zynq_update_intmsk(xadc, XADC_ZYNQ_INT_DFIFO_GTH, XADC_ZYNQ_INT_DFIFO_GTH);
	xadc_zynq_drain_fifo(xadc);
	reinit_completion(&xadc->completion);

	xadc_zynq_write_fifo(xadc, cmd, ARRAY_SIZE(cmd));
	xadc_read_reg(xadc, XADC_ZYNQ_REG_CFG, &tmp);
	tmp &= ~XADC_ZYNQ_CFG_DFIFOTH_MASK;
	tmp |= 1 << XADC_ZYNQ_CFG_DFIFOTH_OFFSET;
	xadc_write_reg(xadc, XADC_ZYNQ_REG_CFG, tmp);

	xadc_zynq_update_intmsk(xadc, XADC_ZYNQ_INT_DFIFO_GTH, 0);
	spin_unlock_irq(&xadc->lock);
	ret = wait_for_completion_interruptible_timeout(&xadc->completion, HZ);
	if (ret == 0)
		ret = -EIO;
	if (ret < 0)
		return ret;

	xadc_read_reg(xadc, XADC_ZYNQ_REG_DFIFO, &resp);
	xadc_read_reg(xadc, XADC_ZYNQ_REG_DFIFO, &resp);

	*val = resp & 0xffff;

	return 0;
}

static unsigned int xadc_zynq_transform_alarm(unsigned int alarm)
{
	return ((alarm & 0x80) >> 4) |
		((alarm & 0x78) << 1) |
		(alarm & 0x07);
}

/*
 * The ZYNQ threshold interrupts are level sensitive. Since we can't make the
 * threshold condition go way from within the interrupt handler, this means as
 * soon as a threshold condition is present we would enter the interrupt handler
 * again and again. To work around this we mask all active thresholds interrupts
 * in the interrupt handler and start a timer. In this timer we poll the
 * interrupt status and only if the interrupt is inactive we unmask it again.
 */
static void xadc_zynq_unmask_worker(struct work_struct *work)
{
	struct xadc *xadc = container_of(work, struct xadc, zynq_unmask_work.work);
	unsigned int misc_sts, unmask;

	xadc_read_reg(xadc, XADC_ZYNQ_REG_STATUS, &misc_sts);

	misc_sts &= XADC_ZYNQ_INT_ALARM_MASK;

	spin_lock_irq(&xadc->lock);

	/* Clear those bits which are not active anymore */
	unmask = (xadc->zynq_masked_alarm ^ misc_sts) & xadc->zynq_masked_alarm;
	xadc->zynq_masked_alarm &= misc_sts;

	/* Also clear those which are masked out anyway */
	xadc->zynq_masked_alarm &= ~xadc->zynq_intmask;

	/* Clear the interrupts before we unmask them */
	xadc_write_reg(xadc, XADC_ZYNQ_REG_INTSTS, unmask);

	xadc_zynq_update_intmsk(xadc, 0, 0);

	spin_unlock_irq(&xadc->lock);

	/* if still pending some alarm re-trigger the timer */
	if (xadc->zynq_masked_alarm)
		schedule_delayed_work(&xadc->zynq_unmask_work,
				      msecs_to_jiffies(XADC_ZYNQ_UNMASK_TIMEOUT));
}

static irqreturn_t xadc_zynq_interrupt_handler(int irq, void *devid)
{
	struct iio_dev *indio_dev = devid;
	struct xadc *xadc = iio_priv(indio_dev);
	u32 status;

	xadc_read_reg(xadc, XADC_ZYNQ_REG_INTSTS, &status);

	status &= ~(xadc->zynq_intmask | xadc->zynq_masked_alarm);

	if (!status)
		return IRQ_NONE;

	spin_lock(&xadc->lock);

	xadc_write_reg(xadc, XADC_ZYNQ_REG_INTSTS, status);

	if (status & XADC_ZYNQ_INT_DFIFO_GTH) {
		xadc_zynq_update_intmsk(xadc, XADC_ZYNQ_INT_DFIFO_GTH,
					XADC_ZYNQ_INT_DFIFO_GTH);
		complete(&xadc->completion);
	}

	status &= XADC_ZYNQ_INT_ALARM_MASK;
	if (status) {
		xadc->zynq_masked_alarm |= status;
		/*
		 * mask the current event interrupt,
		 * unmask it when the interrupt is no more active.
		 */
		xadc_zynq_update_intmsk(xadc, 0, 0);

		xadc_handle_events(indio_dev, xadc_zynq_transform_alarm(status));

		/* unmask the required interrupts in timer. */
		schedule_delayed_work(&xadc->zynq_unmask_work,
				      msecs_to_jiffies(XADC_ZYNQ_UNMASK_TIMEOUT));
	}
	spin_unlock(&xadc->lock);

	return IRQ_HANDLED;
}

#define XADC_ZYNQ_TCK_RATE_MAX 50000000
#define XADC_ZYNQ_IGAP_DEFAULT 20
#define XADC_ZYNQ_PCAP_RATE_MAX 200000000

static int xadc_zynq_setup(struct platform_device *pdev, struct iio_dev *indio_dev, int irq)
{
	unsigned int tck_div, div, igap, tck_rate;
	struct xadc *xadc = iio_priv(indio_dev);
	unsigned long pcap_rate;
	int ret;

	/* TODO: Figure out how to make igap and tck_rate configurable */
	igap = XADC_ZYNQ_IGAP_DEFAULT;
	tck_rate = XADC_ZYNQ_TCK_RATE_MAX;

	xadc->zynq_intmask = ~0;

	pcap_rate = clk_get_rate(xadc->clk);
	if (!pcap_rate)
		return -EINVAL;

	if (pcap_rate > XADC_ZYNQ_PCAP_RATE_MAX) {
		ret = clk_set_rate(xadc->clk, (unsigned long)XADC_ZYNQ_PCAP_RATE_MAX);
		if (ret)
			return ret;
	}

	if (tck_rate > pcap_rate / 2) {
		div = 2;
	} else {
		div = pcap_rate / tck_rate;
		if (pcap_rate / div > XADC_ZYNQ_TCK_RATE_MAX)
			div++;
	}

	if (div <= 3)
		tck_div = XADC_ZYNQ_CFG_TCKRATE_DIV2;
	else if (div <= 7)
		tck_div = XADC_ZYNQ_CFG_TCKRATE_DIV4;
	else if (div <= 15)
		tck_div = XADC_ZYNQ_CFG_TCKRATE_DIV8;
	else
		tck_div = XADC_ZYNQ_CFG_TCKRATE_DIV16;

	xadc_write_reg(xadc, XADC_ZYNQ_REG_CTL, XADC_ZYNQ_CTL_RESET);
	xadc_write_reg(xadc, XADC_ZYNQ_REG_CTL, 0);
	xadc_write_reg(xadc, XADC_ZYNQ_REG_INTSTS, ~0);
	xadc_write_reg(xadc, XADC_ZYNQ_REG_INTMSK, xadc->zynq_intmask);
	xadc_write_reg(xadc, XADC_ZYNQ_REG_CFG, XADC_ZYNQ_CFG_ENABLE |
		       XADC_ZYNQ_CFG_REDGE | XADC_ZYNQ_CFG_WEDGE |
		       tck_div | XADC_ZYNQ_CFG_IGAP(igap));

	if (pcap_rate > XADC_ZYNQ_PCAP_RATE_MAX) {
		ret = clk_set_rate(xadc->clk, pcap_rate);
		if (ret)
			return ret;
	}

	return 0;
}

static unsigned long xadc_zynq_get_dclk_rate(struct xadc *xadc)
{
	unsigned int div;
	u32 val;

	xadc_read_reg(xadc, XADC_ZYNQ_REG_CFG, &val);

	switch (val & XADC_ZYNQ_CFG_TCKRATE_MASK) {
	case XADC_ZYNQ_CFG_TCKRATE_DIV4:
		div = 4;
		break;
	case XADC_ZYNQ_CFG_TCKRATE_DIV8:
		div = 8;
		break;
	case XADC_ZYNQ_CFG_TCKRATE_DIV16:
		div = 16;
		break;
	default:
		div = 2;
		break;
	}

	return clk_get_rate(xadc->clk) / div;
}

static void xadc_zynq_update_alarm(struct xadc *xadc, unsigned int alarm)
{
	unsigned long flags;
	u32 status;

	/* Move OT to bit 7 */
	alarm = ((alarm & 0x08) << 4) | ((alarm & 0xf0) >> 1) | (alarm & 0x07);

	spin_lock_irqsave(&xadc->lock, flags);

	/* Clear previous interrupts if any. */
	xadc_read_reg(xadc, XADC_ZYNQ_REG_INTSTS, &status);
	xadc_write_reg(xadc, XADC_ZYNQ_REG_INTSTS, status & alarm);

	xadc_zynq_update_intmsk(xadc, XADC_ZYNQ_INT_ALARM_MASK,
				~alarm & XADC_ZYNQ_INT_ALARM_MASK);

	spin_unlock_irqrestore(&xadc->lock, flags);
}

static const struct xadc_ops xadc_zynq_ops = {
	.read = xadc_zynq_read_adc_reg,
	.write = xadc_zynq_write_adc_reg,
	.setup = xadc_zynq_setup,
	.get_dclk_rate = xadc_zynq_get_dclk_rate,
	.interrupt_handler = xadc_zynq_interrupt_handler,
	.update_alarm = xadc_zynq_update_alarm,
	.type = XADC_TYPE_S7,
	/* Temp in C = (val * 503.975) / 2**bits - 273.15 */
	.temp_scale = 503975,
	.temp_offset = 273150,
};

static const unsigned int xadc_axi_reg_offsets[] = {
	[XADC_TYPE_S7] = XADC_7S_AXI_ADC_REG_OFFSET,
	[XADC_TYPE_US] = XADC_US_AXI_ADC_REG_OFFSET,
};

static int xadc_axi_read_adc_reg(struct xadc *xadc, unsigned int reg, uint16_t *val)
{
	u32 val32;

	xadc_read_reg(xadc, xadc_axi_reg_offsets[xadc->ops->type] + reg * 4, &val32);
	*val = val32 & 0xffff;

	return 0;
}

static int xadc_axi_write_adc_reg(struct xadc *xadc, unsigned int reg, u16 val)
{
	xadc_write_reg(xadc, xadc_axi_reg_offsets[xadc->ops->type] + reg * 4, val);

	return 0;
}

static int xadc_axi_setup(struct platform_device *pdev, struct iio_dev *indio_dev, int irq)
{
	struct xadc *xadc = iio_priv(indio_dev);

	xadc_write_reg(xadc, XADC_AXI_REG_RESET, XADC_AXI_RESET_MAGIC);
	xadc_write_reg(xadc, XADC_AXI_REG_GIER, XADC_AXI_GIER_ENABLE);

	return 0;
}

static irqreturn_t xadc_axi_interrupt_handler(int irq, void *devid)
{
	struct iio_dev *indio_dev = devid;
	struct xadc *xadc = iio_priv(indio_dev);
	u32 status, mask;
	unsigned int events;

	xadc_read_reg(xadc, XADC_AXI_REG_IPISR, &status);
	xadc_read_reg(xadc, XADC_AXI_REG_IPIER, &mask);
	status &= mask;

	if (!status)
		return IRQ_NONE;

	if ((status & XADC_AXI_INT_EOS) && xadc->trigger)
		iio_trigger_poll(xadc->trigger);

	if (status & XADC_AXI_INT_ALARM_MASK) {
		/*
		 * The order of the bits in the AXI-XADC status register does
		 * not match the order of the bits in the XADC alarm enable
		 * register. xadc_handle_events() expects the events to be in
		 * the same order as the XADC alarm enable register.
		 */
		events = (status & 0x000e) >> 1;
		events |= (status & 0x0001) << 3;
		events |= (status & 0x3c00) >> 6;
		xadc_handle_events(indio_dev, events);
	}

	xadc_write_reg(xadc, XADC_AXI_REG_IPISR, status);

	return IRQ_HANDLED;
}

static void xadc_axi_update_alarm(struct xadc *xadc, unsigned int alarm)
{
	u32 val;
	unsigned long flags;

	/*
	 * The order of the bits in the AXI-XADC status register does not match
	 * the order of the bits in the XADC alarm enable register. We get
	 * passed the alarm mask in the same order as in the XADC alarm enable
	 * register.
	 */
	alarm = ((alarm & 0x07) << 1) | ((alarm & 0x08) >> 3) |
			((alarm & 0xf0) << 6);

	spin_lock_irqsave(&xadc->lock, flags);
	xadc_read_reg(xadc, XADC_AXI_REG_IPIER, &val);
	val &= ~XADC_AXI_INT_ALARM_MASK;
	val |= alarm;
	xadc_write_reg(xadc, XADC_AXI_REG_IPIER, val);
	spin_unlock_irqrestore(&xadc->lock, flags);
}

static unsigned long xadc_axi_get_dclk(struct xadc *xadc)
{
	return clk_get_rate(xadc->clk);
}

static const struct xadc_ops xadc_7s_axi_ops = {
	.read = xadc_axi_read_adc_reg,
	.write = xadc_axi_write_adc_reg,
	.setup = xadc_axi_setup,
	.get_dclk_rate = xadc_axi_get_dclk,
	.update_alarm = xadc_axi_update_alarm,
	.interrupt_handler = xadc_axi_interrupt_handler,
	.flags = XADC_FLAGS_BUFFERED | XADC_FLAGS_IRQ_OPTIONAL,
	.type = XADC_TYPE_S7,
	/* Temp in C = (val * 503.975) / 2**bits - 273.15 */
	.temp_scale = 503975,
	.temp_offset = 273150,
};

static const struct xadc_ops xadc_us_axi_ops = {
	.read = xadc_axi_read_adc_reg,
	.write = xadc_axi_write_adc_reg,
	.setup = xadc_axi_setup,
	.get_dclk_rate = xadc_axi_get_dclk,
	.update_alarm = xadc_axi_update_alarm,
	.interrupt_handler = xadc_axi_interrupt_handler,
	.flags = XADC_FLAGS_BUFFERED | XADC_FLAGS_IRQ_OPTIONAL,
	.type = XADC_TYPE_US,
	/**
	 * Values below are for UltraScale+ (SYSMONE4) using internal reference.
	 * See https://docs.xilinx.com/v/u/en-US/ug580-ultrascale-sysmon
	 */
	.temp_scale = 509314,
	.temp_offset = 280231,
};

static const struct of_device_id xadc_of_match_table[] = {
	{
		.compatible = "xlnx,zynq-xadc-1.00.a",
		.data = &xadc_zynq_ops
	}, {
		.compatible = "xlnx,axi-xadc-1.00.a",
		.data = &xadc_7s_axi_ops
	}, {
		.compatible = "xlnx,system-management-wiz-1.3",
		.data = &xadc_us_axi_ops
	},
	{ }
};
MODULE_DEVICE_TABLE(of, xadc_of_match_table);

static void xadc_cancel_delayed_work(void *data)
{
	struct delayed_work *work = data;

	cancel_delayed_work_sync(work);
}

static int xadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct xadc_ops *ops;
	struct iio_dev *indio_dev;
	unsigned int bipolar_mask;
	unsigned int conf0;
	struct xadc *xadc;
	int ret;
	int irq;
	u32 i;

	indio_dev = xadc_device_setup(dev, sizeof(*xadc), &ops);
	if (IS_ERR(indio_dev))
		return PTR_ERR(indio_dev);

	irq = platform_get_irq_optional(pdev, 0);
	if (irq < 0 &&
	    (irq != -ENXIO || !(ops->flags & XADC_FLAGS_IRQ_OPTIONAL)))
		return irq;

	xadc = iio_priv(indio_dev);
	xadc->ops = ops;
	init_completion(&xadc->completion);
	mutex_init(&xadc->mutex);
	spin_lock_init(&xadc->lock);
	INIT_DELAYED_WORK(&xadc->zynq_unmask_work, xadc_zynq_unmask_worker);

	xadc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(xadc->base))
		return PTR_ERR(xadc->base);

	ret = xadc_device_configure(dev, indio_dev, irq, &conf0, &bipolar_mask);
	if (ret)
		return ret;

	ret = xadc_setup_buffer_and_triggers(dev, indio_dev, xadc, irq);
	if (ret)
		return ret;

	xadc->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(xadc->clk))
		return PTR_ERR(xadc->clk);

	/*
	 * Make sure not to exceed the maximum samplerate since otherwise the
	 * resulting interrupt storm will soft-lock the system.
	 */
	if (xadc->ops->flags & XADC_FLAGS_BUFFERED) {
		ret = xadc_read_samplerate(xadc);
		if (ret < 0)
			return ret;

		if (ret > XADC_MAX_SAMPLERATE) {
			ret = xadc_write_samplerate(xadc, XADC_MAX_SAMPLERATE);
			if (ret < 0)
				return ret;
		}
	}

	if (irq > 0) {
		ret = devm_request_irq(dev, irq, xadc->ops->interrupt_handler,
				       0, dev_name(dev), indio_dev);
		if (ret)
			return ret;

		ret = devm_add_action_or_reset(dev, xadc_cancel_delayed_work,
					       &xadc->zynq_unmask_work);
		if (ret)
			return ret;
	}

	ret = xadc->ops->setup(pdev, indio_dev, irq);
	if (ret)
		return ret;

	for (i = 0; i < 16; i++)
		xadc_read_adc_reg(xadc, XADC_REG_THRESHOLD(i), &xadc->threshold[i]);

	ret = xadc_write_adc_reg(xadc, XADC_REG_CONF0, conf0);
	if (ret)
		return ret;

	ret = xadc_write_adc_reg(xadc, XADC_REG_INPUT_MODE(0), bipolar_mask);
	if (ret)
		return ret;

	ret = xadc_write_adc_reg(xadc, XADC_REG_INPUT_MODE(1), bipolar_mask >> 16);
	if (ret)
		return ret;

	/* Go to non-buffered mode */
	xadc_postdisable(indio_dev);

	return devm_iio_device_register(dev, indio_dev);
}

static struct platform_driver xadc_driver = {
	.probe = xadc_probe,
	.driver = {
		.name = "xadc",
		.of_match_table = xadc_of_match_table,
	},
};
module_platform_driver(xadc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lars-Peter Clausen <lars@metafoo.de>");
MODULE_DESCRIPTION("Xilinx XADC platform driver");
