// SPDX-License-Identifier: GPL-2.0
/*
 * io_uring IPC channel implementation
 *
 * High-performance inter-process communication using io_uring infrastructure.
 * Provides broadcast and multicast channels with zero-copy support.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io_uring.h>
#include <linux/io_uring/cmd.h>
#include <linux/hashtable.h>
#include <linux/anon_inodes.h>
#include <linux/mman.h>
#include <linux/vmalloc.h>

#include <uapi/linux/io_uring.h>

#include "io_uring.h"
#include "rsrc.h"
#include "memmap.h"
#include "ipc.h"

#ifdef CONFIG_IO_URING_IPC

/*
 * Global channel registry
 * Protected by RCU and spinlock
 */
static DEFINE_HASHTABLE(channel_hash, 8);
static DEFINE_SPINLOCK(channel_hash_lock);
static DEFINE_XARRAY_ALLOC(channel_xa);
static atomic_t ipc_global_sub_id = ATOMIC_INIT(0);

static int ipc_channel_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct io_ipc_channel *channel;
	size_t region_size;
	unsigned long uaddr = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;
	void *kaddr;
	unsigned long pfn;
	int ret;

	/*
	 * Safely acquire a channel reference.  A concurrent
	 * io_ipc_channel_destroy() can NULL private_data and drop
	 * all references; the channel is freed via call_rcu(), so
	 * it stays accessible under rcu_read_lock().
	 */
	rcu_read_lock();
	channel = READ_ONCE(file->private_data);
	if (!channel || !refcount_inc_not_zero(&channel->ref_count)) {
		rcu_read_unlock();
		return -EINVAL;
	}
	rcu_read_unlock();

	region_size = io_region_size(channel->region);
	kaddr = channel->region->ptr;

	/* Validate mmap parameters */
	if (vma->vm_pgoff != 0) {
		ret = -EINVAL;
		goto out_put;
	}

	if (size > region_size) {
		ret = -EINVAL;
		goto out_put;
	}

	if (vma->vm_flags & VM_WRITE) {
		ret = -EACCES;
		goto out_put;
	}

	/* Prevent mprotect from later adding write permission */
	vm_flags_clear(vma, VM_MAYWRITE);

	/* Map the vmalloc'd region page by page */
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	while (size > 0) {
		pfn = vmalloc_to_pfn(kaddr);
		ret = remap_pfn_range(vma, uaddr, pfn, PAGE_SIZE, vma->vm_page_prot);
		if (ret)
			goto out_put;

		uaddr += PAGE_SIZE;
		kaddr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	ret = 0;
out_put:
	io_ipc_channel_put(channel);
	return ret;
}

static int ipc_channel_release(struct inode *inode, struct file *file)
{
	struct io_ipc_channel *channel = file->private_data;

	if (channel)
		io_ipc_channel_put(channel);

	return 0;
}

static const struct file_operations ipc_channel_fops = {
	.mmap		= ipc_channel_mmap,
	.release	= ipc_channel_release,
	.llseek		= noop_llseek,
};


static int ipc_calc_region_size(u32 ring_entries, u32 max_msg_size,
				size_t *ring_size_out, size_t *data_size_out,
				size_t *total_size_out)
{
	size_t ring_size, data_size, total_size;

	ring_size = sizeof(struct io_ipc_ring) +
		    ring_entries * sizeof(struct io_ipc_msg_desc);
	ring_size = ALIGN(ring_size, PAGE_SIZE);

	if ((u64)ring_entries * max_msg_size > INT_MAX)
		return -EINVAL;

	data_size = (size_t)ring_entries * max_msg_size;
	data_size = ALIGN(data_size, PAGE_SIZE);

	total_size = ring_size + data_size;

	if (total_size > INT_MAX)
		return -EINVAL;

	*ring_size_out = ring_size;
	*data_size_out = data_size;
	*total_size_out = total_size;

	return 0;
}

