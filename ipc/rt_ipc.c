#define pr_fmt(fmt) "[%s:%d] " fmt, __func__, __LINE__

#include <linux/rculist.h>
#include <linux/idr.h>
#include <linux/syscalls.h>
#include <linux/rt_ipc.h>

static __maybe_unused __cacheline_aligned_in_smp DEFINE_SPINLOCK(rt_ipc_lock);

void rt_ipc_deregister(struct task_struct *tsk)
{
	struct rt_ipc_activation *activation, *tmp;
	struct rt_ipc_action *act = NULL;

	list_for_each_entry_safe(activation, tmp, &tsk->rt_ipc_activation_free, activation_link) {
		act = activation->act;
		list_del_init(&activation->activation_link);
		kfree(activation);
	}

	if (act)
		kfree(act);

	pr_debug("task: %s tsk->pid: %d\n", tsk->comm, tsk->pid);
}

SYSCALL_DEFINE1(rt_ipc_register, const struct rt_ipc_action __user *, act)
{
	struct rt_ipc_action *kact;
	struct rt_ipc_activation *activation;
	struct task_struct *p;
	spinlock_t *rt_ipc_lock, *sighand_lock;
	int ret;

	if (!act) {
		ret = -EINVAL;
		goto out;
	}

	kact = kmalloc(sizeof(*kact), GFP_KERNEL);
	if (!kact) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(kact, act, sizeof(*act))) {
		ret = -EFAULT;
		goto out_free_act;
	}

	rt_ipc_lock = &current->group_leader->rt_ipc_lock;
	sighand_lock = &current->group_leader->sighand->siglock;

	spin_lock_init(&current->group_leader->rt_ipc_lock);

	spin_lock_irq(sighand_lock);

	for_each_thread(current, p) {
		activation = kzalloc(sizeof(*activation), GFP_KERNEL);
		BUG_ON(activation == NULL);

		activation->stack = round_down(task_pt_regs(p)->sp, 128) - 8;
		activation->s = p;
		activation->act = kact;

		spin_lock_irq(rt_ipc_lock);
		list_add_tail(&activation->activation_link, &current->group_leader->rt_ipc_activation_free);
		spin_unlock_irq(rt_ipc_lock);
	}

	spin_unlock_irq(sighand_lock);

	ret = rt_ipc_config_activation(current->group_leader);
	if (ret < 0) {
		// TODO: activation free
		goto out_free_act;
	}

	return ret;

out_free_act:
	kfree(act);
out:
	return ret;
}

static struct rt_ipc_activation *get_available_activation(struct task_struct *task)
{
	struct rt_ipc_activation *activation, *tmp;

	list_for_each_entry_safe(activation, tmp, &task->rt_ipc_activation_free, activation_link) {
		list_del_init(&activation->activation_link);
		return activation;
	}

	return NULL;
}

SYSCALL_DEFINE3(rt_ipc_invoke, pid_t, pid, unsigned int __user, cmd, size_t __user *, args)
{
	struct rt_ipc_activation *activation;
	spinlock_t *lock;
	struct task_struct *p;
	unsigned long flags;
	int errno = -EINVAL;
	struct rt_ipc_info info;

	if (copy_from_user(&info, args, sizeof(info))) {
		return -EFAULT;
	}

	if (pid < 0)
		return errno;

	rcu_read_lock();
	p = pid_task(find_vpid(pid), PIDTYPE_PID);
	rcu_read_unlock();

	if (!p)
		return -ESRCH;

	lock = &p->group_leader->rt_ipc_lock;

	spin_lock_irqsave(lock, flags);

	activation = get_available_activation(p->group_leader);
	if (!activation) {
		spin_unlock_irqrestore(lock, flags);
		return errno;
	}

	spin_unlock_irqrestore(lock, flags);

	activation->info = (void *)args;

	pr_debug("stack: %016lx pid: %d\n", activation->stack, current->pid);

	errno = rt_ipc_migrate_thread(activation, cmd, &info);

	return errno;
}