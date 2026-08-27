// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpuhotplug.h>
#include <linux/highmem.h>
#include <linux/irq_work.h>
#include <linux/list.h>
#include <linux/list_lru.h>
#include <linux/llist.h>
#include <linux/memcontrol.h>
#include <linux/oom.h>
#include <linux/percpu_counter.h>
#include <linux/pgtable.h>
#include <linux/sched/task.h>
#include <linux/spinlock.h>
#include <linux/swait.h>
#include <linux/vmalloc.h>

#include "stack_shrinker.h"

#include "../mm/internal.h"

struct repopulate_work {
	struct work_struct work;
	struct llist_head stacks;
};

static DEFINE_PER_CPU(struct repopulate_work, repopulate_work);

static DEFINE_RAW_SPINLOCK(new_reclaimable_stacks_lock);
static LIST_HEAD(new_reclaimable_stacks);

static struct list_lru reclaimable_stacks_lru;

/*
 * do_shrink_slab() wants us to scan (nr_obj / (1 << priority)), but unless
 * reclaim is really struggling, that can round down to 0 for a lot of
 * memcgs. Compensate for that by scaling count and batch size.
 */
#define STACK_COUNT_SHIFT DEF_PRIORITY
/*
 * Large batch sizes (like the 128 default) can result in spiky behavior, where
 * deferred work acculmulates and then all of a memcg's stacks get scanned all
 * at once.
 */
#define STACK_BATCH_SIZE 4

/*
 * For purposes of shrinker iteration, a stack allocated with NUMA_NO_NODE is
 * associated with the node that created it. This extra bit allows us to
 * determine if NUMA_NO_NODE or the saved node should be used for repopulation.
 */
#define TASK_NUMA_NO_NODE_FLAG BIT(15)

static int task_shrinker_node(struct task_struct *tsk)
{
	return tsk->stack_reclaim_state.node & ~TASK_NUMA_NO_NODE_FLAG;
}

static int task_alloc_node(struct task_struct *tsk)
{
	if (tsk->stack_reclaim_state.node & TASK_NUMA_NO_NODE_FLAG)
		return NUMA_NO_NODE;
	return tsk->stack_reclaim_state.node;
}

#ifdef CONFIG_MEMCG
static void set_stack_obj_cgroup(struct task_struct *tsk)
{
	tsk->stack_obj_cgroup = get_obj_cgroup_from_current();
}

static void put_stack_obj_cgroup(struct task_struct *tsk)
{
	if (tsk->stack_obj_cgroup)
		obj_cgroup_put(tsk->stack_obj_cgroup);
}

static inline struct mem_cgroup *get_stack_memcg(struct task_struct *tsk)
{
	return tsk->stack_obj_cgroup ? get_mem_cgroup_from_objcg(tsk->stack_obj_cgroup)
				     : root_mem_cgroup;
}
#else /* !CONFIG_MEMCG */
static void set_stack_obj_cgroup(struct task_struct *tsk) { }

static void put_stack_obj_cgroup(struct task_struct *tsk) { }

static inline struct mem_cgroup *get_stack_memcg(struct task_struct *tsk)
{
	return NULL;
}
#endif /* CONFIG_MEMCG */

void add_to_stack_shrinker(struct task_struct *tsk, int node)
{
	BUILD_BUG_ON(sizeof(tsk->stack_reclaim_state) != sizeof(tsk->stack_reclaim_state.val));
	BUILD_BUG_ON(NODES_SHIFT > 15);

	tsk->stack_reclaim_state.val = 0;
	tsk->stack_reclaim_state.stack_state = STACK_IN_USE;
	tsk->stack_reclaim_state.lru_state = STACK_LRU_NOT_PRESENT;
	set_stack_obj_cgroup(tsk);
	INIT_LIST_HEAD(&tsk->stack_reclaim_list.reclaim_entry);

	if (node == NUMA_NO_NODE)
		node = numa_node_id() | TASK_NUMA_NO_NODE_FLAG;
	tsk->stack_reclaim_state.node = node;
}

