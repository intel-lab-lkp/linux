// SPDX-License-Identifier: GPL-2.0
/*
 * Amazon Nitro Secure Module driver.
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 */

#include <linux/nsm.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/wait.h>
#include <uapi/linux/nsm.h>

#define NSM_REQUEST_MAX_SIZE  0x1000
#define NSM_RESPONSE_MAX_SIZE 0x3000

/* Timeout for NSM virtqueue respose in milliseconds. */
#define NSM_DEFAULT_TIMEOUT_MSECS (120000) /* 2 minutes */

struct nsm {
	struct list_head       node;
	struct virtio_device   *vdev;
	struct virtqueue       *vq;
	struct mutex           lock;
	wait_queue_head_t      wq;
	bool                   device_notified;
	struct miscdevice      misc;
};

/* NSM device ID */
static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_NITRO_SEC_MOD, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

/*
 * We want to support nsm_rng, but not link the modules. So we create this
 * intermediate layer to allow the rng driver to register lazily into us.
 */
static struct nsm_hwrng *nsm_hwrng;

/*
 * The rng driver can probe at any time, even after we already finished
 * initializing all nsm devices. Keep a list of all devices around so that
 * we can establish the link dynamically
 */
static LIST_HEAD(nsm_devices);

static inline struct nsm *to_nsm(struct file *file)
{
	return container_of(file->private_data, struct nsm, misc);
}

/* Copy an entire message from user-space to kernel-space */
static int message_memdup_from_user(struct nsm_kernel_message *dst, u64 src_addr)
{
	struct nsm_message shallow_copy;

	if (!src_addr || !dst)
		return -EINVAL;

	/* The destination's request and response buffers should be NULL. */
	if (dst->request.iov_base || dst->response.iov_base)
		return -EINVAL;

	/* First, make a shallow copy to be able to read the inner pointers */
	if (copy_from_user(&shallow_copy, u64_to_user_ptr(src_addr),
			   sizeof(shallow_copy)) != 0)
		return -EINVAL;

	/* Verify the user input size. */
	if (shallow_copy.request.len > NSM_REQUEST_MAX_SIZE)
		return -EMSGSIZE;

	/* Allocate kernel memory for the user request */
	dst->request.iov_len = shallow_copy.request.len;
	dst->request.iov_base = kmalloc(dst->request.iov_len, GFP_KERNEL);
	if (!dst->request.iov_base)
		return -ENOMEM;

	/* Copy the request content */
	if (copy_from_user(dst->request.iov_base,
			   u64_to_user_ptr(shallow_copy.request.addr),
			   dst->request.iov_len) != 0) {
		kfree(dst->request.iov_base);
		return -EFAULT;
	}

	/* Allocate kernel memory for the response, up to a fixed limit */
	dst->response.iov_len = shallow_copy.response.len;
	if (dst->response.iov_len > NSM_RESPONSE_MAX_SIZE)
		dst->response.iov_len = NSM_RESPONSE_MAX_SIZE;

	dst->response.iov_base = kmalloc(dst->response.iov_len, GFP_KERNEL);
	if (!dst->response.iov_base) {
		kfree(dst->request.iov_base);
		return -ENOMEM;
	}

	return 0;
}

/* Copy a message back to user-space */
static int message_copy_to_user(u64 user_addr, struct nsm_kernel_message *kern_msg)
{
	struct nsm_message shallow_copy;

	if (!kern_msg || !user_addr)
		return -EINVAL;

	/*
	 * First, do a shallow copy of the user-space message. This is needed in
	 * order to get the request block data, which we do not need to copy but
	 * must preserve in the message sent back to user-space.
	 */
	if (copy_from_user(&shallow_copy, u64_to_user_ptr(user_addr),
			   sizeof(shallow_copy)) != 0)
		return -EINVAL;

	/* Do not exceed the capacity of the user-provided response buffer */
	shallow_copy.response.len = kern_msg->response.iov_len;

	/* Only the response content must be copied back to user-space */
	if (copy_to_user(u64_to_user_ptr(shallow_copy.response.addr),
		kern_msg->response.iov_base,
		shallow_copy.response.len) != 0)
		return -EINVAL;

	if (copy_to_user(u64_to_user_ptr(user_addr), &shallow_copy,
			 sizeof(shallow_copy)) != 0)
		return -EFAULT;

	return 0;
}

