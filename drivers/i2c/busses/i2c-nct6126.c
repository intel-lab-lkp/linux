// SPDX-License-Identifier: GPL-2.0
/*
 * i2c-nct6126 - i2c adapter driver for the Nuvoton NCT6126D Super-I/O chip.
 *
 * The NCT6126D exposes an SMBus master controller inside Logical Device B
 * (the Hardware Monitor / SB-TSI block). Its I/O base address is programmed
 * by BIOS into LD B CR62h (MSB) and CR63h (LSB).
 *
 * Inspired by nct6775-platform.c and gpio-f7188x.c.
 *
 * Copyright (c) Siemens AG, 2026
 *
 * Author: Benedikt Niedermayr <benedikt.niedermayr@siemens.com>
 */

#define DRVNAME "i2c-nct6126"
#define pr_fmt(fmt) DRVNAME ": " fmt

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>

/*
 * Super-I/O configuration space
 */
#define SIO_LDSEL		0x07	/* Logical Device Select */
#define SIO_DEVID		0x20	/* CR20/CR21: 16-bit chip ID */
#define SIO_UNLOCK_KEY		0x87	/* Enter extended function mode */
#define SIO_LOCK_KEY		0xAA	/* Exit extended function mode */

/* Accepted chip IDs */
#define SIO_NCT6126D_A_ID	0xD283	/* NCT6126D, A version */
#define SIO_NCT6126D_B_ID	0xD284	/* NCT6126D, B version */

/* Logical Device B: Hardware Monitor + SB-TSI / SMBus master */
#define SIO_LD_HM_SMBUS		0x0B
#define SIO_LDB_ENABLE		0x30	/* bit0: block active (CR30) */
#define SIO_LDB_SMBUS_BASE	0x62	/* CR62 = MSB, CR63 = LSB */

/* SMBus base address constraints */
#define SMBUS_BASE_MIN		0x100
#define SMBUS_BASE_MAX		0xFFE

/*
 * SMBus controller register offsets (base + offset)
 * All registers are accessed directly at smbus_base + offset.
 */
#define NCT6126D_SMWRSIZE	0x01
#define NCT6126D_SMCMD		0x02
#define NCT6126D_SMIDX		0x03
#define NCT6126D_SMCTL		0x04
#define NCT6126D_SMADDR		0x05
#define NCT6126D_ERROR_STS	0x09
#define NCT6126D_SMCTL3		0x0e
#define NCT6126D_SMBUS_REGION_SIZE	0x20

/* Bit definitions */
#define NCT6126D_FIFO_NULL		BIT(0)
#define NCT6126D_FIFO_FULL		BIT(1)
#define NCT6126D_ERR_NACK		BIT(1)
#define NCT6126D_ERR_BER		BIT(2)
#define NCT6126D_ERR_TIMEOUT		BIT(4)
#define NCT6126D_ERR_ADNACK		BIT(5)
#define NCT6126D_RST			BIT(6)
#define NCT6126D_MANUAL_MODE		BIT(7)
#define NCT6126D_ACTIVE_MANUAL_MODE	BIT(2)

/* SMBus command codes */
#define NCT6126_CMD_READ_BYTE	0
#define NCT6126_CMD_WRITE_BYTE	8

/* Timing / retry limits */
#define SMBUS_RW_LOOP_MAX	5
#define SMBUS_FIFO_WAIT_US	100
#define SMBUS_FIFO_MAX_CLEAR_TIME	0xff

/*
 * Super-I/O functions.
 */
static inline int nct_superio_inb(int base, int reg)
{
	outb(reg, base);
	return inb(base + 1);
}

static int nct_superio_inw(int base, int reg)
{
	int val;

	outb(reg++, base);
	val = inb(base + 1) << 8;
	outb(reg, base);
	val |= inb(base + 1);

	return val;
}

