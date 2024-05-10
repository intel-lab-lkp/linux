// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2024 NXP
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/firmware.h>
#include <linux/firmware/imx/se_api.h>
#include <linux/genalloc.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sys_soc.h>
#include <uapi/linux/se_ioctl.h>

#include "ele_base_msg.h"
#include "ele_common.h"
#include "se_ctrl.h"

#define RESERVED_DMA_POOL		BIT(1)

struct imx_se_node_info {
	u8 se_if_id;
	u8 se_if_did;
	u8 max_dev_ctx;
	u8 cmd_tag;
	u8 rsp_tag;
	u8 success_tag;
	u8 base_api_ver;
	u8 fw_api_ver;
	u8 *se_name;
	u8 *mbox_tx_name;
	u8 *mbox_rx_name;
	u8 *pool_name;
	u8 *fw_name_in_rfs;
	bool soc_register;
	bool reserved_dma_ranges;
	bool imem_mgmt;
};

struct imx_se_node_info_list {
	u8 num_mu;
	u16 soc_id;
	u16 soc_rev;
	struct imx_se_node_info info[];
};

static const struct imx_se_node_info_list imx8ulp_info = {
	.num_mu = 1,
	.soc_id = SOC_ID_OF_IMX8ULP,
	.info = {
			{
				.se_if_id = 2,
				.se_if_did = 7,
				.max_dev_ctx = 4,
				.cmd_tag = 0x17,
				.rsp_tag = 0xe1,
				.success_tag = 0xd6,
				.base_api_ver = MESSAGING_VERSION_6,
				.fw_api_ver = MESSAGING_VERSION_7,
				.se_name = "hsm1",
				.mbox_tx_name = "tx",
				.mbox_rx_name = "rx",
				.pool_name = "sram",
				.fw_name_in_rfs = IMX_ELE_FW_DIR\
						  "mx8ulpa2ext-ahab-container.img",
				.soc_register = true,
				.reserved_dma_ranges = true,
				.imem_mgmt = true,
			},
	},
};

static const struct imx_se_node_info_list imx93_info = {
	.num_mu = 1,
	.soc_id = SOC_ID_OF_IMX93,
	.info = {
			{
				.se_if_id = 2,
				.se_if_did = 3,
				.max_dev_ctx = 4,
				.cmd_tag = 0x17,
				.rsp_tag = 0xe1,
				.success_tag = 0xd6,
				.base_api_ver = MESSAGING_VERSION_6,
				.fw_api_ver = MESSAGING_VERSION_7,
				.se_name = "hsm1",
				.mbox_tx_name = "tx",
				.mbox_rx_name = "rx",
				.reserved_dma_ranges = true,
				.imem_mgmt = true,
				.soc_register = true,
			},
	},
};

static const struct of_device_id se_match[] = {
	{ .compatible = "fsl,imx8ulp-ele", .data = (void *)&imx8ulp_info},
	{ .compatible = "fsl,imx93-ele", .data = (void *)&imx93_info},
	{},
};

static struct imx_se_node_info
		*get_imx_se_node_info(struct imx_se_node_info_list *info_list,
				      const u32 idx)
{
	if (idx < 0 || idx > info_list->num_mu)
		return NULL;

	return &info_list->info[idx];
}

void *get_phy_buf_mem_pool(struct device *dev,
			   u8 *mem_pool_name,
			   dma_addr_t *buf,
			   u32 size)
{
	struct device_node *of_node = dev->of_node;
	struct gen_pool *mem_pool;

	mem_pool = of_gen_pool_get(of_node, mem_pool_name, 0);
	if (!mem_pool) {
		dev_err(dev,
			"Unable to get sram pool = %s\n",
			mem_pool_name);
		return 0;
	}

	return gen_pool_dma_alloc(mem_pool, size, buf);
}

void free_phybuf_mem_pool(struct device *dev,
			  u8 *mem_pool_name,
			  u32 *buf,
			  u32 size)
{
	struct device_node *of_node = dev->of_node;
	struct gen_pool *mem_pool;

	mem_pool = of_gen_pool_get(of_node, mem_pool_name, 0);
	if (!mem_pool)
		dev_err(dev,
			"%s: Failed: Unable to get sram pool.\n",
			__func__);

	gen_pool_free(mem_pool, (u64)buf, size);
}

