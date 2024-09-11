// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corporation, 2021
 *
 * Author: Mike Rapoport <rppt@linux.ibm.com>
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/swap.h>
#include <linux/mount.h>
#include <linux/memfd.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <linux/pagemap.h>
#include <linux/hugetlb.h>
#include <linux/syscalls.h>
#include <linux/pseudo_fs.h>
#include <linux/secretmem.h>
#include <linux/set_memory.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>

#include <uapi/asm-generic/mman-common.h>
#include <uapi/linux/magic.h>
#include <uapi/linux/mman.h>

#include <asm/tlbflush.h>

#include "internal.h"

#undef pr_fmt
#define pr_fmt(fmt) "secretmem: " fmt

/*
 * Define mode and flag masks to allow validation of the system call
 * parameters.
 */
#define SECRETMEM_MODE_MASK	(0x0)
#define SECRETMEM_FLAGS_MASK	SECRETMEM_MODE_MASK

static bool secretmem_enable __ro_after_init = 1;
module_param_named(enable, secretmem_enable, bool, 0400);
MODULE_PARM_DESC(secretmem_enable,
		 "Enable secretmem and memfd_secret(2) system call");

static atomic_t secretmem_users;

/* secretmem file private context */
struct secretmem_ctx {
	struct secretmem_area _area;
	struct page **_pages;
	unsigned long _nr_pages;
	struct file *_file;
	struct mm_struct *_mm;
};


bool secretmem_active(void)
{
	return !!atomic_read(&secretmem_users);
}

static vm_fault_t secretmem_fault(struct vm_fault *vmf)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	pgoff_t offset = vmf->pgoff;
	gfp_t gfp = vmf->gfp_mask;
	unsigned long addr;
	struct page *page;
	struct folio *folio;
	vm_fault_t ret;
	int err;

	if (((loff_t)vmf->pgoff << PAGE_SHIFT) >= i_size_read(inode))
		return vmf_error(-EINVAL);

	filemap_invalidate_lock_shared(mapping);

retry:
	page = find_lock_page(mapping, offset);
	if (!page) {
		folio = folio_alloc(gfp | __GFP_ZERO, 0);
		if (!folio) {
			ret = VM_FAULT_OOM;
			goto out;
		}

		page = &folio->page;
		err = set_direct_map_invalid_noflush(page);
		if (err) {
			folio_put(folio);
			ret = vmf_error(err);
			goto out;
		}

		__folio_mark_uptodate(folio);
		err = filemap_add_folio(mapping, folio, offset, gfp);
		if (unlikely(err)) {
			folio_put(folio);
			/*
			 * If a split of large page was required, it
			 * already happened when we marked the page invalid
			 * which guarantees that this call won't fail
			 */
			set_direct_map_default_noflush(page);
			if (err == -EEXIST)
				goto retry;

			ret = vmf_error(err);
			goto out;
		}

		addr = (unsigned long)page_address(page);
		flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
	}

	vmf->page = page;
	ret = VM_FAULT_LOCKED;

out:
	filemap_invalidate_unlock_shared(mapping);
	return ret;
}

static const struct vm_operations_struct secretmem_vm_ops = {
	.fault = secretmem_fault,
};

static int secretmem_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	atomic_dec(&secretmem_users);
	return 0;
}

static int secretmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	struct secretmem_ctx *ctx = file->private_data;
	unsigned long kernel_no_permissions;

	kernel_no_permissions = (VM_READ | VM_WRITE | VM_EXEC | VM_MAYEXEC);

	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) == 0)
		return -EINVAL;

	if (ctx && (vma->vm_flags & kernel_no_permissions))
		return -EINVAL;

	if (!mlock_future_ok(vma->vm_mm, vma->vm_flags | VM_LOCKED, len))
		return -EAGAIN;

	if (ctx)
		vm_flags_set(vma, VM_MIXEDMAP);

	vm_flags_set(vma, VM_LOCKED | VM_DONTDUMP);
	vma->vm_ops = &secretmem_vm_ops;

	return 0;
}

bool vma_is_secretmem(struct vm_area_struct *vma)
{
	return vma->vm_ops == &secretmem_vm_ops;
}

static const struct file_operations secretmem_fops = {
	.release	= secretmem_release,
	.mmap		= secretmem_mmap,
};

static int secretmem_migrate_folio(struct address_space *mapping,
		struct folio *dst, struct folio *src, enum migrate_mode mode)
{
	return -EBUSY;
}

static void secretmem_free_folio(struct folio *folio)
{
	set_direct_map_default_noflush(&folio->page);
	folio_zero_segment(folio, 0, folio_size(folio));
}

const struct address_space_operations secretmem_aops = {
	.dirty_folio	= noop_dirty_folio,
	.free_folio	= secretmem_free_folio,
	.migrate_folio	= secretmem_migrate_folio,
};

