// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for objpool pushes that nest on the same per-CPU slot.
 *
 * objpool_push() only pushes to the slot of the local CPU and runs with
 * interrupts disabled, but it can still be interrupted by an NMI that
 * pushes to the same slot: kretprobes are allowed in NMI context, and a
 * probed function that returns in NMI context recycles its instance with
 * objpool_push().  UML has no NMIs, so these tests push from task context
 * with interrupts enabled, and a pinned hrtimer pushes to the same slot
 * from hardirq context in between.  Nesting is the same as with an NMI.
 */

#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/objpool.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define OP_NR_OBJS	64
#define OP_RESERVE	4
#define OP_TIMER_NS	(10 * NSEC_PER_USEC)
#define OP_RUN_MS	5000

enum { OP_TASK, OP_IRQ, OP_NR_STOCKS };

struct op_obj {
	atomic_t held;		/* 0 while the object is in the pool */
	int owner;		/* stock the object goes back to */
};

struct op_ctx {
	struct objpool_head pool;
	struct op_obj *objs[OP_NR_OBJS];
	struct op_obj *reserve[OP_RESERVE];
	int nreserve;

	raw_spinlock_t lock;	/* protects the stocks */
	struct op_obj *stock[OP_NR_STOCKS][OP_NR_OBJS];
	int nstock[OP_NR_STOCKS];

	struct hrtimer timer;
	int cpu;
	bool remote_pop;
	bool stop;
	bool popper_done;

	bool in_push;		/* the task is inside __objpool_try_add_slot() */

	unsigned long pushes, irq_pushes, pops;
	unsigned long nested;	/* timer pushes that interrupted a task push */
	unsigned long hidden;	/* last != tail after the task's push returned */
	unsigned long doubles;	/* pop returned an object that is not in the pool */
	struct completion done;
};

static struct op_obj *op_take(struct op_ctx *ctx, int stock)
{
	struct op_obj *obj = NULL;
	unsigned long flags;

	raw_spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->nstock[stock])
		obj = ctx->stock[stock][--ctx->nstock[stock]];
	raw_spin_unlock_irqrestore(&ctx->lock, flags);
	return obj;
}

static void op_give(struct op_ctx *ctx, struct op_obj *obj)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&ctx->lock, flags);
	ctx->stock[obj->owner][ctx->nstock[obj->owner]++] = obj;
	raw_spin_unlock_irqrestore(&ctx->lock, flags);
}

/* an object popped from the pool: it must have been in the pool */
static void op_popped(struct op_ctx *ctx, struct op_obj *obj)
{
	ctx->pops++;
	if (atomic_xchg(&obj->held, 1)) {
		/* someone else holds it: don't put it in a stock twice */
		ctx->doubles++;
		return;
	}
	op_give(ctx, obj);
}

static enum hrtimer_restart op_timer_fn(struct hrtimer *timer)
{
	struct op_ctx *ctx = container_of(timer, struct op_ctx, timer);
	struct op_obj *obj;

	if (READ_ONCE(ctx->stop))
		return HRTIMER_NORESTART;

	/* the "NMI": push to the slot of the CPU it interrupted */
	obj = op_take(ctx, OP_IRQ);
	if (obj) {
		if (READ_ONCE(ctx->in_push))
			ctx->nested++;
		atomic_set(&obj->held, 0);
		objpool_push(obj, &ctx->pool);
		ctx->irq_pushes++;
	}

	hrtimer_forward_now(timer, ns_to_ktime(OP_TIMER_NS));
	return HRTIMER_RESTART;
}

/* push from task context, with interrupts enabled so that the timer can nest */
static void op_task_push(struct op_ctx *ctx, struct op_obj *obj)
{
	struct objpool_slot *slot = ctx->pool.cpu_slots[ctx->cpu];
	unsigned long flags;

	atomic_set(&obj->held, 0);
	WRITE_ONCE(ctx->in_push, true);
	__objpool_try_add_slot(obj, &ctx->pool, ctx->cpu);
	WRITE_ONCE(ctx->in_push, false);
	ctx->pushes++;

	/*
	 * No push to this slot is in flight now: the timer only interrupts
	 * us, and other CPUs never push to it.  Every entry must be visible.
	 */
	local_irq_save(flags);
	if (READ_ONCE(slot->last) != READ_ONCE(slot->tail))
		ctx->hidden++;
	local_irq_restore(flags);
}

static int op_task_fn(void *data)
{
	struct op_ctx *ctx = data;
	unsigned long end = jiffies + msecs_to_jiffies(OP_RUN_MS);
	struct op_obj *obj;
	unsigned long flags;

	hrtimer_start(&ctx->timer, ns_to_ktime(OP_TIMER_NS),
		      HRTIMER_MODE_REL_PINNED_HARD);

	while (time_before(jiffies, end)) {
		obj = op_take(ctx, OP_TASK);
		if (obj)
			op_task_push(ctx, obj);

		if (!ctx->remote_pop) {
			/* consume what is visible, like kretprobe entries would */
			local_irq_save(flags);
			while ((obj = __objpool_try_get_slot(&ctx->pool, ctx->cpu)))
				op_popped(ctx, obj);
			local_irq_restore(flags);
		}
		cond_resched();
	}

	WRITE_ONCE(ctx->stop, true);
	hrtimer_cancel(&ctx->timer);

	/*
	 * A remote pop may be spinning because 'last' went backwards; any
	 * later push to the slot releases it.  Use the reserve for that.
	 */
	while (ctx->remote_pop && !READ_ONCE(ctx->popper_done)) {
		/* keep one reserve object for the teardown */
		if (ctx->nreserve > 1) {
			obj = ctx->reserve[--ctx->nreserve];
			atomic_set(&obj->held, 0);
			local_irq_save(flags);
			__objpool_try_add_slot(obj, &ctx->pool, ctx->cpu);
			local_irq_restore(flags);
		}
		schedule_timeout_uninterruptible(1);
	}

	complete(&ctx->done);
	return 0;
}

