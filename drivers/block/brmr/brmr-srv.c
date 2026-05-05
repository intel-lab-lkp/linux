// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Block device over RMR (BRMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

#include <linux/bio.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/slab.h>

#include "brmr-srv.h"
#include "rmr-srv.h"

MODULE_AUTHOR("The RMR and BRMR developers");
MODULE_VERSION(BRMR_SERVER_VER_STRING);
MODULE_DESCRIPTION("BRMR Server");
MODULE_LICENSE("GPL");

LIST_HEAD(store_list);
DEFINE_MUTEX(store_mutex);	/* mutex to protect store_list */

/**
 * brmr_srv_blk_validate_md() - Parse metadata for the given rmr block device and validate it
 *
 * @dev:	RMR block device against which the md is to be validated
 * @meta:	pointer to metadata to be checked
 *
 * Return:
 *	0:	On success
 *	-Error:	On failure
 */
int brmr_srv_blk_validate_md(struct brmr_srv_blk_dev *dev, struct brmr_srv_blk_dev_meta *meta)
{
	if (meta->magic != BRMR_BLK_STORE_MAGIC) {
		pr_warn("No md found. store %s md magic=%llX does not match %X\n",
			dev->poolname, meta->magic, BRMR_BLK_STORE_MAGIC);
		return -EINVAL;
	}

	// TODO: check version!

	if (dev->dev_size && dev->dev_size != meta->dev_size) {
		pr_err("store %s dev_size %llu does not match md value %llu\n",
		       dev->poolname, dev->dev_size, meta->dev_size);
		return -EINVAL;
	}

	if (dev->mapped_size != meta->mapped_size) {
		pr_err("store %s mapped_size %llu does not match md value %llu\n",
		       dev->poolname, dev->mapped_size, meta->mapped_size);
		return -EINVAL;
	}

	if (strncmp(dev->poolname, meta->poolname, NAME_MAX)) {
		pr_err("store %s does not match md value %s\n",
		       dev->poolname, meta->poolname);
		return -EINVAL;
	}

	pr_debug("store %s md: mapped_size=%llu\n",
		dev->poolname, meta->mapped_size);
	pr_debug("md parsing is done for store %s\n", dev->poolname);

	return 0;
}

/**
 * brmr_srv_blk_fill_md() - Fill metadata from brmr srv block device
 *
 * @dev:	BRMR server block device from which data is to be taken
 * @data:	pointer to metadata
 *
 * Return:
 *	0:	On success
 *	-Error:	On failure
 */
static int brmr_srv_blk_fill_md(struct brmr_srv_blk_dev *dev, void *data)
{
	struct brmr_srv_blk_dev_meta *meta = data;

	meta->magic = BRMR_BLK_STORE_MAGIC;
	meta->version = 0;
	meta->dev_size = dev->dev_size;
	meta->offset = BLK_STR_MD_SIZE_SECTORS;
	meta->ts = jiffies; // or ktime_get_real_seconds();
	meta->mapped_size = dev->mapped_size;
	meta->state = dev->state;

	memcpy(&meta->dev_params, &dev->dev_params, sizeof(struct rmr_blk_dev_params));

	strscpy(meta->poolname, dev->poolname, NAME_MAX);

	pr_debug("md filling pool %s is done for dev %s\n", meta->poolname, dev->name);

	return 0;
}

static int brmr_srv_blk_md_io_sync(struct block_device *bdev, int rw, void *md_data)
{
	int err = 0;
	struct bio *bio;
	blk_opf_t bio_flags = REQ_META;
	u32 bytes;

	bio = bio_alloc(bdev, 1, bio_flags, GFP_NOIO);
	if (!bio) {
		pr_err("Failed to allocate metadata bio\n");
		return -ENOMEM;
	}

	bytes = bio_add_page(bio, virt_to_page(md_data), PAGE_SIZE, 0);
	if (bytes != PAGE_SIZE) {
		pr_err("Failed to add page to bio, bytes returned=%u, expected %lu\n",
		       bytes, PAGE_SIZE);
		err = -EINVAL;
		goto bio_put;
	}

	if (rw == READ)
		bio->bi_opf = REQ_OP_READ;
	else
		bio->bi_opf = REQ_OP_WRITE | REQ_FUA;

	bio->bi_opf |= bio_flags;
	bio->bi_iter.bi_sector = 0;
	bio_set_dev(bio, bdev);

	pr_debug("submit_bio_wait dev %s, rw %s\n",
		 bdev->bd_disk->disk_name, rw == WRITE ? "WRITE" : "READ");
	err = submit_bio_wait(bio);
	if (err) {
		pr_err("Error reading md from %s, err %d\n",
		       bdev->bd_disk->disk_name, err);
		goto bio_put;
	}
	pr_info("%s: for dev %s md rw %s is completed with code %d\n",
		__func__, bdev->bd_disk->disk_name, rw == WRITE ? "WRITE" : "READ", err);

bio_put:
	bio_put(bio);

	return err;
}

/**
 * brmr_srv_blk_read_md() - read md from given block device
 *
 * @bdev:	block device from which to read md
 * @md_page:	buffer to fill with md
 */
static int brmr_srv_blk_bdev_read_md(struct block_device *bdev, char *md_page)
{
	int err = 0;

	err = brmr_srv_blk_md_io_sync(bdev, READ, md_page);
	if (err) {
		pr_err("error reading md from %s, err %d\n", bdev->bd_disk->disk_name, err);
		return err;
	}

	pr_debug("read md from dev %s is done\n", bdev->bd_disk->disk_name);

	return err;
}

static int brmr_srv_blk_write_md(struct brmr_srv_blk_dev *dev)
{
	int err = 0;
	void *md_page;

	pr_debug("flush md to dev %s\n", dev->name);
	md_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!md_page) {
		pr_err("Failed to allocate page to read md\n");
		err = -ENOMEM;
		goto out;
	}

	err = brmr_srv_blk_fill_md(dev, md_page);
	if (err) {
		pr_err("error filling md for dev %s, err %d\n", dev->name, err);
		goto free_md_page;
	}

	err = brmr_srv_blk_md_io_sync(dev->bdev, WRITE, md_page);
	if (err) {
		pr_err("error writing md to %s, err %d\n", dev->name, err);
		goto free_md_page;
	}
	pr_debug("flush md to dev is done %s\n", dev->name);

