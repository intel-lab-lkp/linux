// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/raid/md_u.h>
#include <linux/raid/md_p.h>
#include "md.h"
#include "md-bitmap.h"

static struct mddev *get_mddev_dev(dev_t dev)
{
	struct mddev *mddev, *ret = NULL;

	if (MINOR(dev) >= (1 << MINORBITS))
		return ERR_PTR(-EINVAL);

	if (MAJOR(dev) != MD_MAJOR)
		dev &= ~((1 << MdpMinorShift) - 1);

	spin_lock(&all_mddevs_lock);
	list_for_each_entry(mddev, &all_mddevs, all_mddevs) {
		if (mddev->unit == dev) {
			ret = mddev_get(mddev);
			if (ret && !mddev->gendisk) {
				mddev_put(mddev);
				ret = ERR_PTR(-EBUSY);
			}
			break;
		}
	}
	spin_unlock(&all_mddevs_lock);

	return ret ? ret : ERR_PTR(-ENODEV);
}

static struct mddev *get_mddev_name(const char *name)
{
	unsigned long minor;
	struct mddev *mddev, *ret = NULL;

	if (strncmp(name, "md", 2))
		return ERR_PTR(-EINVAL);

	if (name[2] != '_' && (!isdigit(name[2]) ||
			       kstrtoul(&name[2], 10, &minor) ||
			       minor > MINORMASK))
		return ERR_PTR(-EINVAL);

	spin_lock(&all_mddevs_lock);
	list_for_each_entry(mddev, &all_mddevs, all_mddevs) {
		if (mddev->gendisk &&
		    strcmp(mddev->gendisk->disk_name, name) == 0) {
			ret = mddev_get(mddev);
			break;
		}
	}
	spin_unlock(&all_mddevs_lock);

	return ret ? ret : ERR_PTR(-ENODEV);
}

static struct mddev *get_mddev_uuid(const char *uuid)
{
	struct mddev *mddev, *ret = NULL;

	spin_lock(&all_mddevs_lock);
	list_for_each_entry(mddev, &all_mddevs, all_mddevs) {
		if (memcmp(mddev->uuid, uuid, sizeof(mddev->uuid)) == 0) {
			ret = mddev_get(mddev);
			if (ret && !mddev->gendisk) {
				mddev_put(mddev);
				ret = ERR_PTR(-EBUSY);
			}
			break;
		}
	}
	spin_unlock(&all_mddevs_lock);

	return ret ? ret : ERR_PTR(-ENODEV);
}

static struct mddev *ctl_find_mddev(struct mdu_ioctl *md_ctl)
{
	struct mddev *ret = ERR_PTR(-EINVAL);

	if (*md_ctl->uuid) {
		if (*md_ctl->name || md_ctl->dev) {
			pr_err("md: only supply one of name, uuid or dev number\n");
			return ret;
		}
		ret = get_mddev_uuid(md_ctl->uuid);
	} else if (*md_ctl->name) {
		if (md_ctl->dev) {
			pr_err("md: only supply one of name, uuid or dev number\n");
			return ret;
		}
		ret = get_mddev_name(md_ctl->name);
	} else if (md_ctl->dev) {
		ret = get_mddev_dev(md_ctl->dev);
	}

	return ret;
}

static int get_raid_version(struct mddev *mddev, struct mdu_version64 __user *user)
{
	struct mdu_version64 v = {0};

	v.major = MD_MAJOR_VERSION;
	v.minor = MD_MINOR_VERSION;
	v.patchlevel = MD_PATCHLEVEL_VERSION;

	if (copy_to_user(user, &v, sizeof(v)))
		return -EFAULT;

	return 0;
}

