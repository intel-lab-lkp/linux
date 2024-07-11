// SPDX-License-Identifier: GPL-2.0
/*
 * Framework for userspace DMA-BUF allocations
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019 Linaro Ltd.
 */

#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/xarray.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/nospec.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/dma-heap.h>
#include <linux/vmalloc.h>
#include <uapi/linux/dma-heap.h>

#define DEVNAME "dma_heap"

#define NUM_HEAP_MINORS 128

/**
 * struct dma_heap - represents a dmabuf heap in the system
 * @name:		used for debugging/device-node name
 * @ops:		ops struct for this heap
 * @heap_devt		heap device node
 * @list		list head connecting to list of heaps
 * @heap_cdev		heap char device
 *
 * Represents a heap of memory from which buffers can be made.
 */
struct dma_heap {
	const char *name;
	const struct dma_heap_ops *ops;
	void *priv;
	dev_t heap_devt;
	struct list_head list;
	struct cdev heap_cdev;
};

/**
 * struct dma_heap_file - wrap the file, read task for dma_heap allocate use.
 * @file:		file to read from.
 *
 * @cred:		kthread use, user cred copy to use for the read.
 *
 * @max_batch:		maximum batch size to read, if collect match batch,
 *			trigger read, default 128MB, must below file size.
 *
 * @fsz:		file size.
 *
 * @direct:		use direct IO?
 */
struct dma_heap_file {
	struct file *file;
	struct cred *cred;
	size_t max_batch;
	size_t fsz;
	bool direct;
};

/**
 * struct dma_heap_file_work - represents a dma_heap file read real work.
 * @vaddr:		contigous virtual address alloc by vmap, file read need.
 *
 * @start_size:		file read start offset, same to @dma_heap_file_task->roffset.
 *
 * @need_size:		file read need size, same to @dma_heap_file_task->rsize.
 *
 * @heap_file:		file wrapper.
 *
 * @list:		child node of @dma_heap_file_control->works.
 *
 * @refp:		same @dma_heap_file_task->ref, if end of read, put ref.
 *
 * @failp:		if any work io failed, set it true, pointp @dma_heap_file_task->fail.
 */
struct dma_heap_file_work {
	void *vaddr;
	ssize_t start_size;
	ssize_t need_size;
	struct dma_heap_file *heap_file;
	struct list_head list;
	atomic_t *refp;
	bool *failp;
};

/**
 * struct dma_heap_file_task - represents a dma_heap file read process
 * @ref:		current file work counter, if zero, allocate and read
 *			done.
 *
 * @roffset:		last read offset, current prepared work' begin file
 *			start offset.
 *
 * @rsize:		current allocated page size use to read, if reach rbatch,
 *			trigger commit.
 *
 * @rbatch:		current prepared work's batch, below @dma_heap_file's
 *			batch.
 *
 * @heap_file:		current dma_heap_file
 *
 * @parray:		used for vmap, size is @dma_heap_file's batch's number
 *			pages.(this is maximum). Due to single thread file read,
 *			one page array reuse each work prepare is OK.
 *			Each index in parray is PAGE_SIZE.(vmap need)
 *
 * @pindex:		current allocated page filled in @parray's index.
 *
 * @fail:		any work failed when file read?
 *
 * dma_heap_file_task is the production of file read, will prepare each work
 * during allocate dma_buf pages, if match current batch, then trigger commit
 * and prepare next work. After all batch queued, user going on prepare dma_buf
 * and so on, but before return dma_buf fd, need to wait file read end and
 * check read result.
 */
struct dma_heap_file_task {
	atomic_t ref;
	size_t roffset;
	size_t rsize;
	size_t rbatch;
	struct dma_heap_file *heap_file;
	struct page **parray;
	unsigned int pindex;
	bool fail;
};

/**
 * struct dma_heap_file_control - global control of dma_heap file read.
 * @works:		@dma_heap_file_work's list head.
 *
 * @lock:		only lock for @works.
 *
 * @threadwq:		wait queue for @work_thread, if commit work, @work_thread
 *			wakeup and read this work's file contains.
 *
 * @workwq:		used for main thread wait for file read end, if allocation
 *			end before file read. @dma_heap_file_task ref effect this.
 *
 * @work_thread:	file read kthread. the dma_heap_file_task work's consumer.
 *
 * @heap_fwork_cachep:	@dma_heap_file_work's cachep, it's alloc/free frequently.
 *
 * @nr_work:		global number of how many work committed.
 */
