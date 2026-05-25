// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Nuvoton NCT6694 host-interface (eSPI) transport driver.
 */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>

#define DRVNAME "nct6694-hif"

#define NCT6694_POLL_INTERVAL_US	10
#define NCT6694_POLL_TIMEOUT_US		10000

/*
 * Super-I/O registers
 */
#define SIO_REG_LDSEL		0x07	/* Logical device select */
#define SIO_REG_DEVID		0x20	/* Device ID (2 bytes) */
#define SIO_REG_LD_SHM		0x0F	/* Logical device shared memory control */

#define SIO_REG_SHM_ENABLE	0x30	/* Enable shared memory */
#define SIO_REG_SHM_BASE_ADDR	0x60	/* Shared memory base address (2 bytes) */
#define SIO_REG_SHM_IRQ_NR	0x70	/* Shared memory interrupt number */

#define SIO_REG_UNLOCK_KEY	0x87	/* Key to enable Super-I/O */
#define SIO_REG_LOCK_KEY	0xAA	/* Key to disable Super-I/O */

#define SIO_NCT6694B_ID		0xD029
#define SIO_NCT6694D_ID		0x5832

/*
 * Super-I/O Shared Memory Logical Device registers
 */
#define NCT6694_SHM_COFS_STS			0x2E
#define NCT6694_SHM_COFS_STS_COFS4W		BIT(7)

#define NCT6694_SHM_COFS_CTL2			0x3B
#define NCT6694_SHM_COFS_CTL2_COFS4W_IE		BIT(3)

#define NCT6694_SHM_INTR_STATUS			0x9C	/* Interrupt status register (4 bytes) */

enum nct6694_chips {
	NCT6694B = 0,
	NCT6694D,
};

struct __packed nct6694_msg {
	struct nct6694_cmd_header cmd_header;
	struct nct6694_response_header response_header;
	unsigned char data[];
};

struct nct6694_sio_data {
	enum nct6694_chips chip;
	int sioreg;	/* Super-I/O index port */
};

struct nct6694_hif_data {
	struct regmap *regmap;
	struct mutex msg_lock;
	struct nct6694_sio_data *sio_data;
	void __iomem *msg_base;
	unsigned int shm_base;
};

static const char * const nct6694_chip_names[] = {
	[NCT6694B] = "NCT6694B",
	[NCT6694D] = "NCT6694D",
};

/*
 * Super-I/O functions.
 */
static inline int superio_enter(struct nct6694_sio_data *sio_data)
{
	int ioreg = sio_data->sioreg;

	/*
	 * Try to reserve <ioreg> and <ioreg + 1> for exclusive access.
	 */
	if (!request_muxed_region(ioreg, 2, DRVNAME))
		return -EBUSY;

	outb(SIO_REG_UNLOCK_KEY, ioreg);
	outb(SIO_REG_UNLOCK_KEY, ioreg);

	return 0;
}

static inline void superio_exit(struct nct6694_sio_data *sio_data)
{
	int ioreg = sio_data->sioreg;

	outb(SIO_REG_LOCK_KEY, ioreg);

	release_region(ioreg, 2);
}

static inline void superio_select(struct nct6694_sio_data *sio_data, int ld)
{
	int ioreg = sio_data->sioreg;

	outb(SIO_REG_LDSEL, ioreg);
	outb(ld, ioreg + 1);
}

static inline int superio_inb(struct nct6694_sio_data *sio_data, int reg)
{
	int ioreg = sio_data->sioreg;

	outb(reg, ioreg);
	return inb(ioreg + 1);
}

static inline int superio_inw(struct nct6694_sio_data *sio_data, int reg)
{
	int ioreg = sio_data->sioreg;
	int val;

	outb(reg++, ioreg);
	val = inb(ioreg + 1) << 8;
	outb(reg, ioreg);
	val |= inb(ioreg + 1);

	return val;
}