static inline int nct_superio_enter(int base)
{
	if (!request_muxed_region(base, 2, DRVNAME)) {
		pr_err("SIO config port %#x already in use\n", base);
		return -EBUSY;
	}

	/* Datasheet 7.1.1: key must be written twice */
	outb(SIO_UNLOCK_KEY, base);
	outb(SIO_UNLOCK_KEY, base);

	return 0;
}

static inline void nct_superio_select(int base, int ld)
{
	outb(SIO_LDSEL, base);
	outb(ld, base + 1);
}

static inline void nct_superio_exit(int base)
{
	outb(SIO_LOCK_KEY, base);
	release_region(base, 2);
}

struct nct6126_sio {
	int           addr;       /* SIO config port: 0x2e or 0x4e */
	unsigned long smbus_base; /* from LD B CR62/CR63 */
};

struct nct6126_smbus {
	unsigned long    port_addr;
	struct i2c_adapter adap;
};

static int __init nct6126_find(int addr, struct nct6126_sio *sio)
{
	int err;
	u16 devid;
	u8  enable, msb, lsb;
	unsigned long base;

	err = nct_superio_enter(addr);
	if (err)
		return err;

	/* Verify chip identity */
	devid = nct_superio_inw(addr, SIO_DEVID);
	if (devid != SIO_NCT6126D_A_ID && devid != SIO_NCT6126D_B_ID) {
		err = -ENODEV;
		goto out;
	}

	/* Select Logical Device B (HM + SB-TSI/SMBus master) */
	nct_superio_select(addr, SIO_LD_HM_SMBUS);

	/*
	 * Check CR30 bit 0: if clear, the block is not decoding its I/O
	 * range on the LPC bus.  Every inb() at the SMBus window would
	 * return 0xFF.  We do NOT write this bit: nct6775 owns CR30 and
	 * maintains it across suspend/resume.
	 */
	enable = nct_superio_inb(addr, SIO_LDB_ENABLE);
	if (!(enable & BIT(0))) {
		pr_info("LD B inactive (CR30 bit0=0): BIOS has not enabled the HM/SMBus block\n");
		err = -ENODEV;
		goto out;
	}

	/*
	 * Read the SMBus master base address programmed by BIOS.
	 * Datasheet 19.7.1, 23.11: LD B CR62h (MSB) / CR63h (LSB).
	 * BIOS must program this.
	 */
	msb  = nct_superio_inb(addr, SIO_LDB_SMBUS_BASE);
	lsb  = nct_superio_inb(addr, SIO_LDB_SMBUS_BASE + 1);
	base = ((unsigned long)msb << 8) | lsb;

	if (base < SMBUS_BASE_MIN || base > SMBUS_BASE_MAX || (base & 1)) {
		pr_err("invalid SMBus base %#lx in LD B CR62/63\n", base);
		err = -ENXIO;
		goto out;
	}

	sio->addr = addr;
	sio->smbus_base = base;
	err = 0;

	pr_info("Found nct6126d at %#x, SMBus base %#lx\n", addr, base);

out:
	nct_superio_exit(addr);
	return err;
}

/*
 * SMBus
 */
static int nct_smbus_err_check(u8 err_code)
{
	if (err_code & NCT6126D_ERR_ADNACK)
		return -ENXIO;
	if (err_code & NCT6126D_ERR_TIMEOUT)
		return -ETIMEDOUT;
	if (err_code & (NCT6126D_ERR_BER | NCT6126D_ERR_NACK))
		return -EXDEV;
	return 0;
}

static void nct6126_smbus_init_config(struct nct6126_smbus *priv,
				      u8 slave_addr, u8 slave_reg)
{
	u8 val;

	/* Reset SMBus controller */
	val = inb(priv->port_addr + NCT6126D_SMCTL);
	val |= NCT6126D_RST;
	outb(val, priv->port_addr + NCT6126D_SMCTL);
	outb(0,   priv->port_addr + NCT6126D_SMCTL);

	/* 7-bit slave address; hardware expects it left-shifted by 1 */
	outb(slave_addr << 1, priv->port_addr + NCT6126D_SMADDR);
	outb(slave_reg,       priv->port_addr + NCT6126D_SMIDX);
}

