// SPDX-License-Identifier: GPL-2.0-only
/*
 * syncobj.c - Standalone device for syncobj manipulation.
 *
 * Copyright (C) 2026 Julian Orth <ju.orth@gmail.com>
 */

#include <linux/fdtable.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_utils.h>
#include <uapi/drm/drm.h>
#include <uapi/linux/syncobj.h>

static int syncobj_array_find(void __user *user_fds, u32 count,
			      struct drm_syncobj ***syncobjs_out)
{
	u32 i;
	s32 *fds;
	struct drm_syncobj **syncobjs;
	int ret;

	fds = kmalloc_array(count, sizeof(*fds), GFP_KERNEL);
	if (!fds)
		return -ENOMEM;

	if (copy_from_user(fds, user_fds, sizeof(s32) * count)) {
		ret = -EFAULT;
		goto err_free_fds;
	}

	syncobjs = kmalloc_array(count, sizeof(*syncobjs), GFP_KERNEL);
	if (!syncobjs) {
		ret = -ENOMEM;
		goto err_free_fds;
	}

	for (i = 0; i < count; i++) {
		syncobjs[i] = drm_syncobj_from_fd(fds[i]);
		if (!syncobjs[i]) {
			ret = -EBADF;
			goto err_put_syncobjs;
		}
	}

	kfree(fds);
	*syncobjs_out = syncobjs;
	return 0;

err_put_syncobjs:
	while (i-- > 0)
		drm_syncobj_put(syncobjs[i]);
	kfree(syncobjs);
err_free_fds:
	kfree(fds);
	return ret;
}

static void syncobj_array_free(struct drm_syncobj **syncobjs, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++)
		drm_syncobj_put(syncobjs[i]);
	kfree(syncobjs);
}

static int syncobj_ioctl_create(void __user *argp)
{
	struct syncobj_create_args args;
	struct drm_syncobj *syncobj;
	int fd, ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.flags & ~SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;

	static_assert(SYNCOBJ_CREATE_SIGNALED == DRM_SYNCOBJ_CREATE_SIGNALED);

	ret = drm_syncobj_create(&syncobj, args.flags, NULL);
	if (ret)
		return ret;

	ret = drm_syncobj_get_fd(syncobj, &fd);
	drm_syncobj_put(syncobj);
	if (ret)
		return ret;

	args.fd = fd;
	if (copy_to_user(argp, &args, sizeof(args))) {
		close_fd(fd);
		return -EFAULT;
	}

	return 0;
}

static int syncobj_ioctl_wait(void __user *argp)
{
	struct syncobj_wait_args args;
	struct drm_syncobj **syncobjs;
	signed long timeout;
	u32 first = ~0;
	ktime_t t, *tp = NULL;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.flags & ~(SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			   SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			   SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
			   SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE))
		return -EINVAL;

	static_assert(SYNCOBJ_WAIT_FLAGS_WAIT_ALL        == DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL);
	static_assert(SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT == DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);
	static_assert(SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE  == DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
	static_assert(SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE   == DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE);

	if (args.pad)
		return -EINVAL;

	if (args.count == 0)
		return 0;

	ret = syncobj_array_find(u64_to_user_ptr(args.fds),
				 args.count, &syncobjs);
	if (ret < 0)
		return ret;

	if (args.flags & SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE) {
		t = ns_to_ktime(args.deadline_nsec);
		tp = &t;
	}

	timeout = drm_timeout_abs_to_jiffies(args.timeout_nsec);
	timeout = drm_syncobj_array_wait_timeout(syncobjs,
						 u64_to_user_ptr(args.points),
						 args.count,
						 args.flags,
						 timeout, &first, tp);

	syncobj_array_free(syncobjs, args.count);

	if (timeout < 0)
		return timeout;

	args.first_signaled = first;
	if (copy_to_user(argp, &args, sizeof(args)))
		return -EFAULT;

	return 0;
}

static int syncobj_ioctl_reset(void __user *argp)
{
	struct syncobj_array_args args;
	struct drm_syncobj **syncobjs;
	u32 i;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.flags)
		return -EINVAL;

	if (args.points)
		return -EINVAL;

	if (args.count == 0)
		return -EINVAL;

	ret = syncobj_array_find(u64_to_user_ptr(args.fds),
				 args.count, &syncobjs);
	if (ret < 0)
		return ret;

	for (i = 0; i < args.count; i++)
		drm_syncobj_replace_fence(syncobjs[i], NULL);

	syncobj_array_free(syncobjs, args.count);
	return 0;
}