free_md_page:
	kfree(md_page);
out:
	return err;
}

static void brmr_srv_blk_zero_md(struct brmr_srv_blk_dev *dev)
{
	int err = 0;
	void *md_page;

	pr_debug("zero md on dev %s\n", dev->name);
	md_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!md_page) {
		pr_warn("Failed to allocate page to read md\n");
		return;
	}

	err = brmr_srv_blk_md_io_sync(dev->bdev, WRITE, md_page);
	if (err)
		pr_warn("error writing zero md to %s, err %d\n", dev->name, err);

	pr_debug("zero md on dev is done %s\n", dev->name);
	kfree(md_page);
}

static void brmr_srv_ref_kill(struct brmr_srv_blk_dev *dev)
{
	percpu_ref_kill(&dev->kref);
	wait_for_completion(&dev->comp);
}

static void brmr_srv_blk_release(struct percpu_ref *kref)
{
	struct brmr_srv_blk_dev *dev;

	dev = container_of(kref, struct brmr_srv_blk_dev, kref);
	complete(&dev->comp);
}

/**
 * brmr_srv_blk_close() - Close a brmr srv block device
 *
 * @dev:	BRMR server block device to be closed
 *
 * Description:
 *	Close an opened brmr srv store block device.
 *	This function is the opposite of brmr_srv_blk_open.
 *	This function is supposed to be the check and stop for inflight IOs.
 *
 * Locks:
 *	store_mutex should be held while calling this.
 */
void brmr_srv_blk_close(struct brmr_srv_blk_dev *dev, bool delete)
{
	pr_info("rmr store name: %s; dev %s is closing\n", dev->poolname, dev->name);
	brmr_srv_blk_clear_state(dev, BRMR_SRV_STORE_OPEN);

	list_del(&dev->entry);

	pr_info("brmr server store blk dev %s wait for io to complete.\n", dev->name);
	brmr_srv_ref_kill(dev);

	/*
	 * Reinit the ref counter so that RMR can send metadata requests.
	 */
	reinit_completion(&dev->comp);
	percpu_ref_reinit(&dev->kref);

	rmr_srv_unregister(dev->poolname, delete);
	dev->pool = NULL;
	brmr_srv_ref_kill(dev);

	if (delete)
		brmr_srv_blk_zero_md(dev);
}

static int brmr_srv_blk_do_discard(struct brmr_srv_blk_dev *dev)
{
	struct rmr_pool *pool = dev->pool;
	int err;

	pr_info("store id %s has mapped size of %llu, send discarded chunks to rmr pool %s\n",
		dev->poolname, dev->mapped_size, dev->pool->poolname);

	err = rmr_srv_discard_id(pool, 0, 0, 0, true);
	if (err)
		pr_err("store %s failed to discard all data\n", dev->poolname);

	return err;
}

/**
 * brmr_srv_init_cmd() - Initialize message command
 *
 * @msg:	command message where to init
 */
static void brmr_srv_init_cmd(struct brmr_msg_cmd *msg)
{
	memset(msg, 0, sizeof(*msg));

	msg->hdr.type = cpu_to_le16(BRMR_MSG_CMD);
	msg->hdr.__padding = 0;
	msg->ver = BRMR_PROTO_VER_MAJOR;
}

/**
 * brmr_srv_cmd_conf() - Confirmation function for brmr srv store internal command message
 *
 * @priv:	priv pointer to brmr command private data
 * @errno:	error number passed from RMR.
 *		See description of errno in RMR function.
 *
 * Description:
 *	Command response for a map new command can fail on multiple levels.
 *	If RMR fails to send the message to any or one of the nodes, that would reflect on the
 *	errno. If the command fails on BRMR level, that would reflect on the rsp struct.
 *	The error number will be used differently by different commands accordingly.
 */
static void brmr_srv_cmd_conf(void *priv, int errno)
{
	struct brmr_cmd_priv *cmd_priv = (struct brmr_cmd_priv *)priv;

	cmd_priv->errno = errno;

	switch (cmd_priv->cmd_type) {
	case BRMR_CMD_GET_PARAMS:
		if (cmd_priv->errno)
			pr_err("%s: BRMR_CMD_GET_PARAMS failed with err=%pe on sending",
				 __func__, ERR_PTR(errno));

		break;

	default:
		cmd_priv->errno = -EINVAL;
		pr_err("%s: Unknown command type %d err=%d\n", __func__, cmd_priv->cmd_type, errno);
	}

	complete(&cmd_priv->complete_done);
}

/**
 * brmr_srv_send_msg_cmd() - Sends command message to internal rmr pool through rmr-srv pool
 *
 * @dev:		pointer to brmr device
 * @msg:		msg struct to be sent
 * @rsp_buf:		response buffer where the response of the storage side is stored
 * @rsp_buf_len:	length of the response buffer
 *
 * Return:
 *	Negative if failed to sent command
 *	As handled by each command in brmr_cmd_conf, if succeeded to send command
 *
 * Context:
 *	Would block until response is received
 */
static int brmr_srv_send_msg_cmd(struct brmr_srv_blk_dev *dev, struct brmr_msg_cmd *msg,
				 void *rsp_buf, size_t rsp_buf_len)
{
	struct brmr_cmd_priv cmd_priv;
	struct kvec vec;
	int ret;

	vec = (struct kvec) {
		.iov_base = msg,
		.iov_len  = sizeof(*msg)
	};

	cmd_priv.dev = dev;
	cmd_priv.cmd_type = msg->cmd_type;
	cmd_priv.rsp_buf = rsp_buf;
	cmd_priv.rsp_buf_len = rsp_buf_len;
	cmd_priv.errno = 0;
	init_completion(&cmd_priv.complete_done);

	ret = rmr_srv_pool_cmd_with_rsp(dev->pool, brmr_srv_cmd_conf, &cmd_priv, &vec, 1, rsp_buf,
				       rsp_buf_len, sizeof(struct brmr_msg_cmd_rsp));

	if (!ret) {
		wait_for_completion(&cmd_priv.complete_done);
		ret = cmd_priv.errno;
	}

	return ret;
}