static int nct6126_smbus_enable_manual_mode(struct nct6126_smbus *priv)
{
	u8 val;

	val = inb(priv->port_addr + NCT6126D_SMCTL);
	val |= NCT6126D_MANUAL_MODE;
	outb(val, priv->port_addr + NCT6126D_SMCTL);

	val = inb(priv->port_addr + NCT6126D_SMCTL3);
	val |= NCT6126D_ACTIVE_MANUAL_MODE;
	outb(val, priv->port_addr + NCT6126D_SMCTL3);

	/*
	 * Wait ~200 us for the slave to respond, then read the error
	 * status.  Per Nuvoton FAE: error must be checked after manual
	 * mode is activated due to the one-shot enable behaviour.
	 */
	usleep_range(200, 300);
	val = inb(priv->port_addr + NCT6126D_ERROR_STS);

	return nct_smbus_err_check(val);
}

static int nct6126_smbus_write8(struct nct6126_smbus *priv,
				u8 slave_addr, u8 slave_reg, u8 *data)
{
	int loop_cnt;
	int err;

	if (!request_muxed_region(priv->port_addr,
				  NCT6126D_SMBUS_REGION_SIZE, DRVNAME))
		return -EBUSY;

	nct6126_smbus_init_config(priv, slave_addr, slave_reg);

	outb(1,                      priv->port_addr + NCT6126D_SMWRSIZE);
	outb(NCT6126_CMD_WRITE_BYTE, priv->port_addr + NCT6126D_SMCMD);

	loop_cnt = SMBUS_RW_LOOP_MAX;
	while ((inb(priv->port_addr + NCT6126D_SMCTL3) & NCT6126D_FIFO_FULL) &&
	       --loop_cnt)
		usleep_range(SMBUS_FIFO_WAIT_US, SMBUS_FIFO_WAIT_US * 2);

	if (loop_cnt == 0) {
		err = -ETIMEDOUT;
		goto out;
	}

	outb(*data, priv->port_addr);
	err = nct6126_smbus_enable_manual_mode(priv);

out:
	release_region(priv->port_addr, NCT6126D_SMBUS_REGION_SIZE);
	return err;
}

static int nct6126_smbus_read8(struct nct6126_smbus *priv,
			       u8 slave_addr, u8 slave_reg, u8 *data)
{
	int loop_cnt;
	int err;

	if (!request_muxed_region(priv->port_addr,
				  NCT6126D_SMBUS_REGION_SIZE, DRVNAME))
		return -EBUSY;

	nct6126_smbus_init_config(priv, slave_addr, slave_reg);

	outb(0, priv->port_addr + NCT6126D_SMWRSIZE);
	outb(NCT6126_CMD_READ_BYTE, priv->port_addr + NCT6126D_SMCMD);

	err = nct6126_smbus_enable_manual_mode(priv);
	if (err)
		goto out;

	loop_cnt = SMBUS_RW_LOOP_MAX;
	while ((inb(priv->port_addr + NCT6126D_SMCTL3) & NCT6126D_FIFO_NULL) &&
	       --loop_cnt)
		usleep_range(SMBUS_FIFO_WAIT_US, SMBUS_FIFO_WAIT_US * 2);

	if (loop_cnt == 0) {
		err = -ETIMEDOUT;
		goto out;
	}

	*data = inb(priv->port_addr);

	/* Drain any residual FIFO entries */
	loop_cnt = SMBUS_FIFO_MAX_CLEAR_TIME;
	while (!(inb(priv->port_addr + NCT6126D_SMCTL3) & NCT6126D_FIFO_NULL) &&
	       --loop_cnt)
		inb(priv->port_addr);

	err = (loop_cnt == 0) ? -ETIMEDOUT : 0;

out:
	release_region(priv->port_addr, NCT6126D_SMBUS_REGION_SIZE);
	return err;
}

