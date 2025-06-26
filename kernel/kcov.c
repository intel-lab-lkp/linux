// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "kcov: " fmt

#define DISABLE_BRANCH_PROFILING
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kcov.h>
#include <linux/kcov_types.h>
#include <linux/kmsan-checks.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <asm/setup.h>

#ifdef CONFIG_KCOV_UNIQUE
atomic_t kcov_guard_max_index = ATOMIC_INIT(0);
#endif

#define kcov_debug(fmt, ...) pr_debug("%s: " fmt, __func__, ##__VA_ARGS__)

/* Number of 64-bit words written per one comparison: */
#define KCOV_WORDS_PER_CMP 4

/*
 * kcov descriptor (one per opened debugfs file).
 * State transitions of the descriptor:
 *  - initial state after open()
 *  - then there must be a single ioctl(KCOV_INIT_TRACE) call
 *  - then, mmap() call (several calls are allowed but not useful)
 *  - then, ioctl(KCOV_ENABLE, arg), where arg is
 *	KCOV_TRACE_PC - to trace only the PCs
 *	or
 *	KCOV_TRACE_CMP - to trace only the comparison operands
 *  - then, ioctl(KCOV_DISABLE) to disable the task.
 * Enabling/disabling ioctls can be repeated (only one task a time allowed).
 */
struct kcov {
	/*
	 * Reference counter. We keep one for:
	 *  - opened file descriptor
	 *  - task with enabled coverage (we can't unwire it from another task)
	 *  - each code section for remote coverage collection
	 */
	refcount_t refcount;
	/* The lock protects state and t. */
	spinlock_t lock;
	enum kcov_mode mode;
	struct kcov_state state;

	/* Task for which we collect coverage, or NULL. */
	struct task_struct *t;
	/* Collecting coverage from remote (background) threads. */
	bool remote;
	/* Size of remote area (in long's). */
	unsigned int remote_size;
};

struct kcov_remote_area {
	struct list_head list;
	unsigned int size;
};

struct kcov_remote {
	u64 handle;
	struct kcov *kcov;
	struct hlist_node hnode;
};

static DEFINE_SPINLOCK(kcov_remote_lock);
static DEFINE_HASHTABLE(kcov_remote_map, 4);
static struct list_head kcov_remote_areas = LIST_HEAD_INIT(kcov_remote_areas);

struct kcov_percpu_data {
	void *irq_area;
	local_lock_t lock;
	enum kcov_mode saved_mode;
	struct kcov *saved_kcov;
	struct kcov_state saved_state;
};

static DEFINE_PER_CPU(struct kcov_percpu_data, kcov_percpu_data) = {
	.lock = INIT_LOCAL_LOCK(lock),
};

/* Must be called with kcov_remote_lock locked. */
static struct kcov_remote *kcov_remote_find(u64 handle)
{
	struct kcov_remote *remote;

	hash_for_each_possible(kcov_remote_map, remote, hnode, handle) {
		if (remote->handle == handle)
			return remote;
	}
	return NULL;
}

/* Must be called with kcov_remote_lock locked. */
static struct kcov_remote *kcov_remote_add(struct kcov *kcov, u64 handle)
{
	struct kcov_remote *remote;

	if (kcov_remote_find(handle))
		return ERR_PTR(-EEXIST);
	remote = kmalloc(sizeof(*remote), GFP_ATOMIC);
	if (!remote)
		return ERR_PTR(-ENOMEM);
	remote->handle = handle;
	remote->kcov = kcov;
	hash_add(kcov_remote_map, &remote->hnode, handle);
	return remote;
}

/* Must be called with kcov_remote_lock locked. */
static struct kcov_remote_area *kcov_remote_area_get(unsigned int size)
{
	struct kcov_remote_area *area;
	struct list_head *pos;

	list_for_each(pos, &kcov_remote_areas) {
		area = list_entry(pos, struct kcov_remote_area, list);
		if (area->size == size) {
			list_del(&area->list);
			return area;
		}
	}
	return NULL;
}

/* Must be called with kcov_remote_lock locked. */
static void kcov_remote_area_put(struct kcov_remote_area *area,
				 unsigned int size)
{
	INIT_LIST_HEAD(&area->list);
	area->size = size;
	list_add(&area->list, &kcov_remote_areas);
	/*
	 * KMSAN doesn't instrument this file, so it may not know area->list
	 * is initialized. Unpoison it explicitly to avoid reports in
	 * kcov_remote_area_get().
	 */
	kmsan_unpoison_memory(&area->list, sizeof(area->list));
}

/*
 * Unlike in_serving_softirq(), this function returns false when called during
 * a hardirq or an NMI that happened in the softirq context.
 */
static __always_inline bool in_softirq_really(void)
{
	return in_serving_softirq() && !in_hardirq() && !in_nmi();
}

static notrace enum kcov_mode get_kcov_mode(struct task_struct *t)
{
	enum kcov_mode mode;

	/*
	 * We are interested in code coverage as a function of a syscall inputs,
	 * so we ignore code executed in interrupts, unless we are in a remote
	 * coverage collection section in a softirq.
	 */
	if (!in_task() && !(in_softirq_really() && t->kcov_softirq))
		return KCOV_MODE_INVALID;
	mode = READ_ONCE(t->kcov_mode);
	/*
	 * There is some code that runs in interrupts but for which
	 * in_interrupt() returns false (e.g. preempt_schedule_irq()).
	 * READ_ONCE()/barrier() effectively provides load-acquire wrt
	 * interrupts, there are paired barrier()/WRITE_ONCE() in
	 * kcov_start().
	 */
	barrier();
	return mode;
}

