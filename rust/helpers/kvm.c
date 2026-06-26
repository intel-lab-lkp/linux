// SPDX-License-Identifier: GPL-2.0

#include <linux/kvm_host.h>
#include <linux/kvm_irqfd.h>
#include <linux/irqbypass.h>
#include <kvm/iodev.h>
#include <trace/events/kvm.h>

/* Generic subsystem includes needed for helpers below */
#include <linux/wait.h>
#include <linux/srcu.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/poll.h>

__rust_helper bool rust_helper_kvm_arch_intc_initialized(struct kvm *kvm)
{
	return kvm_arch_intc_initialized(kvm);
}

__rust_helper void
rust_helper_kvm_arch_post_irq_ack_notifier_list_update(struct kvm *kvm)
{
	kvm_arch_post_irq_ack_notifier_list_update(kvm);
}

__rust_helper struct kvm_io_bus *rust_helper_kvm_get_bus(struct kvm *kvm,
							 enum kvm_bus bus_idx)
{
	return kvm_get_bus(kvm, bus_idx);
}

__rust_helper void rust_helper_trace_kvm_ack_irq(unsigned int irqchip,
						 unsigned int pin)
{
	trace_kvm_ack_irq(irqchip, pin);
}

__rust_helper bool rust_helper_kvm_arch_has_irq_bypass(void)
{
	return kvm_arch_has_irq_bypass();
}

__rust_helper void rust_helper_kvm_irqfds_spin_release(struct kvm *kvm)
{
#ifdef CONFIG_LOCKDEP
	spin_release(&kvm->irqfds.lock.dep_map, _RET_IP_);
#endif
}

__rust_helper void rust_helper_kvm_irqfds_spin_acquire(struct kvm *kvm)
{
#ifdef CONFIG_LOCKDEP
	spin_acquire(&kvm->irqfds.lock.dep_map, 0, 0, _RET_IP_);
#endif
}

#ifdef CONFIG_LOCKDEP
__rust_helper void rust_helper_spin_release(struct lockdep_map *map,
					    unsigned long ip)
{
	spin_release(map, ip);
}

__rust_helper void rust_helper_spin_acquire(struct lockdep_map *map,
					    int subclass, int trylock,
					    unsigned long ip)
{
	spin_acquire(map, subclass, trylock, ip);
}
#endif

__rust_helper void rust_helper_init_waitqueue_func_entry(wait_queue_entry_t *p,
							 wait_queue_func_t func)
{
	init_waitqueue_func_entry(p, func);
}

__rust_helper int rust_helper_srcu_read_lock(struct srcu_struct *ssp)
{
	return srcu_read_lock(ssp);
}

__rust_helper void rust_helper_srcu_read_unlock(struct srcu_struct *ssp,
						int idx)
{
	srcu_read_unlock(ssp, idx);
}

__rust_helper void rust_helper_list_del_rcu(struct list_head *entry)
{
	list_del_rcu(entry);
}

__rust_helper int rust_helper_list_empty(const struct list_head *head)
{
	return list_empty(head);
}

__rust_helper void rust_helper_list_del_init(struct list_head *entry)
{
	list_del_init(entry);
}

__rust_helper bool rust_helper_queue_work(struct workqueue_struct *wq,
					  struct work_struct *work)
{
	return queue_work(wq, work);
}

__rust_helper bool rust_helper_schedule_work(struct work_struct *work)
{
	return schedule_work(work);
}

__rust_helper unsigned long rust_helper_spin_lock_irqsave(spinlock_t *lock)
{
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	return flags;
}

__rust_helper void rust_helper_spin_unlock_irqrestore(spinlock_t *lock,
						      unsigned long flags)
{
	spin_unlock_irqrestore(lock, flags);
}

__rust_helper void rust_helper_spin_lock_irq(spinlock_t *lock)
{
	spin_lock_irq(lock);
}

__rust_helper void rust_helper_spin_unlock_irq(spinlock_t *lock)
{
	spin_unlock_irq(lock);
}

__rust_helper void rust_helper_flush_workqueue(struct workqueue_struct *wq)
{
	flush_workqueue(wq);
}

__rust_helper void rust_helper_list_del(struct list_head *entry)
{
	list_del(entry);
}

__rust_helper void *rust_helper_kzalloc(size_t size, gfp_t flags)
{
	return kzalloc(size, flags);
}

__rust_helper int rust_helper_fd_empty(struct fd f)
{
	return fd_empty(f);
}

__rust_helper struct file *rust_helper_fd_file(struct fd f)
{
	return fd_file(f);
}

__rust_helper void rust_helper_fdput(struct fd f)
{
	fdput(f);
}

__rust_helper void rust_helper_list_add_rcu(struct list_head *new,
					    struct list_head *head)
{
	list_add_rcu(new, head);
}

__rust_helper void rust_helper_init_poll_funcptr(poll_table *pt,
						 poll_queue_proc qproc)
{
	init_poll_funcptr(pt, qproc);
}

__rust_helper __poll_t rust_helper_vfs_poll(struct file *file,
					    struct poll_table_struct *pt)
{
	return vfs_poll(file, pt);
}

__rust_helper struct workqueue_struct *
rust_helper_alloc_workqueue(const char *fmt, unsigned int flags, int max_active)
{
	return alloc_workqueue(fmt, flags, max_active);
}

__rust_helper void rust_helper_hlist_add_head_rcu(struct hlist_node *n,
						  struct hlist_head *h)
{
	hlist_add_head_rcu(n, h);
}

__rust_helper void rust_helper_hlist_del_init_rcu(struct hlist_node *n)
{
	hlist_del_init_rcu(n);
}

__rust_helper void rust_helper_lockdep_assert_held_irqfds_lock(struct kvm *kvm)
{
	lockdep_assert_held(&kvm->irqfds.lock);
}

__rust_helper void rust_helper_lockdep_assert_irqfd_access(struct kvm *kvm)
{
	/*
	 * Assert that either irqfds.lock or SRCU is held.
	 * This matches the C lockdep_assert_once() in irqfd_is_active().
	 */
	lockdep_assert_once(lockdep_is_held(&kvm->irqfds.lock) ||
			    srcu_read_lock_held(&kvm->irq_srcu));
}
