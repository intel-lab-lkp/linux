// SPDX-License-Identifier: GPL-2.0-only
#include <linux/anon_inodes.h>
#include <linux/binfmts.h>
#include <linux/close_range.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/kernel.h>
#include <linux/namei.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/spawn_template.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <uapi/linux/openat2.h>
#include <uapi/linux/spawn_template.h>

#include "internal.h"

#define SPAWN_TEMPLATE_MAX_ACTIONS	256

struct spawn_template {
	struct file *exec_file;
	struct spawn_template_file_key exec_key;
	const struct cred *creator_cred;
	char *filename;
	bool deny_write;
};

struct spawn_template_spawn_context {
	struct spawn_template *tmpl;
	struct spawn_template_spawn_args args;
	struct spawn_template_action *actions;
};

static const struct file_operations spawn_template_fops;

static bool spawn_template_file_exec_allowed(struct file *file);

void spawn_template_fill_file_key(struct file *file,
				  struct spawn_template_file_key *key)
{
	struct inode *inode = file_inode(file);
	struct timespec64 ctime = inode_get_ctime(inode);
	struct timespec64 mtime = inode_get_mtime(inode);

	key->dev = inode->i_sb->s_dev;
	key->ino = inode->i_ino;
	key->size = i_size_read(inode);
	key->mode = READ_ONCE(inode->i_mode);
	key->uid = inode->i_uid;
	key->gid = inode->i_gid;
	key->ctime_sec = ctime.tv_sec;
	key->ctime_nsec = ctime.tv_nsec;
	key->mtime_sec = mtime.tv_sec;
	key->mtime_nsec = mtime.tv_nsec;
}

bool spawn_template_file_key_matches(struct file *file,
				     const struct spawn_template_file_key *key)
{
	struct spawn_template_file_key cur;

	spawn_template_fill_file_key(file, &cur);

	return cur.dev == key->dev &&
	       cur.ino == key->ino &&
	       cur.size == key->size &&
	       cur.mode == key->mode &&
	       uid_eq(cur.uid, key->uid) &&
	       gid_eq(cur.gid, key->gid) &&
	       cur.ctime_sec == key->ctime_sec &&
	       cur.ctime_nsec == key->ctime_nsec &&
	       cur.mtime_sec == key->mtime_sec &&
	       cur.mtime_nsec == key->mtime_nsec;
}

static int spawn_template_exit_status(int err)
{
	switch (err) {
	case -ENOENT:
		return 127;
	case -EACCES:
	case -ENOEXEC:
		return 126;
	default:
		return 1;
	}
}

static bool spawn_template_cred_matches(struct spawn_template *tmpl)
{
	return current_cred() == tmpl->creator_cred;
}

static bool spawn_template_key_matches(struct spawn_template *tmpl)
{
	bool matches;

	if (tmpl->filename) {
		struct file *file __free(fput) = NULL;
		struct file *tmp;

		tmp = open_exec(tmpl->filename);
		if (IS_ERR(tmp))
			return false;
		file = tmp;

		matches = spawn_template_file_key_matches(file,
							  &tmpl->exec_key);
		matches = matches && spawn_template_file_exec_allowed(file);
		exe_file_allow_write_access(file);
		if (!matches)
			return false;
	}

	return spawn_template_file_exec_allowed(tmpl->exec_file) &&
	       spawn_template_file_key_matches(tmpl->exec_file,
					       &tmpl->exec_key);
}

static int spawn_template_copy_signal_set(const struct spawn_template_action *action,
					  sigset_t *mask)
{
	struct spawn_template_sigset sigset;

	if (!action->arg)
		return -EINVAL;
	if (copy_from_user(&sigset, u64_to_user_ptr(action->arg),
			   sizeof(sigset)))
		return -EFAULT;
	if (sigset.sigsetsize != sizeof(sigset_t))
		return -EINVAL;
	if (copy_from_user(mask, u64_to_user_ptr(sigset.sigset), sizeof(*mask)))
		return -EFAULT;
	sigdelsetmask(mask, sigmask(SIGKILL) | sigmask(SIGSTOP));

	return 0;
}

static int spawn_template_apply_open(const struct spawn_template_action *action)
{
	struct spawn_template_open open;
	struct file *file __free(fput) = NULL;
	struct file *tmp;
	struct open_flags op;
	int ret;

	if (action->fd < AT_FDCWD || action->newfd < 0 || action->flags ||
	    !action->arg)
		return -EINVAL;

	if (copy_from_user(&open, u64_to_user_ptr(action->arg), sizeof(open)))
		return -EFAULT;

	ret = build_open_flags(&open.how, &op);
	if (ret)
		return ret;

	CLASS(filename_flags, name)(u64_to_user_ptr(open.path), op.lookup_flags);
	tmp = do_file_open(action->fd, name, &op);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);
	file = tmp;

	return replace_fd(action->newfd, file, open.how.flags & O_CLOEXEC);
}