static inline void superio_outb(struct nct6694_sio_data *sio_data, int reg, u8 val)
{
	int ioreg = sio_data->sioreg;

	outb(reg, ioreg);
	outb(val, ioreg + 1);
}

static int nct6694_sio_find(struct nct6694_sio_data *sio_data, u8 sioreg)
{
	int ret;
	u16 devid;

	sio_data->sioreg = sioreg;

	ret = superio_enter(sio_data);
	if (ret)
		return ret;

	/* Check Chip ID */
	devid = superio_inw(sio_data, SIO_REG_DEVID);
	switch (devid) {
	case SIO_NCT6694B_ID:
		sio_data->chip = NCT6694B;
		break;
	case SIO_NCT6694D_ID:
		sio_data->chip = NCT6694D;
		break;
	default:
		pr_debug("Unsupported device ID: 0x%04x\n", devid);
		goto err;
	}

	pr_info("Found %s at %#x\n", nct6694_chip_names[sio_data->chip], sio_data->sioreg);

	superio_exit(sio_data);

	return 0;

err:
	superio_exit(sio_data);
	return -ENODEV;
}

static const struct mfd_cell nct6694_hif_devs[] = {
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),
	MFD_CELL_NAME("nct6694-gpio"),

	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),
	MFD_CELL_NAME("nct6694-i2c"),

	MFD_CELL_NAME("nct6694-canfd"),
	MFD_CELL_NAME("nct6694-canfd"),
};

static int nct6694_response_err_handling(struct nct6694 *nct6694, unsigned char err_status)
{
	switch (err_status) {
	case NCT6694_NO_ERROR:
		return 0;
	case NCT6694_NOT_SUPPORT_ERROR:
		dev_err(nct6694->dev, "Command is not supported!\n");
		break;
	case NCT6694_NO_RESPONSE_ERROR:
		dev_warn(nct6694->dev, "Command received no response!\n");
		break;
	case NCT6694_TIMEOUT_ERROR:
		dev_warn(nct6694->dev, "Command timed out!\n");
		break;
	case NCT6694_PENDING:
		dev_err(nct6694->dev, "Command is pending!\n");
		break;
	default:
		return -EINVAL;
	}

	return -EIO;
}

