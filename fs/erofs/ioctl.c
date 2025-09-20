// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/fs.h>
#include <linux/compat.h>
#include <linux/file.h>

#include "internal.h"

static int erofs_ioctl_get_volume_label(struct inode *inode, void __user *arg)
{
	struct erofs_sb_info *sbi = EROFS_I_SB(inode);
	int ret;

	if (!sbi->volume_name)
		ret = clear_user(arg, 1);
	else
		ret = copy_to_user(arg, sbi->volume_name,
				   strlen(sbi->volume_name));

	return ret ? -EFAULT : 0;
}

long erofs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case FS_IOC_GETFSLABEL:
		return erofs_ioctl_get_volume_label(inode, argp);
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
long erofs_compat_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	return erofs_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif
