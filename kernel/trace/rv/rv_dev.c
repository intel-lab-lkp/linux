// SPDX-License-Identifier: GPL-2.0
/*
 * rv_dev.c - /dev/rv misc device for RV monitor self-instrumentation
 *
 * A single misc device (MISC_DYNAMIC_MINOR) serves all RV monitors.
 * ioctl numbers encode the monitor identity:
 *
 *   0x01 - 0x1F  tlob (task latency over budget)
 *   0x20 - 0x3F  reserved
 *
 * Each monitor exports tlob_start_task() / tlob_stop_task() which are
 * called here.  The calling task is identified by current.
 *
 * Magic: RV_IOC_MAGIC (0xB9), defined in include/uapi/linux/rv.h
 *
 * Per-fd private data (rv_file_priv)
 * ------------------------------------
 * Every open() of /dev/rv allocates an rv_file_priv (defined in tlob.h).
 * When TLOB_IOCTL_TRACE_START is called with args.notify_fd >= 0, violations
 * are pushed as tlob_event records into that fd's per-fd ring buffer (tlob_ring)
 * and its poll/epoll waitqueue is woken.
 *
 * Consumers drain records with read() on the notify_fd; read() blocks until
 * at least one record is available (unless O_NONBLOCK is set).
 *
 * Per-thread "started" tracking (tlob_task_handle)
 * -------------------------------------------------
 * tlob_stop_task() returns -ESRCH in two distinct situations:
 *
 *   (a) The deadline timer already fired and removed the tlob hash-table
 *       entry before TRACE_STOP arrived -> budget was exceeded -> -EOVERFLOW
 *
 *   (b) TRACE_START was never called for this thread -> programming error
 *       -> -ESRCH
 *
 * To distinguish them, rv_dev.c maintains a lightweight hash table
 * (tlob_handles) that records a tlob_task_handle for every task_struct *
 * for which a successful TLOB_IOCTL_TRACE_START has been
 * issued but the corresponding TLOB_IOCTL_TRACE_STOP has not yet arrived.
 *
 * tlob_task_handle is a thin "session ticket"  --  it carries only the
 * task pointer and the owning file descriptor.  The heavy per-task state
 * (hrtimer, DA state, threshold) lives in tlob_task_state inside tlob.c.
 *
 * The table is keyed on task_struct * (same key as tlob.c), protected
 * by tlob_handles_lock (spinlock, irq-safe).  No get_task_struct()
 * refcount is needed here because tlob.c already holds a reference for
 * each live entry.
 *
 * Multiple threads may share the same fd.  Each thread has its own
 * tlob_task_handle in the table, so concurrent TRACE_START / TRACE_STOP
 * calls from different threads do not interfere.
 *
 * The fd release path (rv_release) calls tlob_stop_task() for every
 * handle in tlob_handles that belongs to the closing fd, ensuring cleanup
 * even if the user forgets to call TRACE_STOP.
 */
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/hash.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <uapi/linux/rv.h>

#ifdef CONFIG_RV_MON_TLOB
#include "monitors/tlob/tlob.h"
#endif

/* -----------------------------------------------------------------------
 * tlob_task_handle - per-thread session ticket for the ioctl interface
 *
 * One handle is allocated by TLOB_IOCTL_TRACE_START and freed by
 * TLOB_IOCTL_TRACE_STOP (or by rv_release if the fd is closed).
 *
 * @hlist:  Hash-table linkage in tlob_handles (keyed on task pointer).
 * @task:   The monitored thread.  Plain pointer; no refcount held here
 *          because tlob.c holds one for the lifetime of the monitoring
 *          window, which encompasses the lifetime of this handle.
 * @file:   The /dev/rv file descriptor that issued TRACE_START.
 *          Used by rv_release() to sweep orphaned handles on close().
 * -----------------------------------------------------------------------
 */
#define TLOB_HANDLES_BITS	5
#define TLOB_HANDLES_SIZE	(1 << TLOB_HANDLES_BITS)

struct tlob_task_handle {
	struct hlist_node	hlist;
	struct task_struct	*task;
	struct file		*file;
};

static struct hlist_head tlob_handles[TLOB_HANDLES_SIZE];
static DEFINE_SPINLOCK(tlob_handles_lock);

static unsigned int tlob_handle_hash(const struct task_struct *task)
{
	return hash_ptr((void *)task, TLOB_HANDLES_BITS);
}

/* Must be called with tlob_handles_lock held. */
static struct tlob_task_handle *
tlob_handle_find_locked(struct task_struct *task)
{
	struct tlob_task_handle *h;
	unsigned int slot = tlob_handle_hash(task);

	hlist_for_each_entry(h, &tlob_handles[slot], hlist) {
		if (h->task == task)
			return h;
	}
	return NULL;
}

/*
 * tlob_handle_alloc - record that @task has an active monitoring session
 *                     opened via @file.
 *
 * Returns 0 on success, -EEXIST if @task already has a handle (double
 * TRACE_START without TRACE_STOP), -ENOMEM on allocation failure.
 */
static int tlob_handle_alloc(struct task_struct *task, struct file *file)
{
	struct tlob_task_handle *h;
	unsigned long flags;
	unsigned int slot;

	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->task = task;
	h->file = file;

	spin_lock_irqsave(&tlob_handles_lock, flags);
	if (tlob_handle_find_locked(task)) {
		spin_unlock_irqrestore(&tlob_handles_lock, flags);
		kfree(h);
		return -EEXIST;
	}
	slot = tlob_handle_hash(task);
	hlist_add_head(&h->hlist, &tlob_handles[slot]);
	spin_unlock_irqrestore(&tlob_handles_lock, flags);
	return 0;
}

/*
 * tlob_handle_free - remove the handle for @task and free it.
 *
 * Returns 1 if a handle existed (TRACE_START was called), 0 if not found
 * (TRACE_START was never called for this thread).
 */
static int tlob_handle_free(struct task_struct *task)
{
	struct tlob_task_handle *h;
	unsigned long flags;

	spin_lock_irqsave(&tlob_handles_lock, flags);
	h = tlob_handle_find_locked(task);
	if (h) {
		hlist_del_init(&h->hlist);
		spin_unlock_irqrestore(&tlob_handles_lock, flags);
		kfree(h);
		return 1;
	}
	spin_unlock_irqrestore(&tlob_handles_lock, flags);
	return 0;
}

/*
 * tlob_handle_sweep_file - release all handles owned by @file.
 *
 * Called from rv_release() when the fd is closed without TRACE_STOP.
 * Calls tlob_stop_task() for each orphaned handle to drain the tlob
 * monitoring entries and prevent resource leaks in tlob.c.
 *
 * Handles are collected under the lock (short critical section), then
 * processed outside it (tlob_stop_task() may sleep/spin internally).
 */
#ifdef CONFIG_RV_MON_TLOB
static void tlob_handle_sweep_file(struct file *file)
{
	struct tlob_task_handle *batch[TLOB_HANDLES_SIZE];
	struct tlob_task_handle *h;
	struct hlist_node *tmp;
	unsigned long flags;
	int i, n = 0;

	spin_lock_irqsave(&tlob_handles_lock, flags);
	for (i = 0; i < TLOB_HANDLES_SIZE; i++) {
		hlist_for_each_entry_safe(h, tmp, &tlob_handles[i], hlist) {
			if (h->file == file) {
				hlist_del_init(&h->hlist);
				batch[n++] = h;
			}
		}
	}
	spin_unlock_irqrestore(&tlob_handles_lock, flags);

	for (i = 0; i < n; i++) {
		/*
		 * Ignore -ESRCH: the deadline timer may have already fired
		 * and cleaned up the tlob entry.
		 */
		tlob_stop_task(batch[i]->task);
		kfree(batch[i]);
	}
}
#else
static inline void tlob_handle_sweep_file(struct file *file) {}
#endif /* CONFIG_RV_MON_TLOB */

/* -----------------------------------------------------------------------
 * Ring buffer lifecycle
 * -----------------------------------------------------------------------
 */

/*
 * tlob_ring_alloc - allocate a ring of @cap records (must be a power of 2).
 *
 * Allocates a physically contiguous block of pages:
 *   page 0     : struct tlob_mmap_page  (control page, shared with userspace)
 *   pages 1..N : struct tlob_event[cap] (data pages)
 *
 * Each page is marked reserved so it can be mapped to userspace via mmap().
 */
static int tlob_ring_alloc(struct tlob_ring *ring, u32 cap)
{
	unsigned int total = PAGE_SIZE + cap * sizeof(struct tlob_event);
	unsigned int order = get_order(total);
	unsigned long base;
	unsigned int i;

	base = __get_free_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!base)
		return -ENOMEM;

	for (i = 0; i < (1u << order); i++)
		SetPageReserved(virt_to_page((void *)(base + i * PAGE_SIZE)));

	ring->base  = base;
	ring->order = order;
	ring->page  = (struct tlob_mmap_page *)base;
	ring->data  = (struct tlob_event *)(base + PAGE_SIZE);
	ring->mask  = cap - 1;
	spin_lock_init(&ring->lock);

	ring->page->capacity    = cap;
	ring->page->version     = 1;
	ring->page->data_offset = PAGE_SIZE;
	ring->page->record_size = sizeof(struct tlob_event);
	return 0;
}