static int secretmem_setattr(struct mnt_idmap *idmap,
			     struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	struct address_space *mapping = inode->i_mapping;
	unsigned int ia_valid = iattr->ia_valid;
	int ret;

	filemap_invalidate_lock(mapping);

	if ((ia_valid & ATTR_SIZE) && inode->i_size)
		ret = -EINVAL;
	else
		ret = simple_setattr(idmap, dentry, iattr);

	filemap_invalidate_unlock(mapping);

	return ret;
}

static const struct inode_operations secretmem_iops = {
	.setattr = secretmem_setattr,
};

static struct vfsmount *secretmem_mnt;

static struct file *secretmem_file_create(unsigned long flags)
{
	struct file *file;
	struct inode *inode;
	const char *anon_name = "[secretmem]";
	const struct qstr qname = QSTR_INIT(anon_name, strlen(anon_name));
	int err;

	inode = alloc_anon_inode(secretmem_mnt->mnt_sb);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	err = security_inode_init_security_anon(inode, &qname, NULL);
	if (err) {
		file = ERR_PTR(err);
		goto err_free_inode;
	}

	file = alloc_file_pseudo(inode, secretmem_mnt, "secretmem",
				 O_RDWR, &secretmem_fops);
	if (IS_ERR(file))
		goto err_free_inode;

	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);

	inode->i_op = &secretmem_iops;
	inode->i_mapping->a_ops = &secretmem_aops;

	/* pretend we are a normal file with zero size */
	inode->i_mode |= S_IFREG;
	inode->i_size = 0;

	return file;

err_free_inode:
	iput(inode);
	return file;
}

#ifdef CONFIG_KERNEL_SECRETMEM

struct secretmem_area *secretmem_allocate_pages(unsigned int order)
{
	unsigned long uvaddr, uvaddr_inc, unused, nr_pages, bytes_length;
	struct file *kernel_secfile;
	struct vm_area_struct *vma;
	struct secretmem_ctx *ctx;
	struct page **sec_pages;
	struct mm_struct *mm;
	long nr_pinned_pages;
	pte_t pte, old_pte;
	spinlock_t *ptl;
	pte_t *upte;
	int rc;

	nr_pages = (1 << order);
	bytes_length = nr_pages * PAGE_SIZE;
	mm = current->mm;

	if (!mm || !mmget_not_zero(mm))
		return NULL;

	/* Create secret memory file / truncate it */
	kernel_secfile = secretmem_file_create(0);
	if (IS_ERR(kernel_secfile))
		goto put_mm;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (IS_ERR(ctx))
		goto close_secfile;
	kernel_secfile->private_data = ctx;

	rc = do_truncate(file_mnt_idmap(kernel_secfile),
			 file_dentry(kernel_secfile), bytes_length, 0, NULL);
	if (rc)
		goto close_secfile;

	if (mmap_write_lock_killable(mm))
		goto close_secfile;

	/* Map pages to the secretmem file */
	uvaddr = do_mmap(kernel_secfile, 0, bytes_length, PROT_NONE,
			 MAP_SHARED, 0, 0, &unused, NULL);
	if (IS_ERR_VALUE(uvaddr))
		goto unlock_mmap;

	/* mseal() the VMA to make sure it won't change */
	rc = do_mseal(uvaddr, uvaddr + bytes_length, true);
	if (rc)
		goto unmap_pages;

	/* Make sure VMA is there, and is kernel-secure */
	vma = find_vma(current->mm, uvaddr);
	if (!vma)
		goto unseal_vma;

	if (!vma_is_secretmem(vma) ||
	    !can_access_secretmem_vma(vma))
		goto unseal_vma;

	/* Pin user pages; fault them in */
	sec_pages = kzalloc(sizeof(struct page *) * nr_pages, GFP_KERNEL);
	if (!sec_pages)
		goto unseal_vma;

	nr_pinned_pages = pin_user_pages(uvaddr, nr_pages, FOLL_FORCE | FOLL_LONGTERM, sec_pages);
	if (nr_pinned_pages < 0)
		goto free_sec_pages;
	if (nr_pinned_pages != nr_pages)
		goto unpin_pages;

	/* Modify the existing mapping to be kernel accessible, local to this process mm */
	uvaddr_inc = uvaddr;
	while (uvaddr_inc < uvaddr + bytes_length) {
		upte = get_locked_pte(mm, uvaddr_inc, &ptl);
		if (!upte)
			goto unpin_pages;
		old_pte = ptep_modify_prot_start(vma, uvaddr_inc, upte);
		pte = pte_modify(old_pte, PAGE_KERNEL);
		ptep_modify_prot_commit(vma, uvaddr_inc, upte, old_pte, pte);
		pte_unmap_unlock(upte, ptl);
		uvaddr_inc += PAGE_SIZE;
	}
	flush_tlb_range(vma, uvaddr, uvaddr + bytes_length);

