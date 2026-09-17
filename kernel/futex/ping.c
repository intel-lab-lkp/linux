// SPDX-License-Identifier: GPL-2.0-only

#include <linux/plist.h>
#include <linux/slab.h>
#include <linux/sched/task.h>

#include "futex.h"

static int futex_trylock_ping_state(u32 __user *uaddr,
				    struct futex_pi_state *ping_state);

static void ping_state_update_owner(struct futex_pi_state *ping_state,
				    struct task_struct *new_owner)
{
	struct task_struct *old_owner = ping_state->owner;

	lockdep_assert_held(&ping_state->ping_mutex.wait_lock);

	if (old_owner) {
		raw_spin_lock(&old_owner->pi_futex_lock);
		WARN_ON(list_empty(&ping_state->list));
		list_del_init(&ping_state->list);
		raw_spin_unlock(&old_owner->pi_futex_lock);
	}

	if (new_owner) {
		raw_spin_lock(&new_owner->pi_futex_lock);
		WARN_ON(!list_empty(&ping_state->list));
		list_add(&ping_state->list, &new_owner->futex.ping_state_list);
		ping_state->owner = new_owner;
		raw_spin_unlock(&new_owner->pi_futex_lock);
	}
}

static int lock_pi_update_atomic(u32 __user *uaddr, u32 uval, u32 newval)
{
	int err;
	u32 curval;

	if (unlikely(should_fail_futex(true)))
		return -EFAULT;

	err = futex_cmpxchg_value_locked(&curval, uaddr, uval, newval);
	if (unlikely(err))
		return err;

	/* If user space value changed, let the caller retry */
	return curval != uval ? -EAGAIN : 0;
}

void
get_ping_state(struct futex_pi_state *ping_state)
{
	WARN_ON_ONCE(!refcount_inc_not_zero(&ping_state->refcount));
}

/*
 * Drops a reference to the pi_state object and frees or caches it
 * when the last reference is gone.
 */
void
put_ping_state(struct futex_pi_state *ping_state)
{
	if (!ping_state)
		return;

	if (!refcount_dec_and_test(&ping_state->refcount))
		return;

	/*
	 * If ping_state->owner is NULL, the owner is most probably dying
	 * and has cleaned up the pi_state already
	 */
	if (ping_state->owner) {
		unsigned long flags;

		raw_spin_lock_irqsave(&ping_state->ping_mutex.wait_lock, flags);
		ping_state_update_owner(ping_state, NULL);
		WRITE_ONCE(ping_state->ping_mutex.owner, NULL);
		raw_spin_unlock_irqrestore(&ping_state->ping_mutex.wait_lock,
					   flags);
	}

	if (current->futex.pi_state_cache) {
		kfree(ping_state);
	} else {
		/*
		 * pi_state->list is already empty.
		 * clear pi_state->owner.
		 * refcount is at 0 - put it back to 1.
		 */
		ping_state->owner = NULL;
		refcount_set(&ping_state->refcount, 1);
		current->futex.pi_state_cache = ping_state;
	}
}

static void futex_unqueue_ping(struct futex_q *q)
{
	if (!plist_node_empty(&q->list))
		__futex_unqueue(q);

	WARN_ON(!q->ping_state);
	put_ping_state(q->ping_state);
	q->ping_state = NULL;
}

/*
 * We own the ping_state but weren't able to get the futex.
 * Wake up the next waiter and give them ownership.
 */
static void give_ping_state_to_next_waiter(struct futex_hash_bucket *hb,
					   union futex_key *key,
					   struct futex_pi_state *ping_state)
{
	struct futex_q *top_waiter;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irq(&ping_state->ping_mutex.wait_lock);
	/*
	 * Someone else got the futex and we don't need to do anything
	 * anymore, as it's their responsibility now.
	 */
	if (ping_state->owner && ping_state->owner != current)
		goto out;

	top_waiter = futex_top_waiter(hb, key);
	/*
	 * There are no other waiters but we leave the WAITERS bit set
	 * (with no owner or ping_state) to be cleaned up at a later unlock,
	 * at the cost of an extra syscall at the next lock operation, to
	 * keep things simple.
	 */
	if (!top_waiter)
		goto out;
	get_ping_state(ping_state);
	get_task_struct(top_waiter->task);
	wake_q_add_safe(&wake_q, top_waiter->task);
	ping_state_update_owner(ping_state, top_waiter->task);

out:
	raw_spin_unlock_irq_wake(&ping_state->ping_mutex.wait_lock, &wake_q);
}

