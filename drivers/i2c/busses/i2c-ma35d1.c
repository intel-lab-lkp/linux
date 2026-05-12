// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Nuvoton technology corporation.
 *
 * Author: Zi-Yu Chen <zychennvt@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

/* MA35D1 I2C registers offset */
#define MA35_CTL0		0x00
#define MA35_ADDR0		0x04
#define MA35_DAT		0x08
#define MA35_STATUS0	0x0c
#define MA35_CLKDIV		0x10
#define MA35_TOCTL		0x14
#define MA35_ADDR1		0x18
#define MA35_ADDR2		0x1c
#define MA35_ADDR3		0x20
#define MA35_ADDRMSK0	0x24
#define MA35_ADDRMSK1	0x28
#define MA35_ADDRMSK2	0x2c
#define MA35_ADDRMSK3	0x30
#define MA35_WKCTL		0x3c
#define MA35_WKSTS		0x40
#define MA35_CTL1		0x44
#define MA35_STATUS1	0x48
#define MA35_TMCTL		0x4c
#define MA35_BUSCTL		0x50
#define MA35_BUSTCTL	0x54
#define MA35_BUSSTS		0x58
#define MA35_PKTSIZE	0x5c
#define MA35_PKTCRC		0x60
#define MA35_BUSTOUT	0x64
#define MA35_CLKTOUT	0x68
#define MA35_AUTOCNT	0x78

/* MA35D1 I2C Status */
/* Controller */
#define MA35_M_START				0x08	/* Start */
#define MA35_M_REPEAT_START			0x10	/* Controller Repeat Start */
#define MA35_M_TRAN_ADDR_ACK		0x18	/* Controller Transmit Address ACK */
#define MA35_M_TRAN_ADDR_NACK		0x20	/* Controller Transmit Address NACK */
#define MA35_M_TRAN_DATA_ACK		0x28	/* Controller Transmit Data ACK */
#define MA35_M_TRAN_DATA_NACK		0x30	/* Controller Transmit Data NACK */
#define MA35_M_ARB_LOST				0x38	/* Controller Arbitration Lost */
#define MA35_M_RECE_ADDR_ACK		0x40	/* Controller Receive Address ACK */
#define MA35_M_RECE_ADDR_NACK		0x48	/* Controller Receive Address NACK */
#define MA35_M_RECE_DATA_ACK		0x50	/* Controller Receive Data ACK */
#define MA35_M_RECE_DATA_NACK		0x58	/* Controller Receive Data NACK */
#define MA35_BUS_ERROR				0x00	/* Bus error */

/*  Target */
#define MA35_S_REPEAT_START_STOP	0xa0	/* Target Transmit Repeat Start or Stop */
#define MA35_S_TRAN_ADDR_ACK		0xa8	/* Target Transmit Address ACK */
#define MA35_S_TRAN_DATA_ACK		0xb8	/* Target Transmit Data ACK */
#define MA35_S_TRAN_DATA_NACK		0xc0	/* Target Transmit Data NACK */
#define MA35_S_TRAN_LAST_DATA_ACK	0xc8	/* Target Transmit Last Data ACK */
#define MA35_S_RECE_ADDR_ACK		0x60	/* Target Receive Address ACK */
#define MA35_S_RECE_ARB_LOST		0x68	/* Target Receive Arbitration Lost */
#define MA35_S_RECE_DATA_ACK		0x80	/* Target Receive Data ACK */
#define MA35_S_RECE_DATA_NACK		0x88	/* Target Receive Data NACK */

/* GC Mode */
#define MA35_GC_ADDR_ACK			0x70	/* GC mode Address ACK */
#define MA35_GC_ARB_LOST			0x78	/* GC mode Arbitration Lost */
#define MA35_GC_DATA_ACK			0x90	/* GC mode Data ACK */
#define MA35_GC_DATA_NACK			0x98	/* GC mode Data NACK */

/* Other */
#define MA35_ADDR_TRAN_ARB_LOST		0xb0	/* Address Transmit Arbitration Lost */
#define MA35_BUS_RELEASED			0xf8	/* Bus Released */

