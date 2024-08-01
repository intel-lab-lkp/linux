/*
 * CGEB i2c driver
 *
 * (c) 2011 Sascha Hauer, Pengutronix
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/i2c.h>
#include <linux/mfd/congatec-cgeb.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define CG_I2C_FLAG_START   0x00080    /* send START condition */
#define CG_I2C_FLAG_STOP    0x00040    /* send STOP condition */
#define CG_I2C_FLAG_ALL_ACK 0x08000    /* send ACK on all read bytes */
#define CG_I2C_FLAG_ALL_NAK 0x04000    /* send NAK on all read bytes */

struct cgeb_i2c_priv {
	struct cgeb_board_data *board;
	struct i2c_adapter adapter;
	int unit;
};

static u32 cgeb_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static int cgeb_i2c_set_speed(struct cgeb_i2c_priv *priv, int speed)
{
	struct cgeb_function_parameters fps;

	memset(&fps, 0, sizeof(fps));

	fps.unit = priv->unit;
	fps.pars[0] = speed;

	return cgeb_call(priv->board, &fps, CgebI2CSetFrequency);
}

static int cgeb_i2c_xfer(struct i2c_adapter *adapter,
		struct i2c_msg *msgs, int num)
{
	struct cgeb_function_parameters fps;
	int i, ret;
	unsigned long flags = CG_I2C_FLAG_START;
	struct cgeb_i2c_priv *priv = i2c_get_adapdata(adapter);
	unsigned long rdlen, wrlen;
	unsigned char *rdbuf, *wrbuf, *raw_wrbuf;
	unsigned short lmax = 0;

	/*
	 * With cgeb the I2C address is part of the write data
	 * buffer, so allocate a buffer with the length of the
	 * longest write buffer + 1
	 */
	for (i = 0; i < num; i++)
		if (!(msgs[i].flags & I2C_M_RD))
			lmax = max(lmax, msgs[i].len);

	raw_wrbuf = kmalloc(lmax + 1, GFP_KERNEL);
	if (!raw_wrbuf)
		return -ENOMEM;

	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD) {
			rdbuf = msgs[i].buf;
			rdlen = msgs[i].len;
			wrbuf = NULL;
			wrlen = 0;
		} else {
			rdbuf = NULL;
			rdlen = 0;
			wrbuf = msgs[i].buf;
			wrlen = msgs[i].len;
		}

		raw_wrbuf[0] = msgs[i].addr << 1;
		if (wrlen)
			memcpy(&raw_wrbuf[1], wrbuf, wrlen);

		if (msgs[i].flags & I2C_M_RD)
			raw_wrbuf[0] |= 1;

		if (i == num - 1)
			flags |= CG_I2C_FLAG_STOP;

		dev_dbg(&adapter->dev,
				"%s: rd: %p/%ld wr: %p/%ld flags: 0x%08lx %s\n",
				__func__, rdbuf, rdlen, raw_wrbuf, wrlen + 1,
				flags,
				msgs[i].flags & I2C_M_RD ? "READ" : "WRITE");

		memset(&fps, 0, sizeof(fps));

		fps.unit = priv->unit;
		fps.pars[0] = wrlen + 1;
		fps.pars[1] = rdlen;
		fps.pars[2] = flags;
		fps.iptr = raw_wrbuf;
		fps.optr = rdbuf;
		fps.optr_size = sizeof(*rdbuf) * rdlen;
		fps.iptr_size = (wrlen + 1) * sizeof(*raw_wrbuf);

		ret = cgeb_call(priv->board, &fps, CgebI2CTransfer);
		if (ret) {
			ret = -EREMOTEIO;
			goto out;
		}
	}

	ret = num;

out:
	kfree(raw_wrbuf);

	return ret;
}

static struct i2c_algorithm cgeb_i2c_algo = {
	.master_xfer    = cgeb_i2c_xfer,
	.functionality  = cgeb_i2c_func,
};

static int cgeb_i2c_probe(struct platform_device *pdev)
{
	struct cgeb_i2c_priv *priv;
	struct cgeb_pdata *pdata = pdev->dev.platform_data;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	strcpy(priv->adapter.name, pdev->name);
	priv->adapter.owner             = THIS_MODULE;
	priv->adapter.algo              = &cgeb_i2c_algo;
	priv->adapter.dev.parent        = &pdev->dev;
	priv->unit = pdata->unit;
	priv->board = pdata->board;
	i2c_set_adapdata(&priv->adapter, priv);

	platform_set_drvdata(pdev, priv);

	ret = cgeb_i2c_set_speed(priv, 400000);
	if (ret)
		/* not a critical error, we can continue with the default speed. */
		dev_warn(&pdev->dev, "Could not set speed to 400KHz\n");

	ret = i2c_add_adapter(&priv->adapter);
	if (ret < 0) {
		dev_err(&pdev->dev, "registration failed\n");
		return ret;
	}

	dev_info(&pdev->dev, "registered\n");

	return 0;
};

static int cgeb_i2c_remove(struct platform_device *pdev)
{
	struct cgeb_i2c_priv *priv = platform_get_drvdata(pdev);

	i2c_del_adapter(&priv->adapter);

	return 0;
}

static struct platform_driver cgeb_i2c_driver = {
	.probe          = cgeb_i2c_probe,
	.remove         = cgeb_i2c_remove,
	.driver = {
		.name   = "cgeb-i2c",
	},
};
module_platform_driver(cgeb_i2c_driver);

MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_DESCRIPTION("cgeb i2c driver");
MODULE_LICENSE("GPL");
