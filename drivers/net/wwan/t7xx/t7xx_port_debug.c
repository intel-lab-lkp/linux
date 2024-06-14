// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, MediaTek Inc.
 * Copyright (c) 2024, Fibocom Wireless Inc.
 *
 * Authors: Jinjian Song <jinjian.song@fibocom.com>
 */

#include <linux/bitfield.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>

#include "t7xx_state_monitor.h"
#include "t7xx_port.h"
#include "t7xx_port_proxy.h"

static __poll_t port_char_poll(struct file *fp, struct poll_table_struct *poll)
{
	struct t7xx_port *port;
	__poll_t mask = 0;

	port = fp->private_data;
	poll_wait(fp, &port->rx_wq, poll);

	spin_lock_irq(&port->rx_wq.lock);
	if (!skb_queue_empty(&port->rx_skb_list))
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irq(&port->rx_wq.lock);

	return mask;
}

/**
 * port_char_open() - open char port
 * @inode: pointer to inode structure
 * @file: pointer to file structure
 *
 * Open a char port using pre-defined md_ccci_ports structure in port_proxy
 *
 * Return: 0 for success, -EINVAL for failure
 */
static int port_char_open(struct inode *inode, struct file *file)
{
	struct t7xx_cdev *t7xx_debug;
	struct t7xx_port *port;

	t7xx_debug = container_of(inode->i_cdev, struct t7xx_cdev, cdev);
	port = t7xx_debug->port;

	if (!port)
		return -EINVAL;

	port->port_conf->ops->enable_chl(port);
	atomic_inc(&port->usage_cnt);

	file->private_data = port;

	return nonseekable_open(inode, file);
}

static int port_char_close(struct inode *inode, struct file *file)
{
	struct t7xx_port *port;
	struct sk_buff *skb;

	port = file->private_data;

	/* decrease usage count, so when we ask again,
	 * the packet can be dropped in recv_request.
	 */
	atomic_dec(&port->usage_cnt);
	port->port_conf->ops->disable_chl(port);

	/* purge RX request list */
	spin_lock_irq(&port->rx_wq.lock);
	while ((skb = __skb_dequeue(&port->rx_skb_list)) != NULL)
		dev_kfree_skb(skb);
	spin_unlock_irq(&port->rx_wq.lock);

	return 0;
}

static ssize_t port_char_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	bool full_req_done = false;
	struct t7xx_port *port;
	int ret = 0, read_len;
	struct sk_buff *skb;

	port = file->private_data;

	spin_lock_irq(&port->rx_wq.lock);
	if (skb_queue_empty(&port->rx_skb_list)) {
		if (file->f_flags & O_NONBLOCK) {
			spin_unlock_irq(&port->rx_wq.lock);
			return -EAGAIN;
		}

		ret = wait_event_interruptible_locked_irq(port->rx_wq,
							  !skb_queue_empty(&port->rx_skb_list));
		if (ret == -ERESTARTSYS) {
			spin_unlock_irq(&port->rx_wq.lock);
			return -EINTR;
		}
	}
	skb = skb_peek(&port->rx_skb_list);

	if (count >= skb->len) {
		read_len = skb->len;
		full_req_done = true;
		__skb_unlink(skb, &port->rx_skb_list);
	} else {
		read_len = count;
	}

	spin_unlock_irq(&port->rx_wq.lock);
	if (copy_to_user(buf, skb->data, read_len)) {
		dev_err(port->dev, "Read on %s, copy to user failed, %d/%zu\n",
			port->port_conf->name, read_len, count);
		ret = -EFAULT;
	}

	skb_pull(skb, read_len);
	if (full_req_done)
		dev_kfree_skb(skb);

	return ret ? ret : read_len;
}

static ssize_t port_char_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	unsigned int header_len = sizeof(struct ccci_header);
	size_t  offset, txq_mtu, chunk_len = 0;
	struct t7xx_port *port;
	struct sk_buff *skb;
	bool blocking;
	int ret;

	port = file->private_data;

	blocking = !(file->f_flags & O_NONBLOCK);
	if (!blocking)
		return -EAGAIN;

	if (!port->chan_enable)
		return -EINVAL;

	txq_mtu = t7xx_get_port_mtu(port);
	if (txq_mtu < 0)
		return -EINVAL;

	for (offset = 0; offset < count; offset += chunk_len) {
		chunk_len = min(count - offset, txq_mtu - header_len);

		skb = __dev_alloc_skb(chunk_len + header_len, GFP_KERNEL);
		if (!skb)
			return -ENOMEM;

		ret = copy_from_user(skb_put(skb, chunk_len), buf + offset, chunk_len);

		if (ret) {
			dev_kfree_skb(skb);
			return -EFAULT;
		}

		ret = t7xx_port_send_skb(port, skb, 0, 0);
		if (ret) {
			if (ret == -EBUSY && !blocking)
				ret = -EAGAIN;
			dev_kfree_skb_any(skb);
			return ret;
		}
	}

	return count;
}