/*  I2C_CTL constant definitions. */
#define MA35_CTL_AA			BIT(2)
#define MA35_CTL_SI			BIT(3)
#define MA35_CTL_STO		BIT(4)
#define MA35_CTL_STA		BIT(5)
#define MA35_CTL_I2CEN		BIT(6)
#define MA35_CTL_INTEN		BIT(7)
#define MA35_CTL_SI_AA		(MA35_CTL_SI | MA35_CTL_AA)
#define MA35_CTL_STO_SI		(MA35_CTL_STO | MA35_CTL_SI)
#define MA35_CTL_STA_SI		(MA35_CTL_STA | MA35_CTL_SI)
#define MA35_CTL_STA_SI_AA	(MA35_CTL_STA | MA35_CTL_SI | MA35_CTL_AA)
#define MA35_CTL_STO_SI_AA	(MA35_CTL_STO | MA35_CTL_SI | MA35_CTL_AA)

/* Constants */
#define MA35_CLKDIV_MSK		GENMASK(15, 0)
#define MA35_I2C_ADDR_MASK	(0x7f << 1)
#define I2C_PM_TIMEOUT_MS	5000
#define STOP_TIMEOUT_MS		50
#define MA35_I2C_GC_EN		1
#define MA35_I2C_GC_DIS		0

struct ma35d1_i2c {
	spinlock_t lock; /* Protects I2C register access and state */
	wait_queue_head_t wait;
	struct i2c_msg *msg;
	unsigned int msg_num;
	unsigned int msg_idx;
	unsigned int msg_ptr;
	unsigned int irq;
	unsigned int arblost;
	void __iomem *regs;
	struct clk *clk;
	struct device *dev;
	struct i2c_adapter adap;
	struct i2c_client *target;
	struct reset_control *rst;
};

static inline bool ma35d1_is_controller_status(unsigned int status)
{
	return status >= MA35_M_START && status <= MA35_M_RECE_DATA_NACK;
}

/*
 * ma35d1_i2c_write_CTL - Update the I2C control register
 * @i2c: Pointer to the ma35d1 i2c instance
 * @ctl: Control bits to set (e.g., MA35_CTL_STA, SI, AA)
 *
 * This helper reads CTL0, clears the sticky state-change bits (STA, STO, SI, AA),
 * and then applies the new control bits provided by @ctl.
 */
static void ma35d1_i2c_write_CTL(struct ma35d1_i2c *i2c, unsigned int ctl)
{
	unsigned int val;

	val = readl(i2c->regs + MA35_CTL0);
	val &= ~(MA35_CTL_STA_SI_AA | MA35_CTL_STO);
	val |= ctl;
	writel(val, i2c->regs + MA35_CTL0);
}

static void ma35d1_i2c_set_addr(struct ma35d1_i2c *i2c)
{
	unsigned int rw = i2c->msg->flags & I2C_M_RD;

	writel(((i2c->msg->addr & 0x7f) << 1) | rw, i2c->regs + MA35_DAT);
}

static void ma35d1_i2c_controller_complete(struct ma35d1_i2c *i2c, int ret)
{
	dev_dbg(i2c->dev, "controller_complete %d\n", ret);

	i2c->msg_ptr = 0;
	i2c->msg = NULL;
	i2c->msg_idx++;
	i2c->msg_num = 0;
	if (ret)
		i2c->msg_idx = ret;

	wake_up(&i2c->wait);
}

static void ma35d1_i2c_disable_irq(struct ma35d1_i2c *i2c)
{
	unsigned long tmp;

	tmp = readl(i2c->regs + MA35_CTL0);
	writel(tmp & ~MA35_CTL_INTEN, i2c->regs + MA35_CTL0);
}

static void ma35d1_i2c_enable_irq(struct ma35d1_i2c *i2c)
{
	unsigned long tmp;

	tmp = readl(i2c->regs + MA35_CTL0);
	writel(tmp | MA35_CTL_INTEN, i2c->regs + MA35_CTL0);
}

