// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Loongson Technology Corporation Limited. */

#include <linux/device.h>
#include <linux/mfd/ls6000se.h>
#include <linux/platform_device.h>
#include <linux/wait.h>

#include "tpm.h"

struct tpm_msg {
	u32 cmd;
	u32 data_off;
	u32 data_len;
	u32 info[5];
};

struct tpm_dev {
	struct lsse_ch *se_ch;
	struct completion tpm_completion;
};

static void tpm_complete(struct lsse_ch *ch)
{
	struct tpm_dev *td = ch->priv;

	complete(&td->tpm_completion);
}

static int tpm_ls_recv(struct tpm_chip *chip, u8 *buf, size_t count)
{
	struct tpm_dev *td = dev_get_drvdata(&chip->dev);
	struct tpm_msg *rmsg;
	int sig;

	sig = wait_for_completion_interruptible(&td->tpm_completion);
	if (sig)
		return sig;

	rmsg = td->se_ch->rmsg;
	memcpy(buf, td->se_ch->data_buffer, rmsg->data_len);

	return rmsg->data_len;
}

static int tpm_ls_send(struct tpm_chip *chip, u8 *buf, size_t count)
{
	struct tpm_dev *td = dev_get_drvdata(&chip->dev);
	struct tpm_msg *smsg = td->se_ch->smsg;

	memcpy(td->se_ch->data_buffer, buf, count);
	smsg->data_len = count;

	return se_send_ch_requeset(td->se_ch);
}

static const struct tpm_class_ops lsse_tpm_ops = {
	.flags = TPM_OPS_AUTO_STARTUP,
	.recv = tpm_ls_recv,
	.send = tpm_ls_send,
};

static int lsse_tpm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tpm_chip *chip;
	struct tpm_msg *smsg;
	struct tpm_dev *td;

	td = devm_kzalloc(dev, sizeof(struct tpm_dev), GFP_KERNEL);
	if (!td)
		return -ENOMEM;

	init_completion(&td->tpm_completion);
	td->se_ch = se_init_ch(dev->parent, SE_CH_TPM, PAGE_SIZE,
			       2 * sizeof(struct tpm_msg), td, tpm_complete);
	if (!td->se_ch)
		return -ENODEV;
	smsg = td->se_ch->smsg;
	smsg->cmd = SE_CMD_TPM;
	smsg->data_off = td->se_ch->off;

	chip = tpmm_chip_alloc(dev, &lsse_tpm_ops);
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	chip->flags = TPM_CHIP_FLAG_TPM2 | TPM_CHIP_FLAG_IRQ;
	dev_set_drvdata(&chip->dev, td);

	return tpm_chip_register(chip);
}

static struct platform_driver lsse_tpm_driver = {
	.probe   = lsse_tpm_probe,
	.driver  = {
		.name  = "ls6000se-tpm",
	},
};
module_platform_driver(lsse_tpm_driver);

MODULE_ALIAS("platform:ls6000se-tpm");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yinggang Gu <guyinggang@loongson.cn>");
MODULE_AUTHOR("Qunqin Zhao <zhaoqunqin@loongson.cn>");
MODULE_DESCRIPTION("Loongson TPM driver");