static notrace unsigned long canonicalize_ip(unsigned long ip)
{
#ifdef CONFIG_RANDOMIZE_BASE
	ip -= kaslr_offset();
#endif
	return ip;
}

static notrace void kcov_append_to_buffer(unsigned long *trace, int size,
					  unsigned long ip)
{
	/* The first 64-bit word is the number of subsequent PCs. */
	unsigned long pos = READ_ONCE(trace[0]) + 1;

	if (likely(pos < size)) {
		/*
		 * Some early interrupt code could bypass get_kcov_mode() check
		 * and invoke __sanitizer_cov_trace_pc(). If such interrupt is
		 * raised between writing pc and updating pos, the pc could be
		 * overitten by the recursive __sanitizer_cov_trace_pc().
		 * Update pos before writing pc to avoid such interleaving.
		 */
		WRITE_ONCE(trace[0], pos);
		barrier();
		trace[pos] = ip;
	}
}

/*
 * Entry point from instrumented code.
 * This is called once per basic-block/edge.
 */
#ifdef CONFIG_KCOV_UNIQUE
DEFINE_PER_CPU(u32, saved_index);
/*
 * Assign an index to a guard variable that does not have one yet.
 * For an unlikely case of a race with another task executing the same basic
 * block for the first time with kcov enabled, we store the unused index in a
 * per-cpu variable.
 * In an even less likely case of the current task losing the race and getting
 * rescheduled onto a CPU that already has a saved index, the index is
 * discarded. This will result in an unused hole in the bitmap, but such events
 * should have minor impact on the overall memory consumption.
 */
static __always_inline u32 init_pc_guard(u32 *guard)
{
	/* If the current CPU has a saved free index, use it. */
	u32 index = this_cpu_xchg(saved_index, 0);
	u32 old_guard;

	if (likely(!index))
		/*
		 * Allocate a new index. No overflow is possible, because 2**32
		 * unique basic blocks will take more space than the max size
		 * of the kernel text segment.
		 */
		index = atomic_inc_return(&kcov_guard_max_index);

	/*
	 * Make sure another task is not initializing the same guard
	 * concurrently.
	 */
	old_guard = cmpxchg(guard, 0, index);
	if (unlikely(old_guard)) {
		/* We lost the race, save the index for future use. */
		this_cpu_write(saved_index, index);
		return old_guard;
	}
	return index;
}

void notrace __sanitizer_cov_trace_pc_guard(u32 *guard)
{
	enum kcov_mode mode = get_kcov_mode(current);
	u32 pc_index;

	switch (mode) {
	case KCOV_MODE_UNIQUE_PC:
		pc_index = READ_ONCE(*guard);
		if (unlikely(!pc_index))
			pc_index = init_pc_guard(guard);

		/*
		 * Use the bitmap for coverage deduplication. We assume both
		 * s.bitmap and s.trace are non-NULL.
		 */
		if (likely(pc_index < current->kcov_state.bitmap_size))
			if (__test_and_set_bit(pc_index,
					       current->kcov_state.bitmap))
				return;
		/*
		 * If the PC is new, or the bitmap is too small, write PC to the
		 * trace.
		 */
		fallthrough;
	case KCOV_MODE_TRACE_PC:
		kcov_append_to_buffer(current->kcov_state.trace,
				      current->kcov_state.trace_size,
				      canonicalize_ip(_RET_IP_));
		break;
	default:
		return;
	}
}
EXPORT_SYMBOL(__sanitizer_cov_trace_pc_guard);

void notrace __sanitizer_cov_trace_pc_guard_init(uint32_t *start,
						 uint32_t *stop)
{
}
EXPORT_SYMBOL(__sanitizer_cov_trace_pc_guard_init);
#else /* !CONFIG_KCOV_UNIQUE */
void notrace __sanitizer_cov_trace_pc(void)
{
	if (get_kcov_mode(current) != KCOV_MODE_TRACE_PC)
		return;

	kcov_append_to_buffer(current->kcov_state.trace,
			      current->kcov_state.trace_size,
			      canonicalize_ip(_RET_IP_));
}
EXPORT_SYMBOL(__sanitizer_cov_trace_pc);
#endif

#ifdef CONFIG_KCOV_ENABLE_COMPARISONS
static void notrace write_comp_data(u64 type, u64 arg1, u64 arg2, u64 ip)
{
	u64 count, start_index, end_pos, max_pos;
	struct task_struct *t;
	u64 *trace;

	t = current;
	if (get_kcov_mode(t) != KCOV_MODE_TRACE_CMP)
		return;

	ip = canonicalize_ip(ip);

	/*
	 * We write all comparison arguments and types as u64.
	 * The buffer was allocated for t->kcov_state.size unsigned longs.
	 */
	trace = (u64 *)t->kcov_state.trace;
	max_pos = t->kcov_state.size * sizeof(unsigned long);

	count = READ_ONCE(trace[0]);

	/* Every record is KCOV_WORDS_PER_CMP 64-bit words. */
	start_index = 1 + count * KCOV_WORDS_PER_CMP;
	end_pos = (start_index + KCOV_WORDS_PER_CMP) * sizeof(u64);
	if (likely(end_pos <= max_pos)) {
		/* See comment in kcov_append_to_buffer(). */
		WRITE_ONCE(trace[0], count + 1);
		barrier();
		trace[start_index] = type;
		trace[start_index + 1] = arg1;
		trace[start_index + 2] = arg2;
		trace[start_index + 3] = ip;
	}
}