static void ma35d1_i2c_reset(struct ma35d1_i2c *i2c)
{
	unsigned int tmp;

	tmp = readl(i2c->regs + MA35_CLKDIV);

	reset_control_assert(i2c->rst);
	usleep_range(10, 20);
	reset_control_deassert(i2c->rst);

	writel(tmp, (i2c->regs + MA35_CLKDIV));
	ma35d1_i2c_write_CTL(i2c, MA35_CTL_I2CEN);

	if (i2c->target)
		ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI_AA);
}

static void ma35d1_i2c_stop(struct ma35d1_i2c *i2c, int ret)
{
	unsigned int val;
	int err;

	/* Ensure AA is cleared to prevent the controller
	 * from re-claiming the bus unnecessarily
	 */
	if (readl(i2c->regs + MA35_CTL0) & MA35_CTL_AA) {
		val = readl(i2c->regs + MA35_CTL0);
		val &= ~MA35_CTL_AA;
		writel(val, (i2c->regs + MA35_CTL0));

		err = readl_poll_timeout_atomic(i2c->regs + MA35_CTL0, val,
						!(val & MA35_CTL_AA), 1, 1000);
		if (err)
			dev_warn(i2c->dev,
				 "AA bit could not be cleared in time\n");
	}

	ma35d1_i2c_write_CTL(i2c, MA35_CTL_STO_SI);

	err = readl_poll_timeout_atomic(i2c->regs + MA35_CTL0, val,
					!(val & MA35_CTL_STO), 1, 1 * 1000);
	if (err)
		dev_warn(i2c->dev, "I2C Stop Timeout\n");

	if (i2c->target)
		ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI_AA);
	else
		ma35d1_i2c_disable_irq(i2c);

	ma35d1_i2c_controller_complete(i2c, ret);
}

/* Check if this is the last message in the set */
static inline bool is_last_msg(struct ma35d1_i2c *i2c)
{
	return i2c->msg_idx >= (i2c->msg_num - 1);
}

/* Check if this is the last byte in the current message */
static inline bool is_last_byte(struct ma35d1_i2c *i2c)
{
	return i2c->msg_ptr == i2c->msg->len - 1;
}

/* Check if reached the end of the current message */
static inline bool is_msgend(struct ma35d1_i2c *i2c)
{
	return i2c->msg_ptr >= i2c->msg->len;
}

/*
 * i2c_ma35d1_irq_target_trx - I2C Target state machine handler
 * @i2c: ma35d1 i2c instance
 * @i2c_status: hardware status code from MA35_STATUS0
 */
static void i2c_ma35d1_irq_target_trx(struct ma35d1_i2c *i2c,
				      unsigned long i2c_status)
{
	unsigned char byte;

	switch (i2c_status) {
	case MA35_S_RECE_ADDR_ACK:
		/* Own SLA+W has been receive; ACK has been return */
		i2c_slave_event(i2c->target, I2C_SLAVE_WRITE_REQUESTED, &byte);
		break;
	case MA35_S_TRAN_DATA_NACK:
	/* Data byte or last data in I2CDAT has been transmitted.
	 * Not ACK has been received
	 */
	case MA35_S_RECE_DATA_NACK:
	/* Previously addressed with own SLA address;
	 * NOT ACK has been returned
	 */
		break;

	case MA35_S_RECE_DATA_ACK:
		/* Previously address with own SLA address Data has been received;
		 * ACK has been returned
		 */
		byte = readb(i2c->regs + MA35_DAT);
		i2c_slave_event(i2c->target, I2C_SLAVE_WRITE_RECEIVED, &byte);
		break;

	case MA35_S_TRAN_ADDR_ACK:
		/* Own SLA+R has been receive; ACK has been return */
		i2c_slave_event(i2c->target, I2C_SLAVE_READ_REQUESTED, &byte);

		writel(byte, i2c->regs + MA35_DAT);
		break;

	case MA35_S_TRAN_DATA_ACK:
		i2c_slave_event(i2c->target, I2C_SLAVE_READ_PROCESSED, &byte);
		writel(byte, i2c->regs + MA35_DAT);
		break;

	case MA35_S_REPEAT_START_STOP:
	/* A STOP or repeated START has been received
	 *  while still addressed as Target/Receiver
	 */
		i2c_slave_event(i2c->target, I2C_SLAVE_STOP, &byte);
		break;

	default:
		dev_err(i2c->dev, "Status 0x%02lx is NOT processed\n",
			i2c_status);
		break;
	}
	ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI_AA);
}