static int ipc_region_alloc(struct io_ipc_channel *channel, u32 ring_entries,
			    u32 max_msg_size)
{
	struct io_mapped_region *region;
	size_t ring_size, data_size, total_size;
	void *ptr;
	int ret;

	ret = ipc_calc_region_size(ring_entries, max_msg_size, &ring_size,
				   &data_size, &total_size);
	if (ret)
		return ret;

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	ptr = vmalloc_user(total_size);
	if (!ptr) {
		kfree(region);
		return -ENOMEM;
	}

	region->ptr = ptr;
	region->nr_pages = (total_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	region->flags = 0;

	channel->region = region;
	channel->ring = (struct io_ipc_ring *)ptr;
	channel->desc_array = (struct io_ipc_msg_desc *)((u8 *)ptr + sizeof(struct io_ipc_ring));
	channel->data_region = ptr + ring_size;

	channel->ring->ring_mask = ring_entries - 1;
	channel->ring->ring_entries = ring_entries;
	channel->ring->max_msg_size = max_msg_size;

	return 0;
}

static void ipc_region_free(struct io_ipc_channel *channel)
{
	if (!channel->region)
		return;

	if (channel->region->ptr)
		vfree(channel->region->ptr);
	kfree(channel->region);
	channel->region = NULL;
	channel->ring = NULL;
	channel->data_region = NULL;
}

static void io_ipc_channel_free(struct io_ipc_channel *channel)
{
	struct io_ipc_subscriber *sub;
	unsigned long index;

	xa_for_each(&channel->subscribers, index, sub)
		kfree(sub);
	xa_destroy(&channel->subscribers);

	if (channel->file) {
		/*
		 * Prevent the deferred fput release callback from
		 * accessing the channel after it has been freed.
		 */
		WRITE_ONCE(channel->file->private_data, NULL);
		fput(channel->file);
	}

	ipc_region_free(channel);

	kfree(channel);
}

static void io_ipc_channel_free_rcu(struct rcu_head *rcu)
{
	struct io_ipc_channel *channel;

	channel = container_of(rcu, struct io_ipc_channel, rcu);
	io_ipc_channel_free(channel);
}

void io_ipc_channel_put(struct io_ipc_channel *channel)
{
	if (refcount_dec_and_test(&channel->ref_count)) {
		/*
		 * Remove from global data structures before scheduling
		 * the RCU callback.  Lookups use refcount_inc_not_zero()
		 * so no new references can appear.  Removing here avoids
		 * taking xa_lock (a plain spinlock) inside the RCU
		 * callback which runs in softirq context — that would
		 * deadlock against xa_alloc() in process context.
		 */
		spin_lock_bh(&channel_hash_lock);
		if (!hlist_unhashed(&channel->hash_node))
			hash_del_rcu(&channel->hash_node);
		spin_unlock_bh(&channel_hash_lock);

		xa_erase(&channel_xa, channel->channel_id);

		call_rcu(&channel->rcu, io_ipc_channel_free_rcu);
	}
}

struct io_ipc_channel *io_ipc_channel_get(u32 channel_id)
{
	struct io_ipc_channel *channel;

	rcu_read_lock();
	channel = xa_load(&channel_xa, channel_id);
	if (channel && !refcount_inc_not_zero(&channel->ref_count))
		channel = NULL;
	rcu_read_unlock();

	return channel;
}

struct io_ipc_channel *io_ipc_channel_get_by_key(u64 key)
{
	struct io_ipc_channel *channel;
	u32 hash = hash_64(key, HASH_BITS(channel_hash));

	rcu_read_lock();
	hash_for_each_possible_rcu(channel_hash, channel, hash_node, hash) {
		if (channel->key == key &&
		    refcount_inc_not_zero(&channel->ref_count)) {
			rcu_read_unlock();
			return channel;
		}
	}
	rcu_read_unlock();

	return NULL;
}

static int ipc_check_permission(struct io_ipc_channel *channel, u32 access)
{
	const struct cred *cred = current_cred();
	kuid_t uid = cred->fsuid;
	kgid_t gid = cred->fsgid;
	u16 mode = channel->mode;
	u16 needed = 0;

	/* Map IPC access flags to Unix permission bits: send=write, recv=read */
	if (access & IOIPC_SUB_RECV)
		needed |= 4;	/* read */
	if (access & IOIPC_SUB_SEND)
		needed |= 2;	/* write */

	if (uid_eq(uid, channel->owner_uid))
		return ((mode >> 6) & needed) == needed ? 0 : -EACCES;

	if (gid_eq(gid, channel->owner_gid))
		return ((mode >> 3) & needed) == needed ? 0 : -EACCES;

	return (mode & needed) == needed ? 0 : -EACCES;
}

int io_ipc_channel_create(struct io_ring_ctx *ctx,
			   const struct io_uring_ipc_channel_create __user *arg)
{
	struct io_uring_ipc_channel_create create;
	struct io_ipc_channel *channel;
	u32 hash;
	int ret;

	if (copy_from_user(&create, arg, sizeof(create)))
		return -EFAULT;

	if (!mem_is_zero(create.reserved, sizeof(create.reserved)))
		return -EINVAL;

	if (!create.ring_entries || create.ring_entries > IORING_MAX_ENTRIES)
		return -EINVAL;

	if (!is_power_of_2(create.ring_entries))
		return -EINVAL;

	if (!create.max_msg_size || create.max_msg_size > SZ_1M)
		return -EINVAL;

	if (create.flags & ~(IOIPC_F_BROADCAST | IOIPC_F_MULTICAST |
			     IOIPC_F_PRIVATE))
		return -EINVAL;

	if ((create.flags & IOIPC_F_BROADCAST) &&
	    (create.flags & IOIPC_F_MULTICAST))
		return -EINVAL;

	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel)
		return -ENOMEM;

	refcount_set(&channel->ref_count, 1);
	channel->flags = create.flags;
	channel->key = create.key;
	channel->msg_max_size = create.max_msg_size;
	channel->owner_uid = current_fsuid();
	channel->owner_gid = current_fsgid();
	channel->mode = create.mode & 0666;
	atomic_set(&channel->next_msg_id, 1);
	atomic_set(&channel->next_receiver_idx, 0);
	atomic_set(&channel->recv_count, 0);
	xa_init(&channel->subscribers);

	ret = ipc_region_alloc(channel, create.ring_entries, create.max_msg_size);
	if (ret)
		goto err_free_channel;

	refcount_inc(&channel->ref_count); /* File holds a reference */
	channel->file = anon_inode_getfile("[io_uring_ipc]", &ipc_channel_fops,
					   channel, O_RDWR);
	if (IS_ERR(channel->file)) {
		ret = PTR_ERR(channel->file);
		channel->file = NULL;
		refcount_dec(&channel->ref_count);
		goto err_free_region;
	}

	ret = xa_alloc(&channel_xa, &channel->channel_id, channel,
		       XA_LIMIT(1, INT_MAX), GFP_KERNEL);
	if (ret < 0)
		goto err_put_file;

	hash = hash_64(create.key, HASH_BITS(channel_hash));
	spin_lock_bh(&channel_hash_lock);
	hash_add_rcu(channel_hash, &channel->hash_node, hash);
	spin_unlock_bh(&channel_hash_lock);

	/*
	 * Track the channel in the creating ring so that
	 * io_ipc_ctx_cleanup() can break the channel-file reference
	 * cycle if the ring is torn down without an explicit destroy.
	 */
	ret = xa_insert(&ctx->ipc_channels, channel->channel_id,
			xa_mk_value(0), GFP_KERNEL);
	if (ret)
		goto err_remove_channel;

	create.channel_id_out = channel->channel_id;
	if (copy_to_user((void __user *)arg, &create, sizeof(create))) {
		ret = -EFAULT;
		goto err_remove_ctx;
	}

	return 0;

err_remove_ctx:
	xa_erase(&ctx->ipc_channels, channel->channel_id);
err_remove_channel:
	spin_lock_bh(&channel_hash_lock);
	hash_del_rcu(&channel->hash_node);
	spin_unlock_bh(&channel_hash_lock);
	xa_erase(&channel_xa, channel->channel_id);
err_put_file:
	/*
	 * fput() is deferred — the release callback will call
	 * io_ipc_channel_put() to drop the file's reference.
	 * Drop the initial reference here; it can't reach zero
	 * yet because the file still holds one.
	 */
	fput(channel->file);
	channel->file = NULL;
	io_ipc_channel_put(channel);
	return ret;
err_free_region:
	ipc_region_free(channel);
err_free_channel:
	kfree(channel);
	return ret;
}

