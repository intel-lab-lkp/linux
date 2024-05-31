// SPDX-License-Identifier: GPL-2.0
#include <linux/darray.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/pseudo_fs.h>
#include <linux/ringbuffer_sys.h>
#include <uapi/linux/ringbuffer_sys.h>
#include <linux/syscalls.h>

#define RINGBUFFER_FS_MAGIC			0xa10a10a2

static DEFINE_MUTEX(ringbuffer_lock);

static struct vfsmount *ringbuffer_mnt;

struct ringbuffer_mapping {
	ulong			addr;
	struct mm_struct	*mm;
};

struct ringbuffer {
	wait_queue_head_t	wait[2];
	spinlock_t		lock;
	int			rw;
	u32			size;	/* always a power of two */
	u32			mask;	/* size - 1 */
	struct file		*io_file;
	/* hidden internal file for the mmap */
	struct file		*rb_file;
	struct ringbuffer_ptrs	*ptrs;
	void			*data;
	DARRAY(struct ringbuffer_mapping) mms;
};

static const struct address_space_operations ringbuffer_aops = {
	.dirty_folio	= noop_dirty_folio,
#if 0
	.migrate_folio	= ringbuffer_migrate_folio,
#endif
};

#if 0
static int ringbuffer_mremap(struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;
	struct mm_struct *mm = vma->vm_mm;
	struct kioctx_table *table;
	int i, res = -EINVAL;

	spin_lock(&mm->ioctx_lock);
	rcu_read_lock();
	table = rcu_dereference(mm->ioctx_table);
	if (!table)
		goto out_unlock;

	for (i = 0; i < table->nr; i++) {
		struct kioctx *ctx;

		ctx = rcu_dereference(table->table[i]);
		if (ctx && ctx->ringbuffer_file == file) {
			if (!atomic_read(&ctx->dead)) {
				ctx->user_id = ctx->mmap_base = vma->vm_start;
				res = 0;
			}
			break;
		}
	}

out_unlock:
	rcu_read_unlock();
	spin_unlock(&mm->ioctx_lock);
	return res;
}
#endif

static const struct vm_operations_struct ringbuffer_vm_ops = {
#if 0
	.mremap		= ringbuffer_mremap,
#endif
#if IS_ENABLED(CONFIG_MMU)
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= filemap_page_mkwrite,
#endif
};

static int ringbuffer_mmap(struct file *file, struct vm_area_struct *vma)
{
	vm_flags_set(vma, VM_DONTEXPAND);
	vma->vm_ops = &ringbuffer_vm_ops;
	return 0;
}

static const struct file_operations ringbuffer_fops = {
	.mmap = ringbuffer_mmap,
};

static void ringbuffer_free(struct ringbuffer *rb)
{
	rb->io_file->f_ringbuffers[rb->rw] = NULL;

	darray_for_each(rb->mms, map)
		darray_for_each_reverse(map->mm->ringbuffers, rb2)
			if (rb == *rb2)
				darray_remove_item(&map->mm->ringbuffers, rb2);

	if (rb->rb_file) {
		/* Kills mapping: */
		truncate_setsize(file_inode(rb->rb_file), 0);

		/* Prevent further access to the kioctx from migratepages */
		struct address_space *mapping = rb->rb_file->f_mapping;
		spin_lock(&mapping->i_private_lock);
		mapping->i_private_data = NULL;
		spin_unlock(&mapping->i_private_lock);

		fput(rb->rb_file);
	}

	free_pages((ulong) rb->data, get_order(rb->size));
	free_page((ulong) rb->ptrs);
	kfree(rb);
}

static int ringbuffer_map(struct ringbuffer *rb, ulong *addr)
{
	struct mm_struct *mm = current->mm;

	int ret = darray_make_room(&rb->mms, 1) ?:
		darray_make_room(&mm->ringbuffers, 1);
	if (ret)
		return ret;

	ret = mmap_write_lock_killable(mm);
	if (ret)
		return ret;

	ulong unused;
	struct ringbuffer_mapping map = {
		.addr = do_mmap(rb->rb_file, 0, rb->size + PAGE_SIZE,
				PROT_READ|PROT_WRITE,
				MAP_SHARED, 0, 0, &unused, NULL),
		.mm = mm,
	};
	mmap_write_unlock(mm);

	ret = PTR_ERR_OR_ZERO((void *) map.addr);
	if (ret)
		return ret;

	ret =   darray_push(&mm->ringbuffers, rb) ?:
		darray_push(&rb->mms, map);
	BUG_ON(ret); /* we preallocated */

	*addr = map.addr;
	return 0;
}

static int ringbuffer_get_addr_or_map(struct ringbuffer *rb, ulong *addr)
{
	struct mm_struct *mm = current->mm;

	darray_for_each(rb->mms, map)
		if (map->mm == mm) {
			*addr = map->addr;
			return 0;
		}

	return ringbuffer_map(rb, addr);
}