struct dma_heap_file_control {
	struct list_head works;
	spinlock_t lock;
	wait_queue_head_t threadwq;
	wait_queue_head_t workwq;
	struct task_struct *work_thread;
	struct kmem_cache *heap_fwork_cachep;
	atomic_t nr_work;
};

static struct dma_heap_file_control *heap_fctl;
static LIST_HEAD(heap_list);
static DEFINE_MUTEX(heap_list_lock);
static dev_t dma_heap_devt;
static struct class *dma_heap_class;
static DEFINE_XARRAY_ALLOC(dma_heap_minors);

/**
 * map_pages_to_vaddr - map each scatter page into contiguous virtual address.
 * @heap_ftask:		prepared and need to commit's work.
 *
 * Cached pages need to trigger file read, this function map each scatter page
 * into contiguous virtual address, so that file read can easy use.
 * Now that we get vaddr page, cached pages can return to original user, so we
 * will not effect dma-buf export even if file read not end.
 */
static void *map_pages_to_vaddr(struct dma_heap_file_task *heap_ftask)
{
	return vmap(heap_ftask->parray, heap_ftask->pindex, VM_MAP,
		    PAGE_KERNEL);
}

bool dma_heap_prepare_file_read(struct dma_heap_file_task *heap_ftask,
				struct page *page)
{
	struct page **array = heap_ftask->parray;
	int index = heap_ftask->pindex;
	int num = compound_nr(page), i;
	unsigned long sz = page_size(page);

	heap_ftask->rsize += sz;
	for (i = 0; i < num; ++i)
		array[index++] = &page[i];
	heap_ftask->pindex = index;

	return heap_ftask->rsize >= heap_ftask->rbatch;
}

static struct dma_heap_file_work *
init_file_work(struct dma_heap_file_task *heap_ftask)
{
	struct dma_heap_file_work *heap_fwork;
	struct dma_heap_file *heap_file = heap_ftask->heap_file;

	if (READ_ONCE(heap_ftask->fail))
		return NULL;

	heap_fwork = kmem_cache_alloc(heap_fctl->heap_fwork_cachep, GFP_KERNEL);
	if (unlikely(!heap_fwork))
		return NULL;

	heap_fwork->vaddr = map_pages_to_vaddr(heap_ftask);
	if (unlikely(!heap_fwork->vaddr)) {
		kmem_cache_free(heap_fctl->heap_fwork_cachep, heap_fwork);
		return NULL;
	}

	heap_fwork->heap_file = heap_file;
	heap_fwork->start_size = heap_ftask->roffset;
	heap_fwork->need_size = heap_ftask->rsize;
	heap_fwork->refp = &heap_ftask->ref;
	heap_fwork->failp = &heap_ftask->fail;
	atomic_inc(&heap_ftask->ref);
	return heap_fwork;
}

static void destroy_file_work(struct dma_heap_file_work *heap_fwork)
{
	vunmap(heap_fwork->vaddr);
	atomic_dec(heap_fwork->refp);
	wake_up(&heap_fctl->workwq);

	kmem_cache_free(heap_fctl->heap_fwork_cachep, heap_fwork);
}

int dma_heap_submit_file_read(struct dma_heap_file_task *heap_ftask)
{
	struct dma_heap_file_work *heap_fwork = init_file_work(heap_ftask);
	struct page *last = NULL;
	struct dma_heap_file *heap_file = heap_ftask->heap_file;
	size_t start = heap_ftask->roffset;
	struct file *file = heap_file->file;
	size_t fsz = heap_file->fsz;

	if (unlikely(!heap_fwork))
		return -ENOMEM;

	/**
	 * If file size is not page aligned, direct io can't process the tail.
	 * So, if reach to tail, remain the last page use buffer read.
	 */
	if (heap_file->direct && start + heap_ftask->rsize > fsz) {
		heap_fwork->need_size -= PAGE_SIZE;
		last = heap_ftask->parray[heap_ftask->pindex - 1];
	}

	spin_lock(&heap_fctl->lock);
	list_add_tail(&heap_fwork->list, &heap_fctl->works);
	spin_unlock(&heap_fctl->lock);
	atomic_inc(&heap_fctl->nr_work);

	wake_up(&heap_fctl->threadwq);

	if (last) {
		char *buf, *pathp;
		ssize_t err;
		void *buffer;

		buf = kmalloc(PATH_MAX, GFP_KERNEL);
		if (unlikely(!buf))
			return -ENOMEM;

		start = PAGE_ALIGN_DOWN(fsz);

		pathp = file_path(file, buf, PATH_MAX);
		if (IS_ERR(pathp)) {
			kfree(buf);
			return PTR_ERR(pathp);
		}

		buffer = kmap_local_page(last); // use page's kaddr.
		err = kernel_read_file_from_path(pathp, start, &buffer,
						 fsz - start, &fsz,
						 READING_POLICY);
		kunmap_local(buffer);
		kfree(buf);
		if (err < 0) {
			pr_err("failed to use buffer kernel_read_file %s, err=%ld, [%ld, %ld], f_sz=%ld\n",
			       pathp, err, start, fsz, fsz);

			return err;
		}
	}

	heap_ftask->roffset += heap_ftask->rsize;
	heap_ftask->rsize = 0;
	heap_ftask->pindex = 0;
	heap_ftask->rbatch = min_t(size_t,
				   PAGE_ALIGN(fsz) - heap_ftask->roffset,
				   heap_ftask->rbatch);
	return 0;
}

