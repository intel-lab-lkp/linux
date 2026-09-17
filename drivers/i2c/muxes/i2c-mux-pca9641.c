// SPDX-License-Identifier: GPL-2.0
/*
 * I2C multiplexer driver for PCA9641 2-to-1 I2C bus master arbiter
 *
 * Copyright (C) 2026 Shiv Prakash Gupta <shivprakash.gupta@nxp.com>
 *
 * Based on i2c-mux-pca9541.c by Guenter Roeck <linux@roeck-us.net>
 *
 * Datasheet: https://www.nxp.com/docs/en/data-sheet/PCA9641.pdf
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>

/* Register addresses, selected via the 3 LSBs of the command code byte */
#define PCA9641_ID          0x00  /* Device ID register (R only, value 0x38) */
#define PCA9641_CONTR       0x01  /* Control register (R/W) */
#define PCA9641_STATUS      0x02  /* Status register (R/W) */
#define PCA9641_INT_STATUS  0x04  /* Interrupt Status register (R/W, sticky) */
#define PCA9641_INT_MSK     0x05  /* Interrupt Mask register (R/W) */

/* CONTR register (0x01) bit definitions */
#define PCA9641_CTL_LOCK_REQ        BIT(0)  /* Request downstream bus ownership */
#define PCA9641_CTL_LOCK_GRANT      BIT(1)  /* Downstream bus granted (R only) */
#define PCA9641_CTL_BUS_CONNECT     BIT(2)  /* Connect master to downstream bus */

/* STATUS register (0x02) bit definitions */
#define PCA9641_STA_OTHER_LOCK      BIT(0)  /* Other master currently holds the lock (R only) */

/* INT_STATUS register (0x04) bit definitions (sticky, clear by writing 1) */
#define PCA9641_INTS_BUS_LOST       BIT(1)  /* This master involuntarily lost the bus */
#define PCA9641_INTS_LOCK_GRANT     BIT(2)  /* This master was granted the bus */

/* INT_MSK register (0x05) bit definitions (1 = masked/disabled, POR = 0x7F) */
#define PCA9641_MSK_BUS_LOST        BIT(1)
#define PCA9641_MSK_LOCK_GRANT      BIT(2)

/* POR value of INT_MSK: all interrupt sources masked */
#define PCA9641_INT_MSK_ALL         0x7Fu

/*
 * Interrupt mask value that enables only LOCK_GRANT and BUS_LOST.
 * All other sources remain masked (bit = 1 means masked).
 */
#define PCA9641_INT_MSK_ARB \
	(PCA9641_INT_MSK_ALL & ~(PCA9641_MSK_LOCK_GRANT | PCA9641_MSK_BUS_LOST))

/* Sticky INT_STATUS bits used by the arbitration logic (write 1 to clear) */
#define PCA9641_INTS_ARB \
	(PCA9641_INTS_LOCK_GRANT | PCA9641_INTS_BUS_LOST)

/* Value in ID register that uniquely identifies PCA9641 (vs. PCA9541) */
#define PCA9641_ID_MAGIC            0x38u

/* Arbitration retry delays (microseconds) */
#define PCA9641_DELAY_SHORT         50u
#define PCA9641_DELAY_LONG          1000u

/**
 * struct pca9641 - per-device driver state
 * @client:          I2C client for the arbiter device
 * @select_timeout:  Current polling retry delay in microseconds
 * @irq:             Linux IRQ number for INT0/INT1 GPIO, or -1 if not used
 * @lock_grant_comp: Completion signaled from the IRQ handler on LOCK_GRANT
 */
struct pca9641 {
	struct i2c_client *client;
	unsigned long select_timeout;
	int irq;
	struct completion lock_grant_comp;
};

static const struct i2c_device_id pca9641_id[] = {
	{ .name = "pca9641" },
	{}
};
MODULE_DEVICE_TABLE(i2c, pca9641_id);

static const struct of_device_id pca9641_of_match[] = {
	{ .compatible = "nxp,pca9641" },
	{}
};
MODULE_DEVICE_TABLE(of, pca9641_of_match);

/*
 * Write to chip register. Don't use i2c_transfer()/i2c_smbus_xfer()
 * as they will try to lock the adapter a second time.
 */
static int pca9641_reg_write(struct i2c_client *client, u8 reg, u8 val)
{
	union i2c_smbus_data data = { .byte = val };

	return __i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_WRITE, reg,
				I2C_SMBUS_BYTE_DATA, &data);
}

/*
 * Read from chip register. Don't use i2c_transfer()/i2c_smbus_xfer()
 * as they will try to lock adapter a second time.
 */
static int pca9641_reg_read(struct i2c_client *client, u8 reg)
{
	union i2c_smbus_data data;
	int ret;

	ret = __i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			       I2C_SMBUS_READ, reg,
			       I2C_SMBUS_BYTE_DATA, &data);

	return ret ? ret : (int)data.byte;
}

/*
 * Release bus ownership. Writing 0 clears LOCK_REQ (and BUS_CONNECT),
 * which lets the arbiter grant the downstream bus to the other master.
 */
