// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/delay.h>

/* Amlogic I2C register map */
#define REG_CFG_RDY		0x00
#define REG_CFG_I2C		0x04
#define REG_CFG_START		0x08
#define REG_CFG_BUS		0x0c
#define REG_TX_RD_ADDR		0x10
#define REG_TX_WR_ADDR		0x14
#define REG_RX_RD_ADDR		0x18
#define REG_RX_WR_ADDR		0x1c
#define REG_CGF_TX		0x20
#define REG_CGF_RX		0x24
#define REG_CGF_IRQ_STATE	0x30
#define REG_CGF_IRQ_ENABLE	0x34
#define REG_SHAKE_BLK_CNT	0x38

/* CFG RDY fields */
#define RDY_TEE_ONLY		BIT(1)
#define RDY_IF			BIT(0)

/* CFG I2C fields */
#define I2C_RX_THR		GENMASK(23, 16)
#define I2C_TX_THR		GENMASK(15, 8)

/* CFG BUS fields */
#define BUS_NO_STOP		BIT(18)
#define BUS_SPEED_MODE		BIT(17)
#define BUS_SLAVE_MODE		BIT(16)
#define BUS_FILTER_MASK		GENMASK(15, 12)
/* SCL = clk/b_ratio	if b_ratio<=8, SCL = clk/8 */
#define BUS_RATIO_MASK		GENMASK(11, 0)

/* CFG START fields */
#define START_START		BIT(31)
#define START_LEN		GENMASK(23, 12)
#define START_LEN_SHIFT		12
#define START_SLAVE_ADDR	GENMASK(10, 1)
#define START_READ		BIT(0)

/* CFG TX_RX fields */
#define TX_RX_EMPTY		BIT(9)
#define TX_RX_FULL		BIT(8)
#define TX_RX_DATA		GENMASK(7, 0)

/*CGF IRQ fields */
#define IRQ_ALL_MASK		0xffff
#define A9_NCK_ERROR		BIT(0)
#define A9_RX_EMPTY		BIT(1)
#define A9_RX_FULL		BIT(2)
#define A9_TX_EMPTY		BIT(3)
#define A9_TX_FULL		BIT(4)
#define A9_RX_THRESH_READ	BIT(5)
#define A9_TX_THRESH_WRITE	BIT(6)
#define A9_PHY_DONE		BIT(7)
#define A9_TASK_DONE		BIT(8)
#define A9_ALL_DONE		BIT(9)

#define A9_TRANS_DONE		(A9_PHY_DONE | A9_NCK_ERROR)
#define A9_TRANS_ERROR		(A9_NCK_ERROR)
#define A9_ENABLE_IRQ_BIT	(A9_NCK_ERROR | A9_PHY_DONE)
#define THRESH_MODE		(A9_RX_THRESH_READ | A9_TX_THRESH_WRITE)

#define A9_I2C_FIFO_DEPTH	32
#define A9_I2C_HALF_FIFO	(A9_I2C_FIFO_DEPTH >> 1)

#define I2C_TIMEOUT_MS		500

enum {
	STATE_IDLE,
	STATE_READ,
	STATE_WRITE,
};

enum fifo_fill_mode {
	FIFO_FILL_FULL,
	FIFO_FILL_HALF,
};

/**
 * struct aml_i2c - Amlogic I2C device private data
 *
 * @adap:	I2C adapter instance
 * @dev:	Pointer to device structure
 * @regs:	Base address of the device memory mapped registers
 * @clk:	Pointer to clock structure
 * @msg:	Pointer to the current I2C message
 * @state:	Current state in the driver state machine
 * @count:	Number of bytes to be sent/received in current transfer
 * @pos:	Current position in the send/receive buffer
 * @error:	Flag set when an error is received
 * @lock:	To avoid race conditions between irq handler and xfer code
 * @done:	Completion used to wait for transfer termination
 * @clk_rate:	Clock source rate of I2C module
 * @frequency:	Operating frequency of I2C bus clock
 */
struct aml_i2c {
	struct i2c_adapter	adap;
	struct device		*dev;
	void __iomem		*regs;
	struct clk		*clk;

