// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 Arm Ltd.

#include <linux/arm_mpam.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/mailbox_client.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <acpi/pcc.h>

#include <asm/mpam.h>

#include "mpam_fb.h"

#define MPAM_FB_PROTOCOL_ID	0x1a
#define MPAM_MSC_ATTRIBUTES_CMD	0x3
#define MPAM_MSC_READ_CMD	0x4
#define MPAM_MSC_WRITE_CMD	0x5

#define MPAM_MSC_PROT_ID_MASK	GENMASK(17, 10)
#define MPAM_MSC_TOKEN_MASK	GENMASK(27, 18)

#define PCC_CHAN_FLAGS_IRQ	BIT(0)
#define MPAM_READ_MSG_SIZE	(PCC_TYPE3_MSG_PAYLOAD_OFS + 3 * sizeof(u32))
#define MPAM_WRITE_MSG_SIZE	(PCC_TYPE3_MSG_PAYLOAD_OFS + 4 * sizeof(u32))

static atomic_t mpam_fb_token = ATOMIC_INIT(0);

static int mpam_fb_build_read_message(int msc_id, int reg, unsigned int token,
				      void __iomem *msg_buf)
{
	struct acpi_pcct_ext_pcc_shared_memory *pcc_shmem = msg_buf;
	void __iomem *payload_ofs = msg_buf + sizeof(*pcc_shmem);

	writel_relaxed(PCC_CHAN_FLAGS_IRQ, &pcc_shmem->flags);
	writel_relaxed(MPAM_READ_MSG_SIZE, &pcc_shmem->length);
	writel_relaxed(MPAM_MSC_READ_CMD |
		       FIELD_PREP(MPAM_MSC_TOKEN_MASK, token) |
		       FIELD_PREP(MPAM_MSC_PROT_ID_MASK, MPAM_FB_PROTOCOL_ID),
		       &pcc_shmem->command);

	writel_relaxed(cpu_to_le32(msc_id), payload_ofs + 0x0);
	writel_relaxed(0, payload_ofs + 0x4);
	writel_relaxed(cpu_to_le32(reg), payload_ofs + 0x8);

	return MPAM_READ_MSG_SIZE;
}

static int mpam_fb_build_write_message(int msc_id, int reg, u32 val,
				       unsigned int token,
				       void __iomem *msg_buf)
{
	struct acpi_pcct_ext_pcc_shared_memory *pcc_shmem = msg_buf;
	void __iomem *payload_ofs = msg_buf + sizeof(*pcc_shmem);

	writel_relaxed(MPAM_WRITE_MSG_SIZE, &pcc_shmem->length);
	writel_relaxed(MPAM_MSC_WRITE_CMD |
		       FIELD_PREP(MPAM_MSC_TOKEN_MASK, token) |
		       FIELD_PREP(MPAM_MSC_PROT_ID_MASK, MPAM_FB_PROTOCOL_ID),
		       &pcc_shmem->command);

	writel_relaxed(cpu_to_le32(msc_id), payload_ofs + 0x0);
	writel_relaxed(0, payload_ofs + 0x4);
	writel_relaxed(cpu_to_le32(reg), payload_ofs + 0x8);
	writel_relaxed(cpu_to_le32(val), payload_ofs + 0xc);

	return MPAM_WRITE_MSG_SIZE;
}

static int mpam_fb_send_request(struct mpam_msc *msc, u16 reg, u32 *result,
				bool is_write)
{
	unsigned int token = atomic_inc_return(&mpam_fb_token);
	struct acpi_pcct_ext_pcc_shared_memory *pcc_shmem;
	struct mpam_pcc_chan *pcc_chan = msc->pcc_chan;
	struct pcc_mbox_chan *chan;
	void __iomem *payload_ofs;
	u32 status;
	int ret;

	if (!pcc_chan)
		return -ENODEV;

	chan = pcc_chan->pcc_chan;

	guard(mutex)(&pcc_chan->pcc_chan_lock);

	if (is_write)
		ret = mpam_fb_build_write_message(msc->mpam_fb_msc_id, reg,
						  *result, token, chan->shmem);
	else
		ret = mpam_fb_build_read_message(msc->mpam_fb_msc_id, reg,
						 token, chan->shmem);
	if (ret < 0)
		return ret;

	ret = mbox_send_message(chan->mchan, NULL);
	if (ret < 0)
		return ret;

	pcc_shmem = chan->shmem;
	payload_ofs = chan->shmem + sizeof(*pcc_shmem);
	status = readl(&pcc_shmem->command);
	if (FIELD_GET(MPAM_MSC_TOKEN_MASK, status) != token)
		return -ETIMEDOUT;

	ret = readl(payload_ofs + 0x0);
	if (ret < 0)
		return ret;

	if (!is_write)
		*result = readl(payload_ofs + 0x4);

	return 0;
}

int mpam_fb_send_read_request(struct mpam_msc *msc, u16 reg, u32 *result)
{
	return mpam_fb_send_request(msc, reg, result, false);
}

int mpam_fb_send_write_request(struct mpam_msc *msc, u16 reg, u32 value)
{
	return mpam_fb_send_request(msc, reg, &value, true);
}