/*
 * i2c_algorithm
 */
static int nct6126_smbus_xfer(struct i2c_adapter *adap, u16 addr, u16 flags,
			      char read_write, u8 cmd, int size,
			      union i2c_smbus_data *data)
{
	struct nct6126_smbus *priv = i2c_get_adapdata(adap);

	if (!priv)
		return -ENODEV;

	if (read_write == I2C_SMBUS_READ)
		return nct6126_smbus_read8(priv, addr, cmd, &data->byte);
	else
		return nct6126_smbus_write8(priv, addr, cmd, &data->byte);
}

static u32 nct6126_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA;
}

static const struct i2c_algorithm nct6126_algo = {
	.smbus_xfer    = nct6126_smbus_xfer,
	.functionality = nct6126_functionality,
};

static int nct6126_smbus_probe(struct platform_device *pdev)
{
	struct nct6126_sio   *sio = dev_get_platdata(&pdev->dev);
	struct nct6126_smbus *priv;
	struct i2c_adapter   *adap;
	int err;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->port_addr = sio->smbus_base;

	adap              = &priv->adap;
	adap->owner       = THIS_MODULE;
	adap->class       = I2C_CLASS_HWMON;
	adap->algo        = &nct6126_algo;
	adap->dev.parent  = &pdev->dev;
	adap->nr          = -1;
	strscpy(adap->name, DRVNAME, sizeof(adap->name));
	i2c_set_adapdata(adap, priv);

	err = i2c_add_adapter(adap);
	if (err) {
		dev_err(&pdev->dev, "failed to add i2c adapter: %d\n", err);
		return err;
	}

	platform_set_drvdata(pdev, priv);
	return 0;
}

static void nct6126_smbus_remove(struct platform_device *pdev)
{
	struct nct6126_smbus *priv = platform_get_drvdata(pdev);

	i2c_del_adapter(&priv->adap);
}

static struct platform_driver nct6126_smbus_driver = {
	.driver = {
		.name = DRVNAME,
	},
	.probe  = nct6126_smbus_probe,
	.remove = nct6126_smbus_remove,
};

#define MAX_PDEVS 1
static struct platform_device *nct6126_pdevs[MAX_PDEVS];
static int nct6126_pdevs_cnt;

static int __init nct6126_device_add(const struct nct6126_sio *sio)
{
	struct platform_device *pdev;
	int err;

	pdev = platform_device_alloc(DRVNAME, 0);
	if (!pdev)
		return -ENOMEM;

	err = platform_device_add_data(pdev, sio, sizeof(*sio));
	if (err) {
		pr_err("platform data allocation failed\n");
		goto err_put;
	}

	err = platform_device_add(pdev);
	if (err) {
		pr_err("platform device registration failed\n");
		goto err_put;
	}

	nct6126_pdevs[nct6126_pdevs_cnt++] = pdev;
	return 0;

err_put:
	platform_device_put(pdev);
	return err;
}

static int __init nct6126_smbus_init(void)
{
	struct nct6126_sio sio;
	int err;

	if (nct6126_find(0x2e, &sio) && nct6126_find(0x4e, &sio))
		return -ENODEV;

	err = platform_driver_register(&nct6126_smbus_driver);
	if (err)
		return err;

	err = nct6126_device_add(&sio);
	if (err)
		goto err_unreg_driver;

	return 0;

err_unreg_driver:
	platform_driver_unregister(&nct6126_smbus_driver);
	return err;
}

static void __exit nct6126_smbus_exit(void)
{
	while (nct6126_pdevs_cnt > 0)
		platform_device_unregister(nct6126_pdevs[--nct6126_pdevs_cnt]);

	platform_driver_unregister(&nct6126_smbus_driver);
}

module_init(nct6126_smbus_init);
module_exit(nct6126_smbus_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Benedikt Niedermayr <benedikt.niedermayr@siemens.com>");
MODULE_DESCRIPTION("SMBus master driver for Nuvoton NCT6126D Super-I/O");
