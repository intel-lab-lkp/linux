// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Nuvoton NCT6694 host-interface (eSPI) transport driver.
 */

#include <linux/acpi.h>
#include <linux/bitfield.h>
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
#define SIO_SHM_IRQ_NR_MAX	15	/* Highest ISA interrupt line */

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

/* COFS register block [STS..CTL2] is the only SHM range driven via inb/outb */
#define NCT6694_SHM_COFS_LEN			\
	(NCT6694_SHM_COFS_CTL2 - NCT6694_SHM_COFS_STS + 1)

#define NCT6694_SHM_INTR_STATUS			0x9C	/* Interrupt status register (4 bytes) */

enum nct6694_chips {
	NCT6694B = 0,
	NCT6694D,
};

struct __packed nct6694_hif_msg {
	struct nct6694_cmd_header cmd_header;
	struct nct6694_response_header response_header;
	unsigned char data[];
};

struct nct6694_sio_data {
	enum nct6694_chips chip;
	int sioreg;	/* Super-I/O index port */
};

struct nct6694_hif_data {
	struct regmap *rpt_regmap;
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
		superio_exit(sio_data);
		return -ENODEV;
	}

	superio_exit(sio_data);

	return 0;
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

static int nct6694_hif_err_handling(struct nct6694 *nct6694, unsigned char err_status)
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

/*
 * @tx is const because regmap_bus->write() hands over the caller's buffer. A
 * GET command carries no request payload and passes @tx as NULL; callers that
 * need the response pass @rx, the others pass NULL.
 */