	struct i2c_msg		*msg;
	int			state;
	int			count;
	int			pos;
	int			error;
	/* protect reg access */
	spinlock_t		lock;
	struct completion	done;
	unsigned long		clk_rate;
	unsigned int		frequency;
};

static void aml_i2c_set_mask(struct aml_i2c *i2c, int reg, u32 mask, u32 val)
{
	u32 data;

	data = readl(i2c->regs + reg);
	data &= ~mask;
	data |= val & mask;
	writel(data, i2c->regs + reg);
}

/*
 * SCL = clk/b_ratio. if b_ratio<=8, SCL = clk/8
 */
static int aml_i2c_set_clk_div(struct aml_i2c *i2c)
{
	unsigned long clk_rate = i2c->clk_rate;
	unsigned int div;

	div = DIV_ROUND_UP(clk_rate, i2c->frequency);

	/* clock divider has 12 bits */
	if (div >= (1 << 12)) {
		dev_err(i2c->dev, "requested bus frequency too low\n");
		return -EINVAL;
	} else if (div <= 8) {
		dev_err(i2c->dev, "requested bus frequency too high\n");
		return -EINVAL;
	}
	/* CFG_BUS reg:11 - 0 bits
	 */
	aml_i2c_set_mask(i2c, REG_CFG_BUS, BUS_RATIO_MASK, div);

	return 0;
}

static void reset_fifo_pos(struct aml_i2c *i2c)
{
	writel(0x00, i2c->regs + REG_TX_RD_ADDR);
	writel(0x00, i2c->regs + REG_TX_WR_ADDR);
	writel(0x00, i2c->regs + REG_RX_WR_ADDR);
	writel(0x00, i2c->regs + REG_RX_RD_ADDR);
}

static int controller_is_error(struct aml_i2c *i2c)
{
	int ret = 0;

	if (readl(i2c->regs + REG_CGF_IRQ_STATE) & A9_TRANS_ERROR)
		ret = -ENXIO;

	return ret;
}

static int controller_is_done(struct aml_i2c *i2c)
{
	return readl(i2c->regs + REG_CGF_IRQ_STATE) & A9_TRANS_DONE;
}

static void clear_irq_status(struct aml_i2c *i2c, u32 irq_bits)
{
	writel(irq_bits, i2c->regs + REG_CGF_IRQ_STATE);
}

static int aml_i2c_init(struct aml_i2c *i2c)
{
	int ret;

	clear_irq_status(i2c, IRQ_ALL_MASK);
	writel(A9_ENABLE_IRQ_BIT, i2c->regs + REG_CGF_IRQ_ENABLE);
	aml_i2c_set_mask(i2c, REG_CFG_BUS, BUS_NO_STOP, BUS_NO_STOP);
	aml_i2c_set_mask(i2c, REG_CFG_I2C, I2C_RX_THR | I2C_TX_THR,
			 A9_I2C_FIFO_DEPTH << 16 | A9_I2C_HALF_FIFO << 8);
	reset_fifo_pos(i2c);
	ret = aml_i2c_set_clk_div(i2c);

	return ret;
}

static void aml_i2c_add_stop(struct aml_i2c *i2c)
{
	aml_i2c_set_mask(i2c, REG_CFG_BUS, BUS_NO_STOP, 0);
}

static void aml_i2c_cpy_data_to_user(struct aml_i2c *i2c, u8 data)
{
	u8 *buf = i2c->msg->buf + i2c->pos;

	i2c->count = 1;
	*buf = data;
}

static void aml_i2c_put_data(struct aml_i2c *i2c, char *buf, int len)
{
	int i;

	/* this i2c module when trans 0 byte, must put at least 1.
	 */
	if (!i2c->msg->len) {
		writel(0x00, i2c->regs + REG_CGF_TX);
		return;
	}

	for (i = 0; i < len; i++, buf++)
		writel(*buf, i2c->regs + REG_CGF_TX);
}

static void aml_i2c_prepare_xfer(struct aml_i2c *i2c, enum fifo_fill_mode mode)
{
	bool write = !(i2c->msg->flags & I2C_M_RD);

	if (write) {
		if (mode == FIFO_FILL_FULL)
			i2c->count = min(i2c->msg->len - i2c->pos, A9_I2C_FIFO_DEPTH);
		else
			i2c->count = min(i2c->msg->len - i2c->pos, A9_I2C_HALF_FIFO);
		aml_i2c_put_data(i2c, i2c->msg->buf + i2c->pos, i2c->count);
	}
}

