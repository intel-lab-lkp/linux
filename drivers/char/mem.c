// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/drivers/char/mem.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Added devfs support.
 *    Jan-11-1998, C. Scott Ananian <cananian@alumni.princeton.edu>
 *  Shared /dev/zero mmapping support, Feb 2000, Kanoj Sarcar <kanoj@sgi.com>
 */

#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mman.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/capability.h>
#include <linux/ptrace.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/backing-dev.h>
#include <linux/shmem_fs.h>
#include <linux/splice.h>
#include <linux/pfn.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/uio.h>
#include <linux/uaccess.h>
#include <linux/security.h>

#define DEVMEM_MINOR	1
#define DEVPORT_MINOR	4

static inline unsigned long size_inside_page(unsigned long start,
					     unsigned long size)
{
	unsigned long sz;

	sz = PAGE_SIZE - (start & (PAGE_SIZE - 1));

	return min(sz, size);
}

#ifndef ARCH_HAS_VALID_PHYS_ADDR_RANGE
static inline int valid_phys_addr_range(phys_addr_t addr, size_t count)
{
	return addr + count <= __pa(high_memory);
}

static inline int valid_mmap_phys_addr_range(unsigned long pfn, size_t size)
{
	return 1;
}
#endif

#ifdef CONFIG_STRICT_DEVMEM
static inline int page_is_allowed(unsigned long pfn)
{
	return devmem_is_allowed(pfn);
}
#else
static inline int page_is_allowed(unsigned long pfn)
{
	return 1;
}
#endif

static inline bool should_stop_iteration(void)
{
	if (need_resched())
		cond_resched();
	return signal_pending(current);
}

/**
 * read_mem - read from physical memory (/dev/mem).
 * @file: struct file associated with /dev/mem.
 * @buf: user-space buffer to copy data to.
 * @count: number of bytes to read.
 * @ppos: pointer to the current file position, representing the physical
 *        address to read from.
 *
 * This function checks if the requested physical memory range is valid
 * and accessible by the user, then it copies data to the input
 * user-space buffer up to the requested number of bytes.
 *
 * Function's expectations:
 *
 * 1. This function shall check if the value pointed by ppos exceeds the
 *    maximum addressable physical address;
 *
 * 2. This function shall check if the physical address range to be read
 *    is valid (i.e. it falls within a memory block and if it can be mapped
 *    to the kernel address space);
 *
 * 3. For each memory page falling in the requested physical range
 *    [ppos, ppos + count - 1]:
 *   3.1. this function shall check if user space access is allowed (if
 *        config STRICT_DEVMEM is not set, access is always granted);
 *
 *   3.2. if access is allowed, the memory content from the page range falling
 *        within the requested physical range shall be copied to the user space
 *        buffer;
 *
 *   3.3. zeros shall be copied to the user space buffer (for the page range
 *        falling within the requested physical range):
 *     3.3.1. if access to the memory page is restricted or,
 *     3.2.2. if the current page is page 0 on HW architectures where page 0 is
 *            not mapped.
 *
 * 4. The file position '*ppos' shall be advanced by the number of bytes
 *    successfully copied to user space (including zeros).
 *
 * Context: process context.
 *
 * Return:
 * * the number of bytes copied to user on success
 * * %-EFAULT - the requested address range is not valid or a fault happened
 *   when copying to user-space (i.e. copy_from_kernel_nofault() failed)
 * * %-EPERM - access to any of the required physical pages is not allowed
 * * %-ENOMEM - out of memory error for auxiliary kernel buffers supporting
 *   the operation of copying content from the physical pages
 */