bool dma_heap_wait_for_file_read(struct dma_heap_file_task *heap_ftask)
{
	wait_event_freezable(heap_fctl->workwq,
			     atomic_read(&heap_ftask->ref) == 0);
	return heap_ftask->fail;
}

bool dma_heap_destroy_file_read(struct dma_heap_file_task *heap_ftask)
{
	bool fail;

	dma_heap_wait_for_file_read(heap_ftask);
	fail = heap_ftask->fail;
	kvfree(heap_ftask->parray);
	kfree(heap_ftask);
	return fail;
}

struct dma_heap_file_task *
dma_heap_declare_file_read(struct dma_heap_file *heap_file)
{
	struct dma_heap_file_task *heap_ftask =
		kzalloc(sizeof(*heap_ftask), GFP_KERNEL);
	if (unlikely(!heap_ftask))
		return NULL;

	/**
	 * Batch is the maximum size which we prepare work will meet.
	 * So, direct alloc this number's page array is OK.
	 */
	heap_ftask->parray = kvmalloc_array(heap_file->max_batch >> PAGE_SHIFT,
					    sizeof(struct page *), GFP_KERNEL);
	if (unlikely(!heap_ftask->parray))
		goto put;

	heap_ftask->heap_file = heap_file;
	heap_ftask->rbatch = heap_file->max_batch;
	return heap_ftask;
put:
	kfree(heap_ftask);
	return NULL;
}

static void __work_this_io(struct dma_heap_file_work *heap_fwork)
{
	struct dma_heap_file *heap_file = heap_fwork->heap_file;
	struct file *file = heap_file->file;
	ssize_t start = heap_fwork->start_size;
	ssize_t size = heap_fwork->need_size;
	void *buffer = heap_fwork->vaddr;
	const struct cred *old_cred;
	ssize_t err;

	// use real task's cred to read this file.
	old_cred = override_creds(heap_file->cred);
	err = kernel_read_file(file, start, &buffer, size, &heap_file->fsz,
			       READING_POLICY);
	if (err < 0) {
		pr_err("use kernel_read_file, err=%ld, [%ld, %ld], f_sz=%ld\n",
		       err, start, (start + size), heap_file->fsz);
		WRITE_ONCE(*heap_fwork->failp, true);
	}
	// recovery to my cred.
	revert_creds(old_cred);
}

static int dma_heap_file_control_thread(void *data)
{
	struct dma_heap_file_control *heap_fctl =
		(struct dma_heap_file_control *)data;
	struct dma_heap_file_work *worker, *tmp;
	int nr_work;

	LIST_HEAD(pages);
	LIST_HEAD(workers);

	while (true) {
		wait_event_freezable(heap_fctl->threadwq,
				     atomic_read(&heap_fctl->nr_work) > 0);
recheck:
		spin_lock(&heap_fctl->lock);
		list_splice_init(&heap_fctl->works, &workers);
		spin_unlock(&heap_fctl->lock);

		if (unlikely(kthread_should_stop())) {
			list_for_each_entry_safe(worker, tmp, &workers, list) {
				list_del(&worker->list);
				destroy_file_work(worker);
			}
			break;
		}

		nr_work = 0;
		list_for_each_entry_safe(worker, tmp, &workers, list) {
			++nr_work;
			list_del(&worker->list);
			__work_this_io(worker);

			destroy_file_work(worker);
		}
		atomic_sub(nr_work, &heap_fctl->nr_work);

		if (atomic_read(&heap_fctl->nr_work) > 0)
			goto recheck;
	}
	return 0;
}

