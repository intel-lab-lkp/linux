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
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <uapi/linux/magic.h>
#include <uapi/linux/msharefs.h>
#include <linux/falloc.h>
#include <asm/tlbflush.h>

const unsigned long mshare_align = P4D_SIZE;
const unsigned long mshare_base = mshare_align;

#define MSHARE_INITIALIZED	0x1

struct mshare_data {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long size;
	unsigned long flags;
	struct mmu_notifier mn;
};

static void mshare_invalidate_tlbs(struct mmu_notifier *mn, struct mm_struct *mm,
				   unsigned long start, unsigned long end)
{
	flush_tlb_all();
}

static const struct mmu_notifier_ops mshare_mmu_ops = {
	.arch_invalidate_secondary_tlbs = mshare_invalidate_tlbs,
};

static p4d_t *walk_to_p4d(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;

	pgd = pgd_offset(mm, addr);
	p4d = p4d_alloc(mm, pgd, addr);
	if (!p4d)
		return NULL;

	return p4d;
}

/* Returns holding the host mm's lock for read.  Caller must release. */
vm_fault_t
find_shared_vma(struct vm_area_struct **vmap, unsigned long *addrp)
{
	struct vm_area_struct *vma, *guest = *vmap;
	struct mshare_data *m_data = guest->vm_private_data;
	struct mm_struct *host_mm = m_data->mm;
	unsigned long host_addr;
	p4d_t *p4d, *guest_p4d;

	mmap_read_lock_nested(host_mm, SINGLE_DEPTH_NESTING);
	host_addr = *addrp - guest->vm_start + host_mm->mmap_base;
	p4d = walk_to_p4d(host_mm, host_addr);
	guest_p4d = walk_to_p4d(guest->vm_mm, *addrp);
	if (!p4d_same(*guest_p4d, *p4d)) {
		set_p4d(guest_p4d, *p4d);
		mmap_read_unlock(host_mm);
		return VM_FAULT_NOPAGE;
	}

	*addrp = host_addr;
	vma = find_vma(host_mm, host_addr);

	/* XXX: expand stack? */
	if (vma && vma->vm_start > host_addr)
		vma = NULL;

	*vmap = vma;

	/*
	 * release host mm lock unless a matching vma is found
	 */
	if (!vma)
		mmap_read_unlock(host_mm);
	return 0;
}

static int mshare_vm_op_split(struct vm_area_struct *vma, unsigned long addr)
{
	return -EINVAL;
}

static int mshare_vm_op_mprotect(struct vm_area_struct *vma, unsigned long start,
				 unsigned long end, unsigned long newflags)
{
	return -EINVAL;
}

static void mshare_vm_op_unmap_page_range(struct mmu_gather *tlb,
				struct vm_area_struct *vma,
				unsigned long addr, unsigned long end,
				struct zap_details *details)
{
	/*
	 * The msharefs vma is being unmapped. Do not unmap pages in the
	 * mshare region itself.
	 */
}

static const struct vm_operations_struct msharefs_vm_ops = {
	.may_split = mshare_vm_op_split,
	.mprotect = mshare_vm_op_mprotect,
	.unmap_page_range = mshare_vm_op_unmap_page_range,
};

/*
 * msharefs_mmap() - mmap an mshare region
 */
static int
msharefs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mshare_data *m_data = file->private_data;

	vma->vm_private_data = m_data;
	vm_flags_set(vma, VM_MSHARE | VM_DONTEXPAND);
	vma->vm_ops = &msharefs_vm_ops;

	return 0;
}

static unsigned long
msharefs_get_unmapped_area_bottomup(struct file *file, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct vm_unmapped_area_info info = {};

	info.length = len;
	info.low_limit = current->mm->mmap_base;
	info.high_limit = arch_get_mmap_end(addr, len, flags);
	info.align_mask = PAGE_MASK & (mshare_align - 1);
	return vm_unmapped_area(&info);
}