static ssize_t read_mem(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	phys_addr_t p = *ppos;
	ssize_t read, sz;
	void *ptr;
	char *bounce;
	int err;

	if (p != *ppos)
		return 0;

	if (!valid_phys_addr_range(p, count))
		return -EFAULT;
	read = 0;
#ifdef __ARCH_HAS_NO_PAGE_ZERO_MAPPED
	/* we don't have page 0 mapped on sparc and m68k.. */
	if (p < PAGE_SIZE) {
		sz = size_inside_page(p, count);
		if (sz > 0) {
			if (clear_user(buf, sz))
				return -EFAULT;
			buf += sz;
			p += sz;
			count -= sz;
			read += sz;
		}
	}
#endif

	bounce = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	while (count > 0) {
		unsigned long remaining;
		int allowed, probe;

		sz = size_inside_page(p, count);

		err = -EPERM;
		allowed = page_is_allowed(p >> PAGE_SHIFT);
		if (!allowed)
			goto failed;

		err = -EFAULT;
		if (allowed == 2) {
			/* Show zeros for restricted memory. */
			remaining = clear_user(buf, sz);
		} else {
			/*
			 * On ia64 if a page has been mapped somewhere as
			 * uncached, then it must also be accessed uncached
			 * by the kernel or data corruption may occur.
			 */
			ptr = xlate_dev_mem_ptr(p);
			if (!ptr)
				goto failed;

			probe = copy_from_kernel_nofault(bounce, ptr, sz);
			unxlate_dev_mem_ptr(p, ptr);
			if (probe)
				goto failed;

			remaining = copy_to_user(buf, bounce, sz);
		}

		if (remaining)
			goto failed;

		buf += sz;
		p += sz;
		count -= sz;
		read += sz;
		if (should_stop_iteration())
			break;
	}
	kfree(bounce);

	*ppos += read;
	return read;

failed:
	kfree(bounce);
	return err;
}

/**
 * write_mem - write to physical memory (/dev/mem).
 * @file: struct file associated with /dev/mem.
 * @buf: user-space buffer containing the data to write.
 * @count: number of bytes to write.
 * @ppos: pointer to the current file position, representing the physical
 *        address to write to.
 *
 * This function checks if the target physical memory range is valid
 * and accessible by the user, then it writes data from the input
 * user-space buffer up to the requested number of bytes.
 *
 * Function's expectations:
 * 1. This function shall check if the value pointed by ppos exceeds the
 *    maximum addressable physical address;
 *
 * 2. This function shall check if the physical address range to be written
 *    is valid (i.e. it falls within a memory block and if it can be mapped
 *    to the kernel address space);
 *
 * 3. For each memory page falling in the physical range to be written
 *    [ppos, ppos + count - 1]:
 *   3.1. this function shall check if user space access is allowed (if
 *        config STRICT_DEVMEM is not set, access is always granted);
 *
 *   3.2. the content from the user space buffer shall be copied to the page
 *        range falling within the physical range to be written if access is
 *        allowed;
 *
 *   3.3. the data to be copied from the user space buffer (for the page range
 *        falling within the range to be written) shall be skipped:
 *     3.3.1. if access to the memory page is restricted or,
 *     3.3.2. if the current page is page 0 on HW architectures where page 0
 *            is not mapped.
 *
 * 4. The file position '*ppos' shall be advanced by the number of bytes
 *    successfully copied from user space (including skipped bytes).
 *
 * Context: process context.
 *
 * Return:
 * * the number of bytes copied from user-space on success
 * * %-EFBIG - the value pointed by ppos exceeds the maximum addressable
 *   physical address
 * * %-EFAULT - the physical address range is not valid or no bytes could
 *   be copied from user-space
 * * %-EPERM - access to any of the required pages is not allowed
 */