static irqreturn_t aml_i2c_irq(int irqno, void *dev_id)
{
	struct aml_i2c *i2c = dev_id;
	int ret;
	u32 rd_len, i;
	u32 rd_val;

	spin_lock(&i2c->lock);
	ret = controller_is_error(i2c);
	if (ret) {
		i2c->error = ret;
		goto complete_irq_out;
	}

	switch (i2c->state) {
	case STATE_READ:
		rd_len = min(i2c->msg->len - i2c->pos, A9_I2C_FIFO_DEPTH);
		for (i = 0; i < rd_len; i++) {
			rd_val = readl(i2c->regs + REG_CGF_RX);
			aml_i2c_cpy_data_to_user(i2c, rd_val & TX_RX_DATA);
			i2c->pos += i2c->count;
		}
		/* rx must clear thresh after read fifo */
		clear_irq_status(i2c, A9_RX_THRESH_READ);
		break;
	case STATE_WRITE:
		aml_i2c_prepare_xfer(i2c, FIFO_FILL_HALF);
		if ((i2c->pos + A9_I2C_HALF_FIFO) >= i2c->msg->len)
			aml_i2c_set_mask(i2c, REG_CGF_IRQ_ENABLE, A9_TX_THRESH_WRITE, 0);
		/* tx must clear thresh after write fifo */
		clear_irq_status(i2c, A9_TX_THRESH_WRITE);
		i2c->pos += i2c->count;
		break;
	case STATE_IDLE:
		dev_warn(i2c->dev, "i2c irq triggered abnormally!!!, irq_state:0x%x,irq_en:0x%x\n",
			 readl(i2c->regs + REG_CGF_IRQ_STATE),
			 readl(i2c->regs + REG_CGF_IRQ_ENABLE));
		break;
	default:
		break;
	}

	if (!controller_is_done(i2c)) {
		goto irq_trans_out;
	} else {
		if (i2c->state == STATE_READ && (i2c->msg->len - i2c->pos))
			goto irq_trans_out;
	}

complete_irq_out:
	reset_fifo_pos(i2c);
	complete(&i2c->done);
	i2c->state = STATE_IDLE;
	aml_i2c_set_mask(i2c, REG_CGF_IRQ_ENABLE, A9_RX_THRESH_READ | A9_TX_THRESH_WRITE, 0);
	clear_irq_status(i2c, IRQ_ALL_MASK);
irq_trans_out:
	spin_unlock(&i2c->lock);

	return IRQ_HANDLED;
}

static void aml_i2c_do_start(struct aml_i2c *i2c, struct i2c_msg *msg)
{
	unsigned int reg_val;
	unsigned int trans_len;

	trans_len = msg->len;
	reg_val = (unsigned int)((msg->addr << 1) |
				((msg->flags & I2C_M_RD) ? START_READ : 0) |
				((trans_len & 0xfff) << 12) | START_START);
	aml_i2c_set_mask(i2c, REG_CFG_START,
			 START_START | START_SLAVE_ADDR | START_READ | START_LEN, reg_val);
	clear_irq_status(i2c, IRQ_ALL_MASK);
	/* enable thresh mode */
	if (trans_len > A9_I2C_FIFO_DEPTH) {
		reg_val = (msg->flags & I2C_M_RD) ? A9_RX_THRESH_READ : A9_TX_THRESH_WRITE;
		aml_i2c_set_mask(i2c, REG_CGF_IRQ_ENABLE,
				 A9_RX_THRESH_READ | A9_TX_THRESH_WRITE, reg_val);
	}
}

