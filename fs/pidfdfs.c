// SPDX-License-Identifier: GPL-2.0
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/pid.h>
#include <linux/pidfdfs.h>
#include <linux/pid_namespace.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/pseudo_fs.h>
#include <linux/seq_file.h>
#include <uapi/linux/pidfd.h>

struct pid *pidfd_pid(const struct file *file)
{
	if (file->f_op != &pidfd_fops)
		return ERR_PTR(-EBADF);
#ifdef CONFIG_FS_PIDFD
	return file_inode(file)->i_private;
#else
	return file->private_data;
#endif
}

static int pidfd_release(struct inode *inode, struct file *file)
{
#ifndef CONFIG_FS_PIDFD
	struct pid *pid = file->private_data;

	file->private_data = NULL;
	put_pid(pid);
#endif
	return 0;
}

#ifdef CONFIG_PROC_FS
/**
 * pidfd_show_fdinfo - print information about a pidfd
 * @m: proc fdinfo file
 * @f: file referencing a pidfd
 *
 * Pid:
 * This function will print the pid that a given pidfd refers to in the
 * pid namespace of the procfs instance.
 * If the pid namespace of the process is not a descendant of the pid
 * namespace of the procfs instance 0 will be shown as its pid. This is
 * similar to calling getppid() on a process whose parent is outside of
 * its pid namespace.
 *
 * NSpid:
 * If pid namespaces are supported then this function will also print
 * the pid of a given pidfd refers to for all descendant pid namespaces
 * starting from the current pid namespace of the instance, i.e. the
 * Pid field and the first entry in the NSpid field will be identical.
 * If the pid namespace of the process is not a descendant of the pid
 * namespace of the procfs instance 0 will be shown as its first NSpid
 * entry and no others will be shown.
 * Note that this differs from the Pid and NSpid fields in
 * /proc/<pid>/status where Pid and NSpid are always shown relative to
 * the  pid namespace of the procfs instance. The difference becomes
 * obvious when sending around a pidfd between pid namespaces from a
 * different branch of the tree, i.e. where no ancestral relation is
 * present between the pid namespaces:
 * - create two new pid namespaces ns1 and ns2 in the initial pid
 *   namespace (also take care to create new mount namespaces in the
 *   new pid namespace and mount procfs)
 * - create a process with a pidfd in ns1
 * - send pidfd from ns1 to ns2
 * - read /proc/self/fdinfo/<pidfd> and observe that both Pid and NSpid
 *   have exactly one entry, which is 0
 */
static void pidfd_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct pid *pid = pidfd_pid(f);
	struct pid_namespace *ns;
	pid_t nr = -1;

	if (likely(pid_has_task(pid, PIDTYPE_PID))) {
		ns = proc_pid_ns(file_inode(m->file)->i_sb);
		nr = pid_nr_ns(pid, ns);
	}

	seq_put_decimal_ll(m, "Pid:\t", nr);

#ifdef CONFIG_PID_NS
	seq_put_decimal_ll(m, "\nNSpid:\t", nr);
	if (nr > 0) {
		int i;

		/* If nr is non-zero it means that 'pid' is valid and that
		 * ns, i.e. the pid namespace associated with the procfs
		 * instance, is in the pid namespace hierarchy of pid.
		 * Start at one below the already printed level.
		 */
		for (i = ns->level + 1; i <= pid->level; i++)
			seq_put_decimal_ll(m, "\t", pid->numbers[i].nr);
	}
#endif
	seq_putc(m, '\n');
}
#endif

/*
 * Poll support for process exit notification.
 */
static __poll_t pidfd_poll(struct file *file, struct poll_table_struct *pts)
{
	struct pid *pid = pidfd_pid(file);
	bool thread = file->f_flags & PIDFD_THREAD;
	struct task_struct *task;
	__poll_t poll_flags = 0;

	poll_wait(file, &pid->wait_pidfd, pts);
	/*
	 * Depending on PIDFD_THREAD, inform pollers when the thread
	 * or the whole thread-group exits.
	 */
	rcu_read_lock();
	task = pid_task(pid, PIDTYPE_PID);
	if (!task)
		poll_flags = EPOLLIN | EPOLLRDNORM | EPOLLHUP;
	else if (task->exit_state && (thread || thread_group_empty(task)))
		poll_flags = EPOLLIN | EPOLLRDNORM;
	rcu_read_unlock();

	return poll_flags;
}

const struct file_operations pidfd_fops = {
	.release	= pidfd_release,
	.poll		= pidfd_poll,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= pidfd_show_fdinfo,
#endif
};

#ifdef CONFIG_FS_PIDFD
static struct vfsmount *pidfdfs_mnt __ro_after_init;
static struct super_block *pidfdfs_sb __ro_after_init;
static u64 pidfdfs_ino = 0;