static ssize_t write_mem(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	phys_addr_t p = *ppos;
	ssize_t written, sz;
	unsigned long copied;
	void *ptr;

	if (p != *ppos)
		return -EFBIG;

	if (!valid_phys_addr_range(p, count))
		return -EFAULT;

	written = 0;

#ifdef __ARCH_HAS_NO_PAGE_ZERO_MAPPED
	/* we don't have page 0 mapped on sparc and m68k.. */
	if (p < PAGE_SIZE) {
		sz = size_inside_page(p, count);
		/* Hmm. Do something? */
		buf += sz;
		p += sz;
		count -= sz;
		written += sz;
	}
#endif

	while (count > 0) {
		int allowed;

		sz = size_inside_page(p, count);

		allowed = page_is_allowed(p >> PAGE_SHIFT);
		if (!allowed)
			return -EPERM;

		/* Skip actual writing when a page is marked as restricted. */
		if (allowed == 1) {
			/*
			 * On ia64 if a page has been mapped somewhere as
			 * uncached, then it must also be accessed uncached
			 * by the kernel or data corruption may occur.
			 */
			ptr = xlate_dev_mem_ptr(p);
			if (!ptr) {
				if (written)
					break;
				return -EFAULT;
			}

			copied = copy_from_user(ptr, buf, sz);
			unxlate_dev_mem_ptr(p, ptr);
			if (copied) {
				written += sz - copied;
				if (written)
					break;
				return -EFAULT;
			}
		}

		buf += sz;
		p += sz;
		count -= sz;
		written += sz;
		if (should_stop_iteration())
			break;
	}

	*ppos += written;
	return written;
}

int __weak phys_mem_access_prot_allowed(struct file *file,
	unsigned long pfn, unsigned long size, pgprot_t *vma_prot)
{
	return 1;
}

#ifndef __HAVE_PHYS_MEM_ACCESS_PROT

/*
 * Architectures vary in how they handle caching for addresses
 * outside of main memory.
 *
 */
#ifdef pgprot_noncached
static int uncached_access(struct file *file, phys_addr_t addr)
{
	/*
	 * Accessing memory above the top the kernel knows about or through a
	 * file pointer
	 * that was marked O_DSYNC will be done non-cached.
	 */
	if (file->f_flags & O_DSYNC)
		return 1;
	return addr >= __pa(high_memory);
}
#endif

static pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
				     unsigned long size, pgprot_t vma_prot)
{
#ifdef pgprot_noncached
	phys_addr_t offset = pfn << PAGE_SHIFT;

	if (uncached_access(file, offset))
		return pgprot_noncached(vma_prot);
#endif
	return vma_prot;
}
#endif

#ifndef CONFIG_MMU
static unsigned long get_unmapped_area_mem(struct file *file,
					   unsigned long addr,
					   unsigned long len,
					   unsigned long pgoff,
					   unsigned long flags)
{
	if (!valid_mmap_phys_addr_range(pgoff, len))
		return (unsigned long) -EINVAL;
	return pgoff << PAGE_SHIFT;
}

/* permit direct mmap, for read, write or exec */
static unsigned memory_mmap_capabilities(struct file *file)
{
	return NOMMU_MAP_DIRECT |
		NOMMU_MAP_READ | NOMMU_MAP_WRITE | NOMMU_MAP_EXEC;
}

static unsigned zero_mmap_capabilities(struct file *file)
{
	return NOMMU_MAP_COPY;
}

/* can't do an in-place private mapping if there's no MMU */
static inline int private_mapping_ok(struct vm_area_struct *vma)
{
	return is_nommu_shared_mapping(vma->vm_flags);
}
#else

static inline int private_mapping_ok(struct vm_area_struct *vma)
{
	return 1;
}
#endif

static const struct vm_operations_struct mmap_mem_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys
#endif
};