/* Virtqueue interrupt handler */
static void nsm_vq_callback(struct virtqueue *vq)
{
	struct nsm *nsm = vq->vdev->priv;

	nsm->device_notified = true;
	wake_up(&nsm->wq);
}

/* Forward a message to the NSM device and wait for the response from it */
int nsm_communicate_with_device(struct virtio_device *vdev,
				struct nsm_kernel_message *message)
{
	struct nsm *nsm = vdev->priv;
	struct virtqueue *vq = nsm->vq;
	struct scatterlist sg_in, sg_out;
	unsigned int len;
	void *queue_buf;
	bool kicked;
	int rc;

	if (!vdev)
		return -EINVAL;

	if (!message)
		return -EINVAL;

	/* Verify if buffer memory is valid. */
	if (!virt_addr_valid(message->request.iov_base) ||
		!virt_addr_valid(((u8 *)message->request.iov_base) +
			message->request.iov_len - 1) ||
		!virt_addr_valid(message->response.iov_base) ||
		!virt_addr_valid(((u8 *)message->response.iov_base) +
			message->response.iov_len - 1))
		return -EINVAL;

	/* Initialize scatter-gather lists with request and response buffers. */
	sg_init_one(&sg_out, message->request.iov_base,
		message->request.iov_len);
	sg_init_one(&sg_in, message->response.iov_base,
		message->response.iov_len);

	mutex_lock(&nsm->lock);

	/* Add the request buffer (read by the device). */
	rc = virtqueue_add_outbuf(vq, &sg_out, 1, message->request.iov_base,
		GFP_KERNEL);
	if (rc) {
		mutex_unlock(&nsm->lock);
		return rc;
	}

	/* Add the response buffer (written by the device). */
	rc = virtqueue_add_inbuf(vq, &sg_in, 1, message->response.iov_base,
		GFP_KERNEL);
	if (rc)
		goto cleanup;

	nsm->device_notified = false;
	kicked = virtqueue_kick(vq);
	if (!kicked) {
		/* Cannot kick the virtqueue. */
		rc = -EIO;
		goto cleanup;
	}

	/* If the kick succeeded, wait for the device's response. */
	rc = wait_event_timeout(nsm->wq,
		nsm->device_notified == true,
		msecs_to_jiffies(NSM_DEFAULT_TIMEOUT_MSECS));
	if (!rc) {
		rc = -ETIMEDOUT;
		goto cleanup;
	}

	queue_buf = virtqueue_get_buf(vq, &len);
	if (!queue_buf || (queue_buf != message->request.iov_base)) {
		pr_err("NSM device received wrong request buffer.");
		rc = -ENODATA;
		goto cleanup;
	}

	queue_buf = virtqueue_get_buf(vq, &len);
	if (!queue_buf || (queue_buf != message->response.iov_base)) {
		pr_err("NSM device received wrong response buffer.");
		rc = -ENODATA;
		goto cleanup;
	}

	/* Make sure the response length doesn't exceed the buffer capacity. */
	if (len < message->response.iov_len)
		message->response.iov_len = len;

	rc = 0;

cleanup:
	if (rc) {
		/* Clean the virtqueue. */
		while (virtqueue_get_buf(vq, &len) != NULL)
			;
	}