static inline int calculate_num_unused_pages(struct task_struct *tsk)
{
	unsigned long top_of_stack = (unsigned long)end_of_stack(tsk);
	/*
	 * Since tsk is !on_rq and !on_cpu, top_of_blocked_task_stack() safely
	 * tells us the end of the stack frame of the inner most context switch
	 * function. Any parts of the stack above that are stale stack frames
	 * or never used, and thus can be unmapped and discarded.
	 */
	return (top_of_blocked_task_stack(&tsk->thread) - top_of_stack) >> PAGE_SHIFT;
}

static bool repopulate_stack(struct task_struct *tsk, bool is_deferred,
			     struct llist_head *fail_list)
{
	int num_missing_pages = 0, nr_allocated = 0, ret;
	struct page *pages[THREAD_SIZE >> PAGE_SHIFT] = {};
	struct vm_struct *vm_area = tsk->stack_vm_area;
	unsigned long addr = (unsigned long)vm_area->addr;
	struct mem_cgroup *tsk_memcg, *old_active_memcg;
	int node = task_alloc_node(tsk);

	num_missing_pages = (THREAD_SIZE >> PAGE_SHIFT) - vm_area->nr_pages;
	if (num_missing_pages == 0)
		return true;

	tsk_memcg = get_stack_memcg(tsk);
	old_active_memcg = set_active_memcg(tsk_memcg);

	if (is_deferred) {
		gfp_t gfp = GFP_KERNEL_ACCOUNT | __GFP_ZERO;
		/*
		 * If the oom killer wants to free memory from this process,
		 * allow access to reserves so the task can hopefully run
		 * sooner to die and thus make progress towards freeing the
		 * process's non-mm memory.
		 */
		if (tsk_is_oom_victim(tsk))
			gfp |= __GFP_MEMALLOC;

		for (; nr_allocated < num_missing_pages; nr_allocated++) {
			pages[nr_allocated] = alloc_pages_node_noprof(node, gfp, 0);
			if (!pages[nr_allocated])
				goto repopulate_fail;
		}
	} else {
		/*
		 * PREEMPT_RT turns spin_trylock() from an atomic cmpxchg into
		 * an operation that takes a rt_mutex's internal raw spin lock.
		 * Doing that from inside the scheduler would result in a
		 * circular locking dependency.
		 */
		if (IS_ENABLED(CONFIG_PREEMPT_RT))
			goto repopulate_fail;

		for (; nr_allocated < num_missing_pages; nr_allocated++) {
			pages[nr_allocated] = alloc_pages_nolock_noprof(__GFP_ACCOUNT,
									node, 0);
			if (!pages[nr_allocated])
				goto repopulate_fail;
		}
	}

	set_active_memcg(old_active_memcg);
	mem_cgroup_put(tsk_memcg);

	for (int i = 0; i < num_missing_pages; i++) {
		vm_area->pages[i] = pages[i];
		mod_lruvec_page_state(pages[i], NR_KERNEL_STACK_KB, PAGE_SIZE / 1024);
		mod_node_page_state(page_pgdat(pages[i]), NR_VMALLOC, 1);
	}
	vm_area->nr_pages = THREAD_SIZE >> PAGE_SHIFT;

	/*
	 * The page tables for the stack were allocated when the stack was
	 * originally created, so we're guaranteed not to need to allocate new
	 * ones. As such, vmap_pages_range() won't acquire any locks and can be
	 * called under the scheduler's raw spinlocks.
	 *
	 * The only way it can fail is if we're trying to colbber an existing
	 * mapping or if the page allocator gave us an invalid page. Neither
	 * case is recoverable.
	 */
	ret = vmap_pages_range(addr, addr + num_missing_pages * PAGE_SIZE,
			       PAGE_KERNEL, vm_area->pages, PAGE_SHIFT);
	BUG_ON(ret != 0);

	// TODO: Clearing pages under the scheduler lock is probably too much
	// work under a raw spinlock. We could try maintaining our own small
	// pool of pre-zero'ed pages instead of using alloc_pages_nolock.
	if (!is_deferred)
		clear_pages((void *)addr, num_missing_pages);

	set_task_stack_end_magic(tsk);
	return true;

repopulate_fail:
	set_active_memcg(old_active_memcg);
	mem_cgroup_put(tsk_memcg);