int io_ipc_channel_attach(struct io_ring_ctx *ctx,
			   const struct io_uring_ipc_channel_attach __user *arg)
{
	struct io_uring_ipc_channel_attach attach;
	struct io_ipc_channel *channel = NULL;
	struct io_ipc_subscriber *sub;
	int ret;

	if (copy_from_user(&attach, arg, sizeof(attach)))
		return -EFAULT;

	if (!mem_is_zero(attach.reserved, sizeof(attach.reserved)))
		return -EINVAL;

	if (!attach.flags || (attach.flags & ~IOIPC_SUB_BOTH))
		return -EINVAL;

	if (attach.key)
		channel = io_ipc_channel_get_by_key(attach.key);
	else
		channel = io_ipc_channel_get(attach.channel_id);

	if (!channel)
		return -ENOENT;

	ret = ipc_check_permission(channel, attach.flags);
	if (ret)
		goto err_put_channel;

	sub = kzalloc(sizeof(*sub), GFP_KERNEL);
	if (!sub) {
		ret = -ENOMEM;
		goto err_put_channel;
	}

	sub->ctx = ctx;
	sub->channel = channel;
	sub->flags = attach.flags;
	sub->local_head = 0;
	sub->subscriber_id = atomic_inc_return(&ipc_global_sub_id);

	ret = xa_insert(&channel->subscribers, sub->subscriber_id, sub,
			GFP_KERNEL);
	if (ret) {
		kfree(sub);
		goto err_put_channel;
	}

	refcount_inc(&channel->ref_count);
	if (attach.flags & IOIPC_SUB_RECV)
		atomic_inc(&channel->recv_count);

	ret = xa_insert(&ctx->ipc_subscribers, sub->subscriber_id, sub,
			GFP_KERNEL);
	if (ret)
		goto err_remove_chan_sub;

	ret = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
	if (ret < 0)
		goto err_remove_sub;

	attach.local_id_out = sub->subscriber_id;
	attach.region_size = io_region_size(channel->region);
	attach.channel_fd = ret;
	attach.mmap_offset_out = 0;

	if (copy_to_user((void __user *)arg, &attach, sizeof(attach))) {
		put_unused_fd(ret);
		ret = -EFAULT;
		goto err_remove_sub;
	}

	{
		struct file *f = get_file_active(&channel->file);

		if (!f) {
			put_unused_fd(attach.channel_fd);
			ret = -EINVAL;
			goto err_remove_sub;
		}
		fd_install(attach.channel_fd, f);
	}

	io_ipc_channel_put(channel);
	return 0;

err_remove_sub:
	xa_erase(&ctx->ipc_subscribers, sub->subscriber_id);
err_remove_chan_sub:
	xa_erase(&channel->subscribers, sub->subscriber_id);
	if (attach.flags & IOIPC_SUB_RECV)
		atomic_dec(&channel->recv_count);
	kfree_rcu(sub, rcu);
	io_ipc_channel_put(channel); /* Drop subscriber's reference */
err_put_channel:
	io_ipc_channel_put(channel); /* Drop lookup reference */
	return ret;
}

