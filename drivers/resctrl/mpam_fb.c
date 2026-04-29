// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 Arm Ltd.

#include <linux/arm_mpam.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/iopoll.h>
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

#define SCMI_CHAN_RSVD_OFS	0x00
#define SCMI_CHAN_STATUS_OFS	0x04
#define SCMI_CHAN_STATUS_FREE_BIT	BIT(0)
#define SCMI_CHAN_FLAGS_OFS	0x10
#define SCMI_CHAN_FLAGS_IRQ		BIT(0)
#define SCMI_MSG_LENGTH_OFS	0x14
#define SCMI_MSG_HEADER_OFS	0x18
#define SCMI_MSG_PAYLOAD_OFS	0x1c

#define MPAM_READ_MSG_SIZE	(SCMI_MSG_PAYLOAD_OFS + 3 * sizeof(u32))
#define MPAM_WRITE_MSG_SIZE	(SCMI_MSG_PAYLOAD_OFS + 4 * sizeof(u32))

static atomic_t mpam_fb_token = ATOMIC_INIT(0);

static int mpam_fb_build_read_message(int msc_id, int reg, unsigned int token,
				      void __iomem *msg_buf)
{
	writel_relaxed(SCMI_CHAN_FLAGS_IRQ, msg_buf + SCMI_CHAN_FLAGS_OFS);
	writel_relaxed(MPAM_READ_MSG_SIZE, msg_buf + SCMI_MSG_LENGTH_OFS);
	writel_relaxed(MPAM_MSC_READ_CMD |
		       FIELD_PREP(MPAM_MSC_TOKEN_MASK, token) |
		       FIELD_PREP(MPAM_MSC_PROT_ID_MASK, MPAM_FB_PROTOCOL_ID),
		       msg_buf + SCMI_MSG_HEADER_OFS);

	writel_relaxed(cpu_to_le32(msc_id), msg_buf + SCMI_MSG_PAYLOAD_OFS);
	writel_relaxed(0, msg_buf + SCMI_MSG_PAYLOAD_OFS + 0x4);
	writel_relaxed(cpu_to_le32(reg), msg_buf + SCMI_MSG_PAYLOAD_OFS + 0x8);

	return MPAM_READ_MSG_SIZE;
}

static int mpam_fb_build_write_message(int msc_id, int reg, u32 val,
				       unsigned int token,
				       void __iomem *msg_buf)
{
	writel_relaxed(MPAM_WRITE_MSG_SIZE, msg_buf + SCMI_MSG_LENGTH_OFS);
	writel_relaxed(MPAM_MSC_WRITE_CMD |
		       FIELD_PREP(MPAM_MSC_TOKEN_MASK, token) |
		       FIELD_PREP(MPAM_MSC_PROT_ID_MASK, MPAM_FB_PROTOCOL_ID),
		       msg_buf + SCMI_MSG_HEADER_OFS);

	writel_relaxed(cpu_to_le32(msc_id), msg_buf + SCMI_MSG_PAYLOAD_OFS);
	writel_relaxed(0, msg_buf + SCMI_MSG_PAYLOAD_OFS + 0x4);
	writel_relaxed(cpu_to_le32(reg), msg_buf + SCMI_MSG_PAYLOAD_OFS + 0x8);
	writel_relaxed(cpu_to_le32(val), msg_buf + SCMI_MSG_PAYLOAD_OFS + 0xc);

	return MPAM_WRITE_MSG_SIZE;
}

#define SCMI_CHANNEL_FREE	true
#define SCMI_CHANNEL_BUSY	false
static int mpam_fb_wait_for_channel(struct pcc_mbox_chan *chan,
				    bool free)
{
	u32 status = free ? SCMI_CHAN_STATUS_FREE_BIT : 0;
	u32 val;

	/*
	 * The channel should really be free always at this point, as we take
	 * a lock for every read or write request. Check the free bit anyway,
	 * for good measure and to catch corner cases.
	 */
	return readl_poll_timeout(chan->shmem + SCMI_CHAN_STATUS_OFS, val,
				  (val & SCMI_CHAN_STATUS_FREE_BIT) == status,
				  1, 10000);
}

static int mpam_fb_send_request(struct mpam_msc *msc, u16 reg, u32 *result,
				bool is_write)
{
	unsigned int token = atomic_inc_return(&mpam_fb_token);
	struct pcc_mbox_chan *chan = msc->pcc_chan;
	u32 status;
	int ret;

	guard(mutex)(&msc->pcc_chan_lock);
	ret = mpam_fb_wait_for_channel(chan, SCMI_CHANNEL_FREE);
	if (ret < 0)
		return ret;

	/* Clear error bit and mark the channel as belonging to the callee */
	writel(0, chan->shmem + SCMI_CHAN_STATUS_OFS);

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

	ret = mpam_fb_wait_for_channel(chan, SCMI_CHANNEL_FREE);
	if (ret)
		return ret;

	status = readl(chan->shmem + SCMI_MSG_HEADER_OFS);
	if (FIELD_GET(MPAM_MSC_TOKEN_MASK, status) != token)
		return -ETIMEDOUT;

	ret = readl(chan->shmem + SCMI_MSG_PAYLOAD_OFS + 0x0);
	if (ret < 0)
		return ret;

	if (!is_write)
		*result = readl(chan->shmem + SCMI_MSG_PAYLOAD_OFS + 0x4);

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