	while (nr_allocated--)
		free_pages_nolock(pages[nr_allocated], 0);

	if (!fail_list) {
		/*
		 * Preemption is left disabled until wake_stack_repopulate(), to
		 * guarantee that we queue the correct work.
		 */
		preempt_disable();
		fail_list = &this_cpu_ptr(&repopulate_work)->stacks;
	}
	llist_add(&tsk->stack_reclaim_list.refill_entry, fail_list);
	return false;
}

static void release_stack(struct task_struct *tsk)
{
	struct vm_struct *vm_area = tsk->stack_vm_area;
	unsigned long addr = (unsigned long)vm_area->addr;
	int nr_to_free;

	nr_to_free = calculate_num_unused_pages(tsk);

	if (unlikely(nr_to_free == 0))
		return;

	vm_area_unmap_pages(vm_area, addr, addr + nr_to_free * PAGE_SIZE);
	for (int i = 0; i < nr_to_free; i++) {
		mod_lruvec_page_state(vm_area->pages[i], NR_KERNEL_STACK_KB,
				      -(int)(PAGE_SIZE / 1024));
		mod_node_page_state(page_pgdat(vm_area->pages[i]), NR_VMALLOC, -1);
		__free_pages(vm_area->pages[i], 0);
	}
	vm_area->nr_pages -= nr_to_free;
}

static void do_reclaim_stack(struct task_struct *tsk)
{
	union stack_reclaim_state prev_state, target_state;

	release_stack(tsk);

	prev_state.val = READ_ONCE(tsk->stack_reclaim_state.val);
	do {
		target_state.val = prev_state.val;
		target_state.stack_state = STACK_RECLAIMED;
	} while (!try_cmpxchg(&tsk->stack_reclaim_state.val, &prev_state.val, target_state.val));

	if (prev_state.stack_state == STACK_RECLAIMING_IN_USE) {
		struct repopulate_work *work = get_cpu_ptr(&repopulate_work);

		llist_add(&tsk->stack_reclaim_list.refill_entry, &work->stacks);
		queue_work(system_highpri_wq, &work->work);
		put_cpu_ptr(work);
	}
}

static void process_one_new_reclaimable_stack(struct task_struct *tsk, struct list_head *new_stacks)
{
	union stack_reclaim_state prev_state, target_state;

	prev_state.val = READ_ONCE(tsk->stack_reclaim_state.val);
	do {
		target_state.val = prev_state.val;

		switch (prev_state.stack_state) {
		case STACK_IN_USE:
		case STACK_PREPARE_RECLAIM:
			target_state.lru_state = STACK_LRU_NOT_PRESENT;
			break;
		case STACK_RECLAIMABLE:
			target_state.lru_state = STACK_LRU_NEW;
			break;
		case STACK_RECLAIMING:
		case STACK_RECLAIMING_IN_USE:
		case STACK_RECLAIMED:
			/*
			 * These states only happen after a shrinker has started
			 * processing a stack, so seeing one of these means
			 * multiple shrinkers are somehow targeting one stack.
			 */
			WARN(1, "task with stack state %x on list\n", prev_state.val);
			put_task_struct(tsk);
			return;
		}
	} while (!try_cmpxchg(&tsk->stack_reclaim_state.val, &prev_state.val, target_state.val));

	if (target_state.stack_state == STACK_RECLAIMABLE)
		list_add(&tsk->stack_reclaim_list.reclaim_entry, new_stacks);
	else
		put_task_struct(tsk);
}

