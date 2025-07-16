// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Mailbox communication utilities for command creation
 * and message exchange with the MCU
 *
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 */

#include <asm-generic/errno.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "al_codec_util.h"

#if defined(CONFIG_DEBUG_FS)
/* Log level */
int al_v4l2_dbg_level;
int al_codec_dbg;
#endif

static int al_get_used_space(struct al_codec_mb *mb)
{
	u32 head = mb->hdr->head;
	u32 tail = mb->hdr->tail;

	return head >= tail ? head - tail : mb->size - (tail - head);
}

static int al_get_free_space(struct al_codec_mb *mb)
{
	return mb->size - al_get_used_space(mb) - 1;
}

static int al_has_enough_space(struct al_codec_mb *mb, int len)
{
	return al_get_free_space(mb) >= len;
}

static inline void al_copy_to_mb(struct al_codec_mb *mb, char *data, int len)
{
	u32 head = mb->hdr->head;
	int copy_len = min(mb->size - head, (unsigned int)len);
	int copied_len = len;

	memcpy(&mb->data[head], data, copy_len);
	len -= copy_len;
	if (len)
		memcpy(&mb->data[0], &data[copy_len], len);

	/* Make sure that all messages are written before updating the head */
	dma_wmb();
	mb->hdr->head = (head + copied_len) % mb->size;
	/* Make sure that the head is updated in DDR instead of cache */
	dma_wmb();
}

static inline void al_copy_from_mb(struct al_codec_mb *mb, char *data, int len)
{
	u32 tail = mb->hdr->tail;
	int copy_len = min(mb->size - tail, (unsigned int)len);
	int copied_len = len;

	if (!data)
		goto update_tail;

	memcpy(data, &mb->data[tail], copy_len);
	len -= copy_len;
	if (len)
		memcpy(&data[copy_len], &mb->data[0], len);

update_tail:
	mb->hdr->tail = (tail + copied_len) % mb->size;
	/* Make sure that the head is updated in DDR instead of cache */
	dma_wmb();
}

static int al_codec_mb_send(struct al_codec_mb *mb, char *data, int len)
{
	if (!al_has_enough_space(mb, len))
		return -ENOMEM;

	al_copy_to_mb(mb, data, len);

	return 0;
}

static int al_codec_mb_receive(struct al_codec_mb *mb, char *data, int len)
{
	if (al_get_used_space(mb) < len)
		return -ENOMEM;

	al_copy_from_mb(mb, data, len);

	return 0;
}

void al_codec_mb_init(struct al_codec_mb *mb, char *addr, int size, u32 magic)
{
	mb->hdr = (struct al_mb_itf *)addr;
	mb->hdr->magic = magic;
	mb->hdr->version = MB_IFT_VERSION;
	mb->hdr->head = 0;
	mb->hdr->tail = 0;
	mb->data = addr + sizeof(struct al_mb_itf);
	mb->size = size - sizeof(struct al_mb_itf);
	mutex_init(&mb->lock);
}

int al_codec_msg_get_header(struct al_codec_mb *mb, struct msg_itf_header *hdr)
{
	return al_codec_mb_receive(mb, (char *)hdr, sizeof(*hdr));
}

int al_codec_msg_get_data(struct al_codec_mb *mb, char *data, int len)
{
	return al_codec_mb_receive(mb, data, len);
}

int al_codec_msg_send(struct al_codec_mb *mb, struct msg_itf_header *hdr,
		      void (*trigger)(void *), void *trigger_arg)
{
	const unsigned long timeout = jiffies + HZ;
	int ret;

	guard(mutex)(&mb->lock);
	do {
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;

		ret = al_codec_mb_send(mb, (char *)hdr,
				       hdr->payload_len +
					       sizeof(struct msg_itf_header));

	} while (ret);

	trigger(trigger_arg);

	return 0;
}

static void al_codec_cmd_cleanup(struct kref *ref)
{
	struct al_codec_cmd *cmd = container_of(ref, typeof(*cmd), refcount);

	kfree(cmd->reply);
	kfree(cmd);
}

void al_codec_cmd_put(struct al_codec_cmd *cmd)
{
	if (WARN_ON(!cmd))
		return;

	kref_put(&cmd->refcount, al_codec_cmd_cleanup);
}

struct al_codec_cmd *al_codec_cmd_create(int reply_size)
{
	struct al_codec_cmd *cmd;

	cmd = kmalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return NULL;

	cmd->reply = kmalloc(reply_size, GFP_KERNEL);
	if (!cmd->reply) {
		kfree(cmd);
		return NULL;
	}

	kref_init(&cmd->refcount);
	cmd->reply_size = reply_size;
	init_completion(&cmd->done);

	return cmd;
}