static void pca9641_release_bus(struct i2c_client *client)
{
	pca9641_reg_write(client, PCA9641_CONTR, 0x00);
}

/*
 * Threaded IRQ handler for INT0/INT1. Signals the completion waited on by
 * pca9641_select_chan(). No I2C access here to avoid deadlock on the adapter
 * mutex held by select_chan().
 */
static irqreturn_t pca9641_irq_handler(int irq, void *dev_id)
{
	struct pca9641 *data = dev_id;

	complete(&data->lock_grant_comp);
	return IRQ_HANDLED;
}

/*
 * Arbitration management. Asserts LOCK_REQ and checks LOCK_GRANT to acquire
 * the downstream bus. Returns 1 when acquired, 0 to retry, <0 on error.
 */
static int pca9641_arbitrate(struct i2c_client *client)
{
	struct i2c_mux_core *muxc = i2c_get_clientdata(client);
	struct pca9641 *data = i2c_mux_priv(muxc);
	int ctrl, status, ret;

	ctrl = pca9641_reg_read(client, PCA9641_CONTR);
	if (ctrl < 0)
		return ctrl;

	if (ctrl & PCA9641_CTL_LOCK_GRANT) {
		if (!(ctrl & PCA9641_CTL_BUS_CONNECT)) {
			ret = pca9641_reg_write(client, PCA9641_CONTR,
						(u8)(ctrl | PCA9641_CTL_BUS_CONNECT));
			if (ret < 0)
				return ret;
		}
		return 1;
	}

	if (!(ctrl & PCA9641_CTL_LOCK_REQ)) {
		ret = pca9641_reg_write(client, PCA9641_CONTR,
					(u8)(ctrl | PCA9641_CTL_LOCK_REQ));
		if (ret < 0)
			return ret;
		data->select_timeout = PCA9641_DELAY_SHORT;
		return 0;
	}

	status = pca9641_reg_read(client, PCA9641_STATUS);
	if (status < 0)
		return status;

	data->select_timeout = (status & PCA9641_STA_OTHER_LOCK) ?
				PCA9641_DELAY_LONG : PCA9641_DELAY_SHORT;

	return 0;
}

/*
 * Acquire the downstream bus before a transaction. Uses interrupt mode
 * if IRQ is available, polling otherwise.
 */
static int pca9641_select_chan(struct i2c_mux_core *muxc, u32 chan)
{
	struct pca9641 *data = i2c_mux_priv(muxc);
	struct i2c_client *client = data->client;
	unsigned long timeout = jiffies + 2 * client->adapter->timeout;
	int ctrl, ret;

	if (data->irq > 0) {
		reinit_completion(&data->lock_grant_comp);

		ctrl = pca9641_reg_read(client, PCA9641_CONTR);
		if (ctrl < 0)
			return ctrl;

		if (ctrl & PCA9641_CTL_LOCK_GRANT)
			goto set_bus_connect;

		ret = pca9641_reg_write(client, PCA9641_CONTR,
					(u8)((ctrl & ~PCA9641_CTL_BUS_CONNECT) |
					     PCA9641_CTL_LOCK_REQ));
		if (ret < 0)
			return ret;

		if (!wait_for_completion_timeout(&data->lock_grant_comp,
						 client->adapter->timeout)) {
			ctrl = pca9641_reg_read(client, PCA9641_CONTR);
			if (ctrl < 0)
				return ctrl;
			if (!(ctrl & PCA9641_CTL_LOCK_GRANT)) {
				dev_warn(&client->dev,
					 "Timed out waiting for bus grant\n");
				/*
				 * Clear our latched LOCK_REQ so the arbiter is
				 * not left holding a request for a master that
				 * has given up, which would block the peer.
				 */
				pca9641_release_bus(client);
				return -ETIMEDOUT;
			}
			goto set_bus_connect;
		}

		ctrl = pca9641_reg_read(client, PCA9641_CONTR);
		if (ctrl < 0)
			return ctrl;

		if (!(ctrl & PCA9641_CTL_LOCK_GRANT)) {
			dev_warn(&client->dev,
				 "Interrupt fired but LOCK_GRANT not set\n");
			pca9641_release_bus(client);
			return -ETIMEDOUT;
		}

set_bus_connect:
		(void)pca9641_reg_write(client, PCA9641_INT_STATUS,
					PCA9641_INTS_ARB);

		if (!(ctrl & PCA9641_CTL_BUS_CONNECT)) {
			ret = pca9641_reg_write(client, PCA9641_CONTR,
						(u8)(ctrl | PCA9641_CTL_BUS_CONNECT));
			if (ret < 0)
				return ret;
		}
		return 0;
	}

	do {
		ret = pca9641_arbitrate(client);
		if (ret)
			return ret < 0 ? ret : 0;

		if (data->select_timeout <= PCA9641_DELAY_SHORT)
			udelay(data->select_timeout);
		else
			msleep(data->select_timeout / 1000);
	} while (time_is_after_eq_jiffies(timeout));

	dev_warn(&client->dev, "Failed to acquire I2C bus, timed out\n");
	/* Drop our latched LOCK_REQ so the peer master is not blocked. */
	pca9641_release_bus(client);
	return -ETIMEDOUT;
}