static void process_new_reclaimable_stacks(void)
{
	LIST_HEAD(new_stacks);
	struct task_struct *tsk, *tmp;

	scoped_guard(raw_spinlock_irq, &new_reclaimable_stacks_lock) {
		while ((tsk = list_first_entry_or_null(&new_reclaimable_stacks,
						       typeof(*tsk),
						       stack_reclaim_list.reclaim_entry))) {
			list_del_init(&tsk->stack_reclaim_list.reclaim_entry);

			/*
			 * We hold a ref on the task during the interval when a
			 * stack is being moved from new_reclaimable_stacks
			 * onto the lru, to avoid needing to deal with races
			 * against remove_from_stack_shrinker(). If tryget
			 * fails here, tsk is about to be deleted, so we can
			 * just skip it.
			 */
			if (!tryget_task_struct(tsk))
				continue;

			raw_spin_unlock_irq(&new_reclaimable_stacks_lock);

			process_one_new_reclaimable_stack(tsk, &new_stacks);

			raw_spin_lock_irq(&new_reclaimable_stacks_lock);
		}
	}

	list_for_each_entry_safe(tsk, tmp, &new_stacks, stack_reclaim_list.reclaim_entry) {
		struct mem_cgroup *memcg = get_stack_memcg(tsk);

		list_del_init(&tsk->stack_reclaim_list.reclaim_entry);

		if (memcg_list_lru_alloc(memcg, &reclaimable_stacks_lru, GFP_ATOMIC) == 0) {
			local_bh_disable();
			list_lru_add(&reclaimable_stacks_lru,
				     &tsk->stack_reclaim_list.reclaim_entry,
				     task_shrinker_node(tsk), memcg);
			local_bh_enable();
		} else {
			union stack_reclaim_state prev_state, target_state;

			pr_warn_ratelimited("failed to allocate stack reclaim metadata\n");

			/*
			 * Just give up if memcg_list_lru_alloc() fails. We
			 * could try immediately reclaiming the stack, but
			 * reclaiming a single stack isn't going to help if
			 * things are so bad a GFP_ATOMIC slab allocation fails.
			 */
			prev_state.val = READ_ONCE(tsk->stack_reclaim_state.val);
			do {
				target_state.val = prev_state.val;
				if (prev_state.stack_state == STACK_RECLAIMABLE)
					target_state.stack_state = STACK_RECLAIMED;
				target_state.lru_state = STACK_LRU_NOT_PRESENT;
			} while (!try_cmpxchg(&tsk->stack_reclaim_state.val,
					      &prev_state.val, target_state.val));
		}

		mem_cgroup_put(memcg);
		put_task_struct(tsk);
	}
}

static enum lru_status isolate_lru_stack(struct list_head *item,
					 struct list_lru_one *lru, void *arg)
{
	struct list_head *to_reclaim = arg;
	struct task_struct *tsk = container_of(item, struct task_struct,
					       stack_reclaim_list.reclaim_entry);
	union stack_reclaim_state prev_state, target_state;
	enum lru_status ret;
	bool is_isolated = false;

	prev_state.val = READ_ONCE(tsk->stack_reclaim_state.val);
	do {
		target_state.val = prev_state.val;

		switch (prev_state.stack_state) {
		case STACK_IN_USE:
		case STACK_PREPARE_RECLAIM:
			target_state.lru_state = STACK_LRU_NOT_PRESENT;
			ret = LRU_REMOVED;
			break;
		case STACK_RECLAIMABLE:
			switch (prev_state.lru_state) {
			case STACK_LRU_NEW:
				target_state.lru_state = STACK_LRU_OLD;
				ret = LRU_ROTATE;
				break;
			case STACK_LRU_OLD:
				target_state.stack_state = STACK_RECLAIMING;
				target_state.lru_state = STACK_LRU_NOT_PRESENT;
				ret = LRU_REMOVED;
				break;
			case STACK_LRU_NOT_PRESENT:
			case STACK_LRU_PENDING:
				WARN(1, "item on stack reclaim lru with bad state\n");
				list_lru_isolate(lru, item);
				return LRU_REMOVED;
			}
			break;
		case STACK_RECLAIMING:
		case STACK_RECLAIMING_IN_USE:
		case STACK_RECLAIMED:
			/*
			 * These states only happen after a shrinker has started
			 * processing a stack, so seeing one of these means
			 * multiple shrinkers are somehow targeting one stack.
			 */
			WARN(1, "stack with state %x on list\n", prev_state.val);
			list_lru_isolate(lru, item);
			return LRU_REMOVED;
		}

		/*
		 * We need to isolate the item before the cmpxchg to prevent
		 * races with process_one_new_reclaimable_stack() adding the
		 * entry to its new_stacks list.
		 */
		if (ret == LRU_REMOVED && !is_isolated) {
			is_isolated = true;
			list_lru_isolate(lru, item);
		}
	} while (!try_cmpxchg(&tsk->stack_reclaim_state.val, &prev_state.val, target_state.val));