static int nct6694_xfer_msg(struct nct6694 *nct6694,
			    const struct nct6694_cmd_header *cmd_hd,
			    u8 hctrl, void *buf)
{
	struct nct6694_hif_data *hdata = nct6694->priv;
	void __iomem *hdr = hdata->msg_base + offsetof(struct nct6694_msg, cmd_header);
	struct nct6694_cmd_header cmd = *cmd_hd;
	struct nct6694_response_header resp;
	u16 len = le16_to_cpu(cmd.len);
	u8 status;
	int ret;

	guard(mutex)(&hdata->msg_lock);

	/* Wait until the previous command is completed */
	ret = readb_poll_timeout(hdr + offsetof(struct nct6694_cmd_header, hctrl),
				 status, status == 0, NCT6694_POLL_INTERVAL_US,
				 NCT6694_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	/*
	 * Write cmd header fields, but skip hctrl — writing to it triggers
	 * firmware command processing and must be deferred until data is ready.
	 */
	memcpy_toio(hdr, &cmd, offsetof(struct nct6694_cmd_header, hctrl));
	memcpy_toio(hdr + offsetof(struct nct6694_cmd_header, rsv2), &cmd.rsv2,
		    sizeof(cmd) - offsetof(struct nct6694_cmd_header, rsv2));

	if (hctrl == NCT6694_HCTRL_SET && len)
		memcpy_toio(hdata->msg_base + offsetof(struct nct6694_msg, data),
			    buf, len);

	/* Write hctrl last to trigger command processing */
	writeb(hctrl, hdr + offsetof(struct nct6694_cmd_header, hctrl));

	ret = readb_poll_timeout(hdr + offsetof(struct nct6694_cmd_header, hctrl),
				 status, status == 0, NCT6694_POLL_INTERVAL_US,
				 NCT6694_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	memcpy_fromio(&resp, hdata->msg_base + offsetof(struct nct6694_msg, response_header),
		      sizeof(resp));

	ret = nct6694_response_err_handling(nct6694, resp.sts);
	if (ret)
		return ret;

	if (le16_to_cpu(resp.len))
		memcpy_fromio(buf, hdata->msg_base + offsetof(struct nct6694_msg, data),
			      min(len, le16_to_cpu(resp.len)));

	return 0;
}

/**
 * nct6694_hif_read_msg() - Send a command and read response data via HIF
 * @nct6694: NCT6694 device data
 * @cmd_hd: command header
 * @buf: buffer to store response data
 *
 * Return: 0 on success or negative errno on failure.
 */
static int nct6694_hif_read_msg(struct nct6694 *nct6694,
				const struct nct6694_cmd_header *cmd_hd,
				void *buf)
{
	struct nct6694_hif_data *hdata = nct6694->priv;

	if (cmd_hd->mod == NCT6694_RPT_MOD)
		return regmap_bulk_read(hdata->regmap,
					le16_to_cpu(cmd_hd->offset),
					buf, le16_to_cpu(cmd_hd->len));
	return nct6694_xfer_msg(nct6694, cmd_hd, NCT6694_HCTRL_GET, buf);
}

/**
 * nct6694_hif_write_msg() - Send a command with data payload via HIF
 * @nct6694: NCT6694 device data
 * @cmd_hd: command header
 * @buf: buffer containing data to send
 *
 * Return: 0 on success or negative errno on failure.
 */
static int nct6694_hif_write_msg(struct nct6694 *nct6694,
				 const struct nct6694_cmd_header *cmd_hd,
				 void *buf)
{
	struct nct6694_hif_data *hdata = nct6694->priv;

	if (cmd_hd->mod == NCT6694_RPT_MOD)
		return regmap_bulk_write(hdata->regmap,
					 le16_to_cpu(cmd_hd->offset),
					 buf, le16_to_cpu(cmd_hd->len));
	return nct6694_xfer_msg(nct6694, cmd_hd, NCT6694_HCTRL_SET, buf);
}

static const struct regmap_config nct6694_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
};

static irqreturn_t nct6694_irq_handler(int irq, void *data)
{
	struct nct6694 *nct6694 = data;
	struct nct6694_hif_data *hdata = nct6694->priv;
	u8 reg_data[4];
	u32 intr_status;
	int ret;

	/* Check interrupt status is set */
	if (!(inb(hdata->shm_base + NCT6694_SHM_COFS_STS) & NCT6694_SHM_COFS_STS_COFS4W))
		return IRQ_NONE;

	/* Clear interrupt status */
	outb(NCT6694_SHM_COFS_STS_COFS4W, hdata->shm_base + NCT6694_SHM_COFS_STS);

	ret = regmap_bulk_read(hdata->regmap, NCT6694_SHM_INTR_STATUS,
			       reg_data, ARRAY_SIZE(reg_data));
	if (ret)
		return IRQ_NONE;

	intr_status = get_unaligned_le32(reg_data);

	while (intr_status) {
		int hwirq = __ffs(intr_status);
		unsigned int virq = irq_find_mapping(nct6694->domain, hwirq);

		if (virq)
			generic_handle_irq_safe(virq);
		intr_status &= ~BIT(hwirq);
	}

	return IRQ_HANDLED;
}

static void nct6694_irq_release(void *data)
{
	struct nct6694 *nct6694 = data;
	struct nct6694_hif_data *hdata = nct6694->priv;
	unsigned char cofs_ctl2;

	/* Disable SIRQ interrupt */
	cofs_ctl2 = inb(hdata->shm_base + NCT6694_SHM_COFS_CTL2);
	cofs_ctl2 &= ~NCT6694_SHM_COFS_CTL2_COFS4W_IE;
	outb(cofs_ctl2, hdata->shm_base + NCT6694_SHM_COFS_CTL2);
}

static int nct6694_irq_init(struct nct6694 *nct6694, int irq)
{
	struct nct6694_hif_data *hdata = nct6694->priv;
	struct nct6694_sio_data *sio_data = hdata->sio_data;
	unsigned char cofs_ctl2;
	int ret;

	/* Set SIRQ number */
	ret = superio_enter(sio_data);
	if (ret)
		return ret;

	superio_select(sio_data, SIO_REG_LD_SHM);

	if (!superio_inb(sio_data, SIO_REG_SHM_ENABLE)) {
		superio_exit(sio_data);
		return -EIO;
	}

	hdata->shm_base = superio_inw(sio_data, SIO_REG_SHM_BASE_ADDR);

	superio_outb(sio_data, SIO_REG_SHM_IRQ_NR, irq);

	superio_exit(sio_data);

	/* Enable SIRQ interrupt */
	cofs_ctl2 = inb(hdata->shm_base + NCT6694_SHM_COFS_CTL2);
	cofs_ctl2 |= NCT6694_SHM_COFS_CTL2_COFS4W_IE;
	outb(cofs_ctl2, hdata->shm_base + NCT6694_SHM_COFS_CTL2);

	return 0;
}

static void nct6694_core_remove_action(void *data)
{
	struct nct6694 *nct6694 = data;

	nct6694_core_remove(nct6694);
}

static const u8 sio_addrs[] = { 0x2e, 0x4e };

static int nct6694_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nct6694_sio_data *sio_data;
	struct nct6694_hif_data *hdata;
	struct nct6694 *data;
	void __iomem *rpt_base, *msg_base;
	int ret, i, irq;

	sio_data = devm_kzalloc(dev, sizeof(*sio_data), GFP_KERNEL);
	if (!sio_data)
		return -ENOMEM;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	hdata = devm_kzalloc(dev, sizeof(*hdata), GFP_KERNEL);
	if (!hdata)
		return -ENOMEM;

	rpt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rpt_base))
		return PTR_ERR(rpt_base);
	msg_base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(msg_base))
		return PTR_ERR(msg_base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	for (i = 0; i < ARRAY_SIZE(sio_addrs); i++) {
		ret = nct6694_sio_find(sio_data, sio_addrs[i]);
		if (!ret)
			break;
	}
	if (ret)
		return ret;

	hdata->sio_data = sio_data;
	hdata->msg_base = msg_base;
	hdata->regmap = devm_regmap_init_mmio(dev, rpt_base, &nct6694_regmap_config);
	if (IS_ERR(hdata->regmap))
		return PTR_ERR(hdata->regmap);

	data->dev = dev;
	data->priv = hdata;
	data->read_msg = nct6694_hif_read_msg;
	data->write_msg = nct6694_hif_write_msg;

	ret = devm_mutex_init(dev, &hdata->msg_lock);
	if (ret)
		return ret;

	ret = nct6694_core_probe(dev, data, nct6694_hif_devs, ARRAY_SIZE(nct6694_hif_devs));
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, nct6694_core_remove_action, data);
	if (ret)
		return ret;

	ret = nct6694_irq_init(data, irq);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, nct6694_irq_release, data);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, irq, NULL, nct6694_irq_handler,
					IRQF_ONESHOT | IRQF_SHARED,
					dev_name(dev), data);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, data);

	return 0;
}

static const struct acpi_device_id nct6694_acpi_ids[] = {
	{ "NTN0538", 0 },
	{}
};

static struct platform_driver nct6694_driver = {
	.driver = {
		.name = DRVNAME,
		.acpi_match_table = nct6694_acpi_ids,
	},
	.probe	= nct6694_probe,
};
module_platform_driver(nct6694_driver);

MODULE_DESCRIPTION("Nuvoton NCT6694 host-interface transport driver");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
