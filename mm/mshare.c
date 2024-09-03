// SPDX-License-Identifier: GPL-2.0-only
/*
 * Enable cooperating processes to share page table between
 * them to reduce the extra memory consumed by multiple copies
 * of page tables.
 *
 * This code adds an in-memory filesystem - msharefs.
 * msharefs is used to manage page table sharing
 *
 *
 * Copyright (C) 2024 Oracle Corp. All rights reserved.
 * Author:	Khalid Aziz <khalid@kernel.org>
 * Author:	Matthew Wilcox <willy@infradead.org>
 *
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/spinlock_types.h>
#include <uapi/linux/magic.h>
#include <uapi/linux/msharefs.h>

struct mshare_data {
	struct mm_struct *mm;
	spinlock_t m_lock;
	struct mshare_info minfo;
};

struct msharefs_info {
	struct dentry *info_dentry;
};

static const struct inode_operations msharefs_dir_inode_ops;
static const struct inode_operations msharefs_file_inode_ops;

/*
 * Disallow partial unmaps of an mshare region for now. Unmapping at
 * boundaries aligned to the level page tables are shared at could
 * be allowed in the future.
 */ 
static int mshare_vm_op_split(struct vm_area_struct *vma, unsigned long addr)
{
	return -EINVAL;
}

static const struct vm_operations_struct msharefs_vm_ops = {
	.may_split = mshare_vm_op_split,
};

/*
 * msharefs_mmap() - mmap an mshare region
 */
static int
msharefs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mshare_data *m_data = file->private_data;
	unsigned long mshare_start, mshare_end;
	int err = -EINVAL;

	spin_lock(&m_data->m_lock);
	mshare_start = m_data->minfo.start;
	mshare_end = mshare_start + m_data->minfo.size;
	spin_unlock(&m_data->m_lock);

	/*
	 * Make sure start and end of this mshare region has
	 * been established already
	 */
	if (mshare_start == 0)
		goto err_out;

	/*
	 * Verify alignment and size multiple
	 */
	if ((vma->vm_start | vma->vm_end) & (PGDIR_SIZE - 1))
		goto err_out;

	/*
	 * Verify this mapping does not extend outside of mshare region
	 */
	if (vma->vm_start < mshare_start || vma->vm_end > mshare_end)
		goto err_out;

	err = 0;
	vma->vm_private_data = m_data;
	vm_flags_set(vma, VM_SHARED_PT);
	vma->vm_ops = &msharefs_vm_ops;

err_out:
	return err;
}

static long
msharefs_set_size(struct mm_struct *mm, struct mshare_data *m_data,
			struct mshare_info *minfo)
{
	unsigned long end = minfo->start + minfo->size;

	/*
	 * Validate alignment for start address, and size
	 */
	if ((minfo->start | end) & (PGDIR_SIZE - 1)) {
		spin_unlock(&m_data->m_lock);
		return -EINVAL;
	}

	mm->mmap_base = minfo->start;
	mm->task_size = minfo->size;
	if (!mm->task_size)
		mm->task_size--;

	m_data->minfo.start = mm->mmap_base;
	m_data->minfo.size = mm->task_size;
	spin_unlock(&m_data->m_lock);

	return 0;
}

static long
msharefs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mshare_data *m_data = filp->private_data;
	struct mm_struct *mm = m_data->mm;
	struct mshare_info minfo;

	switch (cmd) {
	case MSHAREFS_GET_SIZE:
		spin_lock(&m_data->m_lock);
		minfo = m_data->minfo;
		spin_unlock(&m_data->m_lock);

		if (copy_to_user((void __user *)arg, &minfo, sizeof(minfo)))
			return -EFAULT;

		return 0;

	case MSHAREFS_SET_SIZE:
		if (copy_from_user(&minfo, (struct mshare_info __user *)arg,
			sizeof(minfo)))
			return -EFAULT;

		/*
		 * If this mshare region has been set up once already, bail out
		 */
		spin_lock(&m_data->m_lock);
		if (m_data->minfo.start != 0) {
			spin_unlock(&m_data->m_lock);
			return -EINVAL;
		}

		return msharefs_set_size(mm, m_data, &minfo);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations msharefs_file_operations = {
	.open		= simple_open,
	.mmap		= msharefs_mmap,
	.unlocked_ioctl	= msharefs_ioctl,
	.llseek		= no_llseek,
};

static int
msharefs_fill_mm(struct inode *inode)
{
	struct mm_struct *mm;
	struct mshare_data *m_data = NULL;
	int ret = 0;

	mm = mm_alloc();
	if (!mm) {
		ret = -ENOMEM;
		goto err_free;
	}

	mm->mmap_base = mm->task_size = 0;

	m_data = kzalloc(sizeof(*m_data), GFP_KERNEL);
	if (!m_data) {
		ret = -ENOMEM;
		goto err_free;
	}
	m_data->mm = mm;
	spin_lock_init(&m_data->m_lock);
	inode->i_private = m_data;

	return 0;

err_free:
	if (mm)
		mmput(mm);
	kfree(m_data);
	return ret;
}

static void
msharefs_delmm(struct mshare_data *m_data)
{
	mmput(m_data->mm);
	kfree(m_data);
}

static struct inode
*msharefs_get_inode(struct mnt_idmap *idmap, struct super_block *sb,
			const struct inode *dir, umode_t mode)
{
	struct inode *inode = new_inode(sb);
	int ret;

	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	simple_inode_init_ts(inode);

	switch (mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &msharefs_file_inode_ops;
		inode->i_fop = &msharefs_file_operations;
		ret = msharefs_fill_mm(inode);
		if (ret) {
			discard_new_inode(inode);
			inode = ERR_PTR(ret);
		}
		break;
	case S_IFDIR:
		inode->i_op = &msharefs_dir_inode_ops;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		break;
	default:
		discard_new_inode(inode);
		return ERR_PTR(-EINVAL);
	}

	return inode;
}

static int
msharefs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, umode_t mode)
{
	struct inode *inode;

	inode = msharefs_get_inode(idmap, dir->i_sb, dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	dget(dentry);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

	return 0;
}

static int
msharefs_create(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, umode_t mode, bool excl)
{
	return msharefs_mknod(idmap, dir, dentry, mode | S_IFREG);
}

static int
msharefs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, umode_t mode)
{
	int ret = msharefs_mknod(idmap, dir, dentry, mode | S_IFDIR);

	if (!ret)
		inc_nlink(dir);
	return ret;
}