	if (target_state.stack_state == STACK_RECLAIMING) {
		/*
		 * The task can't be freed since STACK_RECLAIMING prevents
		 * it from running and exiting, so no need to hold a ref.
		 */
		list_add_tail(item, to_reclaim);
	} else if (target_state.stack_state == STACK_RECLAIMABLE) {
		/*
		 * If we isolated but then lost a cmpxchg race against
		 * __allow_stack_reclaim(), we need to undo the isolation.
		 *
		 * STACK_RECLAIMABLE means we observed the task blocked (i.e.
		 * not dead). Even if the task dies and its refcount hits zero,
		 * RCU cleanup of the task will be delayed because the lru walk
		 * is guarded by local_bh_disable(), so unlocking won't cause
		 * races with remove_from_stack_shrinker().
		 */
		if (is_isolated) {
			struct mem_cgroup *lru_memcg;

			spin_unlock(&lru->lock);

			lru_memcg = get_stack_memcg(tsk);
			list_lru_add(&reclaimable_stacks_lru, item,
				     task_shrinker_node(tsk), lru_memcg);
			mem_cgroup_put(lru_memcg);

			ret = LRU_REMOVED_RETRY;

		} else {
			WARN(target_state.lru_state != STACK_LRU_OLD,
			     "Reclaimable stack with bad state %x\n", target_state.val);
		}
	} else {
		WARN(target_state.lru_state != STACK_LRU_NOT_PRESENT,
		     "Isolate stack with bad state %x\n", target_state.val);
	}

	return ret;
}

static unsigned long scan_reclaimable_stacks(struct shrinker *shrinker,
					     struct shrink_control *sc)
{
	unsigned long freed = 0;
	LIST_HEAD(to_reclaim);
	struct task_struct *tsk, *tmp;

	sc->nr_to_scan >>= STACK_COUNT_SHIFT;
	local_bh_disable();
	list_lru_shrink_walk(&reclaimable_stacks_lru, sc,
			     isolate_lru_stack, &to_reclaim);
	local_bh_enable();

	list_for_each_entry_safe(tsk, tmp, &to_reclaim, stack_reclaim_list.reclaim_entry) {
		freed++;
		list_del_init(&tsk->stack_reclaim_list.reclaim_entry);
		do_reclaim_stack(tsk);
	}

	return freed << STACK_COUNT_SHIFT;
}

static unsigned long get_reclaimable_stack_count(struct shrinker *shrinker,
						 struct shrink_control *sc)
{
	unsigned long count;

	process_new_reclaimable_stacks();
	count = list_lru_shrink_count(&reclaimable_stacks_lru, sc);

	return count ? (unsigned long)(count << STACK_COUNT_SHIFT) : SHRINK_EMPTY;
}

static void do_repopulate_stacks(struct work_struct *w)
{
	struct repopulate_work *work = container_of(w, struct repopulate_work, work);
	struct llist_node *head;

	while ((head = llist_del_all(&work->stacks))) {
		struct task_struct *tsk, *tmp;

		llist_for_each_entry_safe(tsk, tmp, head, stack_reclaim_list.refill_entry) {
			init_llist_node(&tsk->stack_reclaim_list.refill_entry);
			if (repopulate_stack(tsk, true, &work->stacks)) {
				wake_up_state(tsk, TASK_STACK_RECLAIM);
			} else {
				/*
				 * Repopulate only fails due to low memory. If
				 * that happens, give the rest of the system a
				 * chance to free some memory.
				 */
				cond_resched();
			}
		}
	}
}