static int aml_i2c_xfer_msg(struct aml_i2c *i2c, struct i2c_msg *msg, bool last)
{
	unsigned long time_left, flags;
	int ret = 0;

	i2c->msg = msg;
	i2c->pos = 0;
	i2c->count = 0;
	i2c->error = 0;

	i2c->state = (msg->flags & I2C_M_RD) ? STATE_READ : STATE_WRITE;
	aml_i2c_prepare_xfer(i2c, FIFO_FILL_FULL);
	i2c->pos += i2c->count;
	reinit_completion(&i2c->done);

	if (last)
		aml_i2c_add_stop(i2c);
	/* Start the transfer */
	aml_i2c_do_start(i2c, msg);

	time_left = msecs_to_jiffies(I2C_TIMEOUT_MS);
	time_left = wait_for_completion_timeout(&i2c->done, time_left);

	/*
	 * Protect access to i2c struct and registers from interrupt
	 * handlers triggered by a transfer terminated after the
	 * timeout period
	 */
	spin_lock_irqsave(&i2c->lock, flags);

	if (!time_left) {
		i2c->state = STATE_IDLE;
		clear_irq_status(i2c, IRQ_ALL_MASK);
		reset_fifo_pos(i2c);
		ret = -ETIMEDOUT;
	}

	if (i2c->error)
		ret = i2c->error;

	spin_unlock_irqrestore(&i2c->lock, flags);

	return ret;
}

static int aml_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct aml_i2c *i2c = adap->algo_data;
	int i, ret = 0;

	for (i = 0; i < num; i++) {
		ret = aml_i2c_xfer_msg(i2c, msgs + i, i == num - 1);
		if (ret)
			break;
	}

	return ret ?: i;
}

static u32 aml_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm aml_i2c_algorithm = {
	.master_xfer	= aml_i2c_xfer,
	.functionality	= aml_i2c_func,
};

static int aml_i2c_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct aml_i2c *i2c;
	struct i2c_timings timings;
	int irq, ret;

	i2c = devm_kzalloc(&pdev->dev, sizeof(struct aml_i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c_parse_fw_timings(&pdev->dev, &timings, true);
	i2c->frequency = timings.bus_freq_hz;

	i2c->dev = &pdev->dev;
	platform_set_drvdata(pdev, i2c);

	spin_lock_init(&i2c->lock);
	init_completion(&i2c->done);

	i2c->regs  = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c->regs))
		return PTR_ERR(i2c->regs);

	i2c->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(i2c->clk)) {
		dev_err(&pdev->dev, "can't get device clock\n");
		return PTR_ERR(i2c->clk);
	}

	i2c->clk_rate = clk_get_rate(i2c->clk);
	if (i2c->clk_rate == 0) {
		dev_err(&pdev->dev, "failed to get clk rate\n");
		return -EINVAL;
	}

	ret = aml_i2c_init(i2c);
	if (ret < 0) {
		dev_err(&pdev->dev, "fail init i2c\n");
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "can't find IRQ\n");
		return irq;
	}

	ret = devm_request_irq(&pdev->dev, irq, aml_i2c_irq, 0, dev_name(&pdev->dev), i2c);
	if (ret < 0) {
		dev_err(&pdev->dev, "can't request IRQ\n");
		return ret;
	}

	ret = clk_prepare_enable(i2c->clk);
	if (ret < 0) {
		dev_err(&pdev->dev, "can't prepare clock\n");
		return ret;
	}

	strscpy(i2c->adap.name, "Amlogic I2C adapter",
		sizeof(i2c->adap.name));
	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &aml_i2c_algorithm;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.dev.of_node = np;
	i2c->adap.algo_data = i2c;

	ret = i2c_add_adapter(&i2c->adap);
	if (ret < 0) {
		clk_disable_unprepare(i2c->clk);
		return ret;
	}

	return 0;
}

static void aml_i2c_remove(struct platform_device *pdev)
{
	struct aml_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
	clk_disable_unprepare(i2c->clk);
}

static const struct of_device_id aml_i2c_match[] = {
	{	.compatible = "amlogic,a9-i2c",},
	{ }
};

MODULE_DEVICE_TABLE(of, aml_i2c_match);

static struct platform_driver aml_i2c_driver = {
	.probe   = aml_i2c_probe,
	.remove  = aml_i2c_remove,
	.driver  = {
		.name  = "amlogic-i2c",
		.of_match_table = aml_i2c_match,
	},
};

module_platform_driver(aml_i2c_driver);

MODULE_DESCRIPTION("Amlogic I2C Bus driver");
MODULE_AUTHOR("Junyi Zhao <junyi.zhao@amlogic.com>");
MODULE_LICENSE("GPL");
