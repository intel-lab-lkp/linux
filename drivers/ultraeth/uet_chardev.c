// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/netdevice.h>

#include <rdma/ib_umem.h>

#include <uapi/linux/ultraeth.h>
#include <net/ultraeth/uet_context.h>
#include <net/ultraeth/uet_chardev.h>

#define MAX_PDS_HDRLEN	64	/* -ish? */

static int uet_char_open(struct inode *inode, struct file *file)
{
	struct uet_context *ctx;
	struct uet_fep *fep;
	int rv;

	ctx = uet_context_get_by_minor(iminor(inode));
	if (!ctx)
		return -ENOENT;

	fep = kzalloc(sizeof(*fep), GFP_KERNEL);
	if (!fep) {
		rv = -ENOMEM;
		goto err_alloc;
	}

	fep->context = ctx;
	fep->ack_gen_min_pkt_add = UET_DEFAULT_ACK_GEN_MIN_PKT_ADD;
	fep->ack_gen_trigger = UET_DEFAULT_ACK_GEN_TRIGGER;
	skb_queue_head_init(&fep->rxq);
	file->private_data = fep;
	rv = nonseekable_open(inode, file);
	if (rv < 0)
		goto err_open;

	return rv;

err_open:
	kfree(fep);
err_alloc:
	uet_context_put(ctx);

	return rv;
}

static int uet_char_release(struct inode *inode, struct file *file)
{
	struct uet_fep *fep = file->private_data;

	uet_job_reg_disassociate(&fep->context->job_reg, fep->job_id);
	skb_queue_purge(&fep->rxq);
	uet_context_put(fep->context);
	kfree(fep);

	return 0;
}

static long uet_char_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct uet_fep *fep = file->private_data;
	void __user *p = (void __user *)arg;
	int ret = 0;

	switch (cmd) {
	case UET_ADDR_REQ: {
		struct uet_job_addr_req areq;

		if (copy_from_user(&areq, p, sizeof(areq)))
			return -EFAULT;
		// XXX: validate address

		areq.service_name[UET_SVC_MAX_LEN - 1] = '\0';
		memcpy(&fep->addr.in_address, &areq.address,
		       sizeof(fep->addr.in_address));

		ret = uet_job_reg_associate(&fep->context->job_reg, fep,
					    areq.service_name);
		if (!ret) {
			if (areq.ack_gen_trigger > 0)
				fep->ack_gen_trigger = areq.ack_gen_trigger;
			if (areq.ack_gen_min_pkt_add > 0)
				fep->ack_gen_min_pkt_add = areq.ack_gen_min_pkt_add;
		}
		break;
	}
	default:
		return -EOPNOTSUPP;
	}

	return ret;
}

static ssize_t uet_char_read(struct file *file, char __user *ubuf,
			       size_t usize, loff_t *off)
{
	struct uet_fep *fep = file->private_data;
	struct uet_prologue_hdr *prologue;
	struct uet_pds_meta meta = {};
	struct sk_buff *skb = NULL;
	int ret = -ENOTCONN;
	int hdrlen = 0;
	size_t userlen;

	pr_debug("%s file=%p fep=%p size=%zu\n", __func__, file, fep, usize);

	ret = -EAGAIN;
	skb = skb_dequeue(&fep->rxq);
	if (!skb)
		goto out_err;

	ret = skb_linearize(skb);
	if (ret)
		goto out_err;

	prologue = pds_prologue_hdr(skb);
	meta.next_hdr = uet_prologue_next_hdr(prologue);
	meta.addr = UET_SKB_CB(skb)->remote_fep_addr;
	switch (meta.next_hdr) {
	case UET_PDS_NEXT_HDR_RSP_DATA:
	case UET_PDS_NEXT_HDR_RSP_DATA_SMALL:
		/* TODO */
		ret = -EOPNOTSUPP;
		goto out_err;
	case UET_PDS_NEXT_HDR_RSP:
		hdrlen = sizeof(struct uet_pds_ack_hdr);
		break;
	default:
		hdrlen = sizeof(struct uet_pds_req_hdr);
		break;
	}
	userlen = sizeof(meta) + skb->len - hdrlen;
	if (userlen > usize) {
		ret = -EMSGSIZE;
		goto out_err;
	}

	if (copy_to_user(ubuf, &meta, sizeof(meta))) {
		ret = -EFAULT;
		goto out_err;
	}
	if (copy_to_user(ubuf + sizeof(meta), skb->data + hdrlen, skb->len - hdrlen)) {
		ret = -EFAULT;
		goto out_err;
	}

	consume_skb(skb);
	ret = userlen;

	return ret;

out_err:
	kfree_skb(skb);

	return ret;
}