bool __ensure_stack_is_present(struct task_struct *tsk, bool *need_deferred_repopulate)
{
	union stack_reclaim_state prev_state, target_state;

	*need_deferred_repopulate = false;
	prev_state.val = READ_ONCE(tsk->stack_reclaim_state.val);
	do {
		target_state.val = prev_state.val;

		switch (prev_state.stack_state) {
		case STACK_IN_USE:
			return true;
		case STACK_PREPARE_RECLAIM:
		case STACK_RECLAIMABLE:
		case STACK_RECLAIMED:
			/*
			 * Transitioning STACK_RECLAIM -> STACK_IN_USE won't
			 * combine with the prior STACK_IN_USE case to lead to
			 * tasks with unpopulated stacks running. If immediate
			 * repopulation fails, then ttwu() puts the task in the
			 * TASK_STACK_RECLAIM state, so the only ttwu() that can
			 * wake up the task is after we repopulate the stack.
			 */
			target_state.stack_state = STACK_IN_USE;
			break;
		case STACK_RECLAIMING:
			target_state.stack_state = STACK_RECLAIMING_IN_USE;
			break;
		case STACK_RECLAIMING_IN_USE:
			/*
			 * Tasks with stacks in state STACK_RECLAIMING_IN_USE
			 * should have __state == TASK_STACK_RECLAIM state, so
			 * ttwu_state_match() should reject any wakeups other
			 * than the one after the stack gets repopulated.
			 */
			WARN(1, "TASK_STACK_RECLAIM violation");
			return false;
		}
	} while (!try_cmpxchg(&tsk->stack_reclaim_state.val, &prev_state.val, target_state.val));

	switch (prev_state.stack_state) {
	case STACK_RECLAIMABLE:
	case STACK_PREPARE_RECLAIM:
		return true;
	case STACK_RECLAIMING:
		return false;
	case STACK_RECLAIMED:
		if (repopulate_stack(tsk, false, NULL))
			return true;
		*need_deferred_repopulate = true;
		return false;
	default:
		// Unreachable due to return statements in cmpxchg loop
		unreachable();
	}
}

void __prepare_stack_for_reclaim(struct task_struct *tsk)
{
	union stack_reclaim_state prev_state, target_state;

	// TODO: Skip rt threads

	prev_state.val = READ_ONCE(tsk->stack_reclaim_state.val);
	do {
		target_state.val = prev_state.val;

		if (prev_state.stack_state == STACK_IN_USE) {
			target_state.stack_state = STACK_PREPARE_RECLAIM;
		} else {
			WARN(1, "Runnable thread with reclaimable stack state=%x",
			     prev_state.stack_state);
			return;
		}
	} while (!try_cmpxchg(&tsk->stack_reclaim_state.val, &prev_state.val, target_state.val));

	/*
	 * With delayed dequeue, __allow_stack_reclaim() can be called by
	 * finish_task() before __block_task() calls this function. When
	 * that happens, __allow_stack_reclaim() sees STACK_IN_USE and is
	 * thus a no-op. We need a call here to progress the state machine.
	 *
	 * Note that __block_task() is called under the rq lock, so we don't
	 * need to worry about concurrent calls.
	 */
	if (!tsk->on_cpu)
		__allow_stack_reclaim(tsk);
}

void __allow_stack_reclaim(struct task_struct *tsk)
{
	union stack_reclaim_state prev_state, target_state;
	bool add_to_list = false;

	if (WARN_ON_ONCE(tsk->__state == TASK_DEAD))
		return;

	prev_state.val = READ_ONCE(tsk->stack_reclaim_state.val);
	do {
		target_state.val = prev_state.val;

		if (prev_state.stack_state != STACK_PREPARE_RECLAIM) {
			WARN(prev_state.stack_state != STACK_IN_USE,
			     "Reclaimable state %x for previously running task", prev_state.val);
			return;
		}
		target_state.stack_state = STACK_RECLAIMABLE;
		if (prev_state.lru_state == STACK_LRU_NOT_PRESENT) {
			target_state.lru_state = STACK_LRU_PENDING;
			add_to_list = true;
		} else if (prev_state.lru_state == STACK_LRU_OLD) {
			target_state.lru_state = STACK_LRU_NEW;
		}
	} while (!try_cmpxchg(&tsk->stack_reclaim_state.val, &prev_state.val, target_state.val));

	if (add_to_list) {
		guard(raw_spinlock_irqsave)(&new_reclaimable_stacks_lock);
		list_add(&tsk->stack_reclaim_list.reclaim_entry, &new_reclaimable_stacks);
	}
}