int io_ipc_channel_detach(struct io_ring_ctx *ctx, u32 subscriber_id)
{
	struct io_ipc_subscriber *sub;
	struct io_ipc_channel *channel;

	sub = xa_erase(&ctx->ipc_subscribers, subscriber_id);
	if (!sub)
		return -ENOENT;

	channel = sub->channel;
	xa_erase(&channel->subscribers, subscriber_id);

	if (sub->flags & IOIPC_SUB_RECV)
		atomic_dec(&channel->recv_count);

	io_ipc_channel_put(channel);
	kfree_rcu(sub, rcu);
	return 0;
}

/*
 * Recompute consumer.head as the minimum local_head across all receive
 * subscribers.  Called lazily from the broadcast recv path (every 16
 * messages) and from the send path when the ring appears full.
 */
static void ipc_advance_consumer_head(struct io_ipc_channel *channel)
{
	struct io_ipc_subscriber *s;
	struct io_ipc_ring *ring = channel->ring;
	unsigned long index;
	u32 min_head = READ_ONCE(ring->producer.tail);

	rcu_read_lock();
	xa_for_each(&channel->subscribers, index, s) {
		if (s->flags & IOIPC_SUB_RECV) {
			u32 sh = READ_ONCE(s->local_head);

			if ((s32)(sh - min_head) < 0)
				min_head = sh;
		}
	}
	rcu_read_unlock();

	WRITE_ONCE(ring->consumer.head, min_head);
}

