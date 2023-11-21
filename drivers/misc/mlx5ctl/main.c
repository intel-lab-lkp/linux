// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/mlx5/device.h>
#include <linux/mlx5/driver.h>
#include <linux/atomic.h>
#include <linux/refcount.h>

MODULE_DESCRIPTION("mlx5 ConnectX control misc driver");
MODULE_AUTHOR("Saeed Mahameed <saeedm@nvidia.com>");
MODULE_LICENSE("Dual BSD/GPL");

struct mlx5ctl_dev {
	struct mlx5_core_dev *mdev;
	struct miscdevice miscdev;
	struct auxiliary_device *adev;
	struct list_head fd_list;
	spinlock_t fd_list_lock; /* protect list add/del */
	struct rw_semaphore rw_lock;
	struct kref refcount;
};

struct mlx5ctl_fd {
	u16 uctx_uid;
	u32 uctx_cap;
	u32 ucap; /* user cap */
	struct mlx5ctl_dev *mcdev;
	struct list_head list;
};

#define mlx5ctl_err(mcdev, format, ...) \
	dev_err(mcdev->miscdev.parent, format, ##__VA_ARGS__)

#define mlx5ctl_dbg(mcdev, format, ...) \
	dev_dbg(mcdev->miscdev.parent, "PID %d: " format, \
		current->pid, ##__VA_ARGS__)

enum {
	MLX5_UCTX_OBJECT_CAP_RAW_TX                     = 0x1,
	MLX5_UCTX_OBJECT_CAP_INTERNAL_DEVICE_RESOURCES  = 0x2,
	MLX5_UCTX_OBJECT_CAP_TOOLS_RESOURCES            = 0x4,
};

static int mlx5ctl_alloc_uid(struct mlx5ctl_dev *mcdev, u32 cap)
{
	u32 out[MLX5_ST_SZ_DW(create_uctx_out)] = {};
	u32 in[MLX5_ST_SZ_DW(create_uctx_in)] = {};
	void *uctx;
	int err;
	u16 uid;

	uctx = MLX5_ADDR_OF(create_uctx_in, in, uctx);

	mlx5ctl_dbg(mcdev, "MLX5_CMD_OP_CREATE_UCTX: caps 0x%x\n", cap);
	MLX5_SET(create_uctx_in, in, opcode, MLX5_CMD_OP_CREATE_UCTX);
	MLX5_SET(uctx, uctx, cap, cap);

	err = mlx5_cmd_exec(mcdev->mdev, in, sizeof(in), out, sizeof(out));
	if (err)
		return err;

	uid = MLX5_GET(create_uctx_out, out, uid);
	mlx5ctl_dbg(mcdev, "allocated uid %d with caps 0x%x\n", uid, cap);
	return uid;
}

static void mlx5ctl_release_uid(struct mlx5ctl_dev *mcdev, u16 uid)
{
	u32 in[MLX5_ST_SZ_DW(destroy_uctx_in)] = {};
	struct mlx5_core_dev *mdev = mcdev->mdev;
	int err;

	MLX5_SET(destroy_uctx_in, in, opcode, MLX5_CMD_OP_DESTROY_UCTX);
	MLX5_SET(destroy_uctx_in, in, uid, uid);

	err = mlx5_cmd_exec_in(mdev, destroy_uctx, in);
	mlx5ctl_dbg(mcdev, "released uid %d err(%d)\n", uid, err);
}

static void mcdev_get(struct mlx5ctl_dev *mcdev);
static void mcdev_put(struct mlx5ctl_dev *mcdev);

static int mlx5ctl_open_mfd(struct mlx5ctl_fd *mfd)
{
	struct mlx5_core_dev *mdev = mfd->mcdev->mdev;
	struct mlx5ctl_dev *mcdev = mfd->mcdev;
	u32 ucap = 0, cap = 0;
	int uid;

#define MLX5_UCTX_CAP(mdev, cap) \
	(MLX5_CAP_GEN(mdev, uctx_cap) & MLX5_UCTX_OBJECT_CAP_##cap)

	if (capable(CAP_NET_RAW) && MLX5_UCTX_CAP(mdev, RAW_TX)) {
		ucap |= CAP_NET_RAW;
		cap |= MLX5_UCTX_OBJECT_CAP_RAW_TX;
	}

	if (capable(CAP_SYS_RAWIO) && MLX5_UCTX_CAP(mdev, INTERNAL_DEVICE_RESOURCES)) {
		ucap |= CAP_SYS_RAWIO;
		cap |= MLX5_UCTX_OBJECT_CAP_INTERNAL_DEVICE_RESOURCES;
	}

	if (capable(CAP_SYS_ADMIN) && MLX5_UCTX_CAP(mdev, TOOLS_RESOURCES)) {
		ucap |= CAP_SYS_ADMIN;
		cap |= MLX5_UCTX_OBJECT_CAP_TOOLS_RESOURCES;
	}

	uid = mlx5ctl_alloc_uid(mcdev, cap);
	if (uid < 0)
		return uid;

	mfd->uctx_uid = uid;
	mfd->uctx_cap = cap;
	mfd->ucap = ucap;
	mfd->mcdev = mcdev;

	mlx5ctl_dbg(mcdev, "allocated uid %d with uctx caps 0x%x, user cap 0x%x\n",
		    uid, cap, ucap);
	return 0;
}

static void mlx5ctl_release_mfd(struct mlx5ctl_fd *mfd)
{
	struct mlx5ctl_dev *mcdev = mfd->mcdev;

	mlx5ctl_release_uid(mcdev,  mfd->uctx_uid);
}

static int mlx5ctl_open(struct inode *inode, struct file *file)
{
	struct mlx5_core_dev *mdev;
	struct mlx5ctl_dev *mcdev;
	struct mlx5ctl_fd *mfd;
	int err = 0;

	mcdev = container_of(file->private_data, struct mlx5ctl_dev, miscdev);
	mcdev_get(mcdev);
	down_read(&mcdev->rw_lock);
	mdev = mcdev->mdev;
	if (!mdev) {
		err = -ENODEV;
		goto unlock;
	}

	mfd = kzalloc(sizeof(*mfd), GFP_KERNEL_ACCOUNT);
	if (!mfd)
		return -ENOMEM;

	mfd->mcdev = mcdev;
	err = mlx5ctl_open_mfd(mfd);
	if (err)
		goto unlock;

	spin_lock(&mcdev->fd_list_lock);
	list_add_tail(&mfd->list, &mcdev->fd_list);
	spin_unlock(&mcdev->fd_list_lock);

	file->private_data = mfd;

unlock:
	up_read(&mcdev->rw_lock);
	if (err) {
		mcdev_put(mcdev);
		kfree(mfd);
	}
	return err;
}

static int mlx5ctl_release(struct inode *inode, struct file *file)
{
	struct mlx5ctl_fd *mfd = file->private_data;
	struct mlx5ctl_dev *mcdev = mfd->mcdev;

	down_read(&mcdev->rw_lock);
	if (!mcdev->mdev) {
		pr_debug("[%d] UID %d mlx5ctl: mdev is already released\n",
			 current->pid, mfd->uctx_uid);
		/* All mfds are already released, skip ... */
		goto unlock;
	}

	spin_lock(&mcdev->fd_list_lock);
	list_del(&mfd->list);
	spin_unlock(&mcdev->fd_list_lock);

	mlx5ctl_release_mfd(mfd);

unlock:
	kfree(mfd);
	up_read(&mcdev->rw_lock);
	mcdev_put(mcdev);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations mlx5ctl_fops = {
	.owner = THIS_MODULE,
	.open = mlx5ctl_open,
	.release = mlx5ctl_release,
};

static int mlx5ctl_probe(struct auxiliary_device *adev,
			 const struct auxiliary_device_id *id)

{
	struct mlx5_adev *madev = container_of(adev, struct mlx5_adev, adev);
	struct mlx5_core_dev *mdev = madev->mdev;
	struct mlx5ctl_dev *mcdev;
	char *devname = NULL;
	int err;

	mcdev = kzalloc(sizeof(*mcdev), GFP_KERNEL_ACCOUNT);
	if (!mcdev)
		return -ENOMEM;

	kref_init(&mcdev->refcount);
	INIT_LIST_HEAD(&mcdev->fd_list);
	spin_lock_init(&mcdev->fd_list_lock);
	init_rwsem(&mcdev->rw_lock);
	mcdev->mdev = mdev;
	mcdev->adev = adev;
	devname = kasprintf(GFP_KERNEL_ACCOUNT, "mlx5ctl-%s",
			    dev_name(&adev->dev));
	if (!devname) {
		err = -ENOMEM;
		goto abort;
	}

	mcdev->miscdev = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR,
		.name = devname,
		.fops = &mlx5ctl_fops,
		.parent = &adev->dev,
	};

	err = misc_register(&mcdev->miscdev);
	if (err) {
		mlx5ctl_err(mcdev, "mlx5ctl: failed to register misc device err %d\n", err);
		goto abort;
	}

	mlx5ctl_dbg(mcdev, "probe mdev@%s %s\n", dev_driver_string(mdev->device), dev_name(mdev->device));

	auxiliary_set_drvdata(adev, mcdev);

	return 0;

abort:
	kfree(devname);
	kfree(mcdev);
	return err;
}

static void mlx5ctl_remove(struct auxiliary_device *adev)
{
	struct mlx5ctl_dev *mcdev = auxiliary_get_drvdata(adev);
	struct mlx5_core_dev *mdev = mcdev->mdev;
	struct mlx5ctl_fd *mfd, *n;

	misc_deregister(&mcdev->miscdev);
	down_write(&mcdev->rw_lock);

	list_for_each_entry_safe(mfd, n, &mcdev->fd_list, list) {
		mlx5ctl_dbg(mcdev, "UID %d still has open FDs\n", mfd->uctx_uid);
		list_del(&mfd->list);
		mlx5ctl_release_mfd(mfd);
	}

	mlx5ctl_dbg(mcdev, "removed mdev %s %s\n",
		    dev_driver_string(mdev->device), dev_name(mdev->device));

	mcdev->mdev = NULL; /* prevent already open fds from accessing the device */
	up_write(&mcdev->rw_lock);
	mcdev_put(mcdev);
}

static void mcdev_free(struct kref *ref)
{
	struct mlx5ctl_dev *mcdev = container_of(ref, struct mlx5ctl_dev, refcount);

	kfree(mcdev->miscdev.name);
	kfree(mcdev);
}

static void mcdev_get(struct mlx5ctl_dev *mcdev)
{
	kref_get(&mcdev->refcount);
}

static void mcdev_put(struct mlx5ctl_dev *mcdev)
{
	kref_put(&mcdev->refcount, mcdev_free);
}

static const struct auxiliary_device_id mlx5ctl_id_table[] = {
	{ .name = MLX5_ADEV_NAME ".ctl", },
	{},
};

MODULE_DEVICE_TABLE(auxiliary, mlx5ctl_id_table);

static struct auxiliary_driver mlx5ctl_driver = {
	.name = "ctl",
	.probe = mlx5ctl_probe,
	.remove = mlx5ctl_remove,
	.id_table = mlx5ctl_id_table,
};

module_auxiliary_driver(mlx5ctl_driver);