/*
 * i2c_ma35d1_irq_controller_trx - I2C Controller state machine handler
 * @i2c: ma35d1 i2c instance
 * @i2c_status: hardware status code from MA35_STATUS0
 */
static void i2c_ma35d1_irq_controller_trx(struct ma35d1_i2c *i2c,
					  unsigned long i2c_status)
{
	unsigned char byte;

	switch (i2c_status) {
	case MA35_M_START:
	case MA35_M_REPEAT_START:
		ma35d1_i2c_set_addr(i2c);
		ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI);
		break;

	case MA35_M_TRAN_ADDR_ACK:
	case MA35_M_TRAN_DATA_ACK:
		/* SLA+W has been transmitted and ACK has been received */
		if (i2c_status == MA35_M_TRAN_ADDR_ACK) {
			if (is_last_msg(i2c) && i2c->msg->len == 0) {
				ma35d1_i2c_stop(i2c, 0);
				return;
			}
		}

		if (!is_msgend(i2c)) {
			byte = i2c->msg->buf[i2c->msg_ptr++];
			writel(byte, i2c->regs + MA35_DAT);
			ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI);
		} else if (!is_last_msg(i2c)) {
			dev_dbg(i2c->dev, "WRITE: Next Message\n");

			i2c->msg_ptr = 0;
			i2c->msg_idx++;
			i2c->msg++;

			ma35d1_i2c_write_CTL(i2c, MA35_CTL_STA | MA35_CTL_SI);
		} else {
			ma35d1_i2c_stop(i2c, 0);
		}
		break;

	case MA35_M_TRAN_DATA_NACK:
		ma35d1_i2c_stop(i2c, 0);
		break;

	case MA35_M_TRAN_ADDR_NACK:
	case MA35_M_RECE_ADDR_NACK:
		/* Controller Transmit Address NACK */
		/* 0x20: SLA+W has been transmitted and NACK has been received */
		/* 0x48: SLA+R has been transmitted and NACK has been received */
		if (!(i2c->msg->flags & I2C_M_IGNORE_NAK)) {
			dev_dbg(i2c->dev, "\n i2c: ack was not received\n");
			ma35d1_i2c_stop(i2c, -ENXIO);
		}
		break;

	case MA35_M_RECE_ADDR_ACK:
		if (is_last_msg(i2c) && i2c->msg->len == 0)
			ma35d1_i2c_stop(i2c, 0);
		else if (is_last_msg(i2c) && (i2c->msg->len == 1))
			ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI);
		else
			ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI_AA);
		break;

	case MA35_M_RECE_DATA_ACK:
	case MA35_M_RECE_DATA_NACK:
		/* DATA has been transmitted and ACK has been received */
		byte = readb(i2c->regs + MA35_DAT);
		i2c->msg->buf[i2c->msg_ptr++] = byte;

		if (is_last_byte(i2c)) {
			ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI);
		} else if (is_msgend(i2c)) {
			if (is_last_msg(i2c)) {
				dev_dbg(i2c->dev, "READ: Send Stop\n");

				ma35d1_i2c_stop(i2c, 0);
			} else {
				dev_dbg(i2c->dev, "READ: Next Transfer\n");

				i2c->msg_ptr = 0;
				i2c->msg_idx++;
				i2c->msg++;

				ma35d1_i2c_write_CTL(i2c, MA35_CTL_STA_SI);
			}
		} else {
			ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI_AA);
		}
		break;

	default:
		dev_err(i2c->dev, "Status 0x%02lx is NOT processed\n",
			i2c_status);
		ma35d1_i2c_disable_irq(i2c);
		ma35d1_i2c_stop(i2c, 0);
		break;
	}
}