/*
 * This function is called when the task is deleted, which can happen well
 * after the task releases its stack. However, a dead task will never have
 * TASK_STACK_RECLAIM set, so its last context switch will leave the stack
 * state as STACK_IN_USE. As such, if the shrinker processes a task after its
 * death, it will remove the task from the lru without accessing the stack.
 */
void remove_from_stack_shrinker(struct task_struct *tsk)
{
	struct mem_cgroup *lru_memcg;

	/*
	 * Since tsk is being deleted, the tryget_task_struct() call in
	 * process_new_reclaimable_stacks() excludes racing with a shrinker
	 * moving the task from STACK_LRU_PENDING -> STACK_LRU_NEW. A race can
	 * just result in the delete operation being a no-op.
	 */
	switch (READ_ONCE(tsk->stack_reclaim_state.lru_state)) {
	case STACK_LRU_NOT_PRESENT:
		break;
	case STACK_LRU_PENDING:
		scoped_guard(raw_spinlock_irqsave, &new_reclaimable_stacks_lock) {
			if (!list_empty(&tsk->stack_reclaim_list.reclaim_entry))
				list_del_init(&tsk->stack_reclaim_list.reclaim_entry);
		}
		break;
	case STACK_LRU_NEW:
	case STACK_LRU_OLD:
		lru_memcg = get_stack_memcg(tsk);
		local_bh_disable();
		list_lru_del(&reclaimable_stacks_lru, &tsk->stack_reclaim_list.reclaim_entry,
			     task_shrinker_node(tsk), lru_memcg);
		local_bh_enable();
		mem_cgroup_put(lru_memcg);
		break;
	}
	put_stack_obj_cgroup(tsk);
}

void wake_stack_repopulate(void)
{
	queue_work(system_highpri_wq, &this_cpu_ptr(&repopulate_work)->work);
	preempt_enable();
}

static struct lock_class_key stack_shrinker_key;

static int stack_shrinker_cpuhp_setup(unsigned int cpu)
{
	struct repopulate_work *work = per_cpu_ptr(&repopulate_work, cpu);

	init_llist_head(&work->stacks);
	INIT_WORK(&work->work, do_repopulate_stacks);
	return 0;
}

static int stack_shrinker_cpuhp_teardown(unsigned int cpu)
{
	flush_work(&per_cpu_ptr(&repopulate_work, cpu)->work);
	return 0;
}

static int __init fork_late_init(void)
{
	struct shrinker *shrinker;
	const char *msg;
	int ret;
	enum cpuhp_state cpuhp_val;

	ret = cpuhp_setup_state(CPUHP_BP_PREPARE_DYN, "stack_shrinker",
				stack_shrinker_cpuhp_setup,
				stack_shrinker_cpuhp_teardown);
	if (ret < 0) {
		msg = "cpuhp failure";
		goto cpuhp_setup_fail;
	}
	cpuhp_val = ret;

	shrinker = shrinker_alloc(SHRINKER_NUMA_AWARE | SHRINKER_MEMCG_AWARE,
				  "stack_shrinker");
	if (!shrinker) {
		msg = "shrinker alloc failure";
		ret = -ENOMEM;
		goto shrinker_alloc_fail;
	}

	ret = list_lru_init_memcg_key(&reclaimable_stacks_lru, shrinker,
				      &stack_shrinker_key);
	if (ret != 0) {
		msg = "list_lru_init failure";
		goto list_lru_init_fail;
	}

	shrinker->count_objects = get_reclaimable_stack_count;
	shrinker->scan_objects = scan_reclaimable_stacks;
	shrinker->seeks = 4;
	shrinker->batch = STACK_BATCH_SIZE << STACK_COUNT_SHIFT;
	shrinker_register(shrinker);
	return 0;

list_lru_init_fail:
	shrinker_free(shrinker);
shrinker_alloc_fail:
	cpuhp_remove_state(cpuhp_val);
cpuhp_setup_fail:
	WARN(1, "Failed to initialize stack_shrinker %s: %d\n", msg, ret);
	return 0;
}

module_init(fork_late_init);
