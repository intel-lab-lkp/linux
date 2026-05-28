// SPDX-License-Identifier: GPL-2.0-only
#include <linux/anon_inodes.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <uapi/linux/spawn_template.h>

#include "internal.h"

#define SPAWN_TEMPLATE_MAX_ACTIONS	256

struct spawn_template {
	struct file *exec_file;
	const struct cred *creator_cred;
	char *filename;
	bool deny_write;
};

static const struct file_operations spawn_template_fops;

static bool spawn_template_file_exec_allowed(struct file *file)
{
	if (!S_ISREG(file_inode(file)->i_mode))
		return false;
	if (path_noexec(&file->f_path))
		return false;
	if (file_permission(file, MAY_EXEC))
		return false;
	return can_mmap_file(file);
}

static int spawn_template_release(struct inode *inode, struct file *file)
{
	struct spawn_template *tmpl = file->private_data;

	if (tmpl->deny_write)
		exe_file_allow_write_access(tmpl->exec_file);
	fput(tmpl->exec_file);
	put_cred(tmpl->creator_cred);
	kfree(tmpl->filename);
	kfree(tmpl);
	return 0;
}

static const struct file_operations spawn_template_fops = {
	.release	= spawn_template_release,
	.llseek		= noop_llseek,
};

static int spawn_template_open_execfd(int execfd, struct file **file,
				      bool *deny_write)
{
	int ret;

	if (execfd < 0)
		return -EINVAL;

	CLASS(fd, f)(execfd);
	if (fd_empty(f))
		return -EBADF;

	if (!spawn_template_file_exec_allowed(fd_file(f)))
		return -EACCES;

	ret = exe_file_deny_write_access(fd_file(f));
	if (ret)
		return ret;

	*file = get_file(fd_file(f));
	*deny_write = true;
	return 0;
}

static int spawn_template_open_filename(u64 filename, struct file **file,
					char **path,
					bool *deny_write)
{
	char *kfilename __free(kfree) = NULL;
	struct file *exec __free(fput) = NULL;
	struct file *tmp_file;
	char *tmp;

	if (!filename)
		return -EINVAL;

	tmp = strndup_user(u64_to_user_ptr(filename), PATH_MAX);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);
	kfilename = tmp;

	tmp_file = open_exec(kfilename);
	if (IS_ERR(tmp_file))
		return PTR_ERR(tmp_file);
	exec = tmp_file;
	if (!spawn_template_file_exec_allowed(exec)) {
		exe_file_allow_write_access(exec);
		return -EACCES;
	}

	*file = no_free_ptr(exec);
	*path = no_free_ptr(kfilename);
	*deny_write = true;
	return 0;
}

SYSCALL_DEFINE2(spawn_template_create,
		struct spawn_template_create_args __user *, uargs,
		size_t, usize)
{
	struct spawn_template_create_args args;
	struct spawn_template *tmpl;
	int fd_flags = 0;
	int ret;

	BUILD_BUG_ON(sizeof(struct spawn_template_create_args) !=
		     SPAWN_TEMPLATE_CREATE_ARGS_SIZE_VER0);

	if (usize < SPAWN_TEMPLATE_CREATE_ARGS_SIZE_VER0)
		return -EINVAL;
	if (usize > PAGE_SIZE)
		return -E2BIG;

	ret = copy_struct_from_user(&args, sizeof(args), uargs, usize);
	if (ret)
		return ret;

	if (args.flags & ~SPAWN_TEMPLATE_CREATE_CLOEXEC)
		return -EINVAL;
	if (args.exec_flags || args.reserved[0] || args.reserved[1] ||
	    args.reserved[2] || args.reserved[3])
		return -EINVAL;
	if (args.actions || args.actions_len)
		return -EINVAL;
	if ((args.execfd < 0 && !args.filename) ||
	    (args.execfd >= 0 && args.filename))
		return -EINVAL;

	tmpl = kzalloc_obj(*tmpl, GFP_KERNEL);
	if (!tmpl)
		return -ENOMEM;
	tmpl->creator_cred = get_current_cred();

	if (args.filename)
		ret = spawn_template_open_filename(args.filename,
						   &tmpl->exec_file,
						   &tmpl->filename,
						   &tmpl->deny_write);
	else
		ret = spawn_template_open_execfd(args.execfd,
						 &tmpl->exec_file,
						 &tmpl->deny_write);
	if (ret)
		goto out_free_tmpl;

	if (args.flags & SPAWN_TEMPLATE_CREATE_CLOEXEC)
		fd_flags |= O_CLOEXEC;

	ret = anon_inode_getfd("spawn_template", &spawn_template_fops, tmpl,
			       fd_flags);
	if (ret < 0)
		goto out_put_exec;

	return ret;

out_put_exec:
	if (tmpl->deny_write)
		exe_file_allow_write_access(tmpl->exec_file);
	fput(tmpl->exec_file);
out_free_tmpl:
	put_cred(tmpl->creator_cred);
	kfree(tmpl->filename);
	kfree(tmpl);
	return ret;
}