static int spawn_template_apply_sigmask(const struct spawn_template_action *action)
{
	sigset_t mask;
	int ret;

	if (action->fd || action->newfd || action->flags)
		return -EINVAL;

	ret = spawn_template_copy_signal_set(action, &mask);
	if (ret)
		return ret;

	set_current_blocked(&mask);
	return 0;
}

static int spawn_template_apply_sigdefault(const struct spawn_template_action *action)
{
	sigset_t mask;
	struct k_sigaction sa = {};
	int ret;
	int sig;

	if (action->fd || action->newfd || action->flags)
		return -EINVAL;

	ret = spawn_template_copy_signal_set(action, &mask);
	if (ret)
		return ret;

	sa.sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa.sa_mask);

	for (sig = 1; sig < _NSIG; sig++) {
		if (!sigismember(&mask, sig))
			continue;
		ret = do_sigaction(sig, &sa, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static int spawn_template_apply_action(const struct spawn_template_action *action)
{
	switch (action->type) {
	case SPAWN_TEMPLATE_ACTION_CLOSE:
		return close_fd(action->fd);
	case SPAWN_TEMPLATE_ACTION_DUP2:
		if (action->fd == action->newfd) {
			if (action->flags)
				return -EINVAL;
			CLASS(fd, f)(action->fd);

			if (fd_empty(f))
				return -EBADF;
			return 0;
		}
		return ksys_dup3(action->fd, action->newfd, action->flags);
	case SPAWN_TEMPLATE_ACTION_FCHDIR: {
		CLASS(fd, f)(action->fd);
		int ret;

		if (fd_empty(f))
			return -EBADF;
		if (!d_can_lookup(fd_file(f)->f_path.dentry))
			return -ENOTDIR;

		ret = file_permission(fd_file(f), MAY_EXEC | MAY_CHDIR);
		if (!ret)
			set_fs_pwd(current->fs, &fd_file(f)->f_path);
		return ret;
	}
	case SPAWN_TEMPLATE_ACTION_OPEN:
		return spawn_template_apply_open(action);
	case SPAWN_TEMPLATE_ACTION_CLOSE_RANGE:
		return do_close_range(action->fd, action->newfd, action->flags);
	case SPAWN_TEMPLATE_ACTION_SIGMASK:
		return spawn_template_apply_sigmask(action);
	case SPAWN_TEMPLATE_ACTION_SIGDEFAULT:
		return spawn_template_apply_sigdefault(action);
	default:
		return -EINVAL;
	}
}

static int spawn_template_copy_actions(struct spawn_template_action **out_actions,
				       u64 count, u64 uaddr)
{
	struct spawn_template_action __user *uactions;
	struct spawn_template_action *actions __free(kfree) = NULL;
	struct spawn_template_action *tmp;
	u64 i;

	*out_actions = NULL;
	if (!count)
		return 0;
	if (count > SPAWN_TEMPLATE_MAX_ACTIONS)
		return -E2BIG;
	if (!uaddr)
		return -EINVAL;

	uactions = u64_to_user_ptr(uaddr);
	tmp = memdup_array_user(uactions, count, sizeof(*actions));
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);
	actions = tmp;

	for (i = 0; i < count; i++) {
		switch (actions[i].type) {
		case SPAWN_TEMPLATE_ACTION_CLOSE:
			if (actions[i].fd < 0 || actions[i].flags ||
			    actions[i].newfd || actions[i].arg)
				return -EINVAL;
			break;
		case SPAWN_TEMPLATE_ACTION_DUP2:
			if (actions[i].fd < 0 || actions[i].newfd < 0 ||
			    (actions[i].flags & ~O_CLOEXEC) || actions[i].arg)
				return -EINVAL;
			break;
		case SPAWN_TEMPLATE_ACTION_FCHDIR:
			if (actions[i].fd < 0 || actions[i].flags ||
			    actions[i].newfd || actions[i].arg)
				return -EINVAL;
			break;
		case SPAWN_TEMPLATE_ACTION_OPEN:
			if (actions[i].fd < AT_FDCWD || actions[i].newfd < 0 ||
			    actions[i].flags || !actions[i].arg)
				return -EINVAL;
			break;
		case SPAWN_TEMPLATE_ACTION_CLOSE_RANGE:
			if (actions[i].fd < 0 || actions[i].newfd < 0 ||
			    actions[i].fd > actions[i].newfd ||
			    (actions[i].flags &
			     ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) ||
			    actions[i].arg)
				return -EINVAL;
			break;
		case SPAWN_TEMPLATE_ACTION_SIGMASK:
		case SPAWN_TEMPLATE_ACTION_SIGDEFAULT:
			if (actions[i].fd || actions[i].newfd ||
			    actions[i].flags || !actions[i].arg)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
	}

	*out_actions = no_free_ptr(actions);
	return 0;
}

static int spawn_template_child(void *data)
{
	struct spawn_template_spawn_context *ctx = data;
	struct spawn_template *tmpl = ctx->tmpl;
	int ret;
	u64 i;

	for (i = 0; i < ctx->args.actions_len; i++) {
		ret = spawn_template_apply_action(&ctx->actions[i]);
		if (ret < 0)
			goto out_exec_error;
	}

	if (!(ctx->args.flags & SPAWN_TEMPLATE_SPAWN_INHERIT_FDS)) {
		ret = do_close_range(3, ~0U, 0);
		if (ret < 0)
			goto out_exec_error;
	}

	ret = kernel_execveat_file(tmpl->exec_file, "",
				   u64_to_user_ptr(ctx->args.argv),
				   u64_to_user_ptr(ctx->args.envp),
				   AT_EMPTY_PATH);
out_exec_error:
	if (ret < 0)
		do_exit(spawn_template_exit_status(ret));
	return 0;
}

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

static struct file *spawn_template_file_from_fd(int fd)
{
	CLASS(fd, f)(fd);

	if (fd_empty(f))
		return ERR_PTR(-EBADF);
	if (fd_file(f)->f_op != &spawn_template_fops)
		return ERR_PTR(-EINVAL);

	return get_file(fd_file(f));
}

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
	spawn_template_fill_file_key(tmpl->exec_file, &tmpl->exec_key);

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

SYSCALL_DEFINE3(spawn_template_spawn, int, template_fd,
		struct spawn_template_spawn_args __user *, uargs,
		size_t, usize)
{
	struct spawn_template_spawn_context *ctx;
	struct kernel_clone_args kargs;
	struct file *template_file;
	int ret;

	BUILD_BUG_ON(sizeof(struct spawn_template_spawn_args) !=
		     SPAWN_TEMPLATE_SPAWN_ARGS_SIZE_VER0);

	if (usize < SPAWN_TEMPLATE_SPAWN_ARGS_SIZE_VER0)
		return -EINVAL;
	if (usize > PAGE_SIZE)
		return -E2BIG;

	template_file = spawn_template_file_from_fd(template_fd);
	if (IS_ERR(template_file))
		return PTR_ERR(template_file);

	if (!spawn_template_cred_matches(template_file->private_data)) {
		ret = -EACCES;
		goto out_put_template;
	}

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto out_put_template;
	}

	ctx->tmpl = template_file->private_data;

	ret = copy_struct_from_user(&ctx->args, sizeof(ctx->args), uargs,
				    usize);
	if (ret)
		goto out_free_ctx;

	if ((ctx->args.flags & ~SPAWN_TEMPLATE_SPAWN_INHERIT_FDS) ||
	    !ctx->args.pidfd || ctx->args.reserved[0] ||
	    ctx->args.reserved[1] || ctx->args.reserved[2] ||
	    ctx->args.reserved[3]) {
		ret = -EINVAL;
		goto out_free_ctx;
	}

	ret = spawn_template_copy_actions(&ctx->actions, ctx->args.actions_len,
					  ctx->args.actions);
	if (ret)
		goto out_free_ctx;

	if (!spawn_template_key_matches(ctx->tmpl)) {
		ret = -ESTALE;
		goto out_free_actions;
	}

	kargs = (struct kernel_clone_args) {
		.flags		= CLONE_VM | CLONE_VFORK | CLONE_PIDFD,
		.pidfd		= u64_to_user_ptr(ctx->args.pidfd),
		.exit_signal	= SIGCHLD,
		.fn		= spawn_template_child,
		.fn_arg		= ctx,
	};

	ret = kernel_clone(&kargs);

out_free_actions:
	kfree(ctx->actions);
out_free_ctx:
	kfree(ctx);
out_put_template:
	fput(template_file);
	return ret;
}