static int ipc_wake_receivers(struct io_ipc_channel *channel, u32 target)
{
	struct io_ipc_subscriber *sub;
	unsigned long index;

	rcu_read_lock();
	if (target) {
		sub = xa_load(&channel->subscribers, target);
		if (!sub || !(sub->flags & IOIPC_SUB_RECV)) {
			rcu_read_unlock();
			return -ENOENT;
		}
		io_cqring_wake(sub->ctx);
		rcu_read_unlock();
		return 0;
	}

	if (channel->flags & IOIPC_F_MULTICAST) {
		u32 recv_cnt = atomic_read(&channel->recv_count);

		if (recv_cnt) {
			u32 rr = (u32)atomic_inc_return(&channel->next_receiver_idx);
			u32 target_idx = rr % recv_cnt;
			u32 i = 0;

			xa_for_each(&channel->subscribers, index, sub) {
				if (sub->flags & IOIPC_SUB_RECV) {
					if (i == target_idx) {
						io_cqring_wake(sub->ctx);
						break;
					}
					i++;
				}
			}
		}
	} else {
		xa_for_each(&channel->subscribers, index, sub) {
			if (sub->flags & IOIPC_SUB_RECV) {
				io_cqring_wake(sub->ctx);
				if (!(channel->flags & IOIPC_F_BROADCAST))
					break;
			}
		}
	}
	rcu_read_unlock();
	return 0;
}

int io_ipc_send_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_ipc_send *ipc = io_kiocb_to_cmd(req, struct io_ipc_send);

	if (sqe->buf_index || sqe->personality)
		return -EINVAL;

	if (sqe->ioprio || sqe->rw_flags)
		return -EINVAL;

	ipc->channel_id = READ_ONCE(sqe->fd);
	ipc->addr = READ_ONCE(sqe->addr);
	ipc->len = READ_ONCE(sqe->len);
	ipc->target = READ_ONCE(sqe->file_index);

	return 0;
}

int io_ipc_send(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ipc_send *ipc = io_kiocb_to_cmd(req, struct io_ipc_send);
	struct io_ipc_subscriber *sub = NULL;
	struct io_ipc_channel *channel = NULL;
	struct io_ipc_ring *ring;
	struct io_ipc_msg_desc *desc;
	void __user *user_buf;
	void *dest;
	u32 head, tail, next_tail, idx;
	u32 sub_flags;
	int ret;
	u32 fd = ipc->channel_id;
	bool need_put = false;
	bool retried_advance = false;

	/* O(1) subscriber lookup via xarray */
	rcu_read_lock();
	sub = xa_load(&req->ctx->ipc_subscribers, fd);
	if (sub) {
		channel = sub->channel;
		if (!refcount_inc_not_zero(&channel->ref_count)) {
			rcu_read_unlock();
			ret = -ENOENT;
			goto fail;
		}
		sub_flags = sub->flags;
		rcu_read_unlock();
		need_put = true;

		if (!(sub_flags & IOIPC_SUB_SEND)) {
			ret = -EACCES;
			goto fail_put;
		}
		goto found;
	}
	rcu_read_unlock();

	channel = io_ipc_channel_get(fd);
	if (!channel) {
		ret = -ENOENT;
		goto fail;
	}

	need_put = true;

	ret = ipc_check_permission(channel, IOIPC_SUB_SEND);
	if (ret)
		goto fail_put;

found:
	ring = channel->ring;

	if (unlikely(ipc->len > channel->msg_max_size)) {
		ret = -EMSGSIZE;
		goto fail_put;
	}

	/* Lock-free slot reservation via CAS on producer tail */
retry:
	do {
		tail = READ_ONCE(ring->producer.tail);
		next_tail = tail + 1;
		head = READ_ONCE(ring->consumer.head);

		if (unlikely(next_tail - head > ring->ring_entries)) {
			/*
			 * Ring full.  For broadcast channels, try to
			 * advance consumer.head once by scanning the
			 * min local_head across all receivers.
			 */
			if ((channel->flags & IOIPC_F_BROADCAST) &&
			    !retried_advance) {
				ipc_advance_consumer_head(channel);
				retried_advance = true;
				goto retry;
			}
			ret = -ENOBUFS;
			goto fail_put;
		}
		idx = tail & ring->ring_mask;
	} while (cmpxchg(&ring->producer.tail, tail, next_tail) != tail);

	/* Slot exclusively claimed — write data and descriptor */
	dest = channel->data_region + (idx * channel->msg_max_size);
	desc = &channel->desc_array[idx];

	prefetchw(desc);

	user_buf = u64_to_user_ptr(ipc->addr);
	if (unlikely(__copy_from_user_inatomic(dest, user_buf, ipc->len))) {
		if (unlikely(copy_from_user(dest, user_buf, ipc->len))) {
			/*
			 * Slot already claimed via CAS; mark it ready with
			 * zero length so consumers can advance past it.
			 */
			desc->offset = idx * channel->msg_max_size;
			desc->len = 0;
			desc->msg_id = 0;
			desc->sender_data = 0;
			smp_wmb();
			WRITE_ONCE(desc->seq, tail + 1);
			ret = -EFAULT;
			goto fail_put;
		}
	}

	desc->offset = idx * channel->msg_max_size;
	desc->len = ipc->len;
	desc->msg_id = atomic_inc_return(&channel->next_msg_id);
	desc->sender_data = req->cqe.user_data;

	/* Ensure descriptor + data visible before marking slot ready */
	smp_wmb();
	WRITE_ONCE(desc->seq, tail + 1);

	ret = ipc_wake_receivers(channel, ipc->target);
	if (ret)
		goto fail_put;

	if (need_put)
		io_ipc_channel_put(channel);

	io_req_set_res(req, ipc->len, 0);
	return IOU_COMPLETE;

fail_put:
	if (need_put)
		io_ipc_channel_put(channel);
fail:
	req_set_fail(req);
	io_req_set_res(req, ret, 0);
	return IOU_COMPLETE;
}