static unsigned long
msharefs_get_unmapped_area_topdown(struct file *file, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct vm_unmapped_area_info info = {};

	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	info.low_limit = PAGE_SIZE;
	info.high_limit = arch_get_mmap_base(addr, current->mm->mmap_base);
	info.align_mask = PAGE_MASK & (mshare_align - 1);
	addr = vm_unmapped_area(&info);

	/*
	 * A failed mmap() very likely causes application failure,
	 * so fall back to the bottom-up function here. This scenario
	 * can happen with large stack limits and large mmap()
	 * allocations.
	 */
	if (unlikely(offset_in_page(addr))) {
		VM_BUG_ON(addr != -ENOMEM);
		info.flags = 0;
		info.low_limit = current->mm->mmap_base;
		info.high_limit = arch_get_mmap_end(addr, len, flags);
		addr = vm_unmapped_area(&info);
	}

	return addr;
}

static unsigned long
msharefs_get_unmapped_area(struct file *file, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct mshare_data *m_data = file->private_data;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma, *prev;
	unsigned long mshare_start, mshare_size;
	const unsigned long mmap_end = arch_get_mmap_end(addr, len, flags);

	mmap_assert_write_locked(mm);

	if ((flags & MAP_TYPE) == MAP_PRIVATE)
		return -EINVAL;

	if (!test_bit(MSHARE_INITIALIZED, &m_data->flags))
		return -EINVAL;

	mshare_start = m_data->start;
	mshare_size = m_data->size;

	if (len != mshare_size)
		return -EINVAL;

	if (len > mmap_end - mmap_min_addr)
		return -ENOMEM;

	if (flags & MAP_FIXED) {
		if (!IS_ALIGNED(addr, mshare_align))
			return -EINVAL;
		return addr;
	}

	if (addr) {
		addr = ALIGN(addr, mshare_align);
		vma = find_vma_prev(mm, addr, &prev);
		if (mmap_end - len >= addr && addr >= mmap_min_addr &&
		    (!vma || addr + len <= vm_start_gap(vma)) &&
		    (!prev || addr >= vm_end_gap(prev)))
			return addr;
	}

	if (!test_bit(MMF_TOPDOWN, &mm->flags))
		return msharefs_get_unmapped_area_bottomup(file, addr, len,
				pgoff, flags);
	else
		return msharefs_get_unmapped_area_topdown(file, addr, len,
				pgoff, flags);
}

static long
msharefs_create_mapping(struct mshare_data *m_data, struct mshare_create *mcreate)
{
	struct mm_struct *host_mm = m_data->mm;
	unsigned long mshare_start, mshare_end;
	unsigned long region_offset = mcreate->region_offset;
	unsigned long size = mcreate->size;
	unsigned int fd = mcreate->fd;
	int flags = mcreate->flags;
	int prot = mcreate->prot;
	unsigned long populate = 0;
	unsigned long mapped_addr;
	unsigned long addr;
	vm_flags_t vm_flags;
	int error = -EINVAL;

	mshare_start = m_data->start;
	mshare_end = mshare_start + m_data->size;
	addr = mshare_start + region_offset;

	if ((addr < mshare_start) || (addr >= mshare_end) ||
	    (addr + size > mshare_end))
		goto out;

	/*
	 * Only anonymous shared memory at fixed addresses is allowed for now.
	 */
	if ((flags & (MAP_SHARED | MAP_FIXED)) != (MAP_SHARED | MAP_FIXED))
		goto out;
	if (fd != -1)
		goto out;

	if (mmap_write_lock_killable(host_mm)) {
		error = -EINTR;
		goto out;
	}

	error = 0;
	mapped_addr = __do_mmap(NULL, addr, size, prot, flags, vm_flags,
				0, &populate, NULL, host_mm);

	if (IS_ERR_VALUE(mapped_addr))
		error = (long)mapped_addr;

	mmap_write_unlock(host_mm);
out:
	return error;
}

static long
msharefs_unmap(struct mshare_data *m_data, struct mshare_unmap *munmap)
{
	struct mm_struct *host_mm = m_data->mm;
	unsigned long mshare_start, mshare_end, mshare_size;
	unsigned long region_offset = munmap->region_offset;
	unsigned long size = munmap->size;
	unsigned long addr;
	int error;

	mshare_start = m_data->start;
	mshare_size = m_data->size;
	mshare_end = mshare_start + mshare_size;
	addr = mshare_start + region_offset;

	if ((size > mshare_size) || (region_offset >= mshare_size) ||
	    (addr + size > mshare_end))
		return -EINVAL;

	if (mmap_write_lock_killable(host_mm))
		return -EINTR;

	error = do_munmap(host_mm, addr, size, NULL);

	mmap_write_unlock(host_mm);

	return error;
}