static int op_popper_fn(void *data)
{
	struct op_ctx *ctx = data;
	struct op_obj *obj;

	while (!READ_ONCE(ctx->stop)) {
		obj = objpool_pop(&ctx->pool);
		if (obj)
			op_popped(ctx, obj);
		cond_resched();
	}
	WRITE_ONCE(ctx->popper_done, true);
	return 0;
}

static int op_objinit(void *obj, void *context)
{
	atomic_set(&((struct op_obj *)obj)->held, 0);
	return 0;
}

static void op_run(struct kunit *test, bool remote_pop)
{
	struct task_struct *task, *popper = NULL;
	struct objpool_slot *slot;
	struct op_ctx *ctx;
	unsigned long flags;
	int i, n = 0, cpu, pcpu, in_pool = 0, lost;
	struct op_obj *obj;

	if (remote_pop && num_online_cpus() < 2)
		kunit_skip(test, "needs at least 2 CPUs");

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->remote_pop = remote_pop;
	raw_spin_lock_init(&ctx->lock);
	init_completion(&ctx->done);
	hrtimer_setup(&ctx->timer, op_timer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED_HARD);

	KUNIT_ASSERT_EQ(test, 0, objpool_init(&ctx->pool, OP_NR_OBJS,
					      sizeof(struct op_obj), GFP_KERNEL,
					      NULL, op_objinit, NULL));

	/* take every object out of the pool; hand them to the stocks */
	while (n < OP_NR_OBJS && (obj = objpool_pop(&ctx->pool)))
		ctx->objs[n++] = obj;
	KUNIT_ASSERT_EQ(test, n, OP_NR_OBJS);
	for (i = 0; i < OP_NR_OBJS; i++) {
		obj = ctx->objs[i];
		atomic_set(&obj->held, 1);
		if (i < OP_RESERVE) {
			ctx->reserve[ctx->nreserve++] = obj;
			obj->owner = OP_TASK;
			continue;
		}
		obj->owner = i & 1 ? OP_IRQ : OP_TASK;
		ctx->stock[obj->owner][ctx->nstock[obj->owner]++] = obj;
	}

	cpu = cpumask_first(cpu_online_mask);
	pcpu = cpumask_next(cpu, cpu_online_mask);
	ctx->cpu = cpu;
	slot = ctx->pool.cpu_slots[cpu];

	task = kthread_create(op_task_fn, ctx, "objpool_kunit_push");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	kthread_bind(task, cpu);
	if (remote_pop) {
		popper = kthread_create(op_popper_fn, ctx, "objpool_kunit_pop");
		KUNIT_ASSERT_FALSE(test, IS_ERR(popper));
		kthread_bind(popper, pcpu);
		wake_up_process(popper);
	}
	wake_up_process(task);
	wait_for_completion(&ctx->done);

	/*
	 * Quiescent now.  Objects that are in the pool but outside
	 * [head, tail) can never be popped again: they are lost.
	 */
	for (i = 0; i < OP_NR_OBJS; i++)
		if (!atomic_read(&ctx->objs[i]->held))
			in_pool++;
	lost = in_pool - (int)(READ_ONCE(slot->tail) - READ_ONCE(slot->head));

	kunit_info(test, "%lu task pushes, %lu timer pushes (%lu nested), %lu pops: %lu hidden, %lu double handouts, %d lost\n",
		   ctx->pushes, ctx->irq_pushes, ctx->nested, ctx->pops,
		   ctx->hidden, ctx->doubles, lost);

	/* no nesting means the test proved nothing */
	KUNIT_EXPECT_GT(test, ctx->nested, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->hidden, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->doubles, 0UL);
	KUNIT_EXPECT_EQ(test, lost, 0);

	/*
	 * Tear down: republish the slot (a push sets 'last' past 'head'),
	 * drain it, then drop every object and the pool.
	 */
	if (ctx->nreserve) {
		obj = ctx->reserve[--ctx->nreserve];
		atomic_set(&obj->held, 0);
		local_irq_save(flags);
		__objpool_try_add_slot(obj, &ctx->pool, cpu);
		local_irq_restore(flags);
	}
	if ((int)(READ_ONCE(slot->last) - READ_ONCE(slot->head)) >= 0) {
		while (objpool_pop(&ctx->pool))
			;
	}
	for (i = 0; i < OP_NR_OBJS; i++)
		objpool_drop(ctx->objs[i], &ctx->pool);
	objpool_fini(&ctx->pool);
}

static void objpool_test_nested_push(struct kunit *test)
{
	op_run(test, false);
}

static void objpool_test_nested_push_remote_pop(struct kunit *test)
{
	op_run(test, true);
}

static struct kunit_case objpool_test_cases[] = {
	KUNIT_CASE_SLOW(objpool_test_nested_push),
	KUNIT_CASE_SLOW(objpool_test_nested_push_remote_pop),
	{}
};

static struct kunit_suite objpool_test_suite = {
	.name = "objpool",
	.test_cases = objpool_test_cases,
};

kunit_test_suite(objpool_test_suite);

MODULE_DESCRIPTION("KUnit tests for objpool");
MODULE_LICENSE("GPL");
