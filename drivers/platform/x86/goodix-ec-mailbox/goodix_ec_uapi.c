// SPDX-License-Identifier: GPL-2.0-only
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include "goodix_ec_mailbox.h"
#include <linux/goodix_ec.h>

#define GOODIX_EC_RX_FIFO_BYTES (256u * 1024u)

static bool goodix_ec_rx_ready(struct goodix_device *gdev)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&gdev->rx_fifo_lock, flags);
	ready = gdev->rx_fifo_ready && !kfifo_is_empty(&gdev->rx_fifo);
	spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);

	return ready;
}

static int goodix_ec_uapi_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct goodix_device *gdev =
		container_of(misc, struct goodix_device, miscdev);
	unsigned long flags;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (!goodix_ec_device_get(gdev))
		return -ENODEV;
	if (READ_ONCE(gdev->disconnected)) {
		goodix_ec_device_put(gdev);
		return -ENODEV;
	}

	if (file->f_mode & FMODE_READ) {
		spin_lock_irqsave(&gdev->rx_fifo_lock, flags);
		if (!gdev->rx_fifo_ready) {
			spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
			goodix_ec_device_put(gdev);
			return -ENODEV;
		}

		if (gdev->rx_reader_open) {
			spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
			goodix_ec_device_put(gdev);
			return -EBUSY;
		}

		gdev->rx_reader_open = true;
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
	}

	file->private_data = gdev;
	return 0;
}

static int goodix_ec_uapi_release(struct inode *inode, struct file *file)
{
	struct goodix_device *gdev = file->private_data;
	unsigned long flags;

	if (gdev && (file->f_mode & FMODE_READ)) {
		spin_lock_irqsave(&gdev->rx_fifo_lock, flags);
		gdev->rx_reader_open = false;
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
	}
	if (gdev)
		goodix_ec_device_put(gdev);

	return 0;
}

static ssize_t goodix_ec_uapi_write(struct file *file,
				    const char __user *user_buffer,
				    size_t count, loff_t *position)
{
	struct goodix_device *gdev = file->private_data;
	struct goodix_ec_tx_header header;
	u8 *mp_packet;
	size_t required;
	int ret;

	if (!gdev || !user_buffer || READ_ONCE(gdev->disconnected))
		return -ENODEV;

	if (count < sizeof(header))
		return -EINVAL;

	if (copy_from_user(&header, user_buffer, sizeof(header)))
		return -EFAULT;

	if (header.payload_len > GOODIX_EC_UAPI_TX_MAX)
		return -EMSGSIZE;
	if (header.reserved || header.flags)
		return -EINVAL;

	required = sizeof(header) + header.payload_len;
	if (count != required)
		return -EINVAL;

	mp_packet = kmalloc(GOODIX_MP_HEADER_SIZE +
			    header.payload_len, GFP_KERNEL);
	if (!mp_packet)
		return -ENOMEM;

	mp_packet[0] = header.mp_flags;
	put_unaligned_le16(header.payload_len, mp_packet + 1);
	mp_packet[3] = mp_packet[0] + mp_packet[1] + mp_packet[2];

	if (header.payload_len &&
	    copy_from_user(mp_packet + sizeof(struct goodix_mp_header),
			   user_buffer + sizeof(header),
			   header.payload_len)) {
		kfree(mp_packet);
		return -EFAULT;
	}

	mutex_lock(&gdev->transfer_lock);
	if (READ_ONCE(gdev->disconnected))
		ret = -ENODEV;
	else if (READ_ONCE(gdev->suspended))
		ret = -EHOSTDOWN;
	else
		ret = goodix_ec_sync_send(gdev, mp_packet,
					  sizeof(struct goodix_mp_header) +
					  header.payload_len);
	mutex_unlock(&gdev->transfer_lock);

	kfree(mp_packet);

	if (ret)
		return ret;

	return count;
}

