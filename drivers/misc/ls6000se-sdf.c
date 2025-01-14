// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2025 Loongson Technology Corporation Limited */

#include <linux/init.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mfd/ls6000se.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define SE_SDF_BUFSIZE	(PAGE_SIZE * 2)

struct sdf_dev {
	struct miscdevice miscdev;
	struct lsse_ch *se_ch;
	struct completion sdf_completion;
};

struct sdf_msg {
	u32 cmd;
	u32 data_off;
	u32 data_len;
	u32 pad[5];
};

static void sdf_complete(struct lsse_ch *ch)
{
	struct sdf_dev *sdf = ch->priv;

	complete(&sdf->sdf_completion);
}

static int send_sdf_cmd(struct sdf_dev *sdf, int len)
{
	struct sdf_msg *smsg = sdf->se_ch->smsg;

	smsg->data_len = len;

	return se_send_ch_requeset(sdf->se_ch);
}

static ssize_t sdf_read(struct file *file, char __user *buf,
			size_t cnt, loff_t *offt)
{
	struct sdf_dev *sdf = container_of(file->private_data,
					   struct sdf_dev, miscdev);
	struct sdf_msg *rmsg;

	if (!wait_for_completion_timeout(&sdf->sdf_completion, HZ*5))
		return -ETIME;

	rmsg = (struct sdf_msg *)sdf->se_ch->rmsg;
	if (copy_to_user(buf,
			 sdf->se_ch->data_buffer + rmsg->data_off, rmsg->data_len))
		return -EFAULT;

	return rmsg->data_len;
}

static ssize_t sdf_write(struct file *file, const char __user *buf,
			 size_t cnt, loff_t *offt)
{
	struct sdf_dev *sdf = container_of(file->private_data,
					   struct sdf_dev, miscdev);
	int ret;

	if (copy_from_user(sdf->se_ch->data_buffer, buf, cnt))
		return -EFAULT;

	ret = send_sdf_cmd(sdf, cnt);

	return ret ? -EFAULT : cnt;
}

static const struct file_operations sdf_fops = {
	.owner = THIS_MODULE,
	.write = sdf_write,
	.read = sdf_read,
};

static int sdf_probe(struct platform_device *pdev)
{
	struct sdf_msg *smsg;
	struct sdf_dev *sdf;
	static int idx;

	sdf = devm_kzalloc(&pdev->dev, sizeof(*sdf), GFP_KERNEL);
	if (!sdf)
		return -ENOMEM;
	init_completion(&sdf->sdf_completion);

	sdf->se_ch = se_init_ch(pdev->dev.parent, SE_CH_SDF, SE_SDF_BUFSIZE,
				sizeof(struct sdf_msg) * 2, sdf, sdf_complete);
	smsg = sdf->se_ch->smsg;
	smsg->cmd = SE_CMD_SDF;
	smsg->data_off = sdf->se_ch->off;
	sdf->miscdev.minor = MISC_DYNAMIC_MINOR;
	sdf->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					   "lsse_sdf%d", idx++);
	sdf->miscdev.fops = &sdf_fops;

	return misc_register(&sdf->miscdev);
}

static struct platform_driver loongson_sdf_driver = {
	.probe	= sdf_probe,
	.driver  = {
		.name  = "ls6000se-sdf",
	},
};
module_platform_driver(loongson_sdf_driver);

MODULE_ALIAS("platform:ls6000se-sdf");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yinggang Gu <guyinggang@loongson.cn>");
MODULE_AUTHOR("Qunqin Zhao <zhaoqunqin@loongson.cn>");
MODULE_DESCRIPTION("Loongson Secure Device Function driver");