static struct ringbuffer *ringbuffer_alloc(struct file *file, int rw, u32 size,
					   ulong *addr)
{
	unsigned order = get_order(size);
	size = PAGE_SIZE << order;

	struct ringbuffer *rb = kzalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb)
		return ERR_PTR(-ENOMEM);

	init_waitqueue_head(&rb->wait[READ]);
	init_waitqueue_head(&rb->wait[WRITE]);
	spin_lock_init(&rb->lock);
	rb->rw		= rw;
	rb->size	= size;
	rb->mask	= size - 1;
	rb->io_file	= file;

	rb->ptrs = (void *) __get_free_page(GFP_KERNEL|__GFP_ZERO);
	rb->data = (void *) __get_free_pages(GFP_KERNEL|__GFP_ZERO, order);
	if (!rb->ptrs || !rb->data)
		goto err;

	rb->ptrs->size	= size;
	rb->ptrs->mask	= size - 1;
	rb->ptrs->data_offset = PAGE_SIZE;

	struct inode *inode = alloc_anon_inode(ringbuffer_mnt->mnt_sb);
	int ret = PTR_ERR_OR_ZERO(inode);
	if (ret)
		goto err;

	inode->i_mapping->a_ops = &ringbuffer_aops;
	inode->i_mapping->i_private_data = rb;
	inode->i_size = size;

	rb->rb_file = alloc_file_pseudo(inode, ringbuffer_mnt, "[ringbuffer]",
				     O_RDWR, &ringbuffer_fops);
	ret = PTR_ERR_OR_ZERO(rb->rb_file);
	if (ret)
		goto err_iput;

	ret = filemap_add_folio(rb->rb_file->f_mapping,
				page_folio(virt_to_page(rb->ptrs)),
				0, GFP_KERNEL);
	if (ret)
		goto err;

	/* todo - implement a fallback when high order allocation fails */
	ret = filemap_add_folio(rb->rb_file->f_mapping,
				page_folio(virt_to_page(rb->data)),
				1, GFP_KERNEL);
	if (ret)
		goto err;

	ret = ringbuffer_map(rb, addr);
	if (ret)
		goto err;

	return rb;
err_iput:
	iput(inode);
err:
	ringbuffer_free(rb);
	return ERR_PTR(ret);
}

/* file is going away, tear down ringbuffers: */
void ringbuffer_file_exit(struct file *file)
{
	mutex_lock(&ringbuffer_lock);
	for (unsigned i = 0; i < ARRAY_SIZE(file->f_ringbuffers); i++)
		if (file->f_ringbuffers[i])
			ringbuffer_free(file->f_ringbuffers[i]);
	mutex_unlock(&ringbuffer_lock);
}

/*
 * XXX: we require synchronization when killing a ringbuffer (because no longer
 * mapped anywhere) to a file that is still open (and in use)
 */
static void ringbuffer_mm_drop(struct mm_struct *mm, struct ringbuffer *rb)
{
	darray_for_each_reverse(rb->mms, map)
		if (mm == map->mm)
			darray_remove_item(&rb->mms, map);

	if (!rb->mms.nr)
		ringbuffer_free(rb);
}

void ringbuffer_mm_exit(struct mm_struct *mm)
{
	mutex_lock(&ringbuffer_lock);
	darray_for_each_reverse(mm->ringbuffers, rb)
		ringbuffer_mm_drop(mm, *rb);
	mutex_unlock(&ringbuffer_lock);

	darray_exit(&mm->ringbuffers);
}

SYSCALL_DEFINE4(ringbuffer, unsigned, fd, int, rw, u32, size, ulong __user *, ringbufferp)
{
	ulong rb_addr;

	int ret = get_user(rb_addr, ringbufferp);
	if (unlikely(ret))
		return ret;

	if (unlikely(rb_addr || !size || rw > WRITE))
		return -EINVAL;

	struct fd f = fdget(fd);
	if (!f.file)
		return -EBADF;

	if (!(f.file->f_op->fop_flags & (rw == READ ? FOP_RINGBUFFER_READ : FOP_RINGBUFFER_WRITE))) {
		ret = -EOPNOTSUPP;
		goto err;
	}

	mutex_lock(&ringbuffer_lock);
	struct ringbuffer *rb = f.file->f_ringbuffers[rw];
	if (rb) {
		ret = ringbuffer_get_addr_or_map(rb, &rb_addr);
		if (ret)
			goto err_unlock;

		ret = put_user(rb_addr, ringbufferp);
	} else {
		rb = ringbuffer_alloc(f.file, rw, size, &rb_addr);
		ret = PTR_ERR_OR_ZERO(rb);
		if (ret)
			goto err_unlock;

		ret = put_user(rb_addr, ringbufferp);
		if (ret) {
			ringbuffer_free(rb);
			goto err_unlock;
		}

		f.file->f_ringbuffers[rw] = rb;
	}
err_unlock:
	mutex_unlock(&ringbuffer_lock);
err:
	fdput(f);
	return ret;
}