/**
 * brmr_srv_blk_get_params() - Get parameters from other servers
 *
 * @dev:		Backend device for which to be checked
 *
 * Description:
 *	Check whether parameters from other servers are consistent with this server through
 *	internal network.
 *
 * Return:
 *	0 on success of checks
 *	-Negative error value on failure of checks.
 *	-EAGAIN if no sync sessions are connected to this server.
 */
static int brmr_srv_blk_get_params(void *device)
{
	struct brmr_srv_blk_dev *dev;
	struct brmr_msg_cmd msg;
	struct brmr_msg_cmd_rsp *brmr_cmd_rsp;
	void *rsp_buf;
	size_t rsp_buf_len;
	int err = 0, i;
	bool checked = false;

	dev = (struct brmr_srv_blk_dev *)device;
	brmr_srv_init_cmd(&msg);
	msg.cmd_type = BRMR_CMD_GET_PARAMS;

	rsp_buf_len = sizeof(struct brmr_msg_cmd_rsp) * RMR_POOL_MAX_SESS;
	rsp_buf = kzalloc(rsp_buf_len, GFP_KERNEL);
	if (!rsp_buf)
		return -ENOMEM;

	err = brmr_srv_send_msg_cmd(dev, &msg, rsp_buf, rsp_buf_len);
	if (err < 0) {
		pr_warn("%s: brmr_send_msg_cmd failed with errno %d\n", __func__, err);
		/*
		 * Sending could fail for various reasons. The server may be isolated and has
		 * no connected sync sessions to other nodes. Or the connected server has no
		 * store attached.
		 */
		goto free_data;
	}

	/*
	 * We do not care if the command failed for few storage nodes, as long as we get a good
	 * response from one of them.
	 *
	 * The mapped size of all storage nodes which are connected should be the same, whether
	 * the backend device of those nodes is mapped or not.
	 *
	 * TODO: If the responses of other storage nodes are different, then use values from
	 * nodes which are mapped. If there are no mapped devices in the pool, then the check
	 * will fail when the mapped sizes are different.
	 */
	brmr_cmd_rsp = (struct brmr_msg_cmd_rsp *)rsp_buf;
	for (i = 0; i < RMR_POOL_MAX_SESS; i++, brmr_cmd_rsp++) {
		struct brmr_cmd_get_params_rsp *get_params_rsp = &brmr_cmd_rsp->get_params_rsp;
		struct brmr_blk_dev_params *rsp_dev_params;

		/*
		 * If there is no magic, or the command failed,
		 * we do not use that nodes info to perform the check.
		 */
		if (brmr_cmd_rsp->magic != BRMR_CMD_RSP_MAGIC ||
		    brmr_cmd_rsp->status)
			continue;

		if (dev->mapped_size != le64_to_cpu(get_params_rsp->mapped_size)) {
			pr_err("%s: Mismatch in mapped_size: %llu != %llu\n", __func__,
			       dev->mapped_size, le64_to_cpu(get_params_rsp->mapped_size));
			err = -EINVAL;
			goto free_data;
		}

		rsp_dev_params = &get_params_rsp->dev_params;

		dev->dev_params.max_hw_sectors = le32_to_cpu(rsp_dev_params->max_hw_sectors);
		dev->dev_params.max_write_zeroes_sectors =
					le32_to_cpu(rsp_dev_params->max_write_zeroes_sectors);
		dev->dev_params.max_discard_sectors =
					le32_to_cpu(rsp_dev_params->max_discard_sectors);
		dev->dev_params.discard_granularity =
					le32_to_cpu(rsp_dev_params->discard_granularity);
		dev->dev_params.discard_alignment = le32_to_cpu(rsp_dev_params->discard_alignment);
		dev->dev_params.physical_block_size =
					le16_to_cpu(rsp_dev_params->physical_block_size);
		dev->dev_params.logical_block_size =
					le16_to_cpu(rsp_dev_params->logical_block_size);
		dev->dev_params.max_segments = le16_to_cpu(rsp_dev_params->max_segments);
		dev->dev_params.secure_discard = le16_to_cpu(rsp_dev_params->secure_discard);
		dev->dev_params.cache_policy = rsp_dev_params->cache_policy;

		/*
		 * At least check passed with one mapped storage node
		 *
		 * We still perform the check for other mapped storage nodes just for sanity.
		 */
		checked = true;
	}

	if (checked == false) {
		pr_err("%s: Check for mapped_size failed for dev %s.\n",
		       __func__, dev->poolname);
		err = -EINVAL;
	}

free_data:
	kfree(rsp_buf);

	return err;
}

/**
 * brmr_srv_blk_add_handle_replace() - Handle check and discard for a store which was replaced
 *
 * @dev:	RMR block device to be closed
 *
 * Description:
 *	When an empty disk is added to an already existing brmr server store, it means that the
 *	empty disk is to replace the disk which was present in the existing brmr srv store.
 *	Before replacing the disk with the new empty one, there are a number of things to be done.
 *	This function performs the following task,
 *	1) Get some parameters from other storage node through the internal network, and checks
 *	whether the mapped_size passed for the new empty disk is correct or not.
 *	2) If the above check passed, then discard is sent above to rmr-server.
 *
 * Return:
 *	0 on success
 *	-Error value on error
 */
static int brmr_srv_blk_add_handle_replace(struct brmr_srv_blk_dev *dev)
{
	int err = 0;

	/*
	 * The check passed. We can now do the discard safely.
	 */
	err = brmr_srv_blk_do_discard(dev);
	if (err) {
		pr_err("%s: brmr_srv_blk_do_discard failed for dev %s\n", __func__, dev->poolname);
		return err;
	}

	/*
	 * We are done with everything, and we are good.
	 * We now set the MAPPED state and write metadata again so it is persisted.
	 * so that IOs can be served.
	 */
	brmr_srv_blk_set_state(dev, BRMR_SRV_STORE_MAPPED);
	err = brmr_srv_blk_write_md(dev);
	if (err) {
		pr_err("%s: dev %s: write md error %d\n", __func__, dev->name, err);
		brmr_srv_blk_clear_state(dev, BRMR_SRV_STORE_MAPPED);
		return err;
	}

	/*
	 * After the discarded entries are sent to rmr-server, set the map version of
	 * rmr pool to zero.
	 */
	rmr_srv_replace_store(dev->pool);
	return 0;
}