size_t dma_heap_file_size(struct dma_heap_file *heap_file)
{
	return heap_file->fsz;
}

static int prepare_dma_heap_file(struct dma_heap_file *heap_file, int file_fd,
				 size_t batch)
{
	struct file *file;
	size_t fsz;
	int ret;

	file = fget(file_fd);
	if (!file)
		return -EINVAL;

	fsz = i_size_read(file_inode(file));
	if (fsz < batch) {
		ret = -EINVAL;
		goto err;
	}

	/**
	 * Selinux block our read, but actually we are reading the stand-in
	 * for this file.
	 * So save current's cred and when going to read, override mine, and
	 * end of read, revert.
	 */
	heap_file->cred = prepare_kernel_cred(current);
	if (unlikely(!heap_file->cred)) {
		ret = -ENOMEM;
		goto err;
	}

	heap_file->file = file;
	heap_file->max_batch = batch;
	heap_file->fsz = fsz;

	heap_file->direct = file->f_flags & O_DIRECT;

#define DMA_HEAP_SUGGEST_DIRECT_IO_SIZE (1UL << 30)
	if (!heap_file->direct && fsz >= DMA_HEAP_SUGGEST_DIRECT_IO_SIZE)
		pr_warn("alloc read file better to use O_DIRECT to read larget file\n");

	return 0;

err:
	fput(file);
	return ret;
}

static void destroy_dma_heap_file(struct dma_heap_file *heap_file)
{
	fput(heap_file->file);
	put_cred(heap_file->cred);
}

static int dma_heap_buffer_alloc_read_file(struct dma_heap *heap, int file_fd,
					   size_t batch, unsigned int fd_flags,
					   unsigned int heap_flags)
{
	struct dma_buf *dmabuf;
	int fd;
	struct dma_heap_file heap_file;

	fd = prepare_dma_heap_file(&heap_file, file_fd, batch);
	if (fd)
		goto error_file;

	dmabuf = heap->ops->allocate_read_file(heap, &heap_file, fd_flags,
					       heap_flags);
	if (IS_ERR(dmabuf)) {
		fd = PTR_ERR(dmabuf);
		goto error;
	}

	fd = dma_buf_fd(dmabuf, fd_flags);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		/* just return, as put will call release and that will free */
	}

error:
	destroy_dma_heap_file(&heap_file);
error_file:
	return fd;
}

static int dma_heap_buffer_alloc(struct dma_heap *heap, size_t len,
				 u32 fd_flags,
				 u64 heap_flags)
{
	struct dma_buf *dmabuf;
	int fd;

	/*
	 * Allocations from all heaps have to begin
	 * and end on page boundaries.
	 */
	len = PAGE_ALIGN(len);
	if (!len)
		return -EINVAL;

	dmabuf = heap->ops->allocate(heap, len, fd_flags, heap_flags);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = dma_buf_fd(dmabuf, fd_flags);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		/* just return, as put will call release and that will free */
	}
	return fd;
}

static int dma_heap_open(struct inode *inode, struct file *file)
{
	struct dma_heap *heap;

	heap = xa_load(&dma_heap_minors, iminor(inode));
	if (!heap) {
		pr_err("dma_heap: minor %d unknown.\n", iminor(inode));
		return -ENODEV;
	}

	/* instance data as context */
	file->private_data = heap;
	nonseekable_open(inode, file);

	return 0;
}

static long dma_heap_ioctl_allocate_read_file(struct file *file, void *data)
{
	struct dma_heap_allocation_file_data *heap_allocation_file = data;
	struct dma_heap *heap = file->private_data;
	int fd;

	if (heap_allocation_file->fd || !heap_allocation_file->file_fd)
		return -EINVAL;

	if (heap_allocation_file->fd_flags & ~DMA_HEAP_VALID_FD_FLAGS)
		return -EINVAL;

	if (heap_allocation_file->heap_flags & ~DMA_HEAP_VALID_HEAP_FLAGS)
		return -EINVAL;

	if (!heap->ops->allocate_read_file)
		return -EINVAL;

	fd = dma_heap_buffer_alloc_read_file(
		heap, heap_allocation_file->file_fd,
		heap_allocation_file->batch ?
			PAGE_ALIGN(heap_allocation_file->batch) :
			DEFAULT_ADI_BATCH,
		heap_allocation_file->fd_flags,
		heap_allocation_file->heap_flags);
	if (fd < 0)
		return fd;

	heap_allocation_file->fd = fd;
	return 0;
}