static long
msharefs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mshare_data *m_data = filp->private_data;
	struct mshare_create mcreate;
	struct mshare_unmap munmap;

	switch (cmd) {
	case MSHAREFS_CREATE_MAPPING:
		if (copy_from_user(&mcreate, (struct mshare_create __user *)arg,
			sizeof(mcreate)))
			return -EFAULT;

		if (!test_bit(MSHARE_INITIALIZED, &m_data->flags))
			return -EINVAL;

		return msharefs_create_mapping(m_data, &mcreate);

	case MSHAREFS_UNMAP:
		if (copy_from_user(&munmap, (struct mshare_unmap __user *)arg,
			sizeof(munmap)))
			return -EFAULT;

		if (!test_bit(MSHARE_INITIALIZED, &m_data->flags))
			return -EINVAL;

		return msharefs_unmap(m_data, &munmap);

	default:
		return -ENOTTY;
	}
}

static int msharefs_set_size(struct mshare_data *m_data, unsigned long size)
{
	int error = -EINVAL;

	if (test_bit(MSHARE_INITIALIZED, &m_data->flags))
		goto out;

	if (m_data->size || (size & (mshare_align - 1)))
		goto out;

	m_data->mm->task_size = m_data->size = size;

	set_bit(MSHARE_INITIALIZED, &m_data->flags);
	error = 0;
out:
	return error;
}

static long msharefs_fallocate(struct file *file, int mode, loff_t offset,
				loff_t len)
{
	struct inode *inode = file_inode(file);
	struct mshare_data *m_data = inode->i_private;
	int error;

	if (mode != FALLOC_FL_ALLOCATE_RANGE)
		return -EOPNOTSUPP;

	if (offset)
		return -EINVAL;

	inode_lock(inode);

	error = inode_newsize_ok(inode, len);
	if (error)
		goto out;

	error = msharefs_set_size(m_data, len);
	if (error)
		goto out;

	i_size_write(inode, len);
out:
	inode_unlock(inode);

	return error;
}

static const struct inode_operations msharefs_dir_inode_ops;
static const struct inode_operations msharefs_file_inode_ops;

static const struct file_operations msharefs_file_operations = {
	.open			= simple_open,
	.mmap			= msharefs_mmap,
	.get_unmapped_area	= msharefs_get_unmapped_area,
	.unlocked_ioctl		= msharefs_ioctl,
	.fallocate		= msharefs_fallocate,
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

	set_bit(MMF_MSHARE, &mm->flags);
	mm->mmap_base = mshare_base;
	mm->task_size = 0;

	m_data = kzalloc(sizeof(*m_data), GFP_KERNEL);
	if (!m_data) {
		ret = -ENOMEM;
		goto err_free;
	}
	m_data->mm = mm;
	m_data->start = mshare_base;
	inode->i_private = m_data;
	m_data->mn.ops = &mshare_mmu_ops;
	ret = mmu_notifier_register(&m_data->mn, mm);
	if (ret)
		goto err_free;

#ifdef CONFIG_MEMCG
	mm->owner = NULL;
#endif
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

static int msharefs_setattr(struct mnt_idmap *idmap,
			    struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct mshare_data *m_data = inode->i_private;
	unsigned int ia_valid = attr->ia_valid;
	int error;

	error = setattr_prepare(idmap, dentry, attr);
	if (error)
		return error;

	if (ia_valid & ATTR_SIZE) {
		loff_t newsize = attr->ia_size;

		error = msharefs_set_size(m_data, newsize);
		if (error)
			return error;

		i_size_write(inode, newsize);
	}

	setattr_copy(idmap, inode, attr);
	return 0;
}

static int
msharefs_create(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, umode_t mode, bool excl)
{
	return msharefs_mknod(idmap, dir, dentry, mode | S_IFREG);
}

static struct dentry *
msharefs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, umode_t mode)
{
	int ret = msharefs_mknod(idmap, dir, dentry, mode | S_IFDIR);

	if (!ret)
		inc_nlink(dir);
	return ERR_PTR(ret);
}

struct msharefs_info {
	struct dentry *info_dentry;
};

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
	.setattr	= msharefs_setattr,
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

	sprintf(s, "%ld\n", mshare_align);
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
	sb->s_maxbytes		= MAX_LFS_FILESIZE;
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