	mutex_unlock(&nsm->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(nsm_communicate_with_device);

static long nsm_dev_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	struct nsm_kernel_message message = {};
	struct nsm *nsm = to_nsm(file);
	int status = 0;

	if (cmd != NSM_IOCTL_REQUEST)
		return -EINVAL;

	/* Copy the message from user-space to kernel-space */
	status = message_memdup_from_user(&message, arg);
	if (status != 0)
		return status;

	/* Communicate with the NSM device */
	status = nsm_communicate_with_device(nsm->vdev, &message);

	if (status != 0)
		goto out;

	/* Copy the response back to user-space */
	status = message_copy_to_user(arg, &message);

out:
	/* At this point, everything succeeded, so clean up and finish. */
	kfree(message.request.iov_base);
	kfree(message.response.iov_base);

	return status;
}

static int nsm_dev_file_open(struct inode *node, struct file *file)
{
	return 0;
}

static int nsm_dev_file_close(struct inode *inode, struct file *file)
{
	return 0;
}

static int nsm_device_init_vq(struct virtio_device *vdev)
{
	struct virtqueue *vq = virtio_find_single_vq(vdev,
		nsm_vq_callback, "nsm.vq.0");
	struct nsm *nsm = vdev->priv;

	if (IS_ERR(vq))
		return PTR_ERR(vq);

	nsm->vq = vq;

	return 0;
}

static const struct file_operations nsm_dev_fops = {
	.open = nsm_dev_file_open,
	.release = nsm_dev_file_close,
	.unlocked_ioctl = nsm_dev_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

/* Handler for probing the NSM device */
static int nsm_device_probe(struct virtio_device *vdev)
{
	struct nsm *nsm;
	int rc;

	nsm = kzalloc(sizeof(*nsm), GFP_KERNEL);
	if (!nsm)
		return -ENOMEM;

	vdev->priv = nsm;
	nsm->vdev = vdev;

	rc = nsm_device_init_vq(vdev);
	if (rc) {
		pr_err("NSM device queue failed to initialize: %d.\n", rc);
		goto err_init_vq;
	}

	mutex_init(&nsm->lock);
	init_waitqueue_head(&nsm->wq);

	nsm->misc = (struct miscdevice) {
		.minor	= MISC_DYNAMIC_MINOR,
		.name	= "nsm",
		.fops	= &nsm_dev_fops,
		.mode	= 0666
	};

	rc = misc_register(&nsm->misc);
	if (rc) {
		pr_err("NSM misc device registration error: %d.\n", rc);
		goto err_misc;
	}

	if (nsm_hwrng)
		nsm_hwrng->probe(vdev);

	list_add(&nsm->node, &nsm_devices);

	return 0;

err_misc:
	vdev->config->del_vqs(vdev);
err_init_vq:
	kfree(nsm);
	return rc;
}

/* Handler for removing the NSM device */
static void nsm_device_remove(struct virtio_device *vdev)
{
	struct nsm *nsm = vdev->priv;

	if (nsm_hwrng)
		nsm_hwrng->remove(vdev);

	vdev->config->del_vqs(vdev);
	misc_deregister(&nsm->misc);
	list_del(&nsm->node);
}

int nsm_register_hwrng(struct nsm_hwrng *_nsm_hwrng)
{
	struct nsm *nsm;

	if (nsm_hwrng)
		return -EEXIST;

	nsm_hwrng = _nsm_hwrng;

	list_for_each_entry(nsm, &nsm_devices, node)
		nsm_hwrng->probe(nsm->vdev);

	return 0;
}
EXPORT_SYMBOL_GPL(nsm_register_hwrng);

void nsm_unregister_hwrng(struct nsm_hwrng *_nsm_hwrng)
{
	struct nsm *nsm;

	if (_nsm_hwrng != nsm_hwrng)
		return;

	list_for_each_entry(nsm, &nsm_devices, node)
		nsm_hwrng->remove(nsm->vdev);
	nsm_hwrng = NULL;
}
EXPORT_SYMBOL_GPL(nsm_unregister_hwrng);

/* NSM device configuration structure */
static struct virtio_driver virtio_nsm_driver = {
	.feature_table             = 0,
	.feature_table_size        = 0,
	.feature_table_legacy      = 0,
	.feature_table_size_legacy = 0,
	.driver.name               = KBUILD_MODNAME,
	.driver.owner              = THIS_MODULE,
	.id_table                  = id_table,
	.probe                     = nsm_device_probe,
	.remove                    = nsm_device_remove,
};

module_virtio_driver(virtio_nsm_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio NSM driver");
MODULE_LICENSE("GPL");
