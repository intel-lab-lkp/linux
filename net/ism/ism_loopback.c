// SPDX-License-Identifier: GPL-2.0
/*
 *  Functions for loopback-ism device.
 *
 *  Copyright (c) 2024, Alibaba Inc.
 *
 *  Author: Wen Gu <guwen@linux.alibaba.com>
 *          Tony Lu <tonylu@linux.alibaba.com>
 *
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/ism.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "ism_loopback.h"

#define ISM_LO_V2_CAPABLE	0x1 /* loopback-ism acts as ISMv2 */
#define ISM_LO_SUPPORT_NOCOPY	0x1
#define ISM_DMA_ADDR_INVALID	(~(dma_addr_t)0)

static const char ism_lo_dev_name[] = "loopback-ism";
/* global loopback device */
static struct ism_lo_dev *lo_dev;

static int ism_lo_query_rgid(struct ism_dev *ism, uuid_t *rgid,
			     u32 vid_valid, u32 vid)
{
	/* rgid should be the same as lgid; vlan is not supported */
	if (!vid_valid && uuid_equal(rgid, &ism->gid))
		return 0;
	return -ENETUNREACH;
}

static int ism_lo_register_dmb(struct ism_dev *ism, struct ism_dmb *dmb,
			       struct ism_client *client)
{
	struct ism_lo_dmb_node *dmb_node, *tmp_node;
	struct ism_lo_dev *ldev;
	unsigned long flags;
	int sba_idx, rc;

	ldev = container_of(ism, struct ism_lo_dev, ism);
	sba_idx = dmb->sba_idx;
	/* check space for new dmb */
	for_each_clear_bit(sba_idx, ldev->sba_idx_mask, ISM_LO_MAX_DMBS) {
		if (!test_and_set_bit(sba_idx, ldev->sba_idx_mask))
			break;
	}
	if (sba_idx == ISM_LO_MAX_DMBS)
		return -ENOSPC;

	dmb_node = kzalloc(sizeof(*dmb_node), GFP_KERNEL);
	if (!dmb_node) {
		rc = -ENOMEM;
		goto err_bit;
	}

	dmb_node->sba_idx = sba_idx;
	dmb_node->len = dmb->dmb_len;
	dmb_node->cpu_addr = kzalloc(dmb_node->len, GFP_KERNEL |
				     __GFP_NOWARN | __GFP_NORETRY |
				     __GFP_NOMEMALLOC);
	if (!dmb_node->cpu_addr) {
		rc = -ENOMEM;
		goto err_node;
	}
	dmb_node->dma_addr = ISM_DMA_ADDR_INVALID;
	refcount_set(&dmb_node->refcnt, 1);

again:
	/* add new dmb into hash table */
	get_random_bytes(&dmb_node->token, sizeof(dmb_node->token));
	write_lock_bh(&ldev->dmb_ht_lock);
	hash_for_each_possible(ldev->dmb_ht, tmp_node, list, dmb_node->token) {
		if (tmp_node->token == dmb_node->token) {
			write_unlock_bh(&ldev->dmb_ht_lock);
			goto again;
		}
	}
	hash_add(ldev->dmb_ht, &dmb_node->list, dmb_node->token);
	write_unlock_bh(&ldev->dmb_ht_lock);
	atomic_inc(&ldev->dmb_cnt);

	dmb->sba_idx = dmb_node->sba_idx;
	dmb->dmb_tok = dmb_node->token;
	dmb->cpu_addr = dmb_node->cpu_addr;
	dmb->dma_addr = dmb_node->dma_addr;
	dmb->dmb_len = dmb_node->len;

	spin_lock_irqsave(&ism->lock, flags);
	ism->sba_client_arr[sba_idx] = client->id;
	spin_unlock_irqrestore(&ism->lock, flags);

	return 0;

err_node:
	kfree(dmb_node);
err_bit:
	clear_bit(sba_idx, ldev->sba_idx_mask);
	return rc;
}

static void __ism_lo_unregister_dmb(struct ism_lo_dev *ldev,
				    struct ism_lo_dmb_node *dmb_node)
{
	/* remove dmb from hash table */
	write_lock_bh(&ldev->dmb_ht_lock);
	hash_del(&dmb_node->list);
	write_unlock_bh(&ldev->dmb_ht_lock);

	clear_bit(dmb_node->sba_idx, ldev->sba_idx_mask);
	kvfree(dmb_node->cpu_addr);
	kfree(dmb_node);