/**
 * brmr_srv_read_and_check_md() - Read and check metadata if it exists
 *
 * @dev:	BRMR server block device for which the metadata is to be checked
 * @md_page:	pointer to the buf where to read the metadata
 *
 * Description:
 *	Read metadata from the given store device, and check whether metadata exists.
 *
 * Return:
 *	0:	read was successful and metadata exists
 *	-1:	read was successful but metadata doesn't exists
 *	-Errno:	read failed
 */
int brmr_srv_read_and_check_md(struct brmr_srv_blk_dev *dev, void *md_page)
{
	struct brmr_srv_blk_dev_meta *meta = md_page;
	int err;

	err = brmr_srv_blk_bdev_read_md(dev->bdev, md_page);
	if (err) {
		pr_err("%s: failed to read md, err=%d\n", __func__, err);
		return -EINVAL;
	}

	if (meta->magic != BRMR_BLK_STORE_MAGIC) {
		pr_info("%s: No MD exists for block device %s, md magic=%llX does not match %X\n",
			__func__, dev->name, meta->magic, BRMR_BLK_STORE_MAGIC);
		return -1;
	}

	pr_info("%s: %s MD exists for block device %s\n", __func__, meta->poolname, dev->name);

	return 0;
}

/**
 * brmr_srv_blk_open() - Open an brmr srv block device
 *
 * @dev:	BRMR server block device structure to be used.
 * @path:	path to the block device.
 * @create:	Whether to create a new store or open an existing one.
 * @replace:	Whether the device is being added to replace an empty disk.
 *
 * Description:
 *	Open the block device "path", and populate the brmr srv block device "dev"
 *	with the details.
 *	To close the device, call brmr_srv_blk_close()
 *
 * Return:
 *	0 on success
 *	-Error value on error
 *
 * Locks:
 *	store_mutex should be held while calling this.
 */
int brmr_srv_blk_open(struct brmr_srv_blk_dev *dev, const char *path,
		      bool create, bool replace)
{
	struct rmr_attrs attr;
	int err;

	err = rmr_srv_query(NULL, dev->mapped_size, &attr);
	if (err) {
		pr_err("dev %s: rmr srv query failed %d\n", dev->name, err);
		return err;
	}

	if ((dev->mapped_size + BLK_STR_MD_SIZE_SECTORS + attr.rmr_md_size) > dev->dev_size) {
		pr_err("%s: dev %s: No space for rmr metadata %llu(in sectors)\n",
		       __func__, dev->name, attr.rmr_md_size);
		return -ENOSPC;
	}

	/*
	 * After the device registers to the RMR server pool, there will be metadata requests from
	 * RMR server transmitted to the device which starts reference counting. The reference
	 * count of the device must be initialized before any in flight requests are sent to BRMR.
	 */
	err = percpu_ref_init(&dev->kref, brmr_srv_blk_release, PERCPU_REF_ALLOW_REINIT, GFP_KERNEL);
	if (err) {
		pr_err("%s: percpu ref init failed.\n", __func__);
		return -EINVAL;
	}
	init_completion(&dev->comp);

	dev->pool = rmr_srv_register(dev->poolname, &pstore_blk_ops, dev,
				     dev->mapped_size, create ? RMR_SRV_DISK_CREATE :
						       (replace ? RMR_SRV_DISK_REPLACE :
							RMR_SRV_DISK_ADD));
	if (!dev->pool) {
		pr_err("Failed registering blk store %s, err\n", dev->poolname);
		brmr_srv_ref_kill(dev);
		return -EINVAL;
	}

	brmr_srv_blk_set_state(dev, BRMR_SRV_STORE_OPEN);

	if (!create) {
		err = brmr_srv_blk_get_params(dev);
		if (replace) {
			/*
			 * Any failure of getting parameters is not allowed when replacing a store.
			 * Either it failed to send the command or the parameters are different.
			 */
			if (err) {
				pr_err("%s: replace_store: brmr_srv_blk_get_params failed with err %d\n",
				       __func__, err);
				goto close_dev;
			}
		} else {
			/*
			 * The store creation will fail if the connected servers to this server
			 * share different parameter values. If sending the command of getting
			 * parameters failed due to no sync sessions connected to this server
			 * where no parameters are received, the store will be created, delaying
			 * checks when this server is connected to some other servers.
			 */
			if (err && err != -EAGAIN) {
				pr_err("%s: create_store: brmr_srv_blk_get_params failed with err %d\n",
				       __func__, err);
				goto close_dev;
			}
		}

		/*
		 * TODO: Would we be creating the maps for replace (empty disk) at the
		 * same time as we create one for create_disk?
		 */
		if (replace) {
			err = brmr_srv_blk_add_handle_replace(dev);
			if (err) {
				pr_err("%s: replace_store %s: handling replace failed with err %d",
				       __func__, dev->poolname, err);
				goto close_dev;
			}
		}
	}

	/* we write md in both cases (new or old device) just to check if device is ok
	 * for writing
	 */
	err = brmr_srv_blk_write_md(dev);
	if (err) {
		pr_err("dev %s: write md error %d\n", dev->name, err);
		goto close_dev;
	}

	list_add(&dev->entry, &store_list);

	pr_info("%s: brmr srv blk str %s, dev %s set state to open\n", __func__, dev->poolname,
		dev->name);

	return 0;

close_dev:
	brmr_srv_blk_clear_state(dev, BRMR_SRV_STORE_OPEN);
	/*
	 * TODO: Ideally, the unregister should be called with (create || replace).
	 * But right now there is no way to RMR to go ahead with the delete,
	 * even if marked_delete is not set.
	 */
	rmr_srv_unregister(dev->poolname, create);
	dev->pool = NULL;
	brmr_srv_ref_kill(dev);

	return err;
}

/**
 * brmr_srv_blk_cleanup() - Cleanup all the opened and active brmr srv block devices
 *
 * Description:
 *	This function is called when the module brmr server store is getting removed.
 *	It closes, destroys and frees all the open and active brmr server block devices.
 */