/* Release the downstream bus after a transaction completes. */
static int pca9641_release_chan(struct i2c_mux_core *muxc, u32 chan)
{
	struct pca9641 *data = i2c_mux_priv(muxc);

	pca9641_release_bus(data->client);
	return 0;
}

static int pca9641_probe(struct i2c_client *client)
{
	struct i2c_adapter *adap = client->adapter;
	struct i2c_mux_core *muxc;
	struct pca9641 *data;
	int id, ret;

	if (!i2c_check_functionality(adap, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	id = i2c_smbus_read_byte_data(client, PCA9641_ID);
	if (id < 0) {
		dev_err(&client->dev, "Failed to read device ID: %d\n", id);
		return id;
	}
	if ((u8)id != PCA9641_ID_MAGIC) {
		dev_err(&client->dev,
			"Unexpected device ID 0x%02x (expected 0x%02x for PCA9641)\n",
			(u8)id, PCA9641_ID_MAGIC);
		return -ENODEV;
	}

	/* Clear any stale bus ownership from a previous crash. */
	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	pca9641_release_bus(client);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);

	muxc = i2c_mux_alloc(adap, &client->dev, 1, sizeof(*data),
			     I2C_MUX_ARBITRATOR,
			     pca9641_select_chan, pca9641_release_chan);
	if (!muxc)
		return -ENOMEM;

	data = i2c_mux_priv(muxc);
	data->client = client;
	data->irq = -1;
	init_completion(&data->lock_grant_comp);

	i2c_set_clientdata(client, muxc);

	/* Optional interrupt mode; fall back to polling on failure. */
	if (client->irq > 0) {
		/*
		 * Start from a known state: mask all sources and clear any
		 * stale sticky status before the handler is registered.
		 */
		ret = i2c_smbus_write_byte_data(client, PCA9641_INT_MSK,
						PCA9641_INT_MSK_ALL);
		if (ret < 0) {
			dev_warn(&client->dev,
				 "Failed to mask interrupts (%d); using polling\n",
				 ret);
			goto add_adapter;
		}
		i2c_smbus_write_byte_data(client, PCA9641_INT_STATUS,
					  PCA9641_INTS_ARB);

		/*
		 * Register the handler before unmasking the device so a
		 * pending interrupt cannot fire into an unregistered line.
		 */
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, pca9641_irq_handler,
						IRQF_ONESHOT,
						dev_name(&client->dev), data);
		if (ret < 0) {
			dev_warn(&client->dev,
				 "Failed to request IRQ %d (%d); using polling\n",
				 client->irq, ret);
			goto add_adapter;
		}

		/* Handler is ready; now enable LOCK_GRANT + BUS_LOST. */
		ret = i2c_smbus_write_byte_data(client, PCA9641_INT_MSK,
						PCA9641_INT_MSK_ARB);
		if (ret < 0) {
			dev_warn(&client->dev,
				 "Failed to enable interrupts (%d); using polling\n",
				 ret);
			goto add_adapter;
		}

		data->irq = client->irq;
		dev_dbg(&client->dev,
			"Interrupt mode enabled on IRQ %d\n",
			client->irq);
	}

add_adapter:
	ret = i2c_mux_add_adapter(muxc, 0, 0);
	if (ret) {
		/* Re-mask so a left-enabled source cannot storm the line. */
		if (data->irq > 0)
			i2c_smbus_write_byte_data(client, PCA9641_INT_MSK,
						  PCA9641_INT_MSK_ALL);
		return ret;
	}

	dev_info(&client->dev,
		 "PCA9641 I2C master arbiter registered (I2C bus %s, %s mode)\n",
		 client->name,
		 data->irq > 0 ? "interrupt" : "polling");

	return 0;
}

static void pca9641_remove(struct i2c_client *client)
{
	struct i2c_mux_core *muxc = i2c_get_clientdata(client);
	struct pca9641 *data = i2c_mux_priv(muxc);

	/*
	 * Unregister the child adapter first so any in-flight downstream
	 * transaction can finish arbitration before the interrupt that
	 * drives it is masked.
	 */
	i2c_mux_del_adapters(muxc);

	if (data->irq > 0)
		(void)i2c_smbus_write_byte_data(client, PCA9641_INT_MSK,
						PCA9641_INT_MSK_ALL);
}

static struct i2c_driver pca9641_driver = {
	.driver = {
		.name		= "pca9641",
		.of_match_table	= pca9641_of_match,
	},
	.probe		= pca9641_probe,
	.remove		= pca9641_remove,
	.id_table	= pca9641_id,
};

module_i2c_driver(pca9641_driver);

MODULE_AUTHOR("Shiv Prakash Gupta <shivprakash.gupta@nxp.com>");
MODULE_DESCRIPTION("PCA9641 2-to-1 I2C bus master arbiter driver");
MODULE_LICENSE("GPL");