int io_ipc_recv_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_ipc_recv *ipc = io_kiocb_to_cmd(req, struct io_ipc_recv);

	if (sqe->buf_index || sqe->personality)
		return -EINVAL;

	if (sqe->ioprio || sqe->rw_flags)
		return -EINVAL;

	ipc->channel_id = READ_ONCE(sqe->fd);
	ipc->addr = READ_ONCE(sqe->addr);
	ipc->len = READ_ONCE(sqe->len);

	return 0;
}

int io_ipc_recv(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ipc_recv *ipc = io_kiocb_to_cmd(req, struct io_ipc_recv);
	struct io_ipc_subscriber *sub = NULL;
	struct io_ipc_channel *channel;
	struct io_ipc_ring *ring;
	struct io_ipc_msg_desc *desc;
	void __user *user_buf;
	void *src;
	u32 head, tail, idx;
	u32 sub_flags, sub_id;
	size_t copy_len;
	int ret;

	/* O(1) subscriber lookup via xarray */
	rcu_read_lock();
	sub = xa_load(&req->ctx->ipc_subscribers, ipc->channel_id);
	if (sub) {
		channel = sub->channel;
		if (!refcount_inc_not_zero(&channel->ref_count)) {
			rcu_read_unlock();
			ret = -ENOENT;
			goto fail;
		}
		sub_flags = sub->flags;
		sub_id = sub->subscriber_id;
		head = READ_ONCE(sub->local_head);
		rcu_read_unlock();
		goto found;
	}
	rcu_read_unlock();
	ret = -ENOENT;
	goto fail;

found:
	ring = channel->ring;

	if (!(sub_flags & IOIPC_SUB_RECV)) {
		ret = -EACCES;
		goto fail_put;
	}

	/*
	 * For multicast, competing consumers share consumer.head directly
	 * instead of using per-subscriber local_head.
	 */
	if (channel->flags & IOIPC_F_MULTICAST)
		head = READ_ONCE(ring->consumer.head);

	/* Check if there are messages available - punt to io-wq for retry */
	tail = READ_ONCE(ring->producer.tail);
	if (unlikely(head == tail)) {
		io_ipc_channel_put(channel);
		return -EAGAIN;
	}

	idx = head & ring->ring_mask;
	desc = &channel->desc_array[idx];

	/*
	 * Verify slot is fully written by the producer.  With lock-free
	 * CAS-based send, tail advances before data is written; the
	 * per-slot seq number signals completion.
	 * Pairs with smp_wmb() + WRITE_ONCE(desc->seq) in send path.
	 */
	if (READ_ONCE(desc->seq) != head + 1) {
		io_ipc_channel_put(channel);
		return -EAGAIN;
	}

	smp_rmb();

	src = channel->data_region + desc->offset;
	prefetch(src);

	copy_len = min_t(u32, desc->len, ipc->len);
	user_buf = u64_to_user_ptr(ipc->addr);

	if (unlikely(__copy_to_user_inatomic(user_buf, src, copy_len))) {
		if (unlikely(copy_to_user(user_buf, src, copy_len))) {
			ret = -EFAULT;
			goto fail_put;
		}
	}

	if (channel->flags & IOIPC_F_MULTICAST) {
		/*
		 * Multicast: atomically advance the shared consumer head.
		 * Losers retry via -EAGAIN / io-wq.
		 */
		if (cmpxchg(&ring->consumer.head, head, head + 1) != head) {
			io_ipc_channel_put(channel);
			return -EAGAIN;
		}
	} else {
		/*
		 * Re-find subscriber under RCU for atomic local_head update.
		 * O(1) xarray lookup; subscriber is stable while channel
		 * ref is held and we're under RCU.
		 */
		rcu_read_lock();
		sub = xa_load(&req->ctx->ipc_subscribers, sub_id);
		if (!sub) {
			rcu_read_unlock();
			goto done;
		}

		if (cmpxchg(&sub->local_head, head, head + 1) != head) {
			rcu_read_unlock();
			io_ipc_channel_put(channel);
			return -EAGAIN;
		}

		if (channel->flags & IOIPC_F_BROADCAST) {
			/*
			 * Lazy consumer.head advancement: only scan min-head
			 * every 16 messages to amortize the O(N) walk.
			 * The send path also triggers this on ring-full.
			 */
			if ((head & 0xf) == 0)
				ipc_advance_consumer_head(channel);
		} else {
			/* Unicast: single consumer, update directly */
			WRITE_ONCE(ring->consumer.head, head + 1);
		}
		rcu_read_unlock();
	}

done:
	io_ipc_channel_put(channel);
	io_req_set_res(req, copy_len, 0);
	return IOU_COMPLETE;

fail_put:
	io_ipc_channel_put(channel);
fail:
	req_set_fail(req);
	io_req_set_res(req, ret, 0);
	return IOU_COMPLETE;
}