static irqreturn_t ma35d1_i2c_irq(int irqno, void *dev_id)
{
	struct ma35d1_i2c *i2c = dev_id;
	unsigned long status, flags;

	status = readl(i2c->regs + MA35_STATUS0);

	spin_lock_irqsave(&i2c->lock, flags);

	if (status == MA35_M_ARB_LOST) {
		dev_err(i2c->dev, "Arbitration lost\n");
		i2c->arblost = 1;
		ma35d1_i2c_disable_irq(i2c);
		ma35d1_i2c_stop(i2c, -EAGAIN);
		goto out;
	}

	else if (status == MA35_BUS_ERROR) {
		dev_err(i2c->dev, "Bus error during transfer\n");
		ma35d1_i2c_disable_irq(i2c);
		ma35d1_i2c_stop(i2c, 0);
		goto out;
	}

	if (ma35d1_is_controller_status(status))
		i2c_ma35d1_irq_controller_trx(i2c, status);
	else
		i2c_ma35d1_irq_target_trx(i2c, status);

out:
	spin_unlock_irqrestore(&i2c->lock, flags);
	return IRQ_HANDLED;
}

static int ma35d1_i2c_doxfer(struct ma35d1_i2c *i2c, struct i2c_msg *msgs,
			     int num)
{
	unsigned long timeout;
	unsigned int val;
	int ret, err;

	spin_lock_irq(&i2c->lock);

	ma35d1_i2c_enable_irq(i2c);

	i2c->msg = msgs;
	i2c->msg_num = num;
	i2c->msg_ptr = 0;
	i2c->msg_idx = 0;

	ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI | MA35_CTL_STA);
	spin_unlock_irq(&i2c->lock);

	timeout = wait_event_timeout(i2c->wait, i2c->msg_num == 0, HZ * 5);
	ret = i2c->msg_idx;

	if (timeout == 0)
		dev_dbg(i2c->dev, "timeout\n");
	else if (ret != num)
		dev_dbg(i2c->dev, "incomplete xfer (%d)\n", ret);

	err = readl_poll_timeout(i2c->regs + MA35_CTL0, val,
				 !(val & MA35_CTL_STO), 100,
				 STOP_TIMEOUT_MS * 1000);

	if (err) {
		dev_err(i2c->dev, "Bus stuck! Resetting controller...\n");
		ma35d1_i2c_reset(i2c);
	}

	if (i2c->arblost) {
		dev_dbg(i2c->dev, "arb lost, stop\n");
		i2c->arblost = 0;
	}

	return ret;
}

static int ma35d1_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num)
{
	struct ma35d1_i2c *i2c = i2c_get_adapdata(adap);
	int retry, ret;

	ret = pm_runtime_resume_and_get(i2c->dev);
	if (ret)
		return ret;

	for (retry = 0; retry < adap->retries; retry++) {
		ret = ma35d1_i2c_doxfer(i2c, msgs, num);

		if (ret != -EAGAIN)
			break;

		dev_dbg(i2c->dev, "Retrying transmission (%d)\n", retry);
		fsleep(100);
	}

	if (ret == -EAGAIN)
		ret = -EREMOTEIO;

	pm_runtime_put_autosuspend(i2c->dev);

	return ret;
}

static int ma35d1_reg_target(struct i2c_client *target)
{
	struct ma35d1_i2c *i2c = i2c_get_adapdata(target->adapter);
	unsigned int val, slvaddr;
	int ret;

	if (i2c->target)
		return -EBUSY;

	if (target->flags & I2C_CLIENT_TEN)
		return -EAFNOSUPPORT;

	ma35d1_i2c_enable_irq(i2c);

	ret = pm_runtime_resume_and_get(i2c->dev);
	if (ret) {
		dev_err(i2c->dev, "failed to resume i2c controller\n");
		return ret;
	}

	i2c->target = target;

	val = readl(i2c->regs + MA35_CTL0);
	val |= MA35_CTL_I2CEN;
	writel(val, i2c->regs + MA35_CTL0);
	slvaddr = target->addr << 1;
	writel(slvaddr, i2c->regs + MA35_ADDR0);

	/* I2C enter SLV mode */
	ma35d1_i2c_write_CTL(i2c, MA35_CTL_SI_AA);

	return 0;
}