static int syncobj_ioctl_signal(void __user *argp)
{
	struct syncobj_array_args args;
	struct drm_syncobj **syncobjs;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.flags)
		return -EINVAL;

	if (args.count == 0)
		return -EINVAL;

	ret = syncobj_array_find(u64_to_user_ptr(args.fds),
				 args.count, &syncobjs);
	if (ret < 0)
		return ret;

	ret = drm_syncobj_timeline_signal(syncobjs, args.points, args.count);

	syncobj_array_free(syncobjs, args.count);
	return ret;
}

static int syncobj_ioctl_query(void __user *argp)
{
	struct syncobj_array_args args;
	struct drm_syncobj **syncobjs;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.flags & ~SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
		return -EINVAL;

	static_assert(SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED == DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED);

	if (args.count == 0)
		return -EINVAL;

	ret = syncobj_array_find(u64_to_user_ptr(args.fds),
				 args.count, &syncobjs);
	if (ret < 0)
		return ret;

	ret = drm_syncobj_query(syncobjs, args.points, args.count, args.flags);

	syncobj_array_free(syncobjs, args.count);
	return ret;
}

static int syncobj_ioctl_transfer(void __user *argp)
{
	struct syncobj_transfer_args args;
	struct drm_syncobj *src, *dst;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.pad)
		return -EINVAL;

	if (args.flags & ~SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)
		return -EINVAL;

	static_assert(SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT == DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);

	src = drm_syncobj_from_fd(args.src_fd);
	if (!src)
		return -EBADF;

	dst = drm_syncobj_from_fd(args.dst_fd);
	if (!dst) {
		drm_syncobj_put(src);
		return -EBADF;
	}

	ret = drm_syncobj_transfer(src, args.src_point,
				   dst, args.dst_point, args.flags);

	drm_syncobj_put(dst);
	drm_syncobj_put(src);

	return ret;
}

static int syncobj_ioctl_eventfd(void __user *argp)
{
	struct syncobj_eventfd_args args;
	struct drm_syncobj *syncobj;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.flags & ~SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)
		return -EINVAL;

	static_assert(SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE == DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);

	if (args.pad)
		return -EINVAL;

	syncobj = drm_syncobj_from_fd(args.syncobj_fd);
	if (!syncobj)
		return -EBADF;

	ret = drm_syncobj_register_eventfd(syncobj, args.eventfd,
					   args.point, args.flags);

	drm_syncobj_put(syncobj);

	return ret;
}

static int syncobj_ioctl_export_sync_file(void __user *argp)
{
	struct syncobj_sync_file_args args;
	struct drm_syncobj *syncobj;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	syncobj = drm_syncobj_from_fd(args.syncobj_fd);
	if (!syncobj)
		return -EBADF;

	ret = drm_syncobj_export_sync_file(syncobj, args.point,
					   &args.sync_file_fd);
	drm_syncobj_put(syncobj);
	if (ret)
		return ret;

	if (copy_to_user(argp, &args, sizeof(args))) {
		close_fd(args.sync_file_fd);
		return -EFAULT;
	}

	return 0;
}

static int syncobj_ioctl_import_sync_file(void __user *argp)
{
	struct syncobj_sync_file_args args;
	struct drm_syncobj *syncobj;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	syncobj = drm_syncobj_from_fd(args.syncobj_fd);
	if (!syncobj)
		return -EBADF;

	ret = drm_syncobj_import_sync_file(syncobj, args.sync_file_fd,
					   args.point);

	drm_syncobj_put(syncobj);

	return ret;
}

static long syncobj_dev_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case SYNCOBJ_IOC_CREATE:
		return syncobj_ioctl_create(argp);
	case SYNCOBJ_IOC_WAIT:
		return syncobj_ioctl_wait(argp);
	case SYNCOBJ_IOC_RESET:
		return syncobj_ioctl_reset(argp);
	case SYNCOBJ_IOC_SIGNAL:
		return syncobj_ioctl_signal(argp);
	case SYNCOBJ_IOC_QUERY:
		return syncobj_ioctl_query(argp);
	case SYNCOBJ_IOC_TRANSFER:
		return syncobj_ioctl_transfer(argp);
	case SYNCOBJ_IOC_EVENTFD:
		return syncobj_ioctl_eventfd(argp);
	case SYNCOBJ_IOC_EXPORT_SYNC_FILE:
		return syncobj_ioctl_export_sync_file(argp);
	case SYNCOBJ_IOC_IMPORT_SYNC_FILE:
		return syncobj_ioctl_import_sync_file(argp);
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct file_operations syncobj_dev_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= syncobj_dev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice syncobj_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "syncobj",
	.fops	= &syncobj_dev_fops,
	.mode	= 0666,
};

module_misc_device(syncobj_misc);

MODULE_AUTHOR("Julian Orth");
MODULE_DESCRIPTION("DRM syncobj device");
MODULE_LICENSE("GPL");