static void brmr_srv_blk_cleanup(void)
{
	struct brmr_srv_blk_dev *dev, *tmp;

	mutex_lock(&store_mutex);
	list_for_each_entry_safe(dev, tmp, &store_list, entry) {
		blk_str_destroy_sysfs_files(dev, NULL);
		brmr_srv_blk_close(dev, false);

		pr_info("put blkdev %s\n", dev->bdev->bd_disk->disk_name);
		bdev_fput(dev->bdev_file);

		brmr_srv_blk_destroy(dev);
	}
	mutex_unlock(&store_mutex);
}

/**
 * brmr_srv_blk_create() - Create an brmr_srv_blk_dev with the given data
 *
 * @path:	path to the block device.
 * @poolname:	Name to be given to the created block device
 *
 * Description:
 *	To destroy a created brmr server block device, call brmr_srv_blk_destroy()
 *
 * Return:
 *	Pointer to the allocated brmr srv block device on success
 *	Error pointer on error
 */
struct brmr_srv_blk_dev *brmr_srv_blk_create(const char *path, char *poolname)
{
	struct brmr_srv_blk_dev *dev;
	int err = 0;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto err;
	}

	strscpy(dev->poolname, poolname, NAME_MAX);

	dev->io_priv_cache = kmem_cache_create("brmr_srv_io_priv_cache",
					       sizeof(struct brmr_srv_io_priv), 0, 0, NULL);
	if (!dev->io_priv_cache) {
		pr_err("failed to create cache for device %s\n", poolname);
		err = -ENOMEM;
		goto free_dev;
	}

	pr_debug("brmr srv blk store with name %s created\n", poolname);

	return dev;

free_dev:
	kfree(dev);
err:
	return ERR_PTR(err);
}

/**
 * brmr_srv_blk_destroy() - Destroy a given brmr_srv_blk_dev
 *
 * @dev:	brmr server block device to be destroyed
 * @sysfs_self:	Pointer to self attribute
 *
 * Description:
 *	This function is the opposite of brmr_srv_blk_create()
 *	The pointer to the self attribute is used to denote whether the destroy call
 *	is a result of a sysfs task for its own device.
 */
void brmr_srv_blk_destroy(struct brmr_srv_blk_dev *dev)
{
	kmem_cache_destroy(dev->io_priv_cache);
	kfree(dev);
}

/**
 * brmr_srv_blk_map_dev() - Process a map command from the client side
 *
 * @dev:	brmr server block device to be destroyed
 * @map_cmd:	Pointer to structure holding map command info
 *
 * Description:
 *	We save all the data and param sent in the command in out metadata,
 *	since these are assured to have been validated across all storage nodes.
 *
 *	For future get params requests, we send back these instead of reading them
 *	from the underlying block device.
 *
 * Return:
 *	0 on success
 *	-Error value on error
 */
static int brmr_srv_blk_map_dev(struct brmr_srv_blk_dev *dev,
				const struct brmr_msg_map_new_cmd *map_cmd)
{
	const struct brmr_blk_dev_params *cmd_dev_params = &map_cmd->dev_params;
	int err;
	u64 recvd_mapped_size = map_cmd->mapped_size;

	pr_info("%s: Mapping device %s with mapped_size %llu, recvd size %llu\n",
		__func__, dev->name, dev->mapped_size, recvd_mapped_size);

	if (test_bit(BRMR_SRV_STORE_MAPPED, &dev->state)) {
		pr_err("%s: Received map command for already mapped device %s\n",
		       __func__, dev->name);
		return -EINVAL;
	}

	if (recvd_mapped_size > dev->dev_size - BLK_STR_MD_SIZE_SECTORS) {
		pr_err("can not map %llu, only %llu available %s\n",
		       recvd_mapped_size, dev->dev_size - BLK_STR_MD_SIZE_SECTORS, dev->name);
		return -ENOSPC;
	}

	if (dev->mapped_size && dev->mapped_size != recvd_mapped_size) {
		pr_err("dev %s is already mapped with size %llu, does not match %llu",
		       dev->name, dev->mapped_size, recvd_mapped_size);
		return -EINVAL;
	}

	dev->mapped_size = recvd_mapped_size;

	dev->dev_params.max_hw_sectors = le32_to_cpu(cmd_dev_params->max_hw_sectors);
	dev->dev_params.max_write_zeroes_sectors =
				le32_to_cpu(cmd_dev_params->max_write_zeroes_sectors);
	dev->dev_params.max_discard_sectors = le32_to_cpu(cmd_dev_params->max_discard_sectors);
	dev->dev_params.discard_granularity = le32_to_cpu(cmd_dev_params->discard_granularity);
	dev->dev_params.discard_alignment = le32_to_cpu(cmd_dev_params->discard_alignment);
	dev->dev_params.physical_block_size = le16_to_cpu(cmd_dev_params->physical_block_size);
	dev->dev_params.logical_block_size = le16_to_cpu(cmd_dev_params->logical_block_size);
	dev->dev_params.max_segments = le16_to_cpu(cmd_dev_params->max_segments);
	dev->dev_params.secure_discard = le16_to_cpu(cmd_dev_params->secure_discard);
	dev->dev_params.cache_policy = cmd_dev_params->cache_policy;

	brmr_srv_blk_set_state(dev, BRMR_SRV_STORE_MAPPED);

	err = brmr_srv_blk_write_md(dev);
	if (err) {
		pr_err("failed to write md for %s, err %d\n", dev->name, err);
		dev->mapped_size = 0;
		brmr_srv_blk_clear_state(dev, BRMR_SRV_STORE_MAPPED);
		return -EINVAL;
	}

	return 0;
}

/* Always succeeds. */
static int brmr_srv_blk_unmap_dev(struct brmr_srv_blk_dev *dev)
{
	pr_info("unmap device: %s\n", dev->name);
	brmr_srv_blk_clear_state(dev, BRMR_SRV_STORE_MAPPED);

	return 0;
}

static bool brmr_srv_blk_io_allowed(void *store_priv)
{
	struct brmr_srv_blk_dev *dev = store_priv;

	if (!dev) {
		pr_err("no store registered\n");
		return false;
	}

	return test_bit(BRMR_SRV_STORE_OPEN, &dev->state) &&
	       test_bit(BRMR_SRV_STORE_MAPPED, &dev->state);
}

#define bio_disk_name(bio)    ((bio)->bi_bdev->bd_disk->disk_name)
#define bio_first_sector(bio) ((bio_end_sector(bio) - bio_sectors(bio)))