static int ma35d1_unreg_target(struct i2c_client *target)
{
	struct ma35d1_i2c *i2c = i2c_get_adapdata(target->adapter);
	unsigned int val;
	int ret;

	/* Disable I2C */
	val = readl(i2c->regs + MA35_CTL0);
	val &= ~MA35_CTL_I2CEN;
	writel(val, i2c->regs + MA35_CTL0);

	/* Disable I2C interrupt */
	ma35d1_i2c_disable_irq(i2c);

	i2c->target = NULL;

	ret = pm_runtime_put_sync(i2c->dev);
	if (ret)
		dev_err(i2c->dev, "failed to suspend i2c controller");

	return 0;
}

/* Declare Our I2C Functionality */
static u32 ma35d1_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_PROTOCOL_MANGLING | I2C_FUNC_SMBUS_EMUL;
}

/* I2C Bus Registration Info */
static const struct i2c_algorithm ma35d1_i2c_algorithm = {
	.xfer = ma35d1_i2c_xfer,
	.functionality = ma35d1_i2c_func,
	.reg_target = ma35d1_reg_target,
	.unreg_target = ma35d1_unreg_target,
};

static int ma35d1_i2c_probe(struct platform_device *pdev)
{
	struct ma35d1_i2c *i2c;
	struct resource *res;
	int ret, clkdiv;
	unsigned int busfreq;
	struct device *dev = &pdev->dev;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	spin_lock_init(&i2c->lock);
	init_waitqueue_head(&i2c->wait);

	i2c->dev = dev;

	i2c->clk = devm_clk_get_prepared(dev, NULL);
	if (IS_ERR(i2c->clk))
		return dev_err_probe(dev, PTR_ERR(i2c->clk),
				     "failed to get core clk\n");

	i2c->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(i2c->regs))
		return PTR_ERR(i2c->regs);

	i2c->rst = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(i2c->rst))
		return dev_err_probe(dev, PTR_ERR(i2c->rst),
				     "failed to get reset control\n");

	/* Setup info block for the I2C core */
	strscpy(i2c->adap.name, "ma35d1-i2c", sizeof(i2c->adap.name));
	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &ma35d1_i2c_algorithm;
	i2c->adap.retries = 2;
	i2c->adap.algo_data = i2c;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.dev.of_node = pdev->dev.of_node;
	i2c_set_adapdata(&i2c->adap, i2c);

	/* Default to 100kHz if not specified in DT */
	busfreq = 100000;
	device_property_read_u32(dev, "clock-frequency", &busfreq);

	/* Calculate divider based on the current peripheral clock rate */
	clkdiv = DIV_ROUND_CLOSEST(clk_get_rate(i2c->clk), busfreq * 4) - 1;
	if (clkdiv < 0 || clkdiv > 0xffff)
		return dev_err_probe(dev, -EINVAL, "invalid clkdiv value: %d\n",
				     clkdiv);

	i2c->irq = platform_get_irq(pdev, 0);
	if (i2c->irq < 0)
		return dev_err_probe(dev, i2c->irq, "failed to get irq\n");

	platform_set_drvdata(pdev, i2c);

	pm_runtime_set_autosuspend_delay(dev, I2C_PM_TIMEOUT_MS);
	pm_runtime_use_autosuspend(dev);
	devm_pm_runtime_enable(dev);

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to resume device\n");

	writel(FIELD_PREP(MA35_CLKDIV_MSK, clkdiv), i2c->regs + MA35_CLKDIV);

	ret = devm_request_irq(dev, i2c->irq, ma35d1_i2c_irq, IRQF_SHARED,
			       dev_name(dev), i2c);
	if (ret) {
		dev_err_probe(dev, ret, "cannot claim IRQ %d\n", i2c->irq);
		goto rpm_put;
	}

	ret = devm_i2c_add_adapter(dev, &i2c->adap);
	if (ret) {
		dev_err_probe(dev, ret, "failed to add bus to i2c core\n");
		goto rpm_put;
	}

	pm_runtime_put_autosuspend(dev);

	return 0;