/**
 * mmap_mem - map physical memory into user space (/dev/mem).
 * @file: file structure for the device.
 * @vma: virtual memory area structure describing the user mapping.
 *
 * This function checks if the requested physical memory range is valid
 * and accessible by the user, then it maps the physical memory range to
 * user-mode address space.
 *
 * Function's expectations:
 * 1. This function shall check if the requested physical address range to be
 *    mapped fits within the maximum addressable physical range;
 *
 * 2. This function shall check if the requested  physical range corresponds to
 *    a valid physical range and if access is allowed on it (if config STRICT_DEVMEM
 *    is not set, access is always allowed);
 *
 * 3. This function shall check if the input virtual memory area can be used for
 *    a private mapping (always OK if there is an MMU);
 *
 * 4. This function shall set the virtual memory area operations to
 *    &mmap_mem_ops;
 *
 * 5. This function shall establish a mapping between the user-space
 *    virtual memory area described by vma and the physical memory
 *    range specified by vma->vm_pgoff and size;
 *
 * Context: process context.
 *
 * Return:
 * * 0 on success
 * * %-EAGAIN - invalid or unsupported mapping requested (remap_pfn_range()
 *   fails)
 * * %-EINVAL - requested physical range to be mapped is not valid
 * * %-EPERM - no permission to access the requested physical range
 */
static int mmap_mem(struct file *file, struct vm_area_struct *vma)
{
	size_t size = vma->vm_end - vma->vm_start;
	phys_addr_t offset = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;

	/* Does it even fit in phys_addr_t? */
	if (offset >> PAGE_SHIFT != vma->vm_pgoff)
		return -EINVAL;

	/* It's illegal to wrap around the end of the physical address space. */
	if (offset + (phys_addr_t)size - 1 < offset)
		return -EINVAL;

	if (!valid_mmap_phys_addr_range(vma->vm_pgoff, size))
		return -EINVAL;

	if (!private_mapping_ok(vma))
		return -ENOSYS;

	if (!range_is_allowed(vma->vm_pgoff, size))
		return -EPERM;

	if (!phys_mem_access_prot_allowed(file, vma->vm_pgoff, size,
						&vma->vm_page_prot))
		return -EINVAL;

	vma->vm_page_prot = phys_mem_access_prot(file, vma->vm_pgoff,
						 size,
						 vma->vm_page_prot);

	vma->vm_ops = &mmap_mem_ops;

	/* Remap-pfn-range will mark the range VM_IO */
	if (remap_pfn_range(vma,
			    vma->vm_start,
			    vma->vm_pgoff,
			    size,
			    vma->vm_page_prot)) {
		return -EAGAIN;
	}
	return 0;
}

#ifdef CONFIG_DEVPORT
static ssize_t read_port(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	unsigned long i = *ppos;
	char __user *tmp = buf;

	if (!access_ok(buf, count))
		return -EFAULT;
	while (count-- > 0 && i < 65536) {
		if (__put_user(inb(i), tmp) < 0)
			return -EFAULT;
		i++;
		tmp++;
	}
	*ppos = i;
	return tmp-buf;
}

static ssize_t write_port(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	unsigned long i = *ppos;
	const char __user *tmp = buf;

	if (!access_ok(buf, count))
		return -EFAULT;
	while (count-- > 0 && i < 65536) {
		char c;

		if (__get_user(c, tmp)) {
			if (tmp > buf)
				break;
			return -EFAULT;
		}
		outb(c, i);
		i++;
		tmp++;
	}
	*ppos = i;
	return tmp-buf;
}
#endif

static ssize_t read_null(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	return 0;
}

static ssize_t write_null(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	return count;
}

static ssize_t read_iter_null(struct kiocb *iocb, struct iov_iter *to)
{
	return 0;
}

static ssize_t write_iter_null(struct kiocb *iocb, struct iov_iter *from)
{
	size_t count = iov_iter_count(from);
	iov_iter_advance(from, count);
	return count;
}

static int pipe_to_null(struct pipe_inode_info *info, struct pipe_buffer *buf,
			struct splice_desc *sd)
{
	return sd->len;
}

static ssize_t splice_write_null(struct pipe_inode_info *pipe, struct file *out,
				 loff_t *ppos, size_t len, unsigned int flags)
{
	return splice_from_pipe(pipe, out, ppos, len, flags, pipe_to_null);
}

static int uring_cmd_null(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	return 0;
}