static void tlob_ring_free(struct tlob_ring *ring)
{
	unsigned int i;

	if (!ring->base)
		return;

	for (i = 0; i < (1u << ring->order); i++)
		ClearPageReserved(virt_to_page((void *)(ring->base + i * PAGE_SIZE)));

	free_pages(ring->base, ring->order);
	ring->base = 0;
	ring->page = NULL;
	ring->data = NULL;
}

/* -----------------------------------------------------------------------
 * File operations
 * -----------------------------------------------------------------------
 */

static int rv_open(struct inode *inode, struct file *file)
{
	struct rv_file_priv *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = tlob_ring_alloc(&priv->ring, TLOB_RING_DEFAULT_CAP);
	if (ret) {
		kfree(priv);
		return ret;
	}

	init_waitqueue_head(&priv->waitq);
	file->private_data = priv;
	return 0;
}

static int rv_release(struct inode *inode, struct file *file)
{
	struct rv_file_priv *priv = file->private_data;

	tlob_handle_sweep_file(file);
	tlob_ring_free(&priv->ring);
	kfree(priv);
	file->private_data = NULL;
	return 0;
}

static __poll_t rv_poll(struct file *file, poll_table *wait)
{
	struct rv_file_priv *priv = file->private_data;

	if (!priv)
		return EPOLLERR;

	poll_wait(file, &priv->waitq, wait);

	/*
	 * Pairs with smp_store_release(&ring->page->data_head, ...) in
	 * tlob_event_push().  No lock needed: head is written by the kernel
	 * producer and read here; tail is written by the consumer and we only
	 * need an approximate check for the poll fast path.
	 */
	if (smp_load_acquire(&priv->ring.page->data_head) !=
	    READ_ONCE(priv->ring.page->data_tail))
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

/*
 * rv_read - consume tlob_event violation records from this fd's ring buffer.
 *
 * Each read() returns a whole number of struct tlob_event records.  @count must
 * be at least sizeof(struct tlob_event); partial-record sizes are rejected with
 * -EINVAL.
 *
 * Blocking behaviour follows O_NONBLOCK on the fd:
 *   O_NONBLOCK clear: blocks until at least one record is available.
 *   O_NONBLOCK set:   returns -EAGAIN immediately if the ring is empty.
 *
 * Returns the number of bytes copied (always a multiple of sizeof tlob_event),
 * -EAGAIN if non-blocking and empty, or a negative error code.
 *
 * read() and mmap() share the same ring and data_tail cursor; do not use
 * both simultaneously on the same fd.
 */
static ssize_t rv_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	struct rv_file_priv *priv = file->private_data;
	struct tlob_ring *ring;
	size_t rec = sizeof(struct tlob_event);
	unsigned long irqflags;
	ssize_t done = 0;
	int ret;

	if (!priv)
		return -ENODEV;

	ring = &priv->ring;

	if (count < rec)
		return -EINVAL;

	/* Blocking path: sleep until the producer advances data_head. */
	if (!(file->f_flags & O_NONBLOCK)) {
		ret = wait_event_interruptible(priv->waitq,
			/* pairs with smp_store_release() in the producer */
			smp_load_acquire(&ring->page->data_head) !=
			READ_ONCE(ring->page->data_tail));
		if (ret)
			return ret;
	}

	/*
	 * Drain records into the caller's buffer.  ring->lock serialises
	 * concurrent read() callers and the softirq producer.
	 */
	while (done + rec <= count) {
		struct tlob_event record;
		u32 head, tail;

		spin_lock_irqsave(&ring->lock, irqflags);
		/* pairs with smp_store_release() in the producer */
		head = smp_load_acquire(&ring->page->data_head);
		tail = ring->page->data_tail;
		if (head == tail) {
			spin_unlock_irqrestore(&ring->lock, irqflags);
			break;
		}
		record = ring->data[tail & ring->mask];
		WRITE_ONCE(ring->page->data_tail, tail + 1);
		spin_unlock_irqrestore(&ring->lock, irqflags);

		if (copy_to_user(buf + done, &record, rec))
			return done ? done : -EFAULT;
		done += rec;
	}

	return done ? done : -EAGAIN;
}

/*
 * rv_mmap - map the per-fd violation ring buffer into userspace.
 *
 * The mmap region covers the full ring allocation:
 *
 *   offset 0          : struct tlob_mmap_page  (control page)
 *   offset PAGE_SIZE  : struct tlob_event[capacity]  (data pages)
 *
 * The caller must map exactly PAGE_SIZE + capacity * sizeof(struct tlob_event)
 * bytes starting at offset 0 (vm_pgoff must be 0).  The actual capacity is
 * read from tlob_mmap_page.capacity after a successful mmap(2).
 *
 * Private mappings (MAP_PRIVATE) are rejected: the shared data_tail field
 * written by userspace must be visible to the kernel producer.
 */