static int ctl_get_disk_info(struct mddev *mddev, void __user *arg)
{
	struct mdu_disk_info64 info = {0};
	struct md_rdev *rdev;

	if (!mddev->raid_disks && !mddev->external)
		return -ENODEV;

	if (copy_from_user(&info, arg, sizeof(info)))
		return -EFAULT;

	rcu_read_lock();
	rdev = md_find_rdev_nr_rcu(mddev, info.number);
	if (rdev) {
		info.major = MAJOR(rdev->bdev->bd_dev);
		info.minor = MINOR(rdev->bdev->bd_dev);
		info.raid_disk = rdev->raid_disk;
		info.state = 0;
		if (test_bit(Faulty, &rdev->flags))
			info.state |= (1 << MD_DISK_FAULTY);
		else if (test_bit(In_sync, &rdev->flags)) {
			info.state |= (1 << MD_DISK_ACTIVE);
			info.state |= (1 << MD_DISK_SYNC);
		}
		if (test_bit(Journal, &rdev->flags))
			info.state |= (1 << MD_DISK_JOURNAL);
		if (test_bit(WriteMostly, &rdev->flags))
			info.state |= (1 << MD_DISK_WRITEMOSTLY);
		if (test_bit(FailFast, &rdev->flags))
			info.state |= (1 << MD_DISK_FAILFAST);
	} else {
		info.major = info.minor = 0;
		info.raid_disk = -1;
		info.state = (1 << MD_DISK_REMOVED);
	}
	rcu_read_unlock();

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static int ctl_get_bitmap_file(struct mddev *mddev, void __user *arg)
{
	struct mdu_bitmap_file64 *file = NULL; /* too big for stack allocation */
	char *ptr;
	int err;

	file = kzalloc_obj(*file, GFP_NOIO);
	if (!file)
		return -ENOMEM;

	err = 0;
	spin_lock(&mddev->lock);
	/* bitmap enabled */
	if (mddev->bitmap_info.file) {
		ptr = file_path(mddev->bitmap_info.file, file->pathname,
				sizeof(file->pathname));
		if (IS_ERR(ptr))
			err = PTR_ERR(ptr);
		else
			memmove(file->pathname, ptr,
				sizeof(file->pathname)-(ptr-file->pathname));
	}
	spin_unlock(&mddev->lock);

	if (err == 0 &&
	    copy_to_user(arg, file, sizeof(*file)))
		err = -EFAULT;

	kfree(file);
	return err;
}

static int ctl_get_array_info(struct mddev *mddev, void __user *arg)
{
	struct mdu_array_info64 info = {0};
	int nr, working, insync, failed, spare, journal;
	struct md_rdev *rdev;

	if (!mddev->raid_disks && !mddev->external)
		return -ENODEV;

	nr = working = insync = failed = spare = journal = 0;
	rcu_read_lock();
	rdev_for_each_rcu(rdev, mddev) {
		nr++;
		if (test_bit(Faulty, &rdev->flags)) {
			failed++;
		} else {
			working++;
			if (test_bit(In_sync, &rdev->flags))
				insync++;
			else if (test_bit(Journal, &rdev->flags))
				journal++;
			else
				spare++;
		}
	}
	rcu_read_unlock();

	info.major_version = mddev->major_version;
	info.minor_version = mddev->minor_version;
	info.patch_version = MD_PATCHLEVEL_VERSION;
	info.ctime         = mddev->ctime;
	info.level         = mddev->level;
	info.size          = mddev->dev_sectors / 2;
	info.nr_disks      = nr;
	info.raid_disks    = mddev->raid_disks;
	info.md_minor      = mddev->md_minor;
	info.not_persistent = !mddev->persistent;

	info.utime         = mddev->utime;
	info.state         = 0;
	if (mddev->in_sync)
		info.state = (1 << MD_SB_CLEAN);
	if (mddev->bitmap && mddev->bitmap_info.offset)
		info.state |= (1 << MD_SB_BITMAP_PRESENT);
	if (mddev_is_clustered(mddev))
		info.state |= (1 << MD_SB_CLUSTERED);
	info.active_disks  = insync;
	info.working_disks = working;
	info.failed_disks  = failed;
	info.spare_disks   = spare;
	info.journal_disks = journal;

	info.layout        = mddev->layout;
	info.chunk_size    = mddev->chunk_sectors << 9;

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static inline int ctl_ioctl_valid(unsigned int cmd)
{
	switch (cmd) {
	case MD_GET_ARRAY_INFO:
	case MD_GET_DISK_INFO:
	case MD_RAID_VERSION:
		return 0;

	case MD_RAID_AUTORUN:
	case MD_SET_DISK_FAULTY:
	case MD_GET_BITMAP_FILE:
	case MD_ADD_NEW_DISK:
	case MD_HOT_ADD_DISK:
	case MD_HOT_REMOVE_DISK:
	case MD_RESTART_ARRAY_RW:
	case MD_RUN_ARRAY:
	case MD_SET_ARRAY_INFO:
	case MD_SET_BITMAP_FILE:
	case MD_STOP_ARRAY:
	case MD_CLUSTERED_DISK_NACK:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		return 0;
	default:
		return -ENOTTY;
	}
}

static inline int ctl_flags_valid(unsigned int cmd, u32 flags)
{
	switch (cmd) {
	case MD_GET_ARRAY_INFO:
	case MD_GET_DISK_INFO:
	case MD_RAID_VERSION:
	case MD_RAID_AUTORUN:
	case MD_GET_BITMAP_FILE:
	case MD_ADD_NEW_DISK:
	case MD_HOT_ADD_DISK:
	case MD_HOT_REMOVE_DISK:
	case MD_RESTART_ARRAY_RW:
	case MD_RUN_ARRAY:
	case MD_SET_ARRAY_INFO:
	case MD_SET_BITMAP_FILE:
	case MD_SET_DISK_FAULTY:
	case MD_CLUSTERED_DISK_NACK:
		if (flags)
			return -ENOTTY;
		return 0;
	case MD_STOP_ARRAY:
		if (flags && flags != MD_RO_FLAG)
			return -ENOTTY;
		return 0;
	default:
		return -ENOTTY;
	}
}

static inline unsigned int try_convert_ioctl_cmd(unsigned int cmd,
						 unsigned int flags)
{
	int ret = 0;

	switch (cmd) {
	case MD_SET_DISK_FAULTY:
		ret = SET_DISK_FAULTY;
		break;

	case MD_HOT_ADD_DISK:
		ret = HOT_ADD_DISK;
		break;

	case MD_HOT_REMOVE_DISK:
		ret = HOT_REMOVE_DISK;
		break;

	case MD_RAID_AUTORUN:
		ret = RAID_AUTORUN;
		break;

	case MD_RESTART_ARRAY_RW:
		ret = RESTART_ARRAY_RW;
		break;

	case MD_RUN_ARRAY:
		ret = RUN_ARRAY;
		break;

	case MD_SET_BITMAP_FILE:
		ret = SET_BITMAP_FILE;
		break;

	case MD_STOP_ARRAY:
		if (flags == MD_RO_FLAG)
			ret = STOP_ARRAY_RO;
		else
			ret = STOP_ARRAY;
		break;

	case MD_CLUSTERED_DISK_NACK:
		ret = CLUSTERED_DISK_NACK;
		break;
	};

	return ret;
}

static void ctl_convert_array_info(mdu_array_info_t *info,
				  struct mdu_array_info64 *info64)
{
	info->major_version = info64->major_version;
	info->minor_version = info64->minor_version;
	info->patch_version = info64->patch_version;
	info->level = info64->level;
	info->ctime = info64->ctime;
	info->size = info64->size;
	info->nr_disks = info64->nr_disks;
	info->raid_disks = info64->raid_disks;
	info->md_minor = info64->md_minor;
	info->not_persistent = info64->not_persistent;
	info->utime = info64->utime;
	info->state = info64->state;
	info->active_disks = info64->active_disks;
	info->working_disks = info64->working_disks;
	info->failed_disks = info64->failed_disks;
	info->spare_disks = info64->spare_disks;
	info->layout = info64->layout;
	info->chunk_size = info64->chunk_size;
}

static void ctl_convert_disk_info(mdu_disk_info_t *info,
				  struct mdu_disk_info64 *info64)
{
	info->number = info64->number;
	info->major = info64->major;
	info->minor = info64->minor;
	info->raid_disk = info64->raid_disk;
	info->state = info64->state;
}

static int ctl_set_read_write(struct mddev *mddev)
{
	if (!md_is_rdwr(mddev) && mddev->pers) {
		if (mddev->ro != MD_AUTO_READ)
			return -EROFS;

		mddev->ro = MD_RDWR;
		sysfs_notify_dirent_safe(mddev->sysfs_state);
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
		/* mddev_unlock will wake thread */
		/* If a device failed while we were read-only, we
		 * need to make sure the metadata is updated now.
		 */
		if (test_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags)) {
			mddev_unlock(mddev);
			wait_event(mddev->sb_wait,
				   !test_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags) &&
				   !test_bit(MD_SB_CHANGE_PENDING, &mddev->sb_flags));
			mddev_lock_nointr(mddev);
		}
	}

	return 0;
}