static void brmr_srv_bi_end_io(struct bio *bio)
{
	struct brmr_srv_io_priv *io_priv = bio->bi_private;
	struct brmr_srv_blk_dev *dev = io_priv->dev;
	int err;

	err = blk_status_to_errno(bio->bi_status);
	pr_debug("end io called for dev %s, bio=%p, err=%d\n", dev->poolname, bio, err);

	if (err) {
		brmr_srv_blk_clear_state(dev, BRMR_SRV_STORE_OPEN);
		pr_err("Dev %s, Bio %p type %s, err=%d bdev_name=%s\n", dev->poolname,
		       bio, bio_data_dir(bio) == WRITE ? "W" : "R", err, bio_disk_name(bio));
	}

	rmr_srv_req_resp(io_priv->priv, err);

	kmem_cache_free(dev->io_priv_cache, io_priv);
	brmr_srv_blk_put_ref(dev);
	bio_put(bio);
}

static int brmr_srv_submit_bi(struct brmr_srv_blk_dev *dev, void *data, u64 offset, u32 length,
			      unsigned long flags, u16 prio, void *priv)
{
	struct bio *bio;
	struct brmr_srv_io_priv *io_priv;
	blk_opf_t bio_flags;
	int ret = 0;
	bool is_md_op = false;

	switch (rmr_op(flags)) {
	case RMR_OP_READ:
		bio_flags = REQ_OP_READ;
		break;
	case RMR_OP_WRITE:
	case RMR_OP_SYNCREQ:
		bio_flags = REQ_OP_WRITE;
		break;
	case RMR_OP_DISCARD:
		bio_flags = REQ_OP_DISCARD;
		break;
	case RMR_OP_WRITE_ZEROES:
		bio_flags = REQ_OP_WRITE_ZEROES;
		break;
	case RMR_OP_FLUSH:
		bio_flags = REQ_OP_WRITE | REQ_PREFLUSH;
		break;
	case RMR_OP_MD_READ:
		bio_flags = REQ_OP_READ;
		is_md_op = true;
		break;
	case RMR_OP_MD_WRITE:
		bio_flags = REQ_OP_WRITE;
		is_md_op = true;
		break;
	default:
		pr_err("Wrong flags=%lu\n", flags);
		return -EINVAL;
	}

	/*
	 * Most md IO are created on rmr-srv and does not get priority value passed on from rmr-clt
	 */
	if (is_md_op) {
		bio_flags |= REQ_META;
		if (rmr_op(flags) == RMR_OP_MD_WRITE)
			bio_flags |= REQ_FUA;
	}

	if (flags & RMR_F_SYNC)
		bio_flags |= REQ_SYNC;

	if (flags & RMR_F_FUA)
		bio_flags |= REQ_FUA;

	bio = bio_alloc(dev->bdev, 1, bio_flags, GFP_KERNEL);
	if (bio_add_page(bio, virt_to_page(data), length,
			 offset_in_page(data)) != length) {
		pr_err("Failed to map data to bio\n");
		ret = -EINVAL;
		goto put_bio;
	}

	io_priv = kmem_cache_zalloc(dev->io_priv_cache, GFP_KERNEL);
	if (!io_priv) {
		pr_err("Failed to alloc io_priv for op %lx dev %s\n", flags, dev->poolname);
		ret = -ENOMEM;
		goto put_bio;
	}

	io_priv->dev = dev;
	io_priv->priv = priv;

	bio->bi_private = io_priv;
	bio->bi_end_io = brmr_srv_bi_end_io;
	bio->bi_iter.bi_sector = offset;
	bio->bi_iter.bi_size = length;
	bio_set_dev(bio, dev->bdev);

	pr_debug("Submit %s bio=%p, disk=%s, flag=[%lx], bio_flag=[%x], op=[%x]"
		 "first_sect=%llu, sectors=%d\n",
		 is_md_op ? "md req" : "req", bio, bio_disk_name(bio),
		 flags, bio_flags, rmr_op(flags),
		 (u64)bio_first_sector(bio), bio_sectors(bio));

	if (is_md_op) {
		ret = submit_bio_wait(bio);
		if (ret) {
			pr_err("Error waiting md from %s, err %d\n",
			       dev->bdev->bd_disk->disk_name, ret);
		}
		goto end_bio;
	} else {
		/*
		 * Most md IO are created on rmr-srv and does not get priority value passed on from
		 * rmr-clt
		 */
		bio->bi_ioprio = prio;
		submit_bio(bio);
	}

	return 0;
end_bio:
	rmr_srv_req_resp(io_priv->priv, ret);
	kmem_cache_free(dev->io_priv_cache, io_priv);
put_bio:
	bio_put(bio);
	return ret;
}

/**
 * brmr_srv_process_blk_req() - Processes brmr srv store IO messages
 *
 * @dev:               pointer to rmr block device
 * @data:              pointer to data
 * @data_offset:       offset on disk (represented in bytes)
 * @length:            length of data in bytes
 * @flags:             IO flags
 * @prio:              prio from block layer
 * @priv:              pointer to priv data for rmr
 *
 * Return:
 *     0 in case of success
 *     negative in case of failure
 */
static int brmr_srv_process_blk_req(void *device, void *data, u32 data_offset,
				    u32 length, unsigned long flags, u16 prio, void *priv)
{
	struct brmr_srv_blk_dev *dev = (struct brmr_srv_blk_dev *)device;
	u64 offset = 0; /* in sectors */
	int ret = 0;

	if (!brmr_srv_blk_get_ref(dev)) {
		pr_err("for dev %s, name %s, failed to get_ref\n",
		       dev->name, dev->poolname);
		return -EIO;
	}

	if (!brmr_srv_blk_io_allowed(dev)) {
		pr_err("Store name %s, offset %u, length %u, io is not allowed!\n",
		       dev->poolname, data_offset, length);
		ret = -EINVAL;
		goto err;
	}

	offset = BLK_STR_MD_SIZE_SECTORS;
	offset += (data_offset) >> SECTOR_SHIFT; //bytes to sectors;

	pr_debug("Submitted req to %s, flag %lu offset %llu length %u\n",
		 dev->name, flags, offset, length);
	ret = brmr_srv_submit_bi(dev, data, offset, length, flags, prio, priv);
	if (ret) {
		pr_err("%s: bio submission failed for data IO\n", __func__);
		goto err;
	}

	return 0;

err:
	brmr_srv_blk_put_ref(dev);
	return ret;
}