int io_ipc_channel_destroy(struct io_ring_ctx *ctx, u32 channel_id)
{
	struct io_ipc_channel *channel;
	struct file *f;

	/*
	 * Atomically remove from global lookup.  This prevents a second
	 * destroy call from finding the channel and draining refcount
	 * below the number of outstanding references.
	 */
	channel = xa_erase(&channel_xa, channel_id);
	if (!channel)
		return -ENOENT;

	spin_lock_bh(&channel_hash_lock);
	if (!hlist_unhashed(&channel->hash_node))
		hash_del_rcu(&channel->hash_node);
	spin_unlock_bh(&channel_hash_lock);

	/*
	 * Break the reference cycle between channel and file.
	 * The channel holds a file reference (channel->file) and the
	 * file's release callback holds a channel reference via
	 * private_data.  Detach the file here so that the release
	 * callback becomes a no-op and the cycle is broken.
	 */
	f = channel->file;
	if (f) {
		WRITE_ONCE(f->private_data, NULL);
		channel->file = NULL;
		fput(f);
		io_ipc_channel_put(channel); /* Drop file's reference */
	}

	/* Drop the creator's initial reference */
	io_ipc_channel_put(channel);

	return 0;
}

void io_ipc_ctx_cleanup(struct io_ring_ctx *ctx)
{
	struct io_ipc_subscriber *sub;
	struct io_ipc_channel *channel;
	unsigned long index;
	void *entry;

	xa_for_each(&ctx->ipc_subscribers, index, sub) {
		channel = sub->channel;

		xa_erase(&ctx->ipc_subscribers, index);
		xa_erase(&channel->subscribers, sub->subscriber_id);

		if (sub->flags & IOIPC_SUB_RECV)
			atomic_dec(&channel->recv_count);

		io_ipc_channel_put(channel);
		kfree_rcu(sub, rcu);
	}
	xa_destroy(&ctx->ipc_subscribers);

	/*
	 * Destroy any channels created by this ring that were not
	 * explicitly destroyed.  io_ipc_channel_destroy() is
	 * idempotent — it returns -ENOENT if the channel was
	 * already destroyed.
	 */
	xa_for_each(&ctx->ipc_channels, index, entry) {
		io_ipc_channel_destroy(ctx, index);
	}
	xa_destroy(&ctx->ipc_channels);
}

#endif /* CONFIG_IO_URING_IPC */