static long dma_heap_ioctl_allocate(struct file *file, void *data)
{
	struct dma_heap_allocation_data *heap_allocation = data;
	struct dma_heap *heap = file->private_data;
	int fd;

	if (heap_allocation->fd)
		return -EINVAL;

	if (heap_allocation->fd_flags & ~DMA_HEAP_VALID_FD_FLAGS)
		return -EINVAL;

	if (heap_allocation->heap_flags & ~DMA_HEAP_VALID_HEAP_FLAGS)
		return -EINVAL;

	fd = dma_heap_buffer_alloc(heap, heap_allocation->len,
				   heap_allocation->fd_flags,
				   heap_allocation->heap_flags);
	if (fd < 0)
		return fd;

	heap_allocation->fd = fd;

	return 0;
}

static unsigned int dma_heap_ioctl_cmds[] = {
	DMA_HEAP_IOCTL_ALLOC,
	DMA_HEAP_IOCTL_ALLOC_AND_READ,
};

static long dma_heap_ioctl(struct file *file, unsigned int ucmd,
			   unsigned long arg)
{
	char stack_kdata[128];
	char *kdata = stack_kdata;
	unsigned int kcmd;
	unsigned int in_size, out_size, drv_size, ksize;
	int nr = _IOC_NR(ucmd);
	int ret = 0;

	if (nr >= ARRAY_SIZE(dma_heap_ioctl_cmds))
		return -EINVAL;

	nr = array_index_nospec(nr, ARRAY_SIZE(dma_heap_ioctl_cmds));
	/* Get the kernel ioctl cmd that matches */
	kcmd = dma_heap_ioctl_cmds[nr];

	/* Figure out the delta between user cmd size and kernel cmd size */
	drv_size = _IOC_SIZE(kcmd);
	out_size = _IOC_SIZE(ucmd);
	in_size = out_size;
	if ((ucmd & kcmd & IOC_IN) == 0)
		in_size = 0;
	if ((ucmd & kcmd & IOC_OUT) == 0)
		out_size = 0;
	ksize = max(max(in_size, out_size), drv_size);

	/* If necessary, allocate buffer for ioctl argument */
	if (ksize > sizeof(stack_kdata)) {
		kdata = kmalloc(ksize, GFP_KERNEL);
		if (!kdata)
			return -ENOMEM;
	}

	if (copy_from_user(kdata, (void __user *)arg, in_size) != 0) {
		ret = -EFAULT;
		goto err;
	}

	/* zero out any difference between the kernel/user structure size */
	if (ksize > in_size)
		memset(kdata + in_size, 0, ksize - in_size);

	switch (kcmd) {
	case DMA_HEAP_IOCTL_ALLOC:
		ret = dma_heap_ioctl_allocate(file, kdata);
		break;
	case DMA_HEAP_IOCTL_ALLOC_AND_READ:
		ret = dma_heap_ioctl_allocate_read_file(file, kdata);
		break;
	default:
		ret = -ENOTTY;
		goto err;
	}

	if (copy_to_user((void __user *)arg, kdata, out_size) != 0)
		ret = -EFAULT;
err:
	if (kdata != stack_kdata)
		kfree(kdata);
	return ret;
}

static const struct file_operations dma_heap_fops = {
	.owner          = THIS_MODULE,
	.open		= dma_heap_open,
	.unlocked_ioctl = dma_heap_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= dma_heap_ioctl,
#endif
};

/**
 * dma_heap_get_drvdata() - get per-subdriver data for the heap
 * @heap: DMA-Heap to retrieve private data for
 *
 * Returns:
 * The per-subdriver data for the heap.
 */
void *dma_heap_get_drvdata(struct dma_heap *heap)
{
	return heap->priv;
}

/**
 * dma_heap_get_name() - get heap name
 * @heap: DMA-Heap to retrieve private data for
 *
 * Returns:
 * The char* for the heap name.
 */
const char *dma_heap_get_name(struct dma_heap *heap)
{
	return heap->name;
}