/**
 * brmr_srv_process_blk_md_req() - Process the requests for rmr metadata
 *
 * Return:
 *	0 on success
 *
 * Description:
 *	The rmr metadata will be stored at the end of the device.
 */
static int brmr_srv_process_blk_md_req(void *device, void *data, u32 data_offset,
				       u32 length, unsigned long flags, void *priv)
{
	struct brmr_srv_blk_dev *dev = device;
	int err;
	u64 offset = 0; /* in sectors */

	if (!brmr_srv_blk_get_ref(dev)) {
		pr_err("for dev %s, name %s, failed to get_ref\n",
		       dev->name, dev->poolname);
		return -EIO;
	}

	/* The mapped_size is in sectors. */
	offset = BLK_STR_MD_SIZE_SECTORS + dev->mapped_size;
	offset += (data_offset) >> SECTOR_SHIFT; //bytes to sectors;
	pr_debug("Submitted md req to %s, flag %lu offset %llu length %u\n",
		 dev->name, flags, offset, length);
	/*
	 * It's no need to return err to upper layer here. If the submission of md request fails,
	 * it will go through the endreq path after the server req finishes processing.
	 */
	err = brmr_srv_submit_bi(dev, data, offset, length, flags, 0, priv);
	if (err)
		pr_err("%s: bio submission failed for metadata IO\n", __func__);
	brmr_srv_blk_put_ref(dev);
	return 0;
}

/**
 * brmr_srv_init_cmd_rsp() - Initialize command response
 *
 * @msg:	command response to initialize
 */
static void brmr_srv_init_cmd_rsp(struct brmr_msg_cmd_rsp *msg)
{
	memset(msg, 0, sizeof(*msg));

	msg->hdr.type = cpu_to_le16(BRMR_MSG_CMD);
	msg->hdr.__padding = 0;
	msg->magic = BRMR_CMD_RSP_MAGIC;
	msg->ver = BRMR_PROTO_VER_MAJOR;
	msg->cmd_type = BRMR_CMD_RSP;
}

/**
 * brmr_srv_fill_dev_param_dev() - Fill dev params from the saved params in brmr srv block device
 *
 * @dev:	pointer to brmr server block device
 * @rsp:	Pointer to command response structure holding params
 *
 * Return:
 *	0 in case of success
 *	negative in case of failure
 */
static int brmr_srv_fill_dev_param_dev(struct brmr_srv_blk_dev *dev,
				       struct brmr_cmd_get_params_rsp *rsp)
{
	struct brmr_srv_blk_dev_meta *md_page;
	struct brmr_blk_dev_params *rsp_dev_params = &rsp->dev_params;
	int ret;

	md_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!md_page) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * We have to read the metadata from the device.
	 */
	ret = brmr_srv_blk_bdev_read_md(dev->bdev, (void *)md_page);
	if (ret) {
		pr_err("%s: failed to read md, err=%d\n", __func__, ret);
		goto out;
	}

	if (md_page->magic != BRMR_BLK_STORE_MAGIC) {
		pr_warn("%s: No md found. store %s md magic=%llX does not match %X\n",
			__func__, dev->poolname, md_page->magic, BRMR_BLK_STORE_MAGIC);
		ret =  -EINVAL;
		goto out;
	}

	rsp_dev_params->max_hw_sectors = cpu_to_le32(md_page->dev_params.max_hw_sectors);
	rsp_dev_params->max_write_zeroes_sectors =
				cpu_to_le32(md_page->dev_params.max_write_zeroes_sectors);
	rsp_dev_params->max_discard_sectors = cpu_to_le32(md_page->dev_params.max_discard_sectors);
	rsp_dev_params->discard_granularity = cpu_to_le32(md_page->dev_params.discard_granularity);
	rsp_dev_params->discard_alignment = cpu_to_le32(md_page->dev_params.discard_alignment);
	rsp_dev_params->physical_block_size = cpu_to_le16(md_page->dev_params.physical_block_size);
	rsp_dev_params->logical_block_size = cpu_to_le16(md_page->dev_params.logical_block_size);
	rsp_dev_params->max_segments = cpu_to_le16(md_page->dev_params.max_segments);
	rsp_dev_params->secure_discard = cpu_to_le16(md_page->dev_params.secure_discard);
	rsp_dev_params->cache_policy = md_page->dev_params.cache_policy;

out:
	kfree(md_page);
	return ret;
}

/**
 * brmr_srv_fill_dev_param_bdev() - Fill dev params from the underlying block device
 *
 * @dev:	pointer to brmr server block device
 * @rsp:	Pointer to command response structure holding params
 *
 * Return:
 *	0 in case of success
 *	negative in case of failure
 */
static int brmr_srv_fill_dev_param_bdev(struct brmr_srv_blk_dev *dev,
				       struct brmr_cmd_get_params_rsp *rsp)
{
	struct block_device *bdev = dev->bdev;
	struct request_queue *q = bdev_get_queue(bdev);
	struct brmr_blk_dev_params *rsp_dev_params = &rsp->dev_params;

	if (!q) {
		pr_err("%s: no queue for dev %s\n", __func__, dev->name);
		return -EINVAL;
	}

	rsp_dev_params->logical_block_size =
		cpu_to_le16(bdev_logical_block_size(bdev));
	rsp_dev_params->physical_block_size =
		cpu_to_le16(bdev_physical_block_size(bdev));
	rsp_dev_params->max_segments =
		cpu_to_le16(queue_max_segments(q));
	rsp_dev_params->max_hw_sectors =
		cpu_to_le32(queue_max_hw_sectors(q));
	rsp_dev_params->max_write_zeroes_sectors =
		cpu_to_le32(bdev_write_zeroes_sectors(bdev));
	rsp_dev_params->max_discard_sectors = cpu_to_le32(bdev_max_discard_sectors(bdev));
	rsp_dev_params->discard_granularity =
		cpu_to_le32(bdev_get_queue(bdev)->limits.discard_granularity);
	rsp_dev_params->discard_alignment =
		cpu_to_le32(bdev_get_queue(bdev)->limits.discard_alignment);
	rsp_dev_params->secure_discard = cpu_to_le16(bdev_max_secure_erase_sectors(bdev));
	rsp_dev_params->cache_policy = 0;