	if (atomic_dec_and_test(&ldev->dmb_cnt))
		wake_up(&ldev->ldev_release);
}

static int ism_lo_unregister_dmb(struct ism_dev *ism, struct ism_dmb *dmb)
{
	struct ism_lo_dmb_node *dmb_node = NULL, *tmp_node;
	struct ism_lo_dev *ldev;
	unsigned long flags;

	ldev = container_of(ism, struct ism_lo_dev, ism);

	/* find dmb from hash table */
	read_lock_bh(&ldev->dmb_ht_lock);
	hash_for_each_possible(ldev->dmb_ht, tmp_node, list, dmb->dmb_tok) {
		if (tmp_node->token == dmb->dmb_tok) {
			dmb_node = tmp_node;
			break;
		}
	}
	read_unlock_bh(&ldev->dmb_ht_lock);
	if (!dmb_node)
		return -EINVAL;

	if (refcount_dec_and_test(&dmb_node->refcnt)) {
		spin_lock_irqsave(&ism->lock, flags);
		ism->sba_client_arr[dmb_node->sba_idx] = NO_CLIENT;
		spin_unlock_irqrestore(&ism->lock, flags);

		__ism_lo_unregister_dmb(ldev, dmb_node);
	}
	return 0;
}

static int ism_lo_support_dmb_nocopy(struct ism_dev *ism)
{
	return ISM_LO_SUPPORT_NOCOPY;
}

static int ism_lo_attach_dmb(struct ism_dev *ism, struct ism_dmb *dmb)
{
	struct ism_lo_dmb_node *dmb_node = NULL, *tmp_node;
	struct ism_lo_dev *ldev;

	ldev = container_of(ism, struct ism_lo_dev, ism);

	/* find dmb_node according to dmb->dmb_tok */
	read_lock_bh(&ldev->dmb_ht_lock);
	hash_for_each_possible(ldev->dmb_ht, tmp_node, list, dmb->dmb_tok) {
		if (tmp_node->token == dmb->dmb_tok) {
			dmb_node = tmp_node;
			break;
		}
	}
	if (!dmb_node) {
		read_unlock_bh(&ldev->dmb_ht_lock);
		return -EINVAL;
	}
	read_unlock_bh(&ldev->dmb_ht_lock);

	if (!refcount_inc_not_zero(&dmb_node->refcnt))
		/* the dmb is being unregistered, but has
		 * not been removed from the hash table.
		 */
		return -EINVAL;

	/* provide dmb information */
	dmb->sba_idx = dmb_node->sba_idx;
	dmb->dmb_tok = dmb_node->token;
	dmb->cpu_addr = dmb_node->cpu_addr;
	dmb->dma_addr = dmb_node->dma_addr;
	dmb->dmb_len = dmb_node->len;
	return 0;
}

static int ism_lo_detach_dmb(struct ism_dev *ism, u64 token)
{
	struct ism_lo_dmb_node *dmb_node = NULL, *tmp_node;
	struct ism_lo_dev *ldev;

	ldev = container_of(ism, struct ism_lo_dev, ism);

	/* find dmb_node according to dmb->dmb_tok */
	read_lock_bh(&ldev->dmb_ht_lock);
	hash_for_each_possible(ldev->dmb_ht, tmp_node, list, token) {
		if (tmp_node->token == token) {
			dmb_node = tmp_node;
			break;
		}
	}
	if (!dmb_node) {
		read_unlock_bh(&ldev->dmb_ht_lock);
		return -EINVAL;
	}
	read_unlock_bh(&ldev->dmb_ht_lock);

	if (refcount_dec_and_test(&dmb_node->refcnt))
		__ism_lo_unregister_dmb(ldev, dmb_node);
	return 0;
}