/* Returns >0 if lock acquired, <0 on error */
static int futex_trylock_ping_state(u32 __user *uaddr,
				    struct futex_pi_state *ping_state)
{
	struct task_struct *owner;
	u32 uval, new, newtid;
	int ret;

	ret = 0;
	raw_spin_lock_irq(&ping_state->ping_mutex.wait_lock);
	owner = ping_mutex_owner(&ping_state->ping_mutex);
	if (owner == NULL) {
		newtid = task_pid_vnr(current);

		ret = futex_get_value_locked(&uval, uaddr);
		if (ret)
			goto err;
		if (uval & FUTEX_TID_MASK) {
			ret = -EAGAIN;
			goto err;
		}
		new = newtid | FUTEX_WAITERS;
		ret = lock_pi_update_atomic(uaddr, uval, new);
		if (ret)
			goto err;
		if (ping_state->owner != current)
			ping_state_update_owner(ping_state, current);
		WRITE_ONCE(ping_state->ping_mutex.owner, current);
		ret = 1;
	}
	raw_spin_unlock_irq(&ping_state->ping_mutex.wait_lock);
	return ret;

err:
	raw_spin_unlock_irq(&ping_state->ping_mutex.wait_lock);
	switch (ret) {
	case -EFAULT:
		ret = fault_in_user_writeable(uaddr);
		break;
	case -EAGAIN:
		ret = 0;
		break;
	case -EINVAL:
		break;
	default:
		WARN_ON(1);
	}
	return ret;
}

static int futex_lock_ping_atomic(u32 __user *uaddr,
				  struct futex_hash_bucket *hb,
				  union futex_key *key,
				  struct futex_pi_state **ps,
				  struct task_struct *task,
				  struct task_struct **exiting)
{
	u32 uval, newval, vpid = task_pid_vnr(task);
	struct futex_q *top_waiter;
	int ret;

	/*
	 * Read the user space value first so we can validate a few
	 * things before proceeding further.
	 */
	if (futex_get_value_locked(&uval, uaddr))
		return -EFAULT;

	if (unlikely(should_fail_futex(true)))
		return -EFAULT;

	/*
	 * Detect deadlocks.
	 */
	if ((unlikely((uval & FUTEX_TID_MASK) == vpid)))
		return -EDEADLK;

	if ((unlikely(should_fail_futex(true))))
		return -EDEADLK;

	/*
	 * Lookup existing state first. If it exists, try to attach to
	 * its ping_state.
	 */
	top_waiter = futex_top_waiter(hb, key);
	if (top_waiter) {
		struct futex_pi_state *_ps;

		_ps = top_waiter->ping_state;
		if (_ps == NULL)
			return -EINVAL;
		ret = futex_trylock_ping_state(uaddr, _ps);
		if (ret > 0) {
			/* We stole the lock from the top waiter. */
			raw_spin_lock_irq(&_ps->ping_mutex.wait_lock);
			WARN_ON_ONCE(!refcount_read(&_ps->refcount));
			get_ping_state(_ps);
			raw_spin_unlock_irq(&_ps->ping_mutex.wait_lock);
			*ps = _ps;
			return 1;
		} else if (ret < 0)
			return ret;
		return attach_to_pi_state(uaddr, uval, top_waiter->ping_state,
					  ps, true);
	}

	/*
	 * No waiter and user TID is 0. We are here because the
	 * waiters or the owner died bit is set or called from
	 * requeue_cmp_pi or for whatever reason something took the
	 * syscall.
	 */
	if (!(uval & FUTEX_TID_MASK)) {
		/*
		 * We take over the futex. No other waiters and the user space
		 * TID is 0. We preserve the owner died bit.
		 */
		newval = uval & FUTEX_OWNER_DIED;
		newval |= vpid;

		ret = lock_pi_update_atomic(uaddr, uval, newval);
		if (ret)
			return ret;
		return 1;
	}