	if (blk_queue_write_cache(q))
		rsp_dev_params->cache_policy |= BRMR_WRITEBACK;
	if (bdev_fua(bdev))
		rsp_dev_params->cache_policy |= BRMR_FUA;

	return 0;
}

/**
 * brmr_srv_fill_get_params_rsp() - Fill dev params into the command response structure
 *
 * @dev:		pointer to brmr server block device
 * @brmr_cmd_rsp:	Pointer to command response structure
 *
 * Description:
 *	For mapped devices, we need to pick up the params from the brmr server block device itself
 *	These are the same ones which are saved in the metadata of the device.
 *
 *	For unmapped devices, we need to extract this info from the underlying block device
 *
 * Return:
 *	0 in case of success
 *	negative in case of failure
 */
static int brmr_srv_fill_get_params_rsp(struct brmr_srv_blk_dev *dev,
				       struct brmr_msg_cmd_rsp *brmr_cmd_rsp)
{
	struct brmr_cmd_get_params_rsp *rsp;
	int ret;

	if (!dev) {
		pr_err("%s: no brmr srv blk dev to get params\n", __func__);
		return -ENODEV;
	}

	if (!dev->bdev) {
		pr_err("%s: no bdev opened for dev %s\n", __func__, dev->name);
		return -EINVAL;
	}

	rsp = &brmr_cmd_rsp->get_params_rsp;

	/*
	 * For a mapped device, we get the saved params in the device structure (read from md)
	 * since those are the ones which would have gone through validation,
	 * when the map happened.
	 *
	 * For unmapped device, we get params from the underlying bdev.
	 */
	if (test_bit(BRMR_SRV_STORE_MAPPED, &dev->state))
		ret = brmr_srv_fill_dev_param_dev(dev, rsp);
	else
		ret = brmr_srv_fill_dev_param_bdev(dev, rsp);

	if (ret) {
		pr_err("%s: Fill dev params failed for dev %s\n", __func__, dev->name);
		return -EINVAL;
	}

	rsp->mapped = test_bit(BRMR_SRV_STORE_MAPPED, &dev->state);
	rsp->mapped_size = cpu_to_le64(dev->mapped_size);
	pr_info("%s: dev %s, mapped_size %llu\n", __func__,
		dev->name, le64_to_cpu(rsp->mapped_size));

	return 0;
}

/**
 * brmr_srv_blk_cmd() - Processes brmr srv store command messages
 *
 * @device:	brmr server store device
 * @usr_buf:	user buffer containing the command message struct (ones sent as kvec to rmr)
 * @usr_len:	length of the usr_buf
 * @data:	data buffer where the response can be sent back for brmr client to read
 * @datalen:	length of data buffer
 *
 * Return:
 *	0 in case of success
 *	negative in case of failure
 */
static int brmr_srv_blk_cmd(void *device, const void *usr_buf, int usr_len, void *data,
			     int datalen)
{
	struct brmr_srv_blk_dev *dev = device;
	const struct brmr_msg_cmd *msg = (const struct brmr_msg_cmd *)usr_buf;
	struct brmr_msg_cmd_rsp *brmr_cmd_rsp = (struct brmr_msg_cmd_rsp *)data;
	int ret = 0;

	if (datalen < sizeof(*brmr_cmd_rsp)) {
		WARN_ON(1);
		return -EINVAL;
	}

	if (!brmr_srv_blk_get_ref(dev)) {
		pr_err("for dev %s, name %s, failed to get_ref to process command %d\n",
		       dev->name, dev->poolname, msg->cmd_type);
		return -EIO;
	}

	brmr_srv_init_cmd_rsp(brmr_cmd_rsp);

	switch (msg->cmd_type) {
	case BRMR_CMD_MAP:
		pr_info("%s: BRMR_CMD_MAP\n", __func__);

		brmr_cmd_rsp->status = brmr_srv_blk_map_dev(dev, &msg->map_new_cmd);
		if (brmr_cmd_rsp->status) {
			pr_err("Failed to map new dev to %s, err %d\n",
			       dev->name, brmr_cmd_rsp->status);
		}
		break;
	case BRMR_CMD_REMAP:
		pr_info("%s: BRMR_CMD_REMAP\n", __func__);
		break;
	case BRMR_CMD_UNMAP:
		pr_info("%s: BRMR_CMD_UNMAP\n", __func__);

		brmr_cmd_rsp->status = brmr_srv_blk_unmap_dev(dev);
		break;
	case BRMR_CMD_GET_PARAMS:
		pr_info("%s: BRMR_CMD_GET_PARAMS\n", __func__);

		brmr_cmd_rsp->status = brmr_srv_fill_get_params_rsp(dev, brmr_cmd_rsp);
		break;

	default:
		pr_err("%s: Unknown command type %d\n", __func__, msg->cmd_type);
	}

	brmr_srv_blk_put_ref(dev);

	return ret;
}

struct rmr_srv_store_ops pstore_blk_ops = {
	.submit_req = brmr_srv_process_blk_req,
	.submit_md_req = brmr_srv_process_blk_md_req,
	.submit_cmd = brmr_srv_blk_cmd,
	.io_allowed = brmr_srv_blk_io_allowed,
	.get_params = brmr_srv_blk_get_params,
};

static int __init brmr_srv_init_module(void)
{
	int err = 0;

	pr_info("Loading module %s, version %s\n",
		KBUILD_MODNAME, BRMR_SERVER_VER_STRING);

	err = brmr_srv_create_sysfs_files();
	if (err) {
		pr_err("rmr_store_create_sysfs_files(), err: %d\n", err);
		goto out;
	}

	return 0;
out:
	return err;
}

static void __exit brmr_srv_cleanup_module(void)
{
	brmr_srv_blk_cleanup();
	brmr_srv_destroy_sysfs_files();

	pr_info("Module %s unloaded\n", KBUILD_MODNAME);
}

module_init(brmr_srv_init_module);
module_exit(brmr_srv_cleanup_module);