static ssize_t read_iter_zero(struct kiocb *iocb, struct iov_iter *iter)
{
	size_t written = 0;

	while (iov_iter_count(iter)) {
		size_t chunk = iov_iter_count(iter), n;

		if (chunk > PAGE_SIZE)
			chunk = PAGE_SIZE;	/* Just for latency reasons */
		n = iov_iter_zero(chunk, iter);
		if (!n && iov_iter_count(iter))
			return written ? written : -EFAULT;
		written += n;
		if (signal_pending(current))
			return written ? written : -ERESTARTSYS;
		if (!need_resched())
			continue;
		if (iocb->ki_flags & IOCB_NOWAIT)
			return written ? written : -EAGAIN;
		cond_resched();
	}
	return written;
}

static ssize_t read_zero(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	size_t cleared = 0;

	while (count) {
		size_t chunk = min_t(size_t, count, PAGE_SIZE);
		size_t left;

		left = clear_user(buf + cleared, chunk);
		if (unlikely(left)) {
			cleared += (chunk - left);
			if (!cleared)
				return -EFAULT;
			break;
		}
		cleared += chunk;
		count -= chunk;

		if (signal_pending(current))
			break;
		cond_resched();
	}

	return cleared;
}

static int mmap_zero(struct file *file, struct vm_area_struct *vma)
{
#ifndef CONFIG_MMU
	return -ENOSYS;
#endif
	if (vma->vm_flags & VM_SHARED)
		return shmem_zero_setup(vma);
	vma_set_anonymous(vma);
	return 0;
}

static unsigned long get_unmapped_area_zero(struct file *file,
				unsigned long addr, unsigned long len,
				unsigned long pgoff, unsigned long flags)
{
#ifdef CONFIG_MMU
	if (flags & MAP_SHARED) {
		/*
		 * mmap_zero() will call shmem_zero_setup() to create a file,
		 * so use shmem's get_unmapped_area in case it can be huge;
		 * and pass NULL for file as in mmap.c's get_unmapped_area(),
		 * so as not to confuse shmem with our handle on "/dev/zero".
		 */
		return shmem_get_unmapped_area(NULL, addr, len, pgoff, flags);
	}

	/* Otherwise flags & MAP_PRIVATE: with no shmem object beneath it */
	return mm_get_unmapped_area(current->mm, file, addr, len, pgoff, flags);
#else
	return -ENOSYS;
#endif
}

static ssize_t write_full(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	return -ENOSPC;
}

/*
 * Special lseek() function for /dev/null and /dev/zero.  Most notably, you
 * can fopen() both devices with "a" now.  This was previously impossible.
 * -- SRB.
 */
static loff_t null_lseek(struct file *file, loff_t offset, int orig)
{
	return file->f_pos = 0;
}

/**
 * memory_lseek - change the file position.
 * @file: file structure for the device.
 * @offset: file offset to seek to.
 * @orig: where to start seeking from (see whence in the llseek manpage).
 *
 * This function changes the file position according to the input offset
 * and orig parameters.
 *
 * Function's expectations:
 * 1. This function shall lock the semaphore of the inode corresponding to the
 *    input file before any operation and unlock it before returning.
 *
 * 2. This function shall check the orig value and accordingly:
 *   2.1. if it is equal to SEEK_CUR, the current file position shall be
 *        incremented by the input offset;
 *   2.2. if it is equal to SEEK_SET, the current file position shall be
 *        set to the input offset value;
 *   2.3. any other value shall result in an error condition.
 *
 * 3. Before writing the current file position, the new position value
 *    shall be checked to not overlap with Linux ERRNO values.
 *
 * Assumptions of Use:
 * 1. the input file pointer is expected to be valid.
 *
 * Notes:
 * The memory devices use the full 32/64 bits of the offset, and so we cannot
 * check against negative addresses: they are ok. The return value is weird,
 * though, in that case (0).
 *
 * Also note that seeking relative to the "end of file" isn't supported:
 * it has no meaning, so passing orig equal to SEEK_END returns -EINVAL.
 *
 * Context: process context, locks/unlocks inode->i_rwsem
 *
 * Return:
 * * the new file position on success
 * * %-EOVERFLOW - the new position value equals or exceeds
 *   (unsigned long long) -MAX_ERRNO
 * * %-EINVAL - the orig parameter is invalid
 */