	/* Return data */
	mmgrab(mm);
	ctx->_area.ptr = (void *) uvaddr;
	ctx->_pages = sec_pages;
	ctx->_nr_pages = nr_pages;
	ctx->_mm = mm;
	ctx->_file = kernel_secfile;

	mmap_write_unlock(mm);
	mmput(mm);

	return &ctx->_area;

unpin_pages:
	unpin_user_pages(sec_pages, nr_pinned_pages);
free_sec_pages:
	kfree(sec_pages);
unseal_vma:
	rc = do_mseal(uvaddr, uvaddr + bytes_length, false);
	if (rc)
		BUG();
unmap_pages:
	rc = do_munmap(mm, uvaddr, bytes_length, NULL);
	if (rc)
		BUG();
unlock_mmap:
	mmap_write_unlock(mm);
close_secfile:
	fput(kernel_secfile);
put_mm:
	mmput(mm);
	return NULL;
}

void secretmem_release_pages(struct secretmem_area *data)
{
	unsigned long uvaddr, bytes_length;
	struct secretmem_ctx *ctx;
	int rc;

	if (!data || !data->ptr)
		BUG();

	ctx = container_of(data, struct secretmem_ctx, _area);
	if (!ctx || !ctx->_file || !ctx->_pages || !ctx->_mm)
		BUG();

	bytes_length = ctx->_nr_pages * PAGE_SIZE;
	uvaddr = (unsigned long) data->ptr;

	/*
	 * Remove the mapping if mm is still in use.
	 * Not secure to continue if unmapping failed.
	 */
	if (mmget_not_zero(ctx->_mm)) {
		mmap_write_lock(ctx->_mm);
		rc = do_mseal(uvaddr, uvaddr + bytes_length, false);
		if (rc) {
			mmap_write_unlock(ctx->_mm);
			BUG();
		}
		rc = do_munmap(ctx->_mm, uvaddr, bytes_length, NULL);
		if (rc) {
			mmap_write_unlock(ctx->_mm);
			BUG();
		}
		mmap_write_unlock(ctx->_mm);
		mmput(ctx->_mm);
	}

	mmdrop(ctx->_mm);
	unpin_user_pages(ctx->_pages, ctx->_nr_pages);
	fput(ctx->_file);
	kfree(ctx->_pages);

	ctx->_nr_pages = 0;
	ctx->_pages = NULL;
	ctx->_file = NULL;
	ctx->_mm = NULL;
	ctx->_area.ptr = NULL;
}

bool can_access_secretmem_vma(struct vm_area_struct *vma)
{
	struct secretmem_ctx *ctx;

	if (!vma_is_secretmem(vma))
		return true;

	/*
	 * If VMA is owned by running process, and marked for kernel
	 * usage, then allow access.
	 */
	ctx = vma->vm_file->private_data;
	if (ctx && current->mm == vma->vm_mm)
		return true;

	return false;
}

#endif /* CONFIG_KERNEL_SECRETMEM */

SYSCALL_DEFINE1(memfd_secret, unsigned int, flags)
{
	struct file *file;
	int fd, err;

	/* make sure local flags do not confict with global fcntl.h */
	BUILD_BUG_ON(SECRETMEM_FLAGS_MASK & O_CLOEXEC);

	if (!secretmem_enable)
		return -ENOSYS;

	if (flags & ~(SECRETMEM_FLAGS_MASK | O_CLOEXEC))
		return -EINVAL;
	if (atomic_read(&secretmem_users) < 0)
		return -ENFILE;

	fd = get_unused_fd_flags(flags & O_CLOEXEC);
	if (fd < 0)
		return fd;

	file = secretmem_file_create(flags);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_put_fd;
	}

	file->f_flags |= O_LARGEFILE;

	atomic_inc(&secretmem_users);
	fd_install(fd, file);
	return fd;

err_put_fd:
	put_unused_fd(fd);
	return err;
}

static int secretmem_init_fs_context(struct fs_context *fc)
{
	return init_pseudo(fc, SECRETMEM_MAGIC) ? 0 : -ENOMEM;
}

static struct file_system_type secretmem_fs = {
	.name		= "secretmem",
	.init_fs_context = secretmem_init_fs_context,
	.kill_sb	= kill_anon_super,
};

static int __init secretmem_init(void)
{
	if (!secretmem_enable)
		return 0;

	secretmem_mnt = kern_mount(&secretmem_fs);
	if (IS_ERR(secretmem_mnt))
		return PTR_ERR(secretmem_mnt);

	/* prevent secretmem mappings from ever getting PROT_EXEC */
	secretmem_mnt->mnt_flags |= MNT_NOEXEC;

	return 0;
}
fs_initcall(secretmem_init);