static inline bool
is_msharefs_info_file(const struct dentry *dentry)
{
	struct msharefs_info *info = dentry->d_sb->s_fs_info;

	return info->info_dentry == dentry;
}

static int
msharefs_rename(struct mnt_idmap *idmap,
		struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry,
		unsigned int flags)
{
	if (is_msharefs_info_file(old_dentry) ||
	    is_msharefs_info_file(new_dentry))
		return -EPERM;

	return simple_rename(idmap, old_dir, old_dentry, new_dir,
			     new_dentry, flags);
}

static int
msharefs_unlink(struct inode *dir, struct dentry *dentry)
{
	if (is_msharefs_info_file(dentry))
		return -EPERM;

	return simple_unlink(dir, dentry);
}

static const struct inode_operations msharefs_file_inode_ops = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};

static const struct inode_operations msharefs_dir_inode_ops = {
	.create		= msharefs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= msharefs_unlink,
	.mkdir		= msharefs_mkdir,
	.rmdir		= simple_rmdir,
	.rename		= msharefs_rename,
};

static void
mshare_evict_inode(struct inode *inode)
{
	struct mshare_data *m_data = inode->i_private;

	if (m_data)
		msharefs_delmm(m_data);
	clear_inode(inode);
}

static ssize_t
mshare_info_read(struct file *file, char __user *buf, size_t nbytes,
		loff_t *ppos)
{
	char s[80];

	sprintf(s, "%ld\n", PGDIR_SIZE);
	return simple_read_from_buffer(buf, nbytes, ppos, s, strlen(s));
}

static const struct file_operations mshare_info_ops = {
	.read	= mshare_info_read,
	.llseek	= noop_llseek,
};

static const struct super_operations mshare_s_ops = {
	.statfs		= simple_statfs,
	.evict_inode	= mshare_evict_inode,
};

static int
msharefs_create_mshare_info(struct super_block *sb)
{
	struct msharefs_info *info = sb->s_fs_info;
	struct dentry *root = sb->s_root;
	struct dentry *dentry;
	struct inode *inode;
	int ret;

	ret = -ENOMEM;
	inode = new_inode(sb);
	if (!inode)
		goto out;

	inode->i_ino = 2;
	simple_inode_init_ts(inode);
	inode_init_owner(&nop_mnt_idmap, inode, NULL, S_IFREG | 0444);
	inode->i_fop = &mshare_info_ops;

	dentry = d_alloc_name(root, "mshare_info");
	if (!dentry)
		goto out;

	info->info_dentry = dentry;
	d_add(dentry, inode);

	return 0;
out:
	iput(inode);

	return ret;
}

static int
msharefs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct msharefs_info *info;
	struct inode *inode;
	int ret;

	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= MSHARE_MAGIC;
	sb->s_op		= &mshare_s_ops;
	sb->s_time_gran		= 1;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	sb->s_fs_info = info;

	inode = new_inode(sb);
	if (!inode)
		return -ENOMEM;

	inode->i_ino = 1;
	inode->i_mode = S_IFDIR | 0777;
	simple_inode_init_ts(inode);
	inode->i_op = &msharefs_dir_inode_ops;
	inode->i_fop = &simple_dir_operations;
	set_nlink(inode, 2);

	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	ret = msharefs_create_mshare_info(sb);

	return ret;
}

static int
msharefs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, msharefs_fill_super);
}

static const struct fs_context_operations msharefs_context_ops = {
	.get_tree	= msharefs_get_tree,
};

static int
mshare_init_fs_context(struct fs_context *fc)
{
	fc->ops = &msharefs_context_ops;
	return 0;
}

static void
msharefs_kill_super(struct super_block *sb)
{
	struct msharefs_info *info = sb->s_fs_info;

	kfree(info);
	kill_litter_super(sb);
}

static struct file_system_type mshare_fs = {
	.name			= "msharefs",
	.init_fs_context	= mshare_init_fs_context,
	.kill_sb		= msharefs_kill_super,
};

static int __init
mshare_init(void)
{
	int ret;

	ret = sysfs_create_mount_point(fs_kobj, "mshare");
	if (ret)
		return ret;

	ret = register_filesystem(&mshare_fs);
	if (ret)
		sysfs_remove_mount_point(fs_kobj, "mshare");

	return ret;
}

core_initcall(mshare_init);
