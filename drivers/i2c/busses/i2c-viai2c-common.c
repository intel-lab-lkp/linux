// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/of_irq.h>
#include "i2c-viai2c-common.h"

#define WMT_I2C_TIMEOUT		(msecs_to_jiffies(1000))

static int wmt_i2c_wait_bus_not_busy(struct wmt_i2c *i2c)
{
	unsigned long timeout;
	void __iomem *base = i2c->base;

	timeout = jiffies + WMT_I2C_TIMEOUT;
	while (!(readw(base + WMTI2C_REG_CSR) & WMTI2C_CSR_READY_MASK)) {
		if (time_after(jiffies, timeout)) {
			dev_warn(i2c->dev,
					"timeout waiting for bus ready\n");
			return -EBUSY;
		}
		msleep(20);
	}

	return 0;
}

static int wmt_check_status(struct wmt_i2c *i2c)
{
	int ret = 0;
	unsigned long wait_result;

	wait_result = wait_for_completion_timeout(&i2c->complete,
						msecs_to_jiffies(500));
	if (!wait_result)
		return -ETIMEDOUT;

	if (i2c->cmd_status & WMTI2C_ISR_NACK_ADDR)
		ret = -EIO;

	if (i2c->cmd_status & WMTI2C_ISR_SCL_TIMEOUT)
		ret = -ETIMEDOUT;

	return ret;
}

static irqreturn_t wmt_i2c_isr(int irq, void *data)
{
	struct wmt_i2c *i2c = data;

	/* save the status and write-clear it */
	i2c->cmd_status = readw(i2c->base + WMTI2C_REG_ISR);
	writew(i2c->cmd_status, i2c->base + WMTI2C_REG_ISR);

	complete(&i2c->complete);

	return IRQ_HANDLED;
}

static int wmt_i2c_write(struct wmt_i2c *i2c, struct i2c_msg *pmsg,
			 int last)
{
	u16 val, tcr_val = i2c->tcr;
	int ret;
	int xfer_len = 0;
	void __iomem *base = i2c->base;

	if (pmsg->len == 0) {
		/*
		 * We still need to run through the while (..) once, so
		 * start at -1 and break out early from the loop
		 */
		xfer_len = -1;
		writew(0, base + WMTI2C_REG_CDR);
	} else {
		writew(pmsg->buf[0] & 0xFF, base + WMTI2C_REG_CDR);
	}

	if (!(pmsg->flags & I2C_M_NOSTART)) {
		val = readw(base + WMTI2C_REG_CR);
		val &= ~WMTI2C_CR_TX_END;
		val |= WMTI2C_CR_CPU_RDY;
		writew(val, base + WMTI2C_REG_CR);
	}

	reinit_completion(&i2c->complete);

	tcr_val |= (WMTI2C_TCR_MASTER_WRITE
		| (pmsg->addr & WMTI2C_TCR_SLAVE_ADDR_MASK));

	writew(tcr_val, base + WMTI2C_REG_TCR);

	if (pmsg->flags & I2C_M_NOSTART) {
		val = readw(base + WMTI2C_REG_CR);
		val |= WMTI2C_CR_CPU_RDY;
		writew(val, base + WMTI2C_REG_CR);
	}

	while (xfer_len < pmsg->len) {
		ret = wmt_check_status(i2c);
		if (ret)
			return ret;

		xfer_len++;

		val = readw(base + WMTI2C_REG_CSR);
		if (val & WMTI2C_CSR_RCV_NOT_ACK) {
			dev_dbg(i2c->dev, "write RCV NACK error\n");
			return -EIO;
		}

		if (pmsg->len == 0) {
			val = WMTI2C_CR_TX_END | WMTI2C_CR_CPU_RDY
				| WMTI2C_CR_ENABLE;
			writew(val, base + WMTI2C_REG_CR);
			break;
		}

		if (xfer_len == pmsg->len) {
			if (last != 1)
				writew(WMTI2C_CR_ENABLE, base + WMTI2C_REG_CR);
		} else {
			writew(pmsg->buf[xfer_len] & 0xFF,
					base + WMTI2C_REG_CDR);
			writew(WMTI2C_CR_CPU_RDY | WMTI2C_CR_ENABLE,
					base + WMTI2C_REG_CR);
		}
	}

	return 0;
}

static int wmt_i2c_read(struct wmt_i2c *i2c, struct i2c_msg *pmsg)
{
	u16 val, tcr_val = i2c->tcr;
	int ret;
	u32 xfer_len = 0;
	void __iomem *base = i2c->base;

	val = readw(base + WMTI2C_REG_CR);
	val &= ~(WMTI2C_CR_TX_END | WMTI2C_CR_TX_NEXT_NO_ACK);

	if (!(pmsg->flags & I2C_M_NOSTART))
		val |= WMTI2C_CR_CPU_RDY;

	if (pmsg->len == 1)
		val |= WMTI2C_CR_TX_NEXT_NO_ACK;

	writew(val, base + WMTI2C_REG_CR);

	reinit_completion(&i2c->complete);

	tcr_val |= WMTI2C_TCR_MASTER_READ
		| (pmsg->addr & WMTI2C_TCR_SLAVE_ADDR_MASK);

	writew(tcr_val, base + WMTI2C_REG_TCR);

	if (pmsg->flags & I2C_M_NOSTART) {
		val = readw(base + WMTI2C_REG_CR);
		val |= WMTI2C_CR_CPU_RDY;
		writew(val, base + WMTI2C_REG_CR);
	}

	while (xfer_len < pmsg->len) {
		ret = wmt_check_status(i2c);
		if (ret)
			return ret;

		pmsg->buf[xfer_len] = readw(base + WMTI2C_REG_CDR) >> 8;
		xfer_len++;

		val = readw(base + WMTI2C_REG_CR) | WMTI2C_CR_CPU_RDY;
		if (xfer_len == pmsg->len - 1)
			val |= WMTI2C_CR_TX_NEXT_NO_ACK;
		writew(val, base + WMTI2C_REG_CR);
	}

	return 0;
}

int wmt_i2c_xfer(struct i2c_adapter *adap,
			struct i2c_msg msgs[],
			int num)
{
	struct i2c_msg *pmsg;
	int i;
	int ret = 0;
	struct wmt_i2c *i2c = i2c_get_adapdata(adap);

	for (i = 0; ret >= 0 && i < num; i++) {
		pmsg = &msgs[i];
		if (!(pmsg->flags & I2C_M_NOSTART)) {
			ret = wmt_i2c_wait_bus_not_busy(i2c);
			if (ret < 0)
				return ret;
		}

		if (pmsg->flags & I2C_M_RD)
			ret = wmt_i2c_read(i2c, pmsg);
		else
			ret = wmt_i2c_write(i2c, pmsg, (i + 1) == num);
	}

	return (ret < 0) ? ret : i;
}

int wmt_i2c_init(struct platform_device *pdev, struct wmt_i2c **pi2c)
{
	int err;
	struct wmt_i2c *i2c;
	struct device_node *np = pdev->dev.of_node;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	i2c->irq = irq_of_parse_and_map(np, 0);
	if (!i2c->irq)
		return -EINVAL;

	err = devm_request_irq(&pdev->dev, i2c->irq, wmt_i2c_isr,
					0, pdev->name, i2c);
	if (err)
		return dev_err_probe(&pdev->dev, err,
				"failed to request irq %i\n", i2c->irq);

	i2c->dev = &pdev->dev;
	init_completion(&i2c->complete);
	platform_set_drvdata(pdev, i2c);

	*pi2c = i2c;
	return 0;
}