static int imx_fetch_soc_info(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct imx_se_node_info_list *info_list;
	const struct imx_se_node_info *info;
	struct soc_device_attribute *attr;
	struct soc_device *sdev;
	struct soc_info s_info;
	int err = 0;

	info = priv->info;
	info_list = (struct imx_se_node_info_list *)
				device_get_match_data(dev->parent);
	if (info_list->soc_rev)
		return err;

	err = ele_get_info(dev, &s_info);
	if (err)
		s_info.major_ver = DEFAULT_IMX_SOC_VER;

	info_list->soc_rev = s_info.soc_rev;

	if (!info->soc_register)
		return 0;

	attr = devm_kzalloc(dev, sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return -ENOMEM;

	if (s_info.minor_ver)
		attr->revision = devm_kasprintf(dev, GFP_KERNEL, "%x.%x",
					   s_info.major_ver,
					   s_info.minor_ver);
	else
		attr->revision = devm_kasprintf(dev, GFP_KERNEL, "%x",
					   s_info.major_ver);

	switch (s_info.soc_id) {
	case SOC_ID_OF_IMX8ULP:
		attr->soc_id = devm_kasprintf(dev, GFP_KERNEL,
					      "i.MX8ULP");
		break;
	case SOC_ID_OF_IMX93:
		attr->soc_id = devm_kasprintf(dev, GFP_KERNEL,
					      "i.MX93");
		break;
	}

	err = of_property_read_string(of_root, "model",
				      &attr->machine);
	if (err) {
		devm_kfree(dev, attr);
		return -EINVAL;
	}
	attr->family = devm_kasprintf(dev, GFP_KERNEL, "Freescale i.MX");

	attr->serial_number
		= devm_kasprintf(dev, GFP_KERNEL, "%016llX", s_info.serial_num);

	sdev = soc_device_register(attr);
	if (IS_ERR(sdev)) {
		devm_kfree(dev, attr->soc_id);
		devm_kfree(dev, attr->serial_number);
		devm_kfree(dev, attr->revision);
		devm_kfree(dev, attr->family);
		devm_kfree(dev, attr->machine);
		devm_kfree(dev, attr);
		return PTR_ERR(sdev);
	}

	return 0;
}

/*
 * File operations for user-space
 */

/* Write a message to the MU. */
static ssize_t se_if_fops_write(struct file *fp, const char __user *buf,
				size_t size, loff_t *ppos)
{
	struct se_api_msg *tx_msg __free(kfree);
	struct se_if_device_ctx *dev_ctx;
	struct se_if_priv *priv;
	int err;

	dev_ctx = container_of(fp->private_data,
			       struct se_if_device_ctx,
			       miscdev);
	priv = dev_ctx->priv;
	dev_dbg(priv->dev,
		"%s: write from buf (%p)%zu, ppos=%lld\n",
			dev_ctx->miscdev.name,
			buf, size, ((ppos) ? *ppos : 0));

	if (down_interruptible(&dev_ctx->fops_lock))
		return -EBUSY;

	if (dev_ctx->status != MU_OPENED) {
		err = -EINVAL;
		goto exit;
	}

	if (size < SE_MU_HDR_SZ) {
		dev_err(priv->dev,
			"%s: User buffer too small(%zu < %d)\n",
				dev_ctx->miscdev.name,
				size, SE_MU_HDR_SZ);
		err = -ENOSPC;
		goto exit;
	}

	tx_msg = memdup_user((void __user *)ppos, size);
	if (!tx_msg) {
		err = -ENOMEM;
		goto exit;
	}

	/* Copy data to buffer */
	if (copy_from_user(tx_msg, buf, size)) {
		err = -EFAULT;
		dev_err(priv->dev,
			"%s: Fail copy message from user\n",
				dev_ctx->miscdev.name);
		goto exit;
	}

	print_hex_dump_debug("from user ", DUMP_PREFIX_OFFSET, 4, 4,
			     tx_msg, size, false);

	err = imx_ele_miscdev_msg_send(dev_ctx, tx_msg, size);

exit:
	up(&dev_ctx->fops_lock);
	return err;
}

/*
 * Read a message from the MU.
 * Blocking until a message is available.
 */
static ssize_t se_if_fops_read(struct file *fp, char __user *buf,
			       size_t size, loff_t *ppos)
{
	struct se_if_device_ctx *dev_ctx;
	struct se_buf_desc *b_desc;
	struct se_if_priv *priv;
	u32 size_to_copy;
	int err;

	dev_ctx = container_of(fp->private_data,
			       struct se_if_device_ctx,
			       miscdev);
	priv = dev_ctx->priv;
	dev_dbg(priv->dev,
		"%s: read to buf %p(%zu), ppos=%lld\n",
			dev_ctx->miscdev.name,
			buf, size, ((ppos) ? *ppos : 0));

	if (down_interruptible(&dev_ctx->fops_lock))
		return -EBUSY;

	if (dev_ctx->status != MU_OPENED) {
		err = -EINVAL;
		goto exit;
	}

	err = imx_ele_miscdev_msg_rcv(dev_ctx);
	if (err)
		goto exit;

	/* Buffer containing the message from FW, is
	 * allocated in callback function.
	 * Check if buffer allocation failed.
	 */
	if (!dev_ctx->temp_resp) {
		err = -ENOMEM;
		goto exit;
	}

	dev_dbg(priv->dev,
			"%s: %s %s\n",
			dev_ctx->miscdev.name,
			__func__,
			"message received, start transmit to user");

	/*
	 * Check that the size passed as argument is larger than
	 * the one carried in the message.
	 */
	size_to_copy = dev_ctx->temp_resp_size << 2;
	if (size_to_copy > size) {
		dev_dbg(priv->dev,
			"%s: User buffer too small (%zu < %d)\n",
				dev_ctx->miscdev.name,
				size, size_to_copy);
		size_to_copy = size;
	}

	/*
	 * We may need to copy the output data to user before
	 * delivering the completion message.
	 */
	while (!list_empty(&dev_ctx->pending_out)) {
		b_desc = list_first_entry_or_null(&dev_ctx->pending_out,
						  struct se_buf_desc,
						  link);
		if (!b_desc)
			continue;

		if (b_desc->usr_buf_ptr && b_desc->shared_buf_ptr) {

			dev_dbg(priv->dev,
				"%s: Copy output data to user\n",
				dev_ctx->miscdev.name);
			if (copy_to_user(b_desc->usr_buf_ptr,
					 b_desc->shared_buf_ptr,
					 b_desc->size)) {
				dev_err(priv->dev,
					"%s: Failure copying output data to user.",
					dev_ctx->miscdev.name);
				err = -EFAULT;
				goto exit;
			}
		}

		if (b_desc->shared_buf_ptr)
			memset(b_desc->shared_buf_ptr, 0, b_desc->size);

		__list_del_entry(&b_desc->link);
		kfree(b_desc);
	}

	/* Copy data from the buffer */
	print_hex_dump_debug("to user ", DUMP_PREFIX_OFFSET, 4, 4,
			     dev_ctx->temp_resp, size_to_copy, false);
	if (copy_to_user(buf, dev_ctx->temp_resp, size_to_copy)) {
		dev_err(priv->dev,
			"%s: Failed to copy to user\n",
				dev_ctx->miscdev.name);
		err = -EFAULT;
		goto exit;
	}

	err = size_to_copy;
	kfree(dev_ctx->temp_resp);

	/* free memory allocated on the shared buffers. */
	dev_ctx->secure_mem.pos = 0;
	dev_ctx->non_secure_mem.pos = 0;

	dev_ctx->pending_hdr = 0;

exit:
	/*
	 * Clean the used Shared Memory space,
	 * whether its Input Data copied from user buffers, or
	 * Data received from FW.
	 */
	while (!list_empty(&dev_ctx->pending_in) ||
	       !list_empty(&dev_ctx->pending_out)) {
		if (!list_empty(&dev_ctx->pending_in))
			b_desc = list_first_entry_or_null(&dev_ctx->pending_in,
							  struct se_buf_desc,
							  link);
		else
			b_desc = list_first_entry_or_null(&dev_ctx->pending_out,
							  struct se_buf_desc,
							  link);

		if (!b_desc)
			continue;

		if (b_desc->shared_buf_ptr)
			memset(b_desc->shared_buf_ptr, 0, b_desc->size);

		__list_del_entry(&b_desc->link);
		kfree(b_desc);
	}

	up(&dev_ctx->fops_lock);
	return err;
}

/* Give access to EdgeLock Enclave, to the memory we want to share */
static int se_if_setup_se_mem_access(struct se_if_device_ctx *dev_ctx,
				     u64 addr, u32 len)
{
	/* Assuming EdgeLock Enclave has access to all the memory regions */
	int ret = 0;

	if (ret) {
		dev_err(dev_ctx->priv->dev,
			"%s: Fail find memreg\n", dev_ctx->miscdev.name);
		goto exit;
	}

	if (ret) {
		dev_err(dev_ctx->priv->dev,
			"%s: Fail set permission for resource\n",
				dev_ctx->miscdev.name);
		goto exit;
	}

exit:
	return ret;
}

static int se_ioctl_get_mu_info(struct se_if_device_ctx *dev_ctx,
				u64 arg)
{
	struct se_if_priv *priv = dev_get_drvdata(dev_ctx->dev);
	struct imx_se_node_info *if_node_info;
	struct se_ioctl_get_if_info info;
	int err = 0;

	if_node_info = (struct imx_se_node_info *)priv->info;

	info.se_if_id = if_node_info->se_if_id;
	info.interrupt_idx = 0;
	info.tz = 0;
	info.did = if_node_info->se_if_did;
	info.cmd_tag = if_node_info->cmd_tag;
	info.rsp_tag = if_node_info->rsp_tag;
	info.success_tag = if_node_info->success_tag;
	info.base_api_ver = if_node_info->base_api_ver;
	info.fw_api_ver = if_node_info->fw_api_ver;

	dev_dbg(priv->dev,
		"%s: info [se_if_id: %d, irq_idx: %d, tz: 0x%x, did: 0x%x]\n",
			dev_ctx->miscdev.name,
			info.se_if_id, info.interrupt_idx, info.tz, info.did);

	if (copy_to_user((u8 *)arg, &info, sizeof(info))) {
		dev_err(dev_ctx->priv->dev,
			"%s: Failed to copy mu info to user\n",
				dev_ctx->miscdev.name);
		err = -EFAULT;
		goto exit;
	}

exit:
	return err;
}

/*
 * Copy a buffer of data to/from the user and return the address to use in
 * messages
 */
static int se_ioctl_setup_iobuf_handler(struct se_if_device_ctx *dev_ctx,
					    u64 arg)
{
	struct se_ioctl_setup_iobuf io = {0};
	struct se_shared_mem *shared_mem;
	struct se_buf_desc *b_desc;
	int err = 0;
	u32 pos;

	if (copy_from_user(&io, (u8 *)arg, sizeof(io))) {
		dev_err(dev_ctx->priv->dev,
			"%s: Failed copy iobuf config from user\n",
				dev_ctx->miscdev.name);
		err = -EFAULT;
		goto exit;
	}

	dev_dbg(dev_ctx->priv->dev,
			"%s: io [buf: %p(%d) flag: %x]\n",
			dev_ctx->miscdev.name,
			io.user_buf, io.length, io.flags);

	if (io.length == 0 || !io.user_buf) {
		/*
		 * Accept NULL pointers since some buffers are optional
		 * in FW commands. In this case we should return 0 as
		 * pointer to be embedded into the message.
		 * Skip all data copy part of code below.
		 */
		io.ele_addr = 0;
		goto copy;
	}

	/* Select the shared memory to be used for this buffer. */
	if (io.flags & SE_MU_IO_FLAGS_USE_SEC_MEM) {
		/* App requires to use secure memory for this buffer.*/
		dev_err(dev_ctx->priv->dev,
			"%s: Failed allocate SEC MEM memory\n",
				dev_ctx->miscdev.name);
		err = -EFAULT;
		goto exit;
	} else {
		/* No specific requirement for this buffer. */
		shared_mem = &dev_ctx->non_secure_mem;
	}

	/* Check there is enough space in the shared memory. */
	if (shared_mem->size < shared_mem->pos
			|| io.length >= shared_mem->size - shared_mem->pos) {
		dev_err(dev_ctx->priv->dev,
			"%s: Not enough space in shared memory\n",
				dev_ctx->miscdev.name);
		err = -ENOMEM;
		goto exit;
	}

	/* Allocate space in shared memory. 8 bytes aligned. */
	pos = shared_mem->pos;
	shared_mem->pos += round_up(io.length, 8u);
	io.ele_addr = (u64)shared_mem->dma_addr + pos;

	if ((io.flags & SE_MU_IO_FLAGS_USE_SEC_MEM) &&
	    !(io.flags & SE_MU_IO_FLAGS_USE_SHORT_ADDR)) {
		/*Add base address to get full address.*/
		dev_err(dev_ctx->priv->dev,
			"%s: Failed allocate SEC MEM memory\n",
				dev_ctx->miscdev.name);
		err = -EFAULT;
		goto exit;
	}

	memset(shared_mem->ptr + pos, 0, io.length);
	if ((io.flags & SE_IO_BUF_FLAGS_IS_INPUT) ||
	    (io.flags & SE_IO_BUF_FLAGS_IS_IN_OUT)) {
		/*
		 * buffer is input:
		 * copy data from user space to this allocated buffer.
		 */
		if (copy_from_user(shared_mem->ptr + pos, io.user_buf,
				   io.length)) {
			dev_err(dev_ctx->priv->dev,
				"%s: Failed copy data to shared memory\n",
				dev_ctx->miscdev.name);
			err = -EFAULT;
			goto exit;
		}
	}

	b_desc = kzalloc(sizeof(*b_desc), GFP_KERNEL);
	if (!b_desc) {
		err = -ENOMEM;
		goto exit;
	}

copy:
	/* Provide the EdgeLock Enclave address to user space only if success.*/
	if (copy_to_user((u8 *)arg, &io, sizeof(io))) {
		dev_err(dev_ctx->priv->dev,
			"%s: Failed to copy iobuff setup to user\n",
				dev_ctx->miscdev.name);
		kfree(b_desc);
		err = -EFAULT;
		goto exit;
	}

	if (b_desc) {
		b_desc->shared_buf_ptr = shared_mem->ptr + pos;
		b_desc->usr_buf_ptr = io.user_buf;
		b_desc->size = io.length;

		if (io.flags & SE_IO_BUF_FLAGS_IS_INPUT) {
			/*
			 * buffer is input:
			 * add an entry in the "pending input buffers" list so
			 * that copied data can be cleaned from shared memory
			 * later.
			 */
			list_add_tail(&b_desc->link, &dev_ctx->pending_in);
		} else {
			/*
			 * buffer is output:
			 * add an entry in the "pending out buffers" list so data
			 * can be copied to user space when receiving Secure-Enclave
			 * response.
			 */
			list_add_tail(&b_desc->link, &dev_ctx->pending_out);
		}
	}

exit:
	return err;
}

/* IOCTL to provide SoC information */
static int se_ioctl_get_soc_info_handler(struct se_if_device_ctx *dev_ctx,
					     u64 arg)
{
	struct imx_se_node_info_list *info_list;
	struct se_ioctl_get_soc_info soc_info;
	int err = -EINVAL;

	info_list = (struct imx_se_node_info_list *)
			device_get_match_data(dev_ctx->priv->dev->parent);
	if (!info_list)
		goto exit;

	soc_info.soc_id = info_list->soc_id;
	soc_info.soc_rev = info_list->soc_rev;

	err = (int)copy_to_user((u8 *)arg, (u8 *)(&soc_info), sizeof(soc_info));
	if (err) {
		dev_err(dev_ctx->priv->dev,
			"%s: Failed to copy soc info to user\n",
			dev_ctx->miscdev.name);
		err = -EFAULT;
		goto exit;
	}

exit:
	return err;
}

/* Open a character device. */
static int se_if_fops_open(struct inode *nd, struct file *fp)
{
	struct se_if_device_ctx *dev_ctx = container_of(fp->private_data,
							struct se_if_device_ctx,
							miscdev);
	int err;

	/* Avoid race if opened at the same time */
	if (down_trylock(&dev_ctx->fops_lock))
		return -EBUSY;

	/* Authorize only 1 instance. */
	if (dev_ctx->status != MU_FREE) {
		err = -EBUSY;
		goto exit;
	}

	/*
	 * Allocate some memory for data exchanges with S40x.
	 * This will be used for data not requiring secure memory.
	 */
	dev_ctx->non_secure_mem.ptr = dmam_alloc_coherent(dev_ctx->dev,
					MAX_DATA_SIZE_PER_USER,
					&dev_ctx->non_secure_mem.dma_addr,
					GFP_KERNEL);
	if (!dev_ctx->non_secure_mem.ptr) {
		err = -ENOMEM;
		goto exit;
	}

	err = se_if_setup_se_mem_access(dev_ctx,
					  dev_ctx->non_secure_mem.dma_addr,
					  MAX_DATA_SIZE_PER_USER);
	if (err) {
		err = -EPERM;
		dev_err(dev_ctx->priv->dev,
			"%s: Failed to share access to shared memory\n",
			   dev_ctx->miscdev.name);
		goto free_coherent;
	}

	dev_ctx->non_secure_mem.size = MAX_DATA_SIZE_PER_USER;
	dev_ctx->non_secure_mem.pos = 0;
	dev_ctx->status = MU_OPENED;

	dev_ctx->pending_hdr = 0;

	goto exit;

free_coherent:
	dmam_free_coherent(dev_ctx->priv->dev, MAX_DATA_SIZE_PER_USER,
			   dev_ctx->non_secure_mem.ptr,
			   dev_ctx->non_secure_mem.dma_addr);

exit:
	up(&dev_ctx->fops_lock);
	return err;
}

/* Close a character device. */
static int se_if_fops_close(struct inode *nd, struct file *fp)
{
	struct se_if_device_ctx *dev_ctx = container_of(fp->private_data,
							struct se_if_device_ctx,
							miscdev);
	struct se_if_priv *priv = dev_ctx->priv;
	struct se_buf_desc *b_desc;

	/* Avoid race if closed at the same time */
	if (down_trylock(&dev_ctx->fops_lock))
		return -EBUSY;

	/* The device context has not been opened */
	if (dev_ctx->status != MU_OPENED)
		goto exit;

	/* check if this device was registered as command receiver. */
	if (priv->cmd_receiver_dev == dev_ctx)
		priv->cmd_receiver_dev = NULL;

	/* check if this device was registered as waiting response. */
	if (priv->waiting_rsp_dev == dev_ctx) {
		priv->waiting_rsp_dev = NULL;
		mutex_unlock(&priv->se_if_cmd_lock);
	}

	/* Unmap secure memory shared buffer. */
	if (dev_ctx->secure_mem.ptr)
		devm_iounmap(dev_ctx->dev, dev_ctx->secure_mem.ptr);

	dev_ctx->secure_mem.ptr = NULL;
	dev_ctx->secure_mem.dma_addr = 0;
	dev_ctx->secure_mem.size = 0;
	dev_ctx->secure_mem.pos = 0;

	/* Free non-secure shared buffer. */
	dmam_free_coherent(dev_ctx->priv->dev, MAX_DATA_SIZE_PER_USER,
			   dev_ctx->non_secure_mem.ptr,
			   dev_ctx->non_secure_mem.dma_addr);

	dev_ctx->non_secure_mem.ptr = NULL;
	dev_ctx->non_secure_mem.dma_addr = 0;
	dev_ctx->non_secure_mem.size = 0;
	dev_ctx->non_secure_mem.pos = 0;

	while (!list_empty(&dev_ctx->pending_in) ||
	       !list_empty(&dev_ctx->pending_out)) {
		if (!list_empty(&dev_ctx->pending_in))
			b_desc = list_first_entry_or_null(&dev_ctx->pending_in,
							  struct se_buf_desc,
							  link);
		else
			b_desc = list_first_entry_or_null(&dev_ctx->pending_out,
							  struct se_buf_desc,
							  link);

		if (!b_desc)
			continue;

		if (b_desc->shared_buf_ptr)
			memset(b_desc->shared_buf_ptr, 0, b_desc->size);

		__list_del_entry(&b_desc->link);
		devm_kfree(dev_ctx->dev, b_desc);
	}

	dev_ctx->status = MU_FREE;

exit:
	up(&dev_ctx->fops_lock);
	return 0;
}

/* IOCTL entry point of a character device */
static long se_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
//static long se_ioctl(struct file *fp, u32 cmd, u64 arg)
{
	struct se_if_device_ctx *dev_ctx = container_of(fp->private_data,
							struct se_if_device_ctx,
							miscdev);
	struct se_if_priv *se_if_priv = dev_ctx->priv;
	int err = -EINVAL;

	/* Prevent race during change of device context */
	if (down_interruptible(&dev_ctx->fops_lock))
		return -EBUSY;

	switch (cmd) {
	case SE_IOCTL_ENABLE_CMD_RCV:
		if (!se_if_priv->cmd_receiver_dev) {
			se_if_priv->cmd_receiver_dev = dev_ctx;
			err = 0;
		}
		break;
	case SE_IOCTL_GET_MU_INFO:
		err = se_ioctl_get_mu_info(dev_ctx, arg);
		break;
	case SE_IOCTL_SETUP_IOBUF:
		err = se_ioctl_setup_iobuf_handler(dev_ctx, arg);
		break;
	case SE_IOCTL_GET_SOC_INFO:
		err = se_ioctl_get_soc_info_handler(dev_ctx, arg);
		break;

	default:
		err = -EINVAL;
		dev_dbg(se_if_priv->dev,
			"%s: IOCTL %.8x not supported\n",
				dev_ctx->miscdev.name,
				cmd);
	}

	up(&dev_ctx->fops_lock);
	return (long)err;
}

/* Char driver setup */
static const struct file_operations se_if_fops = {
	.open		= se_if_fops_open,
	.owner		= THIS_MODULE,
	.release	= se_if_fops_close,
	.unlocked_ioctl = se_ioctl,
	.read		= se_if_fops_read,
	.write		= se_if_fops_write,
};

/* interface for managed res to free a mailbox channel */
static void if_mbox_free_channel(void *mbox_chan)
{
	mbox_free_channel(mbox_chan);
}

/* interface for managed res to unregister a character device */
static void if_misc_deregister(void *miscdevice)
{
	misc_deregister(miscdevice);
}

static int se_if_request_channel(struct device *dev,
				 struct mbox_chan **chan,
				 struct mbox_client *cl,
				 const u8 *name)
{
	struct mbox_chan *t_chan;
	int ret = 0;

	t_chan = mbox_request_channel_byname(cl, name);
	if (IS_ERR(t_chan)) {
		ret = PTR_ERR(t_chan);
		if (ret != -EPROBE_DEFER)
			dev_err(dev,
				"Failed to request chan %s ret %d\n", name,
				ret);
		goto exit;
	}

	ret = devm_add_action(dev, if_mbox_free_channel, t_chan);
	if (ret) {
		dev_err(dev, "failed to add devm removal of mbox %s\n", name);
		goto exit;
	}

	*chan = t_chan;

exit:
	return ret;
}

static int se_probe_if_cleanup(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct se_if_priv *priv;
	int ret = 0;
	int i;

	priv = dev_get_drvdata(dev);
	if (!priv) {
		ret = 0;
		dev_dbg(dev, "SE-MU Priv data is NULL;");
		return ret;
	}

	if (priv->tx_chan)
		mbox_free_channel(priv->tx_chan);
	if (priv->rx_chan)
		mbox_free_channel(priv->rx_chan);

	/* free the buffer in se remove, previously allocated
	 * in se probe to store encrypted IMEM
	 */
	if (priv->imem.buf) {
		dmam_free_coherent(dev,
				   ELE_IMEM_SIZE,
				   priv->imem.buf,
				   priv->imem.phyaddr);
		priv->imem.buf = NULL;
	}

	if (priv->ctxs) {
		for (i = 0; i < priv->max_dev_ctx; i++) {
			if (priv->ctxs[i]) {
				devm_remove_action(dev,
						   if_misc_deregister,
						   &priv->ctxs[i]->miscdev);
				misc_deregister(&priv->ctxs[i]->miscdev);
				devm_kfree(dev, priv->ctxs[i]);
			}
		}
		devm_kfree(dev, priv->ctxs);
	}

	if (priv->flags & RESERVED_DMA_POOL) {
		of_reserved_mem_device_release(dev);
		priv->flags &= (~RESERVED_DMA_POOL);
	}

	devm_kfree(dev, priv);
	of_node_put(dev->of_node);
	of_platform_device_destroy(dev, NULL);

	return ret;
}

static int se_probe_cleanup(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *if_dn;

	/* Enumerate se-interface device nodes. */
	for_each_child_of_node(dev->of_node, if_dn) {
		struct platform_device *if_pdev
					= of_find_device_by_node(if_dn);
		if (se_probe_if_cleanup(if_pdev))
			dev_err(dev,
				"Failed to clean-up child node probe.\n");
	}

	return 0;
}

static int init_device_context(struct device *dev)
{
	const struct imx_se_node_info *info;
	struct se_if_device_ctx *dev_ctx;
	struct se_if_priv *priv;
	u8 *devname;
	int ret = 0;
	int i;

	priv = dev_get_drvdata(dev);

	if (!priv) {
		ret = -EINVAL;
		dev_err(dev, "Invalid SE-MU Priv data");
		return ret;
	}
	info = priv->info;

	priv->ctxs = devm_kzalloc(dev, sizeof(dev_ctx) * priv->max_dev_ctx,
				  GFP_KERNEL);

	if (!priv->ctxs) {
		ret = -ENOMEM;
		return ret;
	}

	/* Create users */
	for (i = 0; i < priv->max_dev_ctx; i++) {
		dev_ctx = devm_kzalloc(dev, sizeof(*dev_ctx), GFP_KERNEL);
		if (!dev_ctx) {
			ret = -ENOMEM;
			return ret;
		}

		dev_ctx->dev = dev;
		dev_ctx->status = MU_FREE;
		dev_ctx->priv = priv;

		priv->ctxs[i] = dev_ctx;

		/* Default value invalid for an header. */
		init_waitqueue_head(&dev_ctx->wq);

		INIT_LIST_HEAD(&dev_ctx->pending_out);
		INIT_LIST_HEAD(&dev_ctx->pending_in);
		sema_init(&dev_ctx->fops_lock, 1);

		devname = devm_kasprintf(dev, GFP_KERNEL, "%s_ch%d",
					 info->se_name, i);
		if (!devname) {
			ret = -ENOMEM;
			return ret;
		}

		dev_ctx->miscdev.name = devname;
		dev_ctx->miscdev.minor = MISC_DYNAMIC_MINOR;
		dev_ctx->miscdev.fops = &se_if_fops;
		dev_ctx->miscdev.parent = dev;
		ret = misc_register(&dev_ctx->miscdev);
		if (ret) {
			dev_err(dev, "failed to register misc device %d\n",
				ret);
			return ret;
		}

		ret = devm_add_action(dev, if_misc_deregister,
				      &dev_ctx->miscdev);
		if (ret) {
			dev_err(dev,
				"failed[%d] to add action to the misc-dev\n",
				ret);
			return ret;
		}
	}

	return ret;
}

static void se_load_firmware(const struct firmware *fw, void *context)
{
	struct se_if_priv *priv = (struct se_if_priv *) context;
	const struct imx_se_node_info *info = priv->info;
	const u8 *se_fw_name = info->fw_name_in_rfs;
	phys_addr_t se_fw_phyaddr;
	u8 *se_fw_buf;

	if (!fw) {
		if (priv->fw_fail > MAX_FW_LOAD_RETRIES)
			dev_dbg(priv->dev,
				 "External FW not found, using ROM FW.\n");
		else {
			/*add a bit delay to wait for firmware priv released */
			msleep(20);

			/* Load firmware one more time if timeout */
			request_firmware_nowait(THIS_MODULE,
					FW_ACTION_UEVENT, info->fw_name_in_rfs,
					priv->dev, GFP_KERNEL, priv,
					se_load_firmware);
			priv->fw_fail++;
			dev_dbg(priv->dev, "Value of retries = 0x%x.\n",
				priv->fw_fail);
		}

		return;
	}

	/* allocate buffer to store the SE FW */
	se_fw_buf = dmam_alloc_coherent(priv->dev, fw->size,
					 &se_fw_phyaddr,
					 GFP_KERNEL);
	if (!se_fw_buf) {
		dev_err(priv->dev, "Failed to alloc SE fw buffer memory\n");
		goto exit;
	}

	memcpy(se_fw_buf, fw->data, fw->size);

	if (ele_fw_authenticate(priv->dev, se_fw_phyaddr))
		dev_err(priv->dev,
			"Failed to authenticate & load SE firmware %s.\n",
			se_fw_name);

exit:
	dmam_free_coherent(priv->dev,
			   fw->size,
			   se_fw_buf,
			   se_fw_phyaddr);

	release_firmware(fw);
}

static int se_if_probe(struct platform_device *pdev)
{
	struct imx_se_node_info_list *info_list;
	struct device *dev = &pdev->dev;
	struct imx_se_node_info *info;
	struct se_if_priv *priv;
	u32 idx;
	int ret;

	if (of_property_read_u32(dev->of_node, "reg", &idx)) {
		ret = -EINVAL;
		goto exit;
	}

	info_list = (struct imx_se_node_info_list *)
			device_get_match_data(dev->parent);
	info = get_imx_se_node_info(info_list, idx);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto exit;
	}

	dev_set_drvdata(dev, priv);

	/* Mailbox client configuration */
	priv->se_mb_cl.dev		= dev;
	priv->se_mb_cl.tx_block		= false;
	priv->se_mb_cl.knows_txdone	= true;
	priv->se_mb_cl.rx_callback	= se_if_rx_callback;

	ret = se_if_request_channel(dev, &priv->tx_chan,
			&priv->se_mb_cl, info->mbox_tx_name);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			dev_err(dev, "Mailbox tx channel, is not ready.\n");
		else
			dev_err(dev, "Failed to request tx channel\n");

		goto exit;
	}

	ret = se_if_request_channel(dev, &priv->rx_chan,
			&priv->se_mb_cl, info->mbox_rx_name);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			dev_err(dev, "Mailbox rx channel, is not ready.\n");
		else
			dev_dbg(dev, "Failed to request rx channel\n");

		goto exit;
	}

	priv->dev = dev;
	priv->info = info;

	/* Initialize the mutex. */
	mutex_init(&priv->se_if_lock);
	mutex_init(&priv->se_if_cmd_lock);

	priv->cmd_receiver_dev = NULL;
	priv->waiting_rsp_dev = NULL;
	priv->max_dev_ctx = info->max_dev_ctx;
	priv->cmd_tag = info->cmd_tag;
	priv->rsp_tag = info->rsp_tag;
	priv->mem_pool_name = info->pool_name;
	priv->success_tag = info->success_tag;
	priv->base_api_ver = info->base_api_ver;
	priv->fw_api_ver = info->fw_api_ver;

	init_completion(&priv->done);
	spin_lock_init(&priv->lock);

	if (info->reserved_dma_ranges) {
		ret = of_reserved_mem_device_init(dev);
		if (ret) {
			dev_err(dev,
				"failed to init reserved memory region %d\n",
				ret);
			priv->flags &= (~RESERVED_DMA_POOL);
			goto exit;
		}
		priv->flags |= RESERVED_DMA_POOL;
	}

	if (info->fw_name_in_rfs) {
		ret = request_firmware_nowait(THIS_MODULE,
					      FW_ACTION_UEVENT,
					      info->fw_name_in_rfs,
					      dev, GFP_KERNEL, priv,
					      se_load_firmware);
		if (ret)
			dev_warn(dev, "Failed to get firmware [%s].\n",
				 info->fw_name_in_rfs);
	}

	ret = imx_fetch_soc_info(dev);
	if (ret) {
		dev_err(dev,
			"failed[%d] to fetch SoC Info\n", ret);
		goto exit;
	}

	if (info->imem_mgmt) {
		/* allocate buffer where SE store encrypted IMEM */
		priv->imem.buf = dmam_alloc_coherent(dev, ELE_IMEM_SIZE,
						     &priv->imem.phyaddr,
						     GFP_KERNEL);
		if (!priv->imem.buf) {
			dev_err(dev,
				"dmam-alloc-failed: To store encr-IMEM.\n");
			ret = -ENOMEM;
			goto exit;
		}
	}

	if (info->max_dev_ctx) {
		ret = init_device_context(dev);
		if (ret) {
			dev_err(dev,
				"Failed[0x%x] to create device contexts.\n",
				ret);
			goto exit;
		}
	}

	dev_info(dev, "i.MX secure-enclave: %s interface to firmware, configured.\n",
		 info->se_name);
	return devm_of_platform_populate(dev);