static bool __ringbuffer_read(struct ringbuffer *rb, void **data, size_t *len,
			       bool nonblocking, size_t *ret)
{
	u32 head = rb->ptrs->head;
	u32 tail = rb->ptrs->tail;

	if (head == tail)
		return 0;

	ulong flags;
	spin_lock_irqsave(&rb->lock, flags);
	/* Multiple consumers - recheck under lock: */
	tail = rb->ptrs->tail;

	while (*len && tail != head) {
		u32 tail_masked = tail & rb->mask;
		u32 b = min(*len,
			min(head - tail,
			    rb->size - tail_masked));

		memcpy(*data, rb->data + tail_masked, b);
		tail	+= b;
		*data	+= b;
		*len	-= b;
		*ret	+= b;
	}

	smp_store_release(&rb->ptrs->tail, tail);
	spin_unlock_irqrestore(&rb->lock, flags);

	return !*len || nonblocking;
}

size_t ringbuffer_read(struct ringbuffer *rb, void *data, size_t len, bool nonblocking)
{
	size_t ret = 0;
	wait_event(rb->wait[READ], __ringbuffer_read(rb, &data, &len, nonblocking, &ret));
	return ret;
}
EXPORT_SYMBOL_GPL(ringbuffer_read);

static bool __ringbuffer_write(struct ringbuffer *rb, void **data, size_t *len,
			       bool nonblocking, size_t *ret)
{
	u32 head = rb->ptrs->head;
	u32 tail = rb->ptrs->tail;

	if (head - tail >= rb->size)
		return 0;

	ulong flags;
	spin_lock_irqsave(&rb->lock, flags);
	/* Multiple producers - recheck under lock: */
	head = rb->ptrs->head;

	while (*len && head - tail < rb->size) {
		u32 head_masked = head & rb->mask;
		u32 b = min(*len,
			min(tail + rb->size - head,
			    rb->size - head_masked));

		memcpy(rb->data + head_masked, *data, b);
		head	+= b;
		*data	+= b;
		*len	-= b;
		*ret	+= b;
	}

	smp_store_release(&rb->ptrs->head, head);
	spin_unlock_irqrestore(&rb->lock, flags);

	return !*len || nonblocking;
}

size_t ringbuffer_write(struct ringbuffer *rb, void *data, size_t len, bool nonblocking)
{
	size_t ret = 0;
	wait_event(rb->wait[WRITE], __ringbuffer_write(rb, &data, &len, nonblocking, &ret));
	return ret;
}
EXPORT_SYMBOL_GPL(ringbuffer_write);

SYSCALL_DEFINE2(ringbuffer_wait, unsigned, fd, int, rw)
{
	int ret = 0;

	if (rw > WRITE)
		return -EINVAL;

	struct fd f = fdget(fd);
	if (!f.file)
		return -EBADF;

	struct ringbuffer *rb = f.file->f_ringbuffers[rw];
	if (!rb) {
		ret = -EINVAL;
		goto err;
	}

	struct ringbuffer_ptrs *rp = rb->ptrs;
	wait_event(rb->wait[rw], rw == READ
		   ? rp->head != rp->tail
		   : rp->head - rp->tail < rb->size);
err:
	fdput(f);
	return ret;
}

SYSCALL_DEFINE2(ringbuffer_wakeup, unsigned, fd, int, rw)
{
	int ret = 0;

	if (rw > WRITE)
		return -EINVAL;

	struct fd f = fdget(fd);
	if (!f.file)
		return -EBADF;

	struct ringbuffer *rb = f.file->f_ringbuffers[rw];
	if (!rb) {
		ret = -EINVAL;
		goto err;
	}

	wake_up(&rb->wait[rw]);
err:
	fdput(f);
	return ret;
}

static int ringbuffer_init_fs_context(struct fs_context *fc)
{
	if (!init_pseudo(fc, RINGBUFFER_FS_MAGIC))
		return -ENOMEM;
	fc->s_iflags |= SB_I_NOEXEC;
	return 0;
}

static int __init ringbuffer_setup(void)
{
	static struct file_system_type ringbuffer_fs = {
		.name		= "ringbuffer",
		.init_fs_context = ringbuffer_init_fs_context,
		.kill_sb	= kill_anon_super,
	};
	ringbuffer_mnt = kern_mount(&ringbuffer_fs);
	if (IS_ERR(ringbuffer_mnt))
		panic("Failed to create ringbuffer fs mount.");
	return 0;
}
__initcall(ringbuffer_setup);