static ssize_t uet_char_write(struct file *file, const char __user *ubuf,
			      size_t usize, loff_t *off)
{
	struct uet_fep *fep = file->private_data;
	struct sk_buff *skb = NULL;
	struct uet_pds_meta *meta;
	struct uet_job *job;
	__be32 daddr, saddr;
	int ret = -ENODEV;
	__be16 dport;
	void *buf;

	pr_debug("%s file=%p fep=%p size=%zu\n", __func__, file, fep, usize);

	rcu_read_lock();
	job = uet_job_find(&fep->context->job_reg, fep->job_id);
	if (!job)
		goto out_err;

	ret = -ENOMEM;
	skb = alloc_skb(MAX_HEADER + MAX_PDS_HDRLEN + usize, GFP_ATOMIC);
	if (!skb)
		goto out_err;
	skb_reserve(skb, MAX_HEADER + MAX_PDS_HDRLEN);
	buf = skb_put(skb, usize);
	ret = -EFAULT;
	if (copy_from_user(buf, ubuf, usize))
		goto out_err;

	print_hex_dump_bytes("pds tx ", DUMP_PREFIX_OFFSET, skb->data, skb->len);

	meta = skb_pull_data(skb, sizeof(*meta));
	if (!meta) {
		ret = -EINVAL;
		goto out_err;
	}
	/* TODO: IPv6 */
	/* TODO: per-packet daddr */
	saddr = fep->addr.in_address.ip;
	daddr = meta->addr;
	dport = meta->port;

	switch (meta->next_hdr) {
	case UET_PDS_NEXT_HDR_RSP_DATA:
	case UET_PDS_NEXT_HDR_RSP_DATA_SMALL:
		ret = -EOPNOTSUPP; /* TODO */
		goto out_err;
	case UET_PDS_NEXT_HDR_RSP:
		ret = 0; /* FIXME: ACK PSN would be wrong */
		break;
	default:
		ret = uet_pds_tx(&fep->context->pds, skb, saddr, daddr, dport,
				 job->id);
		break;
	}

	if (ret < 0)
		goto out_err;
	rcu_read_unlock();

	return usize;

out_err:
	rcu_read_unlock();
	kfree_skb(skb);

	return ret;
}

static const struct file_operations uet_char_ops = {
	.owner		= THIS_MODULE,
	.open		= uet_char_open,
	.release	= uet_char_release,
	.read		= uet_char_read,
	.write		= uet_char_write,
	.unlocked_ioctl	= uet_char_ioctl,
};

#define UET_CHAR_MAX_NAME 20

int uet_char_init(struct miscdevice *cdev, int id)
{
	int ret = -ENOMEM;

	cdev->minor = MISC_DYNAMIC_MINOR;
	cdev->name = kzalloc(UET_CHAR_MAX_NAME, GFP_KERNEL);
	if (!cdev->name)
		return ret;
	snprintf((char *)cdev->name, UET_CHAR_MAX_NAME, "ultraeth%d", id);
	cdev->fops = &uet_char_ops;

	ret = misc_register(cdev);
	if (ret)
		kfree(cdev->name);

	return ret;
}

void uet_char_uninit(struct miscdevice *cdev)
{
	kfree(cdev->name);
	misc_deregister(cdev);
}
