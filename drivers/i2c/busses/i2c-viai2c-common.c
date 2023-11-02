// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/of_irq.h>
#include "i2c-viai2c-common.h"

#define WMT_I2C_TIMEOUT		(msecs_to_jiffies(1000))

int viai2c_wait_bus_ready(struct viai2c *i2c)
{
	unsigned long timeout;
	void __iomem *base = i2c->base;

	timeout = jiffies + WMT_I2C_TIMEOUT;
	while (!(readw(base + VIAI2C_REG_CSR) & VIAI2C_CSR_READY_MASK)) {
		if (time_after(jiffies, timeout)) {
			dev_warn(i2c->dev, "timeout waiting for bus ready\n");
			return -EBUSY;
		}
		msleep(20);
	}

	return 0;
}

int viai2c_wait_status(struct viai2c *i2c, u8 status)
{
	unsigned long wait_result;

	wait_result = wait_for_completion_timeout(&i2c->complete,
						msecs_to_jiffies(500));
	if (!wait_result)
		return -ETIMEDOUT;

	if (i2c->cmd_status & status)
		return 0;

	return -EIO;
}

static int viai2c_write(struct viai2c *i2c, struct i2c_msg *pmsg, bool last)
{
	u16 val, tcr_val = i2c->tcr;
	int xfer_len = 0;
	void __iomem *base = i2c->base;

	if (pmsg->len == 0) {
		/*
		 * We still need to run through the while (..) once, so
		 * start at -1 and break out early from the loop
		 */
		xfer_len = -1;
		writew(0, base + VIAI2C_REG_CDR);
	} else {
		writew(pmsg->buf[0] & 0xFF, base + VIAI2C_REG_CDR);
	}

	if (!(pmsg->flags & I2C_M_NOSTART)) {
		val = readw(base + VIAI2C_REG_CR);
		val &= ~VIAI2C_CR_TX_END;
		val |= VIAI2C_CR_CPU_RDY;
		writew(val, base + VIAI2C_REG_CR);
	}

	reinit_completion(&i2c->complete);
	writew(tcr_val | pmsg->addr, base + VIAI2C_REG_TCR);

	if (pmsg->flags & I2C_M_NOSTART) {
		val = readw(base + VIAI2C_REG_CR);
		val |= VIAI2C_CR_CPU_RDY;
		writew(val, base + VIAI2C_REG_CR);
	}

	while (xfer_len < pmsg->len) {
		int err;

		err = viai2c_wait_status(i2c, VIAI2C_ISR_BYTE_END);
		if (err)
			return err;

		xfer_len++;

		val = readw(base + VIAI2C_REG_CSR);
		if (val & VIAI2C_CSR_RCV_NOT_ACK) {
			dev_dbg(i2c->dev, "write RCV NACK error\n");
			return -EIO;
		}

		if (pmsg->len == 0) {
			val = VIAI2C_CR_TX_END | VIAI2C_CR_CPU_RDY
				| VIAI2C_CR_ENABLE;
			writew(val, base + VIAI2C_REG_CR);
			break;
		}

		if (xfer_len == pmsg->len) {
			if (!last)
				writew(VIAI2C_CR_ENABLE, base + VIAI2C_REG_CR);
		} else {
			writew(pmsg->buf[xfer_len] & 0xFF,
					base + VIAI2C_REG_CDR);
			writew(VIAI2C_CR_CPU_RDY | VIAI2C_CR_ENABLE,
					base + VIAI2C_REG_CR);
		}
	}

	return 0;
}

static int viai2c_read(struct viai2c *i2c, struct i2c_msg *pmsg)
{
	u16 val, tcr_val = i2c->tcr;
	u32 xfer_len = 0;
	void __iomem *base = i2c->base;

	val = readw(base + VIAI2C_REG_CR);
	val &= ~(VIAI2C_CR_TX_END | VIAI2C_CR_RX_END);

	if (!(pmsg->flags & I2C_M_NOSTART))
		val |= VIAI2C_CR_CPU_RDY;

	if (pmsg->len == 1)
		val |= VIAI2C_CR_RX_END;

	writew(val, base + VIAI2C_REG_CR);

	reinit_completion(&i2c->complete);

	tcr_val |= VIAI2C_TCR_MASTER_READ | pmsg->addr;

	writew(tcr_val, base + VIAI2C_REG_TCR);

	if (pmsg->flags & I2C_M_NOSTART) {
		val = readw(base + VIAI2C_REG_CR);
		val |= VIAI2C_CR_CPU_RDY;
		writew(val, base + VIAI2C_REG_CR);
	}

	while (xfer_len < pmsg->len) {
		int err;

		err = viai2c_wait_status(i2c, VIAI2C_ISR_BYTE_END);
		if (err)
			return err;

		pmsg->buf[xfer_len] = readw(base + VIAI2C_REG_CDR) >> 8;
		xfer_len++;

		val = readw(base + VIAI2C_REG_CR) | VIAI2C_CR_CPU_RDY;
		if (xfer_len == pmsg->len - 1)
			val |= VIAI2C_CR_RX_END;
		writew(val, base + VIAI2C_REG_CR);
	}

	return 0;
}

int viai2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	struct i2c_msg *pmsg;
	int i;
	int ret = 0;
	struct viai2c *i2c = i2c_get_adapdata(adap);

	for (i = 0; ret >= 0 && i < num; i++) {
		pmsg = &msgs[i];
		if (!(pmsg->flags & I2C_M_NOSTART)) {
			ret = viai2c_wait_bus_ready(i2c);
			if (ret < 0)
				return ret;
		}

		if (pmsg->flags & I2C_M_RD)
			ret = viai2c_read(i2c, pmsg);
		else
			ret = viai2c_write(i2c, pmsg, i == (num - 1));
	}

	return (ret < 0) ? ret : i;
}

static irqreturn_t viai2c_isr(int irq, void *data)
{
	struct viai2c *i2c = data;

	/* save the status and write-clear it */
	i2c->cmd_status = readw(i2c->base + VIAI2C_REG_ISR);
	writew(i2c->cmd_status, i2c->base + VIAI2C_REG_ISR);

	complete(&i2c->complete);

	return IRQ_HANDLED;
}

int viai2c_init(struct platform_device *pdev, struct viai2c **pi2c)
{
	int err;
	int irq_flags;
	struct viai2c *i2c;
	struct device_node *np = pdev->dev.of_node;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	if (np) {
		irq_flags = 0;
		i2c->irq = irq_of_parse_and_map(np, 0);
		if (!i2c->irq)
			return -EINVAL;
	} else {
		irq_flags = IRQF_SHARED;
		i2c->irq = platform_get_irq(pdev, 0);
		if (i2c->irq < 0)
			return i2c->irq;
	}

	err = devm_request_irq(&pdev->dev, i2c->irq, viai2c_isr,
					irq_flags, pdev->name, i2c);
	if (err)
		return dev_err_probe(&pdev->dev, err,
				"failed to request irq %i\n", i2c->irq);

	i2c->dev = &pdev->dev;
	init_completion(&i2c->complete);
	platform_set_drvdata(pdev, i2c);

	*pi2c = i2c;
	return 0;
}