static int ctl_add_new_disk(struct mddev *mddev,
			    struct mdu_disk_info64 __user *user)
{
	mdu_disk_info_t info;
	struct mdu_disk_info64 info64;
	int err;

	if (copy_from_user(&info64, user, sizeof(info64)))
		return -EFAULT;

	ctl_convert_disk_info(&info, &info64);

	/*
	 * We can support ADD_NEW_DISK on read-only arrays
	 * only if we are re-adding a preexisting device.
	 * So require mddev->pers and MD_DISK_SYNC.
	 */
	if (mddev->pers && (info.state & (1 << MD_DISK_SYNC)))
		return md_add_new_disk(mddev, &info);

	err = ctl_set_read_write(mddev);
	if (err)
		return err;

	return md_add_new_disk(mddev, &info);
}

static int ctl_set_array_info(struct mddev *mddev,
			      struct mdu_array_info64 __user *user)
{
	mdu_array_info_t info;
	struct mdu_array_info64 info64;
	int err;

	if (!user)
		memset(&info64, 0, sizeof(info64));
	else if (copy_from_user(&info64, user, sizeof(info64)))
		return -EFAULT;

	ctl_convert_array_info(&info, &info64);

	if (mddev->pers) {
		err = update_array_info(mddev, &info);
		if (err)
			pr_warn("md: couldn't update array info. %d\n", err);
		return err;
	}