static void pidfdfs_evict_inode(struct inode *inode)
{
	struct pid *pid = inode->i_private;

	clear_inode(inode);
	put_pid(pid);
}

static const struct super_operations pidfdfs_sops = {
	.statfs		= simple_statfs,
	.evict_inode	= pidfdfs_evict_inode,
};

static void pidfdfs_prune_dentry(struct dentry *dentry)
{
	struct inode *inode;
	struct pid *pid;

	inode = d_inode(dentry);
	if (!inode)
		return;

	pid = inode->i_private;
	atomic_long_set(&pid->stashed, 0);
}

static char *pidfdfs_dname(struct dentry *dentry, char *buffer, int buflen)
{
	return dynamic_dname(buffer, buflen, "pidfd:[%lu]",
			     d_inode(dentry)->i_ino);
}

const struct dentry_operations pidfdfs_dentry_operations = {
	.d_prune	= pidfdfs_prune_dentry,
	.d_delete	= always_delete_dentry,
	.d_dname	= pidfdfs_dname,
};

static int pidfdfs_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx;

	ctx = init_pseudo(fc, PIDFDFS_MAGIC);
	if (!ctx)
		return -ENOMEM;

	ctx->ops = &pidfdfs_sops;
	ctx->dops = &pidfdfs_dentry_operations;
	return 0;
}

static struct file_system_type pidfdfs_type = {
	.name			= "pidfdfs",
	.init_fs_context	= pidfdfs_init_fs_context,
	.kill_sb		= kill_anon_super,
};

static struct dentry *pidfdfs_dentry(struct pid *pid)
{
	struct inode *inode;
	struct dentry *dentry;
	unsigned long i_ptr;

	inode = new_inode_pseudo(pidfdfs_sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_ino	= pid->ino;
	inode->i_mode	= S_IFREG | S_IRUGO;
	inode->i_fop	= &pidfd_fops;
	inode->i_flags |= S_IMMUTABLE;
	simple_inode_init_ts(inode);
	/* grab a reference */
	inode->i_private = get_pid(pid);

	/* consumes @inode */
	dentry = d_make_root(inode);
	if (!dentry)
		return ERR_PTR(-ENOMEM);

	i_ptr = atomic_long_cmpxchg(&pid->stashed, 0, (unsigned long)dentry);
	if (i_ptr) {
		d_delete(dentry); /* make sure ->d_prune() does nothing */
		dput(dentry);
		cpu_relax();
		return ERR_PTR(-EAGAIN);
	}

	return dentry;
}

struct file *pidfdfs_alloc_file(struct pid *pid, unsigned int flags)
{

	struct path path;
	struct dentry *dentry;
	struct file *pidfd_file;

	for (;;) {
		rcu_read_lock();
		dentry = (struct dentry *)atomic_long_read(&pid->stashed);
		if (!dentry || !lockref_get_not_dead(&dentry->d_lockref)) {
			rcu_read_unlock();

			dentry = pidfdfs_dentry(pid);
			if (!IS_ERR(dentry))
				break;
			if (PTR_ERR(dentry) == -EAGAIN)
				continue;
		}
		rcu_read_unlock();
		break;
	}
	if (IS_ERR(dentry))
		return ERR_CAST(dentry);

	path.mnt = mntget(pidfdfs_mnt);
	path.dentry = dentry;

	pidfd_file = dentry_open(&path, flags, current_cred());
	path_put(&path);

	return pidfd_file;
}

void pid_init_pidfdfs(struct pid *pid)
{
	atomic_long_set(&pid->stashed, 0);
	pid->ino = ++pidfdfs_ino;
}

void __init pidfdfs_init(void)
{
	int err;

	err = register_filesystem(&pidfdfs_type);
	if (err)
		panic("Failed to register pidfdfs pseudo filesystem");

	pidfdfs_mnt = kern_mount(&pidfdfs_type);
	if (IS_ERR(pidfdfs_mnt))
		panic("Failed to mount pidfdfs pseudo filesystem");

	pidfdfs_sb = pidfdfs_mnt->mnt_sb;
}

#else /* !CONFIG_FS_PIDFD */

struct file *pidfdfs_alloc_file(struct pid *pid, unsigned int flags)
{
	struct file *pidfd_file;

	pidfd_file = anon_inode_getfile("[pidfd]", &pidfd_fops, pid,
					flags | O_RDWR);
	if (IS_ERR(pidfd_file))
		return pidfd_file;

	get_pid(pid);
	return pidfd_file;
}

void pid_init_pidfdfs(struct pid *pid) { }
void __init pidfdfs_init(void) { }
#endif