rpm_put:
	pm_runtime_put_noidle(dev);
	return ret;
}

static int ma35d1_i2c_suspend(struct device *dev)
{
	struct ma35d1_i2c *i2c = dev_get_drvdata(dev);
	unsigned int val;

	spin_lock_irq(&i2c->lock);

	/* Prepare for wake-up from I2C events if target mode is active */
	if (i2c->target) {
		val = readl(i2c->regs + MA35_CTL0);
		val |= (MA35_CTL_SI | MA35_CTL_AA);
		writel(val, i2c->regs + MA35_CTL0);
		ma35d1_i2c_enable_irq(i2c);
	}

	spin_unlock_irq(&i2c->lock);

	/* Setup wake-up control */
	writel(0x1, i2c->regs + MA35_WKCTL);

	/* Clear pending wake-up flags */
	val = readl(i2c->regs + MA35_WKSTS);
	writel(val, i2c->regs + MA35_WKSTS);

	enable_irq_wake(i2c->irq);

	return 0;
}

static int ma35d1_i2c_resume(struct device *dev)
{
	struct ma35d1_i2c *i2c = dev_get_drvdata(dev);
	unsigned int val;

	/* Disable wake-up */
	writel(0x0, i2c->regs + MA35_WKCTL);

	/* Clear pending wake-up flags */
	val = readl(i2c->regs + MA35_WKSTS);
	writel(val, i2c->regs + MA35_WKSTS);

	disable_irq_wake(i2c->irq);

	return 0;
}

static int ma35d1_i2c_runtime_suspend(struct device *dev)
{
	struct ma35d1_i2c *i2c = dev_get_drvdata(dev);
	unsigned int val;

	/* Disable I2C controller */
	val = readl(i2c->regs + MA35_CTL0);
	val &= ~MA35_CTL_I2CEN;
	writel(val, i2c->regs + MA35_CTL0);

	clk_disable(i2c->clk);

	return 0;
}

static int ma35d1_i2c_runtime_resume(struct device *dev)
{
	struct ma35d1_i2c *i2c = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = clk_enable(i2c->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock in resume\n");
		return ret;
	}

	/* Enable I2C controller */
	val = readl(i2c->regs + MA35_CTL0);
	val |= MA35_CTL_I2CEN;
	writel(val, i2c->regs + MA35_CTL0);

	return 0;
}

static const struct dev_pm_ops ma35d1_i2c_pmops = {
	SYSTEM_SLEEP_PM_OPS(ma35d1_i2c_suspend, ma35d1_i2c_resume)
		RUNTIME_PM_OPS(ma35d1_i2c_runtime_suspend,
			       ma35d1_i2c_runtime_resume, NULL)
};

static const struct of_device_id ma35d1_i2c_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-i2c" },
	{},
};
MODULE_DEVICE_TABLE(of, ma35d1_i2c_of_match);

static struct platform_driver ma35d1_i2c_driver = {
	.probe      = ma35d1_i2c_probe,
	.driver     = {
		.name   = "ma35d1-i2c",
		.of_match_table = ma35d1_i2c_of_match,
		.pm = pm_ptr(&ma35d1_i2c_pmops),
	},
};
module_platform_driver(ma35d1_i2c_driver);

MODULE_AUTHOR("Zi-Yu Chen <zychennvt@gmail.com>");
MODULE_DESCRIPTION("MA35D1 I2C Bus Driver");
MODULE_LICENSE("GPL");