static ssize_t goodix_ec_uapi_read(struct file *file, char __user *user_buffer,
				   size_t count, loff_t *position)
{
	struct goodix_device *gdev = file->private_data;
	struct goodix_ec_record_header header;
	unsigned long flags;
	u8 *record;
	size_t required;
	int ret;

	if (!gdev || !user_buffer || READ_ONCE(gdev->disconnected))
		return -ENODEV;

	ret = mutex_lock_interruptible(&gdev->rx_read_lock);
	if (ret)
		return ret;

	for (;;) {
		spin_lock_irqsave(&gdev->rx_fifo_lock, flags);

		if (!gdev->rx_fifo_ready) {
			spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
			ret = -ENODEV;
			goto out_unlock;
		}
		if (READ_ONCE(gdev->suspended)) {
			spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
			ret = -EHOSTDOWN;
			goto out_unlock;
		}

		if (kfifo_len(&gdev->rx_fifo) >= sizeof(header))
			break;

		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);

		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out_unlock;
		}

		ret = wait_event_interruptible(gdev->rx_wait,
					       goodix_ec_rx_ready(gdev) ||
					       !READ_ONCE(gdev->rx_fifo_ready) ||
					       READ_ONCE(gdev->suspended));
		if (ret)
			goto out_unlock;
	}

	if (kfifo_out_peek(&gdev->rx_fifo, &header,
			   sizeof(header)) != sizeof(header)) {
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
		ret = -EIO;
		goto out_unlock;
	}

	required = sizeof(header) + header.len;
	if (kfifo_len(&gdev->rx_fifo) < required) {
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
		ret = -EIO;
		goto out_unlock;
	}

	if (count < required) {
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
		ret = -EMSGSIZE;
		goto out_unlock;
	}

	record = kmalloc(required, GFP_ATOMIC);
	if (!record) {
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
		ret = -ENOMEM;
		goto out_unlock;
	}

	if (kfifo_out(&gdev->rx_fifo, record, required) != required) {
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
		kfree(record);
		ret = -EIO;
		goto out_unlock;
	}

	spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);

	if (copy_to_user(user_buffer, record, required)) {
		kfree(record);
		ret = -EFAULT;
		goto out_unlock;
	}

	kfree(record);
	ret = required;

out_unlock:
	mutex_unlock(&gdev->rx_read_lock);
	return ret;
}

static __poll_t goodix_ec_uapi_poll(struct file *file, poll_table *wait)
{
	struct goodix_device *gdev = file->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	if (!gdev || READ_ONCE(gdev->disconnected))
		return EPOLLERR;

	poll_wait(file, &gdev->rx_wait, wait);

	spin_lock_irqsave(&gdev->rx_fifo_lock, flags);
	if (!gdev->rx_fifo_ready || READ_ONCE(gdev->disconnected))
		mask = EPOLLERR | EPOLLHUP;
	else if (READ_ONCE(gdev->suspended))
		mask = EPOLLERR;
	else if (!kfifo_is_empty(&gdev->rx_fifo))
		mask = EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);

	return mask;
}

static long goodix_ec_uapi_ioctl(struct file *file,
				 unsigned int command,
				 unsigned long argument)
{
	struct goodix_device *gdev = file->private_data;
	unsigned long flags;

	if (!gdev || READ_ONCE(gdev->disconnected))
		return -ENODEV;

	switch (command) {
	case GOODIX_EC_IOCTL_FLUSH_RX:
		spin_lock_irqsave(&gdev->rx_fifo_lock, flags);
		if (gdev->rx_fifo_ready)
			kfifo_reset(&gdev->rx_fifo);
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations goodix_ec_uapi_fops = {
	.owner = THIS_MODULE,
	.open = goodix_ec_uapi_open,
	.release = goodix_ec_uapi_release,
	.read = goodix_ec_uapi_read,
	.write = goodix_ec_uapi_write,
	.poll = goodix_ec_uapi_poll,
	.unlocked_ioctl = goodix_ec_uapi_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

int goodix_ec_uapi_register(struct goodix_device *gdev)
{
	int ret;

	spin_lock_init(&gdev->rx_fifo_lock);
	mutex_init(&gdev->rx_read_lock);
	init_waitqueue_head(&gdev->rx_wait);

	ret = kfifo_alloc(&gdev->rx_fifo, GOODIX_EC_RX_FIFO_BYTES, GFP_KERNEL);
	if (ret)
		return ret;

	gdev->rx_fifo_ready = true;
	gdev->rx_reader_open = false;

	gdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	gdev->miscdev.name = "gxfp";
	gdev->miscdev.fops = &goodix_ec_uapi_fops;
	gdev->miscdev.parent = gdev->dev;

	ret = misc_register(&gdev->miscdev);
	if (ret) {
		gdev->rx_fifo_ready = false;
		kfifo_free(&gdev->rx_fifo);
		return ret;
	}

	gdev->misc_registered = true;
	dev_info(gdev->dev, "userspace interface registered: /dev/gxfp\n");
	return 0;
}

void goodix_ec_uapi_unregister(struct goodix_device *gdev)
{
	unsigned long flags;

	if (!gdev)
		return;

	if (gdev->misc_registered) {
		misc_deregister(&gdev->miscdev);
		gdev->misc_registered = false;
	}

	if (!gdev->rx_fifo_ready)
		return;

	spin_lock_irqsave(&gdev->rx_fifo_lock, flags);
	gdev->rx_fifo_ready = false;
	gdev->rx_reader_open = false;
	kfifo_reset(&gdev->rx_fifo);
	spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);

	wake_up_interruptible_all(&gdev->rx_wait);
	kfifo_free(&gdev->rx_fifo);
}