static int rv_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct rv_file_priv *priv = file->private_data;
	struct tlob_ring    *ring;
	unsigned long        size = vma->vm_end - vma->vm_start;
	unsigned long        ring_size;

	if (!priv)
		return -ENODEV;

	ring = &priv->ring;

	if (vma->vm_pgoff != 0)
		return -EINVAL;

	ring_size = PAGE_ALIGN(PAGE_SIZE + ((unsigned long)(ring->mask + 1) *
					    sizeof(struct tlob_event)));
	if (size != ring_size)
		return -EINVAL;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			       page_to_pfn(virt_to_page((void *)ring->base)),
			       ring_size, vma->vm_page_prot);
}

/* -----------------------------------------------------------------------
 * ioctl dispatcher
 * -----------------------------------------------------------------------
 */

static long rv_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned int nr = _IOC_NR(cmd);

	/*
	 * Verify the magic byte so we don't accidentally handle ioctls
	 * intended for a different device.
	 */
	if (_IOC_TYPE(cmd) != RV_IOC_MAGIC)
		return -ENOTTY;

#ifdef CONFIG_RV_MON_TLOB
	/* tlob: ioctl numbers 0x01 - 0x1F */
	switch (cmd) {
	case TLOB_IOCTL_TRACE_START: {
		struct tlob_start_args args;
		struct file *notify_file = NULL;
		int ret, hret;

		if (copy_from_user(&args,
				   (struct tlob_start_args __user *)arg,
				   sizeof(args)))
			return -EFAULT;
		if (args.threshold_us == 0)
			return -EINVAL;
		if (args.flags != 0)
			return -EINVAL;

		/*
		 * If notify_fd >= 0, resolve it to a file pointer.
		 * fget() bumps the reference count; tlob.c drops it
		 * via fput() when the monitoring window ends.
		 * Reject non-/dev/rv fds to prevent type confusion.
		 */
		if (args.notify_fd >= 0) {
			notify_file = fget(args.notify_fd);
			if (!notify_file)
				return -EBADF;
			if (notify_file->f_op != file->f_op) {
				fput(notify_file);
				return -EINVAL;
			}
		}

		ret = tlob_start_task(current, args.threshold_us,
				      notify_file, args.tag);
		if (ret != 0) {
			/* tlob.c did not take ownership; drop ref. */
			if (notify_file)
				fput(notify_file);
			return ret;
		}

		/*
		 * Record session handle.  Free any stale handle left by
		 * a previous window whose deadline timer fired (timer
		 * removes tlob_task_state but cannot touch tlob_handles).
		 */
		tlob_handle_free(current);
		hret = tlob_handle_alloc(current, file);
		if (hret < 0) {
			tlob_stop_task(current);
			return hret;
		}
		return 0;
	}
	case TLOB_IOCTL_TRACE_STOP: {
		int had_handle;
		int ret;

		/*
		 * Atomically remove the session handle for current.
		 *
		 *   had_handle == 0: TRACE_START was never called for
		 *                    this thread -> caller bug -> -ESRCH
		 *
		 *   had_handle == 1: TRACE_START was called.  If
		 *                    tlob_stop_task() now returns
		 *                    -ESRCH, the deadline timer already
		 *                    fired -> budget exceeded -> -EOVERFLOW
		 */
		had_handle = tlob_handle_free(current);
		if (!had_handle)
			return -ESRCH;

		ret = tlob_stop_task(current);
		return (ret == -ESRCH) ? -EOVERFLOW : ret;
	}
	default:
		break;
	}
#endif /* CONFIG_RV_MON_TLOB */

	return -ENOTTY;
}

/* -----------------------------------------------------------------------
 * Module init / exit
 * -----------------------------------------------------------------------
 */

static const struct file_operations rv_fops = {
	.owner		= THIS_MODULE,
	.open		= rv_open,
	.release	= rv_release,
	.read		= rv_read,
	.poll		= rv_poll,
	.mmap		= rv_mmap,
	.unlocked_ioctl	= rv_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= rv_ioctl,
#endif
	.llseek		= noop_llseek,
};

/*
 * 0666: /dev/rv is a self-instrumentation device.  All ioctls operate
 * exclusively on the calling task (current); no task can monitor another
 * via this interface.  Opening the device does not grant any privilege
 * beyond observing one's own latency, so world-read/write is appropriate.
 */
static struct miscdevice rv_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "rv",
	.fops	= &rv_fops,
	.mode	= 0666,
};

static int __init rv_ioctl_init(void)
{
	int i;

	for (i = 0; i < TLOB_HANDLES_SIZE; i++)
		INIT_HLIST_HEAD(&tlob_handles[i]);

	return misc_register(&rv_miscdev);
}

static void __exit rv_ioctl_exit(void)
{
	misc_deregister(&rv_miscdev);
}

module_init(rv_ioctl_init);
module_exit(rv_ioctl_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RV ioctl interface via /dev/rv");