static int t7xx_cdev_init(struct t7xx_port *port)
{
	struct t7xx_cdev *t7xx_debug;
	struct device *dev;

	dev = &port->t7xx_dev->pdev->dev;

	t7xx_debug = devm_kzalloc(dev, sizeof(*t7xx_debug), GFP_KERNEL);
	if (!t7xx_debug)
		return -ENOMEM;

	t7xx_debug->port = port;
	port->debug.debug_port = t7xx_debug;

	return 0;
}

static void t7xx_cdev_uninit(struct t7xx_port *port)
{
	struct device *dev;

	if (!port->debug.debug_port)
		return;

	dev = &port->t7xx_dev->pdev->dev;

	devm_kfree(dev, port->debug.debug_port);
	port->debug.debug_port = NULL;
}

static const struct file_operations char_fops = {
	.owner = THIS_MODULE,
	.open = &port_char_open,
	.read = &port_char_read,
	.write = &port_char_write,
	.release = &port_char_close,
	.poll = &port_char_poll,
};

static int port_char_init(struct t7xx_port *port)
{
	const struct t7xx_port_conf *port_conf = port->port_conf;
	struct t7xx_cdev *t7xx_debug;
	struct device *dev;
	int ret;

	if (port->debug.debug_port)
		return 0;

	t7xx_cdev_init(port);

	t7xx_debug = port->debug.debug_port;

	port->rx_length_th = RX_QUEUE_MAXLEN;

	ret = alloc_chrdev_region(&t7xx_debug->dev_num, port_conf->baseminor, 1, "t7xx_cdev");
	if (ret) {
		dev_err(port->dev, "Alloc chrdev region failed, ret=%d\n", ret);
		return ret;
	}

	cdev_init(&t7xx_debug->cdev, &char_fops);
	t7xx_debug->cdev.owner = THIS_MODULE;

	ret = cdev_add(&t7xx_debug->cdev, t7xx_debug->dev_num, 1);
	if (ret) {
		dev_err(port->dev, "Add cdev failed, ret=%d\n", ret);
		goto err_cdev_add;
	}

	t7xx_debug->dev_class = class_create(port_conf->class_name);
	if (IS_ERR(t7xx_debug->dev_class)) {
		ret = PTR_ERR(t7xx_debug->dev_class);
		dev_err(port->dev, "Create class failed, ret=%d\n", ret);
		goto err_class_create;
	}

	dev = device_create(t7xx_debug->dev_class, NULL, t7xx_debug->dev_num,
			    NULL, port->port_conf->name);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		dev_err(port->dev, "Create device failed, ret=%d\n", ret);
		goto err_device_create;
	}

	port->debug.debug_port->cdev = t7xx_debug->cdev;
	t7xx_debug->port = port;

	return 0;

err_device_create:
	class_destroy(t7xx_debug->dev_class);
err_class_create:
	cdev_del(&t7xx_debug->cdev);
err_cdev_add:
	unregister_chrdev_region(t7xx_debug->dev_num, 1);
	return ret;
}

static void port_char_uninit(struct t7xx_port *port)
{
	struct t7xx_cdev *t7xx_debug;
	unsigned long flags;
	struct sk_buff *skb;

	if (!port->debug.debug_port)
		return;

	t7xx_debug = port->debug.debug_port;

	device_destroy(t7xx_debug->dev_class, t7xx_debug->dev_num);
	class_destroy(t7xx_debug->dev_class);
	cdev_del(&t7xx_debug->cdev);
	unregister_chrdev_region(t7xx_debug->dev_num, 1);

	t7xx_cdev_uninit(port);

	spin_lock_irqsave(&port->rx_wq.lock, flags);
	while ((skb = __skb_dequeue(&port->rx_skb_list)) != NULL)
		dev_kfree_skb(skb);
	spin_unlock_irqrestore(&port->rx_wq.lock, flags);
}

static int port_char_recv_skb(struct t7xx_port *port, struct sk_buff *skb)
{
	const struct t7xx_port_conf *port_conf = port->port_conf;
	unsigned long flags;

	if (!atomic_read(&port->usage_cnt) || !port->chan_enable) {
		dev_dbg_ratelimited(port->dev, "Port %s is not opened, drop packets\n",
				    port_conf->name);
		return -ENETDOWN;
	}

	spin_lock_irqsave(&port->rx_wq.lock, flags);
	if (port->rx_skb_list.qlen >= port->rx_length_th) {
		spin_unlock_irqrestore(&port->rx_wq.lock, flags);
		return -ENOBUFS;
	}

	__skb_queue_tail(&port->rx_skb_list, skb);
	spin_unlock_irqrestore(&port->rx_wq.lock, flags);

	wake_up_all(&port->rx_wq);

	return 0;
}

static int port_char_enable_chl(struct t7xx_port *port)
{
	spin_lock(&port->port_update_lock);
	port->chan_enable = true;
	spin_unlock(&port->port_update_lock);

	return 0;
}

static int port_char_disable_chl(struct t7xx_port *port)
{
	spin_lock(&port->port_update_lock);
	port->chan_enable = false;
	spin_unlock(&port->port_update_lock);

	return 0;
}

struct port_ops debug_port_ops = {
	.init = &port_char_init,
	.recv_skb = &port_char_recv_skb,
	.uninit = &port_char_uninit,
	.enable_chl = &port_char_enable_chl,
	.disable_chl = &port_char_disable_chl,
};