static loff_t memory_lseek(struct file *file, loff_t offset, int orig)
{
	loff_t ret;

	inode_lock(file_inode(file));
	switch (orig) {
	case SEEK_CUR:
		offset += file->f_pos;
		fallthrough;
	case SEEK_SET:
		/* to avoid userland mistaking f_pos=-9 as -EBADF=-9 */
		if ((unsigned long long)offset >= -MAX_ERRNO) {
			ret = -EOVERFLOW;
			break;
		}
		file->f_pos = offset;
		ret = file->f_pos;
		force_successful_syscall_return();
		break;
	default:
		ret = -EINVAL;
	}
	inode_unlock(file_inode(file));
	return ret;
}

/**
 * open_port - open the I/O port device (/dev/port).
 * @inode: inode of the device file.
 * @filp: file structure for the device.
 *
 * This function checks if the caller can access the port device and sets
 * the f_mapping pointer of filp to the i_mapping pointer of inode.
 *
 * Function's expectations:
 * 1. This function shall check if the caller has sufficient capabilities to
 *    perform raw I/O access;
 *
 * 2. This function shall check if the kernel is locked down with the
 *    &LOCKDOWN_DEV_MEM restriction;
 *
 * 3. If the input inode corresponds to /dev/mem, the f_mapping pointer
 *    of the input file structure shall be set to the i_mapping pointer
 *    of the input inode;
 *
 * Assumptions of Use:
 * 1. The input inode and filp are expected to be valid.
 *
 * Context: process context.
 *
 * Return:
 * * 0 on success
 * * %-EPERM - caller lacks the required capability (CAP_SYS_RAWIO)
 * * any error returned by securty_locked_down()
 */
static int open_port(struct inode *inode, struct file *filp)
{
	int rc;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	rc = security_locked_down(LOCKDOWN_DEV_MEM);
	if (rc)
		return rc;

	if (iminor(inode) != DEVMEM_MINOR)
		return 0;

	/*
	 * Use a unified address space to have a single point to manage
	 * revocations when drivers want to take over a /dev/mem mapped
	 * range.
	 */
	filp->f_mapping = iomem_get_mapping();

	return 0;
}

#define zero_lseek	null_lseek
#define full_lseek      null_lseek
#define write_zero	write_null
#define write_iter_zero	write_iter_null
#define splice_write_zero	splice_write_null
#define open_mem	open_port

static const struct file_operations __maybe_unused mem_fops = {
	.llseek		= memory_lseek,
	.read		= read_mem,
	.write		= write_mem,
	.mmap		= mmap_mem,
	.open		= open_mem,
#ifndef CONFIG_MMU
	.get_unmapped_area = get_unmapped_area_mem,
	.mmap_capabilities = memory_mmap_capabilities,
#endif
	.fop_flags	= FOP_UNSIGNED_OFFSET,
};

static const struct file_operations null_fops = {
	.llseek		= null_lseek,
	.read		= read_null,
	.write		= write_null,
	.read_iter	= read_iter_null,
	.write_iter	= write_iter_null,
	.splice_write	= splice_write_null,
	.uring_cmd	= uring_cmd_null,
};

#ifdef CONFIG_DEVPORT
static const struct file_operations port_fops = {
	.llseek		= memory_lseek,
	.read		= read_port,
	.write		= write_port,
	.open		= open_port,
};
#endif

static const struct file_operations zero_fops = {
	.llseek		= zero_lseek,
	.write		= write_zero,
	.read_iter	= read_iter_zero,
	.read		= read_zero,
	.write_iter	= write_iter_zero,
	.splice_read	= copy_splice_read,
	.splice_write	= splice_write_zero,
	.mmap		= mmap_zero,
	.get_unmapped_area = get_unmapped_area_zero,
#ifndef CONFIG_MMU
	.mmap_capabilities = zero_mmap_capabilities,
#endif
};