struct dma_heap *dma_heap_add(const struct dma_heap_export_info *exp_info)
{
	struct dma_heap *heap, *h, *err_ret;
	struct device *dev_ret;
	unsigned int minor;
	int ret;

	if (!exp_info->name || !strcmp(exp_info->name, "")) {
		pr_err("dma_heap: Cannot add heap without a name\n");
		return ERR_PTR(-EINVAL);
	}

	if (!exp_info->ops || !exp_info->ops->allocate) {
		pr_err("dma_heap: Cannot add heap with invalid ops struct\n");
		return ERR_PTR(-EINVAL);
	}

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap)
		return ERR_PTR(-ENOMEM);

	heap->name = exp_info->name;
	heap->ops = exp_info->ops;
	heap->priv = exp_info->priv;

	/* Find unused minor number */
	ret = xa_alloc(&dma_heap_minors, &minor, heap,
		       XA_LIMIT(0, NUM_HEAP_MINORS - 1), GFP_KERNEL);
	if (ret < 0) {
		pr_err("dma_heap: Unable to get minor number for heap\n");
		err_ret = ERR_PTR(ret);
		goto err0;
	}

	/* Create device */
	heap->heap_devt = MKDEV(MAJOR(dma_heap_devt), minor);

	cdev_init(&heap->heap_cdev, &dma_heap_fops);
	ret = cdev_add(&heap->heap_cdev, heap->heap_devt, 1);
	if (ret < 0) {
		pr_err("dma_heap: Unable to add char device\n");
		err_ret = ERR_PTR(ret);
		goto err1;
	}

	dev_ret = device_create(dma_heap_class,
				NULL,
				heap->heap_devt,
				NULL,
				heap->name);
	if (IS_ERR(dev_ret)) {
		pr_err("dma_heap: Unable to create device\n");
		err_ret = ERR_CAST(dev_ret);
		goto err2;
	}

	mutex_lock(&heap_list_lock);
	/* check the name is unique */
	list_for_each_entry(h, &heap_list, list) {
		if (!strcmp(h->name, exp_info->name)) {
			mutex_unlock(&heap_list_lock);
			pr_err("dma_heap: Already registered heap named %s\n",
			       exp_info->name);
			err_ret = ERR_PTR(-EINVAL);
			goto err3;
		}
	}

	/* Add heap to the list */
	list_add(&heap->list, &heap_list);
	mutex_unlock(&heap_list_lock);

	return heap;

err3:
	device_destroy(dma_heap_class, heap->heap_devt);
err2:
	cdev_del(&heap->heap_cdev);
err1:
	xa_erase(&dma_heap_minors, minor);
err0:
	kfree(heap);
	return err_ret;
}

static char *dma_heap_devnode(const struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "dma_heap/%s", dev_name(dev));
}

static int dma_heap_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&dma_heap_devt, 0, NUM_HEAP_MINORS, DEVNAME);
	if (ret)
		return ret;

	dma_heap_class = class_create(DEVNAME);
	if (IS_ERR(dma_heap_class)) {
		ret = PTR_ERR(dma_heap_class);
		goto fail_class;
	}
	dma_heap_class->devnode = dma_heap_devnode;

	heap_fctl = kzalloc(sizeof(*heap_fctl), GFP_KERNEL);
	if (unlikely(!heap_fctl)) {
		ret =  -ENOMEM;
		goto fail_alloc;
	}

	INIT_LIST_HEAD(&heap_fctl->works);
	init_waitqueue_head(&heap_fctl->threadwq);
	init_waitqueue_head(&heap_fctl->workwq);

	heap_fctl->work_thread = kthread_run(dma_heap_file_control_thread,
					     heap_fctl, "heap_fwork_t");
	if (IS_ERR(heap_fctl->work_thread)) {
		ret = -ENOMEM;
		goto fail_thread;
	}

	heap_fctl->heap_fwork_cachep = KMEM_CACHE(dma_heap_file_work, 0);
	if (unlikely(!heap_fctl->heap_fwork_cachep)) {
		ret = -ENOMEM;
		goto fail_cache;
	}

	return 0;

fail_cache:
	kthread_stop(heap_fctl->work_thread);
fail_thread:
	kfree(heap_fctl);
fail_alloc:
	class_destroy(dma_heap_class);
fail_class:
	unregister_chrdev_region(dma_heap_devt, NUM_HEAP_MINORS);
	return ret;
}
subsys_initcall(dma_heap_init);