	/*
	 * First waiter. Set the waiters bit before attaching ourself to
	 * the owner. If owner tries to unlock, it will be forced into
	 * the kernel and blocked on hb->lock.
	 */
	newval = uval | FUTEX_WAITERS;
	ret = lock_pi_update_atomic(uaddr, uval, newval);
	if (ret)
		return ret;
	/*
	 * If the update of the user space value succeeded, we try to
	 * attach to the owner. If that fails, no harm done, we only
	 * set the FUTEX_WAITERS bit in the user space variable.
	 */
	return attach_to_pi_owner(uaddr, newval, key, ps, exiting, true);
}

/*
 * Return values:
 *     < 0: error.
 *     0: did not get the lock.
 *     1: got the lock.
 */
int futex_lock_ping(u32 __user *uaddr, unsigned int flags, ktime_t *time,
		    int trylock)
{
	struct hrtimer_sleeper timeout, *to;
	struct task_struct *exiting;
	struct futex_q q = futex_q_init;
	bool queued;
	int ret;

	if (refill_pi_state_cache())
		return -ENOMEM;

	to = futex_setup_timer(time, &timeout, flags, 0);

retry:
	ret = get_futex_key(uaddr, flags, &q.key, FUTEX_WRITE);
	if (unlikely(ret != 0))
		goto out;

retry_private:
	if (1) {
		CLASS(hbr, hbr)(&q.key);
		auto hb = hbr.hb;

		futex_q_lock(&q, hb);

		ret = futex_lock_ping_atomic(uaddr, hb, &q.key, &q.ping_state,
					     current, &exiting);
		if (unlikely(ret)) {
			/*
			 * Atomic work succeeded and we got the lock,
			 * or failed. Either way, we do _not_ block.
			 */
			switch (ret) {
			case 1:
				ret = 0;
				/* Got the lock */
				put_ping_state(q.ping_state);
				q.ping_state = NULL;
				goto out_unlock;
			case -EFAULT:
				goto uaddr_faulted;
			case -EBUSY:
			case -EAGAIN:
				/*
				 * Two reasons for this:
				 * - EBUSY: Task is exiting and we just wait
				 * for the exit to complete.
				 * - EAGAIN: The user space value changed.
				 */
				futex_q_unlock(hb);
				/*
				 * Handle the case where the owner is in the
				 * middle of exiting. Wait for the exit to
				 * complete otherwise this task might loop
				 * forever, aka. live lock.
				 */
				wait_for_owner_exiting(ret, exiting);
				cond_resched();
				goto retry;
			default:
				goto out_unlock;
			}
		}

		WARN_ON(!q.ping_state);
		if (trylock) {
			/*
			 * futex_lock_ping_atomic() trylocks and we did not
			 * get the lock.
			 */
			put_ping_state(q.ping_state);
			q.ping_state = NULL;
			ret = -EWOULDBLOCK;
			goto out_unlock;
		}

		queued = false;
		while (1) {
			set_task_blocked_on(current, &q.ping_state->ping_mutex,
					    BO_T_PING_FUTEX);

			set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
			if (!queued) {
				futex_queue(&q, hb, current);
				queued = true;
			} else {
				WARN_ON_ONCE(plist_node_empty(&q.list));
				spin_unlock(&hb->lock);
				__release(q->lock_ptr);
			}

			futex_do_wait(&q, to);

			clear_task_blocked_on(current,
					      &q.ping_state->ping_mutex);

			futex_q_lockptr_lock(&q);
			if (to && !to->task) {
				ret = -ETIMEDOUT;
				goto out_unqueue;
			}
			if (signal_pending(current)) {
				ret = -EINTR;
				goto out_unqueue;
			}

			ret = futex_trylock_ping_state(uaddr, q.ping_state);
			if (ret > 0) {
				/* Got the futex */
				ret = 0;
				goto out_unqueue;
			} else if (ret < 0)
				goto out_unqueue;
		}

out_unqueue:
		/*
		 * We got a pending signal or timeout, but the futex was handed
		 * off to us. Fix up the return value to indicate success.
		 */
		if ((ret == -EINTR || ret == -ETIMEDOUT) &&
		    ping_mutex_owner(&q.ping_state->ping_mutex) == current)
			ret = 0;

		/*
		 * We are the pi_state owner but don't own the futex.
		 * This can happen if we get picked by the previous
		 * owner but get out without acquiring the lock for
		 * some reason.
		 * Wake up the next waiter and give the ping_state to them.
		 */
		if (ret != 0 && q.ping_state->owner == current) {
			if (!plist_node_empty(&q.list))
				__futex_unqueue(&q);
			give_ping_state_to_next_waiter(hb, &q.key,
						       q.ping_state);
			put_ping_state(q.ping_state);
		} else
			/* This also puts the ping_state */
			futex_unqueue_ping(&q);

out_unlock:
		futex_q_unlock(hb);
		__release(q.lock_ptr);
		goto out;

uaddr_faulted:
		futex_q_unlock(hb);
		__release(q.lock_ptr);

		ret = fault_in_user_writeable(uaddr);
		if (ret)
			goto out;
		if (!(flags & FLAGS_SHARED))
			goto retry_private;
		goto retry;
	}

out:
	if (to) {
		hrtimer_cancel(&to->timer);
		destroy_hrtimer_on_stack(&to->timer);
	}

	return ret;
}