static const struct file_operations full_fops = {
	.llseek		= full_lseek,
	.read_iter	= read_iter_zero,
	.write		= write_full,
	.splice_read	= copy_splice_read,
};

static const struct memdev {
	const char *name;
	const struct file_operations *fops;
	fmode_t fmode;
	umode_t mode;
} devlist[] = {
#ifdef CONFIG_DEVMEM
	[DEVMEM_MINOR] = { "mem", &mem_fops, 0, 0 },
#endif
	[3] = { "null", &null_fops, FMODE_NOWAIT, 0666 },
#ifdef CONFIG_DEVPORT
	[4] = { "port", &port_fops, 0, 0 },
#endif
	[5] = { "zero", &zero_fops, FMODE_NOWAIT, 0666 },
	[7] = { "full", &full_fops, 0, 0666 },
	[8] = { "random", &random_fops, FMODE_NOWAIT, 0666 },
	[9] = { "urandom", &urandom_fops, FMODE_NOWAIT, 0666 },
#ifdef CONFIG_PRINTK
	[11] = { "kmsg", &kmsg_fops, 0, 0644 },
#endif
};

/**
 * memory_open - set the filp f_op to the memory device fops and invoke open().
 * @inode: inode of the device file.
 * @filp: file structure for the device.
 *
 * Function's expectations:
 * 1. This function shall retrieve the minor number associated with the input
 *   inode and the memory device corresponding to such minor number;
 *
 * 2. The file operations pointer shall be set to the memory device file operations;
 *
 * 3. The file mode member of the input filp shall be OR'd with the device mode;
 *
 * 4. The memory device open() file operation shall be invoked.
 *
 * Assumptions of Use:
 * 1. The input inode and filp are expected to be non-NULL.
 *
 * Context: process context.
 *
 * Return:
 * * 0 on success
 * * %-ENXIO - the minor number corresponding to the input inode cannot be
 *   associated with any device or the corresponding device has a NULL fops
 *   pointer
 * * any error returned by the device specific open function pointer
 */
static int memory_open(struct inode *inode, struct file *filp)
{
	int minor;
	const struct memdev *dev;

	minor = iminor(inode);
	if (minor >= ARRAY_SIZE(devlist))
		return -ENXIO;

	dev = &devlist[minor];
	if (!dev->fops)
		return -ENXIO;

	filp->f_op = dev->fops;
	filp->f_mode |= dev->fmode;

	if (dev->fops->open)
		return dev->fops->open(inode, filp);

	return 0;
}

static const struct file_operations memory_fops = {
	.open = memory_open,
	.llseek = noop_llseek,
};

static char *mem_devnode(const struct device *dev, umode_t *mode)
{
	if (mode && devlist[MINOR(dev->devt)].mode)
		*mode = devlist[MINOR(dev->devt)].mode;
	return NULL;
}

static const struct class mem_class = {
	.name		= "mem",
	.devnode	= mem_devnode,
};

static int __init chr_dev_init(void)
{
	int retval;
	int minor;

	if (register_chrdev(MEM_MAJOR, "mem", &memory_fops))
		printk("unable to get major %d for memory devs\n", MEM_MAJOR);

	retval = class_register(&mem_class);
	if (retval)
		return retval;

	for (minor = 1; minor < ARRAY_SIZE(devlist); minor++) {
		if (!devlist[minor].name)
			continue;

		/*
		 * Create /dev/port?
		 */
		if ((minor == DEVPORT_MINOR) && !arch_has_dev_port())
			continue;

		device_create(&mem_class, NULL, MKDEV(MEM_MAJOR, minor),
			      NULL, devlist[minor].name);
	}

	return tty_init();
}

fs_initcall(chr_dev_init);