	if (!list_empty(&mddev->disks)) {
		pr_warn("md: array %s already has disks!\n", mdname(mddev));
		return -EBUSY;
	}

	if (mddev->raid_disks) {
		pr_warn("md: array %s already initialised!\n", mdname(mddev));
		return -EBUSY;
	}

	err = md_set_array_info(mddev, &info);
	if (err)
		pr_warn("md: couldn't set array info. %d\n", err);

	return err;
}

static long md_ctl_ioctl(struct file *filp, uint cmd, ulong u)
{
	int err;
	unsigned int old_cmd;
	unsigned int noio_flags = 0;
	struct mddev *mddev;
	void __user *arg;
	struct mdu_ioctl md_ctl;

	err = ctl_ioctl_valid(cmd);
	if (err)
		return err;

	if (cmd == MD_RAID_VERSION)
		return get_raid_version(mddev, (struct mdu_version64 __user *)u);

	if (copy_from_user(&md_ctl, (void __user *)u, sizeof(md_ctl)))
		return -EFAULT;

	err = ctl_flags_valid(cmd, md_ctl.flags);
	if (err)
		return err;

	mddev = ctl_find_mddev(&md_ctl);
	if (IS_ERR(mddev))
		return PTR_ERR(mddev);

	old_cmd = try_convert_ioctl_cmd(cmd, md_ctl.flags);
	if (old_cmd) {
		err = md_do_ioctl(mddev->gendisk->part0, 0, old_cmd, md_ctl.arg);
		goto out;
	}

	arg = (void __user *)md_ctl.arg;
	switch (cmd) {
	case MD_GET_ARRAY_INFO:
		err = ctl_get_array_info(mddev, arg);
		goto out;

	case MD_GET_DISK_INFO:
		err = ctl_get_disk_info(mddev, arg);
		goto out;

	case MD_GET_BITMAP_FILE:
		err = ctl_get_bitmap_file(mddev, arg);
		goto out;
	};

	if (!md_is_rdwr(mddev))
		flush_work(&mddev->sync_work);

	err = mddev_suspend_and_lock(mddev);
	if (err) {
		pr_debug("md: ioctl lock interrupted, reason %d, cmd %d\n",
			 err, cmd);
		goto out;
	}
	noio_flags = memalloc_noio_save();

	switch (cmd) {
	case MD_SET_ARRAY_INFO:
		err = ctl_set_array_info(mddev, arg);
		break;

	case MD_ADD_NEW_DISK:
		err = ctl_add_new_disk(mddev, arg);
		break;
	}

	if (mddev->hold_active == UNTIL_IOCTL && err != -EINVAL)
		mddev->hold_active = 0;

	memalloc_noio_restore(noio_flags);
	mddev_unlock_and_resume(mddev);
out:
	mddev_put(mddev);
	return err;
}

static const struct file_operations file_ops = {
	.open = nonseekable_open,
	.unlocked_ioctl = md_ctl_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.owner = THIS_MODULE,
	.llseek  = noop_llseek,
};

static struct miscdevice md_ctl_misc = {
	.name = MD_CTL_NODE,
	.minor = MD_CTRL_MINOR,
	.fops = &file_ops,
};

MODULE_ALIAS_MISCDEV(MD_CTRL_MINOR);
MODULE_ALIAS("devname:" MD_CTL_NODE);

int __init md_ctl_init(void)
{
	int err;

	err = misc_register(&md_ctl_misc);
	if (err)
		return err;

	return 0;
}

void __init md_ctl_exit(void)
{
	misc_deregister(&md_ctl_misc);
}