int futex_unlock_ping(u32 __user *uaddr, unsigned int flags)
{
	struct futex_pi_state *ping_state;
	u32 new, uval, vpid = task_pid_vnr(current);
	union futex_key key = FUTEX_KEY_INIT;
	struct futex_q *top_waiter;
	DEFINE_WAKE_Q(wake_q);
	int ret;

retry:
	if (get_user(uval, uaddr))
		return -EFAULT;
	/*
	 * We release only a lock we actually own:
	 */
	if ((uval & FUTEX_TID_MASK) != vpid)
		return -EPERM;

	ret = get_futex_key(uaddr, flags, &key, FUTEX_WRITE);
	if (ret)
		return ret;

	CLASS(hbr, hbr)(&key);
	auto hb = hbr.hb;
	spin_lock(&hb->lock);
	top_waiter = futex_top_waiter(hb, &key);

	if (!top_waiter) {
		spin_unlock(&hb->lock);
		/* No waiters in the kernel, we can just clear FUTEX_WAITERS */
		ret = lock_pi_update_atomic(uaddr, uval, 0);
		if (ret) {
			switch (ret) {
			case -EFAULT:
				goto uaddr_faulted;
			case -EAGAIN:
				cond_resched();
				goto retry;
			default:
				WARN_ON_ONCE(1);
			}
		}
		return ret;
	}

	ping_state = top_waiter->ping_state;
	ret = -EINVAL;
	if (!ping_state)
		goto out_unlock;
	raw_spin_lock_irq(&ping_state->ping_mutex.wait_lock);
	if (ping_state->owner != current) {
		raw_spin_unlock_irq(&ping_state->ping_mutex.wait_lock);
		goto out_unlock;
	}
	get_ping_state(ping_state);
	/* Leave it queued, it gets unqueued on the lock side */
	get_task_struct(top_waiter->task);
	wake_q_add_safe(&wake_q, top_waiter->task);
	clear_task_blocked_on(top_waiter->task, &ping_state->ping_mutex);
	spin_unlock(&hb->lock);

	/*
	 * Unconditionally set FUTEX_WAITERS.
	 * It will get removed by the next unlocker who notices there is
	 * no top_waiter.
	 */
	new = FUTEX_WAITERS;
	ret = lock_pi_update_atomic(uaddr, uval, new);
	if (ret) {
		raw_spin_unlock_irq(&ping_state->ping_mutex.wait_lock);
		put_ping_state(ping_state);
		switch (ret) {
		case -EFAULT:
			goto uaddr_faulted;
		case -EAGAIN:
			cond_resched();
			goto retry;
		default:
			WARN_ON_ONCE(1);
			return ret;
		}
	}

	ping_state_update_owner(ping_state, top_waiter->task);
	WRITE_ONCE(ping_state->ping_mutex.owner, NULL);
	raw_spin_unlock_irq_wake(&ping_state->ping_mutex.wait_lock, &wake_q);
	put_ping_state(ping_state);
	return 0;

out_unlock:
	spin_unlock(&hb->lock);
	return ret;

uaddr_faulted:
	ret = fault_in_user_writeable(uaddr);
	if (ret)
		return ret;
	goto retry;
}