static int nct6694_hif_xfer_msg(struct nct6694 *nct6694,
				const struct nct6694_cmd_header *cmd_hd,
				u8 hctrl, const void *tx, void *rx)
{
	struct nct6694_hif_data *hdata = nct6694->priv;
	void __iomem *hdr = hdata->msg_base + offsetof(struct nct6694_hif_msg, cmd_header);
	void __iomem *payload = hdata->msg_base + offsetof(struct nct6694_hif_msg, data);
	struct nct6694_cmd_header cmd = *cmd_hd;
	struct nct6694_response_header resp;
	u16 len = le16_to_cpu(cmd.len);
	u16 resp_len;
	u8 status;
	int ret;

	if (len > NCT6694_MAX_PACKET_SIZE)
		return -EINVAL;

	/* Wait until the previous command is completed */
	ret = readb_poll_timeout(hdr + offsetof(struct nct6694_cmd_header, hctrl),
				 status, status == 0, NCT6694_POLL_INTERVAL_US,
				 NCT6694_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	/*
	 * Write cmd header fields, but skip hctrl - writing to it triggers
	 * firmware command processing and must be deferred until data is ready.
	 */
	memcpy_toio(hdr, &cmd, offsetof(struct nct6694_cmd_header, hctrl));
	memcpy_toio(hdr + offsetof(struct nct6694_cmd_header, rsv2), &cmd.rsv2,
		    sizeof(cmd) - offsetof(struct nct6694_cmd_header, rsv2));

	if (tx)
		memcpy_toio(payload, tx, len);

	/* Write hctrl last to trigger command processing */
	writeb(hctrl, hdr + offsetof(struct nct6694_cmd_header, hctrl));

	ret = readb_poll_timeout(hdr + offsetof(struct nct6694_cmd_header, hctrl),
				 status, status == 0, NCT6694_POLL_INTERVAL_US,
				 NCT6694_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	memcpy_fromio(&resp, hdata->msg_base + offsetof(struct nct6694_hif_msg, response_header),
		      sizeof(resp));

	ret = nct6694_hif_err_handling(nct6694, resp.sts);
	if (ret)
		return ret;

	if (!rx)
		return 0;

	/* The firmware may answer with fewer bytes than requested */
	resp_len = min(len, le16_to_cpu(resp.len));
	memcpy_fromio(rx, payload, resp_len);
	memset(rx + resp_len, 0, len - resp_len);

	return 0;
}

static int nct6694_hif_regmap_read(void *context, const void *reg_buf,
				   size_t reg_size, void *val_buf,
				   size_t val_size)
{
	struct nct6694 *nct6694 = context;
	struct nct6694_hif_data *hdata = nct6694->priv;
	u32 reg = get_unaligned_be32(reg_buf);
	u8 hctrl = FIELD_GET(NCT6694_REG_HCTRL, reg);
	const struct nct6694_cmd_header cmd_hd = {
		.mod = FIELD_GET(NCT6694_REG_MOD, reg),
		.offset = cpu_to_le16(FIELD_GET(NCT6694_REG_OFFSET, reg)),
		.len = cpu_to_le16(val_size),
	};

	if (cmd_hd.mod == NCT6694_RPT_MOD)
		return regmap_bulk_read(hdata->rpt_regmap,
					FIELD_GET(NCT6694_REG_OFFSET, reg),
					val_buf, val_size);

	return nct6694_hif_xfer_msg(nct6694, &cmd_hd, hctrl,
				    hctrl == NCT6694_HCTRL_SET ? val_buf : NULL,
				    val_buf);
}

static int nct6694_hif_regmap_write(void *context, const void *data,
				    size_t count)
{
	struct nct6694 *nct6694 = context;
	struct nct6694_hif_data *hdata = nct6694->priv;
	u32 reg = get_unaligned_be32(data);
	size_t len = count - sizeof(reg);
	const struct nct6694_cmd_header cmd_hd = {
		.mod = FIELD_GET(NCT6694_REG_MOD, reg),
		.offset = cpu_to_le16(FIELD_GET(NCT6694_REG_OFFSET, reg)),
		.len = cpu_to_le16(len),
	};

	if (cmd_hd.mod == NCT6694_RPT_MOD)
		return regmap_bulk_write(hdata->rpt_regmap,
					 FIELD_GET(NCT6694_REG_OFFSET, reg),
					 data + sizeof(reg), len);

	return nct6694_hif_xfer_msg(nct6694, &cmd_hd, NCT6694_HCTRL_SET,
				    data + sizeof(reg), NULL);
}

static const struct regmap_bus nct6694_hif_regmap_bus = {
	.read = nct6694_hif_regmap_read,
	.write = nct6694_hif_regmap_write,
};

static const struct regmap_config nct6694_hif_msg_regmap_config = {
	.name = "msg",
	.reg_bits = 32,
	.val_bits = 8,
	.reg_stride = 1,
	.max_raw_read = NCT6694_MAX_PACKET_SIZE,
	.max_raw_write = NCT6694_MAX_PACKET_SIZE,
};

static const struct regmap_config nct6694_hif_rpt_regmap_config = {
	.name = "rpt",
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
};

static irqreturn_t nct6694_hif_irq_handler(int irq, void *data)
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

	ret = regmap_bulk_read(hdata->rpt_regmap, NCT6694_SHM_INTR_STATUS,
			       reg_data, ARRAY_SIZE(reg_data));
	if (ret)
		return IRQ_HANDLED;

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

static void nct6694_hif_irq_disable(void *data)
{
	struct nct6694 *nct6694 = data;
	struct nct6694_hif_data *hdata = nct6694->priv;
	u8 cofs_ctl2;

	/* Disable SIRQ interrupt */
	cofs_ctl2 = inb(hdata->shm_base + NCT6694_SHM_COFS_CTL2);
	cofs_ctl2 &= ~NCT6694_SHM_COFS_CTL2_COFS4W_IE;
	outb(cofs_ctl2, hdata->shm_base + NCT6694_SHM_COFS_CTL2);
}

static void nct6694_hif_irq_enable(struct nct6694 *nct6694)
{
	struct nct6694_hif_data *hdata = nct6694->priv;
	u8 cofs_ctl2;

	/* Enable SIRQ interrupt */
	cofs_ctl2 = inb(hdata->shm_base + NCT6694_SHM_COFS_CTL2);
	cofs_ctl2 |= NCT6694_SHM_COFS_CTL2_COFS4W_IE;
	outb(cofs_ctl2, hdata->shm_base + NCT6694_SHM_COFS_CTL2);
}

static int nct6694_hif_irq_init(struct nct6694 *nct6694, int irq)
{
	struct nct6694_hif_data *hdata = nct6694->priv;
	struct nct6694_sio_data *sio_data = hdata->sio_data;
	struct irq_data *irq_data;
	irq_hw_number_t hwirq;
	int ret;

	/* The chip is programmed with the SIRQ line, not the Linux irq number */
	irq_data = irq_get_irq_data(irq);
	if (!irq_data)
		return -EINVAL;

	hwirq = irqd_to_hwirq(irq_data);
	if (hwirq > SIO_SHM_IRQ_NR_MAX)
		return -ERANGE;

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
	if (!hdata->shm_base) {
		superio_exit(sio_data);
		return -ENODEV;
	}

	superio_outb(sio_data, SIO_REG_SHM_IRQ_NR, hwirq);

	superio_exit(sio_data);

	if (!devm_request_region(nct6694->dev,
				 hdata->shm_base + NCT6694_SHM_COFS_STS,
				 NCT6694_SHM_COFS_LEN, DRVNAME))
		return -EBUSY;

	/* Keep the device quiet until the IRQ domain is ready */
	nct6694_hif_irq_disable(nct6694);
	outb(NCT6694_SHM_COFS_STS_COFS4W, hdata->shm_base + NCT6694_SHM_COFS_STS);

	return 0;
}

static void nct6694_hif_core_remove_action(void *data)
{
	struct nct6694 *nct6694 = data;

	nct6694_core_remove(nct6694);
}

static const u8 sio_addrs[] = { 0x2e, 0x4e };

static int nct6694_hif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nct6694_sio_data *sio_data;
	struct nct6694_hif_data *hdata;
	struct nct6694 *nct6694;
	void __iomem *rpt_base, *msg_base;
	int ret, i, irq;

	rpt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rpt_base))
		return PTR_ERR(rpt_base);

	msg_base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(msg_base))
		return PTR_ERR(msg_base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	sio_data = devm_kzalloc(dev, sizeof(*sio_data), GFP_KERNEL);
	if (!sio_data)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(sio_addrs); i++) {
		if (!nct6694_sio_find(sio_data, sio_addrs[i]))
			break;
	}
	if (i == ARRAY_SIZE(sio_addrs))
		return -ENODEV;

	dev_dbg(dev, "Found %s at %#x\n", nct6694_chip_names[sio_data->chip], sio_data->sioreg);

	nct6694 = devm_kzalloc(dev, sizeof(*nct6694), GFP_KERNEL);
	if (!nct6694)
		return -ENOMEM;

	hdata = devm_kzalloc(dev, sizeof(*hdata), GFP_KERNEL);
	if (!hdata)
		return -ENOMEM;

	hdata->sio_data = sio_data;
	hdata->msg_base = msg_base;
	hdata->rpt_regmap = devm_regmap_init_mmio(dev, rpt_base,
						  &nct6694_hif_rpt_regmap_config);
	if (IS_ERR(hdata->rpt_regmap))
		return PTR_ERR(hdata->rpt_regmap);

	nct6694->dev = dev;
	nct6694->priv = hdata;
	nct6694->regmap = devm_regmap_init(dev, &nct6694_hif_regmap_bus, nct6694,
					   &nct6694_hif_msg_regmap_config);
	if (IS_ERR(nct6694->regmap))
		return PTR_ERR(nct6694->regmap);

	ret = nct6694_hif_irq_init(nct6694, irq);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, nct6694);

	/* Request the interrupt first so that devres releases it last */
	ret = devm_request_threaded_irq(dev, irq, NULL, nct6694_hif_irq_handler,
					IRQF_ONESHOT | IRQF_SHARED,
					dev_name(dev), nct6694);
	if (ret)
		return ret;

	ret = nct6694_core_probe(dev, nct6694, nct6694_hif_devs,
				 ARRAY_SIZE(nct6694_hif_devs));
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, nct6694_hif_core_remove_action, nct6694);
	if (ret)
		return ret;

	nct6694_hif_irq_enable(nct6694);

	return devm_add_action_or_reset(dev, nct6694_hif_irq_disable, nct6694);
}

static const struct acpi_device_id nct6694_hif_acpi_ids[] = {
	{ "NTN0538", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, nct6694_hif_acpi_ids);

static struct platform_driver nct6694_hif_driver = {
	.driver = {
		.name = DRVNAME,
		.acpi_match_table = nct6694_hif_acpi_ids,
	},
	.probe	= nct6694_hif_probe,
};
module_platform_driver(nct6694_hif_driver);

MODULE_DESCRIPTION("Nuvoton NCT6694 host-interface transport driver");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
