// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nuvoton NPCM7xx OTP (One-Time Programmable) NVMEM driver
 *
 * Copyright (C) 2026 Kuan-Wei Chiu <visitorckw@gmail.com>
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* Register offsets and bitmasks */
#define NPCM_OTP_FST		0x00
#define NPCM_OTP_FADDR		0x04
#define NPCM_OTP_FDATA		0x08
#define NPCM_OTP_FCTL		0x14

#define FST_RDY			BIT(0)
#define FST_RDST		BIT(1)
#define FCTL_READ_CMD		0x02

/* OTP total capacity is 8192 bits (1024 Bytes) */
#define NPCM_OTP_SIZE		1024

struct npcm_otp {
	void __iomem *base;
	struct mutex lock; /* protects concurrent OTP accesses */
};

static int npcm_otp_read_byte(struct npcm_otp *otp, unsigned int offset, u8 *val)
{
	u32 fst;
	int ret;

	writel(offset, otp->base + NPCM_OTP_FADDR);
	writel(FCTL_READ_CMD, otp->base + NPCM_OTP_FCTL);

	ret = readl_poll_timeout(otp->base + NPCM_OTP_FST, fst,
				 (fst & FST_RDY), 10, 10000);
	if (ret)
		return ret;

	*val = (u8)(readl(otp->base + NPCM_OTP_FDATA) & 0xFF);

	/* Clear the status bit to prepare for the next read */
	writel(FST_RDST, otp->base + NPCM_OTP_FST);

	return 0;
}

static int npcm_otp_read(void *context, unsigned int offset,
			 void *val, size_t bytes)
{
	struct npcm_otp *otp = context;
	u8 *buf = val;
	int ret = 0;
	size_t i;

	mutex_lock(&otp->lock);

	for (i = 0; i < bytes; i++) {
		ret = npcm_otp_read_byte(otp, offset + i, &buf[i]);
		if (ret)
			break;
	}

	mutex_unlock(&otp->lock);

	return ret;
}

static int npcm_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct npcm_otp *otp;
	struct nvmem_config config = { 0 };
	struct nvmem_device *nvmem;

	otp = devm_kzalloc(dev, sizeof(*otp), GFP_KERNEL);
	if (!otp)
		return -ENOMEM;

	otp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(otp->base))
		return PTR_ERR(otp->base);

	mutex_init(&otp->lock);

	config.dev = dev;
	config.name = dev_name(dev);
	config.read_only = true;
	config.word_size = 1;
	config.stride = 1;
	config.reg_read = npcm_otp_read;
	config.priv = otp;
	config.size = NPCM_OTP_SIZE;

	nvmem = devm_nvmem_register(dev, &config);
	if (IS_ERR(nvmem))
		return dev_err_probe(dev, PTR_ERR(nvmem), "Failed to register nvmem\n");

	return 0;
}

static const struct of_device_id npcm_otp_dt_ids[] = {
	{ .compatible = "nuvoton,npcm750-key-storage" },
	{ .compatible = "nuvoton,npcm750-fuse-array" },
	{ }
};
MODULE_DEVICE_TABLE(of, npcm_otp_dt_ids);

static struct platform_driver npcm_otp_driver = {
	.probe		= npcm_otp_probe,
	.driver		= {
		.name	= "npcm-otp",
		.of_match_table = npcm_otp_dt_ids,
	},
};
module_platform_driver(npcm_otp_driver);

MODULE_AUTHOR("Kuan-Wei Chiu <visitorckw@gmail.com>");
MODULE_DESCRIPTION("Nuvoton NPCM7xx OTP NVMEM driver");
MODULE_LICENSE("GPL");