static int ism_lo_move_data(struct ism_dev *ism, u64 dmb_tok,
			    unsigned int idx, bool sf, unsigned int offset,
			    void *data, unsigned int size)
{
	struct ism_lo_dmb_node *rmb_node = NULL, *tmp_node;
	struct ism_lo_dev *ldev;
	u16 s_mask;
	u8 client_id;
	u32 sba_idx;

	ldev = container_of(ism, struct ism_lo_dev, ism);

	if (!sf)
		/* since sndbuf is merged with peer DMB, there is
		 * no need to copy data from sndbuf to peer DMB.
		 */
		return 0;

	read_lock_bh(&ldev->dmb_ht_lock);
	hash_for_each_possible(ldev->dmb_ht, tmp_node, list, dmb_tok) {
		if (tmp_node->token == dmb_tok) {
			rmb_node = tmp_node;
			break;
		}
	}
	if (!rmb_node) {
		read_unlock_bh(&ldev->dmb_ht_lock);
		return -EINVAL;
	}
	// So why copy the data now?? SMC usecase? Data buffer is attached,
	// rw-pointer are not attached?
	memcpy((char *)rmb_node->cpu_addr + offset, data, size);
	sba_idx = rmb_node->sba_idx;
	read_unlock_bh(&ldev->dmb_ht_lock);

	spin_lock(&ism->lock);
	client_id = ism->sba_client_arr[sba_idx];
	s_mask = ror16(0x1000, idx);
	if (likely(client_id != NO_CLIENT && ism->subs[client_id]))
		ism->subs[client_id]->handle_irq(ism, sba_idx, s_mask);
	spin_unlock(&ism->lock);

	return 0;
}

static int ism_lo_supports_v2(void)
{
	return ISM_LO_V2_CAPABLE;
}

static u16 ism_lo_get_chid(struct ism_dev *ism)
{
	return ISM_LO_RESERVED_CHID;
}

static const struct ism_ops ism_lo_ops = {
	.query_remote_gid = ism_lo_query_rgid,
	.register_dmb = ism_lo_register_dmb,
	.unregister_dmb = ism_lo_unregister_dmb,
	.support_dmb_nocopy = ism_lo_support_dmb_nocopy,
	.attach_dmb = ism_lo_attach_dmb,
	.detach_dmb = ism_lo_detach_dmb,
	.add_vlan_id = NULL,
	.del_vlan_id = NULL,
	.set_vlan_required = NULL,
	.reset_vlan_required = NULL,
	.signal_event = NULL,
	.move_data = ism_lo_move_data,
	.supports_v2 = ism_lo_supports_v2,
	.get_chid = ism_lo_get_chid,
};

static void ism_lo_dev_init(struct ism_lo_dev *ldev)
{
	rwlock_init(&ldev->dmb_ht_lock);
	hash_init(ldev->dmb_ht);
	atomic_set(&ldev->dmb_cnt, 0);
	init_waitqueue_head(&ldev->ldev_release);
}

static void ism_lo_dev_exit(struct ism_lo_dev *ldev)
{
	ism_dev_unregister(&ldev->ism);
	if (atomic_read(&ldev->dmb_cnt))
		wait_event(ldev->ldev_release, !atomic_read(&ldev->dmb_cnt));
}

static void ism_lo_dev_release(struct device *dev)
{
	struct ism_dev *ism;
	struct ism_lo_dev *ldev;

	ism = container_of(dev, struct ism_dev, dev);
	ldev = container_of(ism, struct ism_lo_dev, ism);

	kfree(ldev);
}

static int ism_lo_dev_probe(void)
{
	struct ism_lo_dev *ldev;
	struct ism_dev *ism;

	ldev = kzalloc(sizeof(*ldev), GFP_KERNEL);
	if (!ldev)
		return -ENOMEM;

	ism_lo_dev_init(ldev);
	ism = &ldev->ism;
	uuid_gen(&ism->gid);
	ism->ops = &ism_lo_ops;

	ism->sba_client_arr = kzalloc(ISM_LO_MAX_DMBS, GFP_KERNEL);
	if (!ism->sba_client_arr)
		return -ENOMEM;
	memset(ism->sba_client_arr, NO_CLIENT, ISM_LO_MAX_DMBS);

	ism->dev.parent = NULL;
	ism->dev.release = ism_lo_dev_release;
	device_initialize(&ism->dev);
	dev_set_name(&ism->dev, ism_lo_dev_name);
	// No device_add() for loopback?

	ism_dev_register(ism);
	lo_dev = ldev;
	return 0;
}

static void ism_lo_dev_remove(void)
{
	if (!lo_dev)
		return;

	ism_lo_dev_exit(lo_dev);
	put_device(&lo_dev->dev); /* device_initialize in ism_lo_dev_probe */
	//Missing anyhow?:
	lo_dev = NULL;
}

int ism_loopback_init(void)
{
	return ism_lo_dev_probe();
}

void ism_loopback_exit(void)
{
	ism_lo_dev_remove();
}