exit:
	/* if execution control reaches here, if probe fails.
	 * hence doing the cleanup
	 */
	if (se_probe_if_cleanup(pdev))
		dev_err(dev,
			"Failed to clean-up the child node probe.\n");

	return ret;
}

static int se_probe(struct platform_device *pdev)
{
	struct device_node *enum_dev_node;
	struct device *dev = &pdev->dev;
	int enum_count;
	int ret;

	enum_count = of_get_child_count(dev->of_node);
	if (!enum_count) {
		ret = -EINVAL;
		dev_err(dev, "Zero Tx/Rx path MU nodes.\n");
		return ret;
	}

	for_each_child_of_node(dev->of_node, enum_dev_node) {
		struct platform_device *enum_plat_dev __maybe_unused;

		if (!of_device_is_available(enum_dev_node))
			continue;

		enum_plat_dev = of_platform_device_create(enum_dev_node,
							  NULL,
							  dev);
		if (!enum_plat_dev) {
			ret = -EINVAL;
			of_node_put(enum_dev_node);
			dev_err(dev,
				"Failed to create enumerated platform device.");
			break;
		}

		ret = se_if_probe(enum_plat_dev);
	}
	return ret;
}

static int se_remove(struct platform_device *pdev)
{
	if (se_probe_cleanup(pdev))
		dev_err(&pdev->dev,
			"i.MX Secure Enclave is not cleanly un-probed.");

	return 0;
}

static int se_suspend(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	const struct imx_se_node_info *info
					= priv->info;

	if (info && info->imem_mgmt)
		priv->imem.size = se_save_imem_state(dev);

	return 0;
}

static int se_resume(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	const struct imx_se_node_info *info
					= priv->info;
	int i;

	for (i = 0; i < priv->max_dev_ctx; i++)
		wake_up_interruptible(&priv->ctxs[i]->wq);

	if (info && info->imem_mgmt)
		se_restore_imem_state(dev);

	return 0;
}

static const struct dev_pm_ops se_pm = {
	RUNTIME_PM_OPS(se_suspend, se_resume, NULL)
};

static struct platform_driver se_driver = {
	.driver = {
		.name = "fsl-se-fw",
		.of_match_table = se_match,
		.pm = &se_pm,
	},
	.probe = se_probe,
	.remove = se_remove,
};
MODULE_DEVICE_TABLE(of, se_match);

module_platform_driver(se_driver);

MODULE_AUTHOR("Pankaj Gupta <pankaj.gupta@nxp.com>");
MODULE_DESCRIPTION("iMX Secure Enclave Driver.");
MODULE_LICENSE("GPL");