void notrace __sanitizer_cov_trace_cmp1(u8 arg1, u8 arg2)
{
	write_comp_data(KCOV_CMP_SIZE(0), arg1, arg2, _RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_cmp1);

void notrace __sanitizer_cov_trace_cmp2(u16 arg1, u16 arg2)
{
	write_comp_data(KCOV_CMP_SIZE(1), arg1, arg2, _RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_cmp2);

void notrace __sanitizer_cov_trace_cmp4(u32 arg1, u32 arg2)
{
	write_comp_data(KCOV_CMP_SIZE(2), arg1, arg2, _RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_cmp4);

void notrace __sanitizer_cov_trace_cmp8(kcov_u64 arg1, kcov_u64 arg2)
{
	write_comp_data(KCOV_CMP_SIZE(3), arg1, arg2, _RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_cmp8);

void notrace __sanitizer_cov_trace_const_cmp1(u8 arg1, u8 arg2)
{
	write_comp_data(KCOV_CMP_SIZE(0) | KCOV_CMP_CONST, arg1, arg2,
			_RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_const_cmp1);

void notrace __sanitizer_cov_trace_const_cmp2(u16 arg1, u16 arg2)
{
	write_comp_data(KCOV_CMP_SIZE(1) | KCOV_CMP_CONST, arg1, arg2,
			_RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_const_cmp2);

void notrace __sanitizer_cov_trace_const_cmp4(u32 arg1, u32 arg2)
{
	write_comp_data(KCOV_CMP_SIZE(2) | KCOV_CMP_CONST, arg1, arg2,
			_RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_const_cmp4);

void notrace __sanitizer_cov_trace_const_cmp8(kcov_u64 arg1, kcov_u64 arg2)
{
	write_comp_data(KCOV_CMP_SIZE(3) | KCOV_CMP_CONST, arg1, arg2,
			_RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_const_cmp8);

void notrace __sanitizer_cov_trace_switch(kcov_u64 val, void *arg)
{
	u64 i;
	u64 *cases = arg;
	u64 count = cases[0];
	u64 size = cases[1];
	u64 type = KCOV_CMP_CONST;

	switch (size) {
	case 8:
		type |= KCOV_CMP_SIZE(0);
		break;
	case 16:
		type |= KCOV_CMP_SIZE(1);
		break;
	case 32:
		type |= KCOV_CMP_SIZE(2);
		break;
	case 64:
		type |= KCOV_CMP_SIZE(3);
		break;
	default:
		return;
	}
	for (i = 0; i < count; i++)
		write_comp_data(type, cases[i + 2], val, _RET_IP_);
}
EXPORT_SYMBOL(__sanitizer_cov_trace_switch);
#endif /* ifdef CONFIG_KCOV_ENABLE_COMPARISONS */

static void kcov_start(struct task_struct *t, struct kcov *kcov,
		       enum kcov_mode mode, struct kcov_state *state)
{
	kcov_debug("t = %px, size = %u, area = %px\n", t, state->size,
		   state->area);
	t->kcov = kcov;
	/* Cache in task struct for performance. */
	t->kcov_state = *state;
	barrier();
	/* See comment in get_kcov_mode(). */
	WRITE_ONCE(t->kcov_mode, mode);
}

static void kcov_stop(struct task_struct *t)
{
	int saved_sequence = t->kcov_state.sequence;

	WRITE_ONCE(t->kcov_mode, KCOV_MODE_DISABLED);
	barrier();
	t->kcov = NULL;
	t->kcov_state = (typeof(t->kcov_state)){ 0 };
	t->kcov_state.sequence = saved_sequence;
}

static void kcov_task_reset(struct task_struct *t)
{
	kcov_stop(t);
	t->kcov_state.sequence = 0;
	t->kcov_handle = 0;
}

void kcov_task_init(struct task_struct *t)
{
	kcov_task_reset(t);
	t->kcov_handle = current->kcov_handle;
}

static void kcov_reset(struct kcov *kcov)
{
	kcov->t = NULL;
	kcov->mode = KCOV_MODE_INIT;
	kcov->remote = false;
	kcov->remote_size = 0;
	kcov->state.trace = kcov->state.area;
	kcov->state.trace_size = kcov->state.size;
	kcov->state.bitmap = NULL;
	kcov->state.bitmap_size = 0;
	kcov->state.sequence++;
}

static void kcov_remote_reset(struct kcov *kcov)
{
	int bkt;
	struct kcov_remote *remote;
	struct hlist_node *tmp;
	unsigned long flags;

	spin_lock_irqsave(&kcov_remote_lock, flags);
	hash_for_each_safe(kcov_remote_map, bkt, tmp, remote, hnode) {
		if (remote->kcov != kcov)
			continue;
		hash_del(&remote->hnode);
		kfree(remote);
	}
	/* Do reset before unlock to prevent races with kcov_remote_start(). */
	kcov_reset(kcov);
	spin_unlock_irqrestore(&kcov_remote_lock, flags);
}

static void kcov_disable(struct task_struct *t, struct kcov *kcov)
{
	kcov_task_reset(t);
	if (kcov->remote)
		kcov_remote_reset(kcov);
	else
		kcov_reset(kcov);
}

static void kcov_get(struct kcov *kcov)
{
	refcount_inc(&kcov->refcount);
}

static void kcov_put(struct kcov *kcov)
{
	if (refcount_dec_and_test(&kcov->refcount)) {
		kcov_remote_reset(kcov);
		vfree(kcov->state.area);
		kfree(kcov);
	}
}

void kcov_task_exit(struct task_struct *t)
{
	struct kcov *kcov;
	unsigned long flags;

	kcov = t->kcov;
	if (kcov == NULL)
		return;

	spin_lock_irqsave(&kcov->lock, flags);
	kcov_debug("t = %px, kcov->t = %px\n", t, kcov->t);
	/*
	 * For KCOV_ENABLE devices we want to make sure that t->kcov->t == t,
	 * which comes down to:
	 *        WARN_ON(!kcov->remote && kcov->t != t);
	 *
	 * For KCOV_REMOTE_ENABLE devices, the exiting task is either:
	 *
	 * 1. A remote task between kcov_remote_start() and kcov_remote_stop().
	 *    In this case we should print a warning right away, since a task
	 *    shouldn't be exiting when it's in a kcov coverage collection
	 *    section. Here t points to the task that is collecting remote
	 *    coverage, and t->kcov->t points to the thread that created the
	 *    kcov device. Which means that to detect this case we need to
	 *    check that t != t->kcov->t, and this gives us the following:
	 *        WARN_ON(kcov->remote && kcov->t != t);
	 *
	 * 2. The task that created kcov exiting without calling KCOV_DISABLE,
	 *    and then again we make sure that t->kcov->t == t:
	 *        WARN_ON(kcov->remote && kcov->t != t);
	 *
	 * By combining all three checks into one we get:
	 */
	if (WARN_ON(kcov->t != t)) {
		spin_unlock_irqrestore(&kcov->lock, flags);
		return;
	}
	/* Just to not leave dangling references behind. */
	kcov_disable(t, kcov);
	spin_unlock_irqrestore(&kcov->lock, flags);
	kcov_put(kcov);
}

static int kcov_mmap(struct file *filep, struct vm_area_struct *vma)
{
	int res = 0;
	struct kcov *kcov = vma->vm_file->private_data;
	unsigned long size, off;
	struct page *page;
	unsigned long flags;

	spin_lock_irqsave(&kcov->lock, flags);
	size = kcov->state.size * sizeof(unsigned long);
	if (kcov->state.area == NULL || vma->vm_pgoff != 0 ||
	    vma->vm_end - vma->vm_start != size) {
		res = -EINVAL;
		goto exit;
	}
	spin_unlock_irqrestore(&kcov->lock, flags);
	vm_flags_set(vma, VM_DONTEXPAND);
	for (off = 0; off < size; off += PAGE_SIZE) {
		page = vmalloc_to_page(kcov->state.area + off);
		res = vm_insert_page(vma, vma->vm_start + off, page);
		if (res) {
			pr_warn_once("kcov: vm_insert_page() failed\n");
			return res;
		}
	}
	return 0;
exit:
	spin_unlock_irqrestore(&kcov->lock, flags);
	return res;
}

static int kcov_open(struct inode *inode, struct file *filep)
{
	struct kcov *kcov;

	kcov = kzalloc(sizeof(*kcov), GFP_KERNEL);
	if (!kcov)
		return -ENOMEM;
	kcov->mode = KCOV_MODE_DISABLED;
	kcov->state.sequence = 1;
	refcount_set(&kcov->refcount, 1);
	spin_lock_init(&kcov->lock);
	filep->private_data = kcov;
	return nonseekable_open(inode, filep);
}

static int kcov_close(struct inode *inode, struct file *filep)
{
	kcov_put(filep->private_data);
	return 0;
}

static enum kcov_mode kcov_arg_to_mode(unsigned long arg, int *error)
{
	if (arg == KCOV_TRACE_PC) {
		return KCOV_MODE_TRACE_PC;
	} else if (arg == KCOV_TRACE_CMP) {
#ifdef CONFIG_KCOV_ENABLE_COMPARISONS
		return KCOV_MODE_TRACE_CMP;
#else
		if (error)
			*error = -ENOTSUPP;
		return KCOV_MODE_INVALID;
#endif
	} else {
		if (error)
			*error = -EINVAL;
		return KCOV_MODE_INVALID;
	}
}

/*
 * Fault in a lazily-faulted vmalloc area before it can be used by
 * __santizer_cov_trace_pc(), to avoid recursion issues if any code on the
 * vmalloc fault handling path is instrumented.
 */
static void kcov_fault_in_area(struct kcov *kcov)
{
	unsigned long stride = PAGE_SIZE / sizeof(unsigned long);
	unsigned long *area = kcov->state.area;
	unsigned long offset;

	for (offset = 0; offset < kcov->state.size; offset += stride)
		READ_ONCE(area[offset]);
}

static inline bool kcov_check_handle(u64 handle, bool common_valid,
				     bool uncommon_valid, bool zero_valid)
{
	if (handle & ~(KCOV_SUBSYSTEM_MASK | KCOV_INSTANCE_MASK))
		return false;
	switch (handle & KCOV_SUBSYSTEM_MASK) {
	case KCOV_SUBSYSTEM_COMMON:
		return (handle & KCOV_INSTANCE_MASK) ? common_valid :
						       zero_valid;
	case KCOV_SUBSYSTEM_USB:
		return uncommon_valid;
	default:
		return false;
	}
	return false;
}

static long kcov_handle_unique_enable(struct kcov *kcov,
				      unsigned long bitmap_words)
{
	struct task_struct *t = current;

	if (!IS_ENABLED(CONFIG_KCOV_UNIQUE))
		return -ENOTSUPP;
	if (kcov->mode != KCOV_MODE_INIT || !kcov->state.area)
		return -EINVAL;
	if (kcov->t != NULL || t->kcov != NULL)
		return -EBUSY;

	/*
	 * Cannot use zero-sized bitmap, also the bitmap must leave at least two
	 * words for the trace.
	 */
	if ((!bitmap_words) || (bitmap_words >= (kcov->state.size - 1)))
		return -EINVAL;

	kcov->state.bitmap_size = bitmap_words * sizeof(unsigned long) * 8;
	kcov->state.bitmap = kcov->state.area;
	kcov->state.trace_size = kcov->state.size - bitmap_words;
	kcov->state.trace = ((unsigned long *)kcov->state.area + bitmap_words);

	kcov_fault_in_area(kcov);
	kcov->mode = KCOV_MODE_UNIQUE_PC;
	kcov_start(t, kcov, kcov->mode, &kcov->state);
	kcov->t = t;
	/* Put either in kcov_task_exit() or in KCOV_DISABLE. */
	kcov_get(kcov);

	return 0;
}

static int kcov_ioctl_locked(struct kcov *kcov, unsigned int cmd,
			     unsigned long arg)
{
	struct task_struct *t;
	unsigned long flags, unused;
	enum kcov_mode mode;
	int error = 0, i;
	struct kcov_remote_arg *remote_arg;
	struct kcov_remote *remote;

	switch (cmd) {
	case KCOV_ENABLE:
		/*
		 * Enable coverage for the current task.
		 * At this point user must have been enabled trace mode,
		 * and mmapped the file. Coverage collection is disabled only
		 * at task exit or voluntary by KCOV_DISABLE. After that it can
		 * be enabled for another task.
		 */
		if (kcov->mode != KCOV_MODE_INIT || !kcov->state.area)
			return -EINVAL;
		t = current;
		if (kcov->t != NULL || t->kcov != NULL)
			return -EBUSY;
		mode = kcov_arg_to_mode(arg, &error);
		if (mode == KCOV_MODE_INVALID)
			return error;
		kcov_fault_in_area(kcov);
		kcov->mode = mode;
		kcov_start(t, kcov, mode, &kcov->state);
		kcov->t = t;
		/* Put either in kcov_task_exit() or in KCOV_DISABLE. */
		kcov_get(kcov);
		return 0;
	case KCOV_UNIQUE_ENABLE:
		return kcov_handle_unique_enable(kcov, arg);
	case KCOV_RESET_TRACE:
		unused = arg;
		if (unused != 0 || current->kcov != kcov)
			return -EINVAL;
		t = current;
		if (WARN_ON(kcov->t != t))
			return -EINVAL;
		mode = kcov->mode;
		if (mode < KCOV_MODE_TRACE_PC)
			return -EINVAL;
		if (kcov->state.bitmap)
			bitmap_zero(kcov->state.bitmap,
				    kcov->state.bitmap_size);
		WRITE_ONCE(kcov->state.trace[0], 0);
		return 0;
	case KCOV_DISABLE:
		/* Disable coverage for the current task. */
		unused = arg;
		if (unused != 0 || current->kcov != kcov)
			return -EINVAL;
		t = current;
		if (WARN_ON(kcov->t != t))
			return -EINVAL;
		kcov_disable(t, kcov);
		kcov_put(kcov);
		return 0;
	case KCOV_REMOTE_ENABLE:
		if (kcov->mode != KCOV_MODE_INIT || !kcov->state.area)
			return -EINVAL;
		t = current;
		if (kcov->t != NULL || t->kcov != NULL)
			return -EBUSY;
		remote_arg = (struct kcov_remote_arg *)arg;
		mode = kcov_arg_to_mode(remote_arg->trace_mode, &error);
		if (mode == KCOV_MODE_INVALID)
			return error;
		if ((unsigned long)remote_arg->area_size >
		    LONG_MAX / sizeof(unsigned long))
			return -EINVAL;
		kcov->mode = mode;
		t->kcov = kcov;
		t->kcov_mode = KCOV_MODE_REMOTE;
		kcov->t = t;
		kcov->remote = true;
		kcov->remote_size = remote_arg->area_size;
		spin_lock_irqsave(&kcov_remote_lock, flags);
		for (i = 0; i < remote_arg->num_handles; i++) {
			if (!kcov_check_handle(remote_arg->handles[i], false,
					       true, false)) {
				spin_unlock_irqrestore(&kcov_remote_lock,
						       flags);
				kcov_disable(t, kcov);
				return -EINVAL;
			}
			remote = kcov_remote_add(kcov, remote_arg->handles[i]);
			if (IS_ERR(remote)) {
				spin_unlock_irqrestore(&kcov_remote_lock,
						       flags);
				kcov_disable(t, kcov);
				return PTR_ERR(remote);
			}
		}
		if (remote_arg->common_handle) {
			if (!kcov_check_handle(remote_arg->common_handle, true,
					       false, false)) {
				spin_unlock_irqrestore(&kcov_remote_lock,
						       flags);
				kcov_disable(t, kcov);
				return -EINVAL;
			}
			remote = kcov_remote_add(kcov,
						 remote_arg->common_handle);
			if (IS_ERR(remote)) {
				spin_unlock_irqrestore(&kcov_remote_lock,
						       flags);
				kcov_disable(t, kcov);
				return PTR_ERR(remote);
			}
			t->kcov_handle = remote_arg->common_handle;
		}
		spin_unlock_irqrestore(&kcov_remote_lock, flags);
		/* Put either in kcov_task_exit() or in KCOV_DISABLE. */
		kcov_get(kcov);
		return 0;
	default:
		return -ENOTTY;
	}
}

static long kcov_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct kcov *kcov;
	int res;
	struct kcov_remote_arg *remote_arg = NULL;
	unsigned int remote_num_handles;
	unsigned long remote_arg_size;
	unsigned long size, flags;
	size_t handles_off;
	void *area;

	kcov = filep->private_data;
	switch (cmd) {
	case KCOV_INIT_TRACE:
		/*
		 * Enable kcov in trace mode and setup buffer size.
		 * Must happen before anything else.
		 *
		 * First check the size argument - it must be at least 2
		 * to hold the current position and one PC.
		 */
		size = arg;
		if (size < 2 || size > INT_MAX / sizeof(unsigned long))
			return -EINVAL;
		area = vmalloc_user(size * sizeof(unsigned long));
		if (area == NULL)
			return -ENOMEM;
		spin_lock_irqsave(&kcov->lock, flags);
		if (kcov->mode != KCOV_MODE_DISABLED) {
			spin_unlock_irqrestore(&kcov->lock, flags);
			vfree(area);
			return -EBUSY;
		}
		kcov->state.area = area;
		kcov->state.size = size;
		kcov->state.trace = area;
		kcov->state.trace_size = size;
		kcov->mode = KCOV_MODE_INIT;
		spin_unlock_irqrestore(&kcov->lock, flags);
		return 0;
	case KCOV_REMOTE_ENABLE:
		handles_off = offsetof(struct kcov_remote_arg, num_handles);
		if (get_user(remote_num_handles,
			     (unsigned __user *)(arg + handles_off)))
			return -EFAULT;
		if (remote_num_handles > KCOV_REMOTE_MAX_HANDLES)
			return -EINVAL;
		remote_arg_size =
			struct_size(remote_arg, handles, remote_num_handles);
		remote_arg = memdup_user((void __user *)arg, remote_arg_size);
		if (IS_ERR(remote_arg))
			return PTR_ERR(remote_arg);
		if (remote_arg->num_handles != remote_num_handles) {
			kfree(remote_arg);
			return -EINVAL;
		}
		arg = (unsigned long)remote_arg;
		fallthrough;
	default:
		/*
		 * All other commands can be normally executed under a spin lock, so we
		 * obtain and release it here in order to simplify kcov_ioctl_locked().
		 */
		spin_lock_irqsave(&kcov->lock, flags);
		res = kcov_ioctl_locked(kcov, cmd, arg);
		spin_unlock_irqrestore(&kcov->lock, flags);
		kfree(remote_arg);
		return res;
	}
}

static const struct file_operations kcov_fops = {
	.open = kcov_open,
	.unlocked_ioctl = kcov_ioctl,
	.compat_ioctl = kcov_ioctl,
	.mmap = kcov_mmap,
	.release = kcov_close,
};

/*
 * kcov_remote_start() and kcov_remote_stop() can be used to annotate a section
 * of code in a kernel background thread or in a softirq to allow kcov to be
 * used to collect coverage from that part of code.
 *
 * The handle argument of kcov_remote_start() identifies a code section that is
 * used for coverage collection. A userspace process passes this handle to
 * KCOV_REMOTE_ENABLE ioctl to make the used kcov device start collecting
 * coverage for the code section identified by this handle.
 *
 * The usage of these annotations in the kernel code is different depending on
 * the type of the kernel thread whose code is being annotated.
 *
 * For global kernel threads that are spawned in a limited number of instances
 * (e.g. one USB hub_event() worker thread is spawned per USB HCD) and for
 * softirqs, each instance must be assigned a unique 4-byte instance id. The
 * instance id is then combined with a 1-byte subsystem id to get a handle via
 * kcov_remote_handle(subsystem_id, instance_id).
 *
 * For local kernel threads that are spawned from system calls handler when a
 * user interacts with some kernel interface (e.g. vhost workers), a handle is
 * passed from a userspace process as the common_handle field of the
 * kcov_remote_arg struct (note, that the user must generate a handle by using
 * kcov_remote_handle() with KCOV_SUBSYSTEM_COMMON as the subsystem id and an
 * arbitrary 4-byte non-zero number as the instance id). This common handle
 * then gets saved into the task_struct of the process that issued the
 * KCOV_REMOTE_ENABLE ioctl. When this process issues system calls that spawn
 * kernel threads, the common handle must be retrieved via kcov_common_handle()
 * and passed to the spawned threads via custom annotations. Those kernel
 * threads must in turn be annotated with kcov_remote_start(common_handle) and
 * kcov_remote_stop(). All of the threads that are spawned by the same process
 * obtain the same handle, hence the name "common".
 *
 * See Documentation/dev-tools/kcov.rst for more details.
 *
 * Internally, kcov_remote_start() looks up the kcov device associated with the
 * provided handle, allocates an area for coverage collection, and saves the
 * pointers to kcov and area into the current task_struct to allow coverage to
 * be collected via __sanitizer_cov_trace_pc().
 * In turns kcov_remote_stop() clears those pointers from task_struct to stop
 * collecting coverage and copies all collected coverage into the kcov area.
 */

static inline bool kcov_mode_enabled(unsigned int mode)
{
	return (mode & ~KCOV_IN_CTXSW) != KCOV_MODE_DISABLED;
}

static void kcov_remote_softirq_start(struct task_struct *t)
{
	struct kcov_percpu_data *data = this_cpu_ptr(&kcov_percpu_data);
	unsigned int mode;

	mode = READ_ONCE(t->kcov_mode);
	barrier();
	if (kcov_mode_enabled(mode)) {
		data->saved_state = t->kcov_state;
		data->saved_mode = mode;
		data->saved_kcov = t->kcov;
		kcov_stop(t);
	}
}

static void kcov_remote_softirq_stop(struct task_struct *t)
{
	struct kcov_percpu_data *data = this_cpu_ptr(&kcov_percpu_data);

	if (data->saved_kcov) {
		kcov_start(t, data->saved_kcov, t->kcov_mode,
			   &data->saved_state);
		data->saved_state = (struct kcov_state){};
		data->saved_kcov = NULL;
	}
}

void kcov_remote_start(u64 handle)
{
	struct task_struct *t = current;
	struct kcov_remote *remote;
	struct kcov_state state;
	enum kcov_mode mode;
	unsigned long flags;
	unsigned int size;
	struct kcov *kcov;
	void *area;

	if (WARN_ON(!kcov_check_handle(handle, true, true, true)))
		return;
	if (!in_task() && !in_softirq_really())
		return;

	local_lock_irqsave(&kcov_percpu_data.lock, flags);

	/*
	 * Check that kcov_remote_start() is not called twice in background
	 * threads nor called by user tasks (with enabled kcov).
	 */
	mode = READ_ONCE(t->kcov_mode);
	if (WARN_ON(in_task() && kcov_mode_enabled(mode))) {
		local_unlock_irqrestore(&kcov_percpu_data.lock, flags);
		return;
	}
	/*
	 * Check that kcov_remote_start() is not called twice in softirqs.
	 * Note, that kcov_remote_start() can be called from a softirq that
	 * happened while collecting coverage from a background thread.
	 */
	if (WARN_ON(in_serving_softirq() && t->kcov_softirq)) {
		local_unlock_irqrestore(&kcov_percpu_data.lock, flags);
		return;
	}

	spin_lock(&kcov_remote_lock);
	remote = kcov_remote_find(handle);
	if (!remote) {
		spin_unlock(&kcov_remote_lock);
		local_unlock_irqrestore(&kcov_percpu_data.lock, flags);
		return;
	}
	kcov_debug("handle = %llx, context: %s\n", handle,
		   in_task() ? "task" : "softirq");
	kcov = remote->kcov;
	/* Put in kcov_remote_stop(). */
	kcov_get(kcov);
	/*
	 * Read kcov fields before unlock to prevent races with
	 * KCOV_DISABLE / kcov_remote_reset().
	 */
	mode = kcov->mode;
	state.sequence = kcov->state.sequence;
	if (in_task()) {
		size = kcov->remote_size;
		area = kcov_remote_area_get(size);
	} else {
		size = CONFIG_KCOV_IRQ_AREA_SIZE;
		area = this_cpu_ptr(&kcov_percpu_data)->irq_area;
	}
	spin_unlock(&kcov_remote_lock);

	/* Can only happen when in_task(). */
	if (!area) {
		local_unlock_irqrestore(&kcov_percpu_data.lock, flags);
		area = vmalloc(size * sizeof(unsigned long));
		if (!area) {
			kcov_put(kcov);
			return;
		}
		local_lock_irqsave(&kcov_percpu_data.lock, flags);
	}

	state.area = area;
	state.size = size;
	state.trace = area;
	state.trace_size = size;
	/* Reset coverage size. */
	state.trace[0] = 0;

	if (in_serving_softirq()) {
		kcov_remote_softirq_start(t);
		t->kcov_softirq = 1;
	}
	kcov_start(t, kcov, t->kcov_mode, &state);

	local_unlock_irqrestore(&kcov_percpu_data.lock, flags);
}
EXPORT_SYMBOL(kcov_remote_start);

static void kcov_move_area(enum kcov_mode mode, void *dst_area,
			   unsigned int dst_area_size, void *src_area)
{
	u64 word_size = sizeof(unsigned long);
	u64 count_size, entry_size_log;
	u64 dst_len, src_len;
	void *dst_entries, *src_entries;
	u64 dst_occupied, dst_free, bytes_to_move, entries_moved;

	kcov_debug("%px %u <= %px %lu\n", dst_area, dst_area_size, src_area,
		   *(unsigned long *)src_area);

	switch (mode) {
	case KCOV_MODE_TRACE_PC:
		dst_len = READ_ONCE(*(unsigned long *)dst_area);
		src_len = *(unsigned long *)src_area;
		count_size = sizeof(unsigned long);
		entry_size_log = __ilog2_u64(sizeof(unsigned long));
		break;
	case KCOV_MODE_TRACE_CMP:
		dst_len = READ_ONCE(*(u64 *)dst_area);
		src_len = *(u64 *)src_area;
		count_size = sizeof(u64);
		BUILD_BUG_ON(!is_power_of_2(KCOV_WORDS_PER_CMP));
		entry_size_log = __ilog2_u64(sizeof(u64) * KCOV_WORDS_PER_CMP);
		break;
	default:
		WARN_ON(1);
		return;
	}

	/* As arm can't divide u64 integers use log of entry size. */
	if (dst_len >
	    ((dst_area_size * word_size - count_size) >> entry_size_log))
		return;
	dst_occupied = count_size + (dst_len << entry_size_log);
	dst_free = dst_area_size * word_size - dst_occupied;
	bytes_to_move = min(dst_free, src_len << entry_size_log);
	dst_entries = dst_area + dst_occupied;
	src_entries = src_area + count_size;
	memcpy(dst_entries, src_entries, bytes_to_move);
	entries_moved = bytes_to_move >> entry_size_log;

	switch (mode) {
	case KCOV_MODE_TRACE_PC:
		WRITE_ONCE(*(unsigned long *)dst_area, dst_len + entries_moved);
		break;
	case KCOV_MODE_TRACE_CMP:
		WRITE_ONCE(*(u64 *)dst_area, dst_len + entries_moved);
		break;
	default:
		break;
	}
}

/* See the comment before kcov_remote_start() for usage details. */
void kcov_remote_stop(void)
{
	struct task_struct *t = current;
	struct kcov *kcov;
	unsigned int mode;
	void *area, *trace;
	unsigned int size, trace_size;
	int sequence;
	unsigned long flags;

	if (!in_task() && !in_softirq_really())
		return;

	local_lock_irqsave(&kcov_percpu_data.lock, flags);

	mode = READ_ONCE(t->kcov_mode);
	barrier();
	if (!kcov_mode_enabled(mode)) {
		local_unlock_irqrestore(&kcov_percpu_data.lock, flags);
		return;
	}
	/*
	 * When in softirq, check if the corresponding kcov_remote_start()
	 * actually found the remote handle and started collecting coverage.
	 */
	if (in_serving_softirq() && !t->kcov_softirq) {
		local_unlock_irqrestore(&kcov_percpu_data.lock, flags);
		return;
	}
	/* Make sure that kcov_softirq is only set when in softirq. */
	if (WARN_ON(!in_serving_softirq() && t->kcov_softirq)) {
		local_unlock_irqrestore(&kcov_percpu_data.lock, flags);
		return;
	}

	kcov = t->kcov;
	area = t->kcov_state.area;
	size = t->kcov_state.size;
	trace = t->kcov_state.trace;
	trace_size = t->kcov_state.trace_size;
	sequence = t->kcov_state.sequence;

	kcov_stop(t);
	if (in_serving_softirq()) {
		t->kcov_softirq = 0;
		kcov_remote_softirq_stop(t);
	}

	spin_lock(&kcov->lock);
	/*
	 * KCOV_DISABLE could have been called between kcov_remote_start()
	 * and kcov_remote_stop(), hence the sequence check.
	 */
	if (sequence == kcov->state.sequence && kcov->remote)
		kcov_move_area(kcov->mode, kcov->state.area, kcov->state.size,
			       area);
	spin_unlock(&kcov->lock);

	if (in_task()) {
		spin_lock(&kcov_remote_lock);
		kcov_remote_area_put(area, size);
		spin_unlock(&kcov_remote_lock);
	}

	local_unlock_irqrestore(&kcov_percpu_data.lock, flags);

	/* Get in kcov_remote_start(). */
	kcov_put(kcov);
}
EXPORT_SYMBOL(kcov_remote_stop);

/* See the comment before kcov_remote_start() for usage details. */
u64 kcov_common_handle(void)
{
	if (!in_task())
		return 0;
	return current->kcov_handle;
}
EXPORT_SYMBOL(kcov_common_handle);

#ifdef CONFIG_KCOV_SELFTEST
static void __init selftest(void)
{
	unsigned long start;

	pr_err("running self test\n");
	/*
	 * Test that interrupts don't produce spurious coverage.
	 * The coverage callback filters out interrupt code, but only
	 * after the handler updates preempt count. Some code periodically
	 * leaks out of that section and leads to spurious coverage.
	 * It's hard to call the actual interrupt handler directly,
	 * so we just loop here for a bit waiting for a timer interrupt.
	 * We set kcov_mode to enable tracing, but don't setup the area,
	 * so any attempt to trace will crash. Note: we must not call any
	 * potentially traced functions in this region.
	 */
	start = jiffies;
	current->kcov_mode = KCOV_MODE_TRACE_PC;
	while ((jiffies - start) * MSEC_PER_SEC / HZ < 300)
		;
	current->kcov_mode = 0;
	pr_err("done running self test\n");
}
#endif

static int __init kcov_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		void *area = vmalloc_node(CONFIG_KCOV_IRQ_AREA_SIZE *
						  sizeof(unsigned long),
					  cpu_to_node(cpu));
		if (!area)
			return -ENOMEM;
		per_cpu_ptr(&kcov_percpu_data, cpu)->irq_area = area;
	}

	/*
	 * The kcov debugfs file won't ever get removed and thus,
	 * there is no need to protect it against removal races. The
	 * use of debugfs_create_file_unsafe() is actually safe here.
	 */
	debugfs_create_file_unsafe("kcov", 0600, NULL, NULL, &kcov_fops);

#ifdef CONFIG_KCOV_SELFTEST
	selftest();
#endif

	return 0;
}

device_initcall(kcov_init);
