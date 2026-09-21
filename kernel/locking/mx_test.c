// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
#include <linux/osq_lock.h>
#endif
#include <linux/random.h>
#include <linux/rwlock.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/ww_mutex.h>

#if IS_MODULE(CONFIG_MX_TEST) && defined(CONFIG_LOCK_SPIN_ON_OWNER)
#include "osq_lock.c"
#endif

/*
 * The default here is derived from an empirical test, where
 * "osq_lock_busted" was run 100 times of one second, varying the
 * number of elements. This was run on a 160 CPU Arm BM system. The
 * percentage failures were:
 *    #elements	      percent failures
 * ===================================
 *	   1024		     90
 *	   2048		     95
 *	   4096		     92
 *	   8192		     94
 *	  16384		     85
 *	  32768		     79
 *	  65536		     66
 */
static unsigned int mx_nmbr_elems = 2048;
static unsigned long mx_scnds_per_test = 1;
static char *mx_test = "spin_lock";

module_param(mx_nmbr_elems, uint, 0444);
MODULE_PARM_DESC(mx_nmbr_elems, "Number of elements (default 2048)");

module_param(mx_scnds_per_test, ulong, 0444);
MODULE_PARM_DESC(mx_scnds_per_test, "Number of seconds to run for each test iteration (default is 1)");

module_param(mx_test, charp, 0444);
MODULE_PARM_DESC(mx_test,
		 "Mutual exclusion method or lock-free method (spin_lock, spin_lock_irq, spin_lock_irqsave, write_lock, ...)");

#define MX_RDS_IN_XMIT 2
#define MX_SOME_BIT 13
static DEFINE_WD_CLASS(wd_class);
static DEFINE_WW_CLASS(ww_class);

enum mx_test {
	MX_ILLEGAL,
	MX_BUSTED,
	MX_SPIN_LOCK,
	MX_SPIN_LOCK_IRQ,
	MX_SPIN_LOCK_IRQSAVE,
	MX_RW_LOCK_W,
	MX_RW_LOCK_W_BH,
	MX_RW_LOCK_TRW,
	MX_RW_LOCK_TRW_BH,
	MX_MUTEX,
	MX_ATOMIC_ADD,
	MX_ATOMIC64_ADD,
	MX_CMPXCHG,
	MX_RDS_BUSTED,
	MX_TEST_AND_SET_BIT_LOCK,
	MX_TEST_AND_SET_BIT_INNOV,
	MX_TEST_AND_SET_BIT_PLAIN,
	MX_TEST_AND_CLEAR_BIT_INNOV,
	MX_TEST_AND_CLEAR_BIT_PLAIN,
	MX_SINGLE_WW_MUTEX_WW,	/* Wound-Wait */
	MX_SINGLE_WW_MUTEX_WD,	/* Wait-Die */
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
	MX_OSQ_LOCK_BUSTED,
#endif
	MX_ATOMIC_XCHG,
};

struct mx_locks {
	/* This union contains locks and lock-free data types */
	union {
		/* Protecting the counter below */
		spinlock_t spinlock;
		rwlock_t rwlock;
		/* Protecting the counter below */
		struct mutex mutex;
		atomic_t atomic_lock;
		atomic_t atomic_counter;
		atomic64_t atomic64_counter;
		long cmpxchg_counter;
		unsigned long bits;
		struct ww_mutex ww_mutex;
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
		struct optimistic_spin_queue osq_lock;
#endif
	};
};

struct mx_inc_dec_params {
	struct work_struct work;
	struct mx_locks *locks;
	long *counters;
	unsigned int nelems;
	unsigned long timeout;
	enum mx_test mx_type;
	u64 iter;
	struct completion completion;
};

static bool mx_lockless_test(enum mx_test typ)
{
	return
		(typ == MX_ATOMIC_ADD)  ||
		(typ == MX_ATOMIC64_ADD)  ||
		(typ == MX_CMPXCHG);
}

static inline unsigned int mx_rand(unsigned int *seed)
{
	*seed = *seed * 69069 + 111;
	return *seed;
}

static inline void mx_acquire(struct mx_locks *locks, enum mx_test typ, unsigned long *flags_ptr)
{
	int ret;

	switch (typ) {
	case MX_BUSTED:
		break;
	case MX_SPIN_LOCK:
		spin_lock(&locks->spinlock);
		break;
	case MX_SPIN_LOCK_IRQ:
		spin_lock_irq(&locks->spinlock);
		break;
	case MX_SPIN_LOCK_IRQSAVE:
		spin_lock_irqsave(&locks->spinlock, *flags_ptr);
		break;
	case MX_RW_LOCK_W:
		write_lock(&locks->rwlock);
		break;
	case MX_RW_LOCK_W_BH:
		write_lock_bh(&locks->rwlock);
		break;
	case MX_RW_LOCK_TRW:
		while (!read_trylock(&locks->rwlock))
			;
		read_unlock(&locks->rwlock);
		write_lock(&locks->rwlock);
		break;
	case MX_RW_LOCK_TRW_BH:
		while (!read_trylock(&locks->rwlock))
			;
		read_unlock(&locks->rwlock);
		write_lock_bh(&locks->rwlock);
		break;
	case MX_MUTEX:
		mutex_lock(&locks->mutex);
		break;
	case MX_ATOMIC_ADD:
	case MX_ATOMIC64_ADD:
	case MX_CMPXCHG:
		break;
	case MX_RDS_BUSTED:
		while (test_and_set_bit(MX_RDS_IN_XMIT, &locks->bits))
			;
		break;
	case MX_TEST_AND_SET_BIT_LOCK:
		while (test_and_set_bit_lock(MX_RDS_IN_XMIT, &locks->bits))
			;
		break;
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
		while (test_and_set_bit(MX_SOME_BIT, &locks->bits))
			;
		break;
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
		while (!test_and_clear_bit(MX_SOME_BIT, &locks->bits))
			;
		break;
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
		ret = ww_mutex_lock(&locks->ww_mutex, NULL);
		WARN_ONCE(ret, "ww_mutex_lock returned %d for the single w/w mutex case\n", ret);
		break;
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
	case MX_OSQ_LOCK_BUSTED:
		preempt_disable();
		while (!osq_lock(&locks->osq_lock)) {
			preempt_enable();
			cond_resched();
			preempt_disable();
		}
		break;
#endif
	case MX_ATOMIC_XCHG:
		while (atomic_xchg_acquire(&locks->atomic_lock, 1) != 0)
			cpu_relax();
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

static inline void mx_release(struct mx_locks *locks, enum mx_test typ, unsigned long *flags_ptr)
{
	switch (typ) {
	case MX_BUSTED:
		break;
	case MX_SPIN_LOCK:
		spin_unlock(&locks->spinlock);
		break;
	case MX_SPIN_LOCK_IRQ:
		spin_unlock_irq(&locks->spinlock);
		break;
	case MX_SPIN_LOCK_IRQSAVE:
		spin_unlock_irqrestore(&locks->spinlock, *flags_ptr);
		break;
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_TRW:
		write_unlock(&locks->rwlock);
		break;
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW_BH:
		write_unlock_bh(&locks->rwlock);
		break;
	case MX_MUTEX:
		mutex_unlock(&locks->mutex);
		break;
	case MX_ATOMIC_ADD:
	case MX_ATOMIC64_ADD:
	case MX_CMPXCHG:
		break;
	case MX_RDS_BUSTED:
		clear_bit(MX_RDS_IN_XMIT, &locks->bits);
		/* Deliberately outside the critical region, as RDS did prior to 1422f288 */
		smp_mb__after_atomic();
		break;
	case MX_TEST_AND_SET_BIT_LOCK:
		clear_bit_unlock(MX_RDS_IN_XMIT, &locks->bits);
		break;
	case MX_TEST_AND_SET_BIT_INNOV:
		/*
		 * Ensuring global visibility on other processors by
		 * means of a failing test_and_set_bit()
		 */
		(void)test_and_set_bit(MX_SOME_BIT, &locks->bits);
		clear_bit(MX_SOME_BIT, &locks->bits);
		break;
	case MX_TEST_AND_SET_BIT_PLAIN:
		/* Ensure the counter gets global visiblity before releasing the lock*/
		smp_mb__before_atomic();
		clear_bit(MX_SOME_BIT, &locks->bits);
		break;
	case MX_TEST_AND_CLEAR_BIT_INNOV:
		/*
		 * Ensuring global visibility on other processors by
		 * means of a failing test_and_clear_bit()
		 */
		(void)test_and_clear_bit(MX_SOME_BIT, &locks->bits);
		set_bit(MX_SOME_BIT, &locks->bits);
		break;
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
		/* Ensure the counter gets global visiblity before releasing the lock*/
		smp_mb__before_atomic();
		set_bit(MX_SOME_BIT, &locks->bits);
		break;
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
		ww_mutex_unlock(&locks->ww_mutex);
		break;
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
	case MX_OSQ_LOCK_BUSTED:
		osq_unlock(&locks->osq_lock);
		preempt_enable();
		break;
#endif
	case MX_ATOMIC_XCHG:
		atomic_set_release(&locks->atomic_lock, 0);
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

static inline void mx_add(struct mx_locks *locks, long *counters, enum mx_test typ, long addend)
{
	long old;

	switch (typ) {
	case MX_BUSTED:
	case MX_SPIN_LOCK:
	case MX_SPIN_LOCK_IRQ:
	case MX_SPIN_LOCK_IRQSAVE:
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW:
	case MX_RW_LOCK_TRW_BH:
	case MX_MUTEX:
	case MX_RDS_BUSTED:
	case MX_TEST_AND_SET_BIT_LOCK:
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
	case MX_OSQ_LOCK_BUSTED:
#endif
	case MX_ATOMIC_XCHG:
		*counters += addend;
		break;
	case MX_ATOMIC_ADD:
		atomic_add(addend, &locks->atomic_counter);
		break;
	case MX_ATOMIC64_ADD:
		atomic64_add(addend, &locks->atomic64_counter);
		break;
	case MX_CMPXCHG:
		do {
			old = READ_ONCE(locks->cmpxchg_counter);
		} while (cmpxchg(&locks->cmpxchg_counter, old, old + addend) != old);
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

static inline long mx_read(struct mx_locks *locks, long *counters, enum mx_test typ)
{
	switch (typ) {
	case MX_BUSTED:
	case MX_SPIN_LOCK:
	case MX_SPIN_LOCK_IRQ:
	case MX_SPIN_LOCK_IRQSAVE:
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW:
	case MX_RW_LOCK_TRW_BH:
	case MX_MUTEX:
	case MX_RDS_BUSTED:
	case MX_TEST_AND_SET_BIT_LOCK:
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
	case MX_OSQ_LOCK_BUSTED:
#endif
	case MX_ATOMIC_XCHG:
		return *counters;
	case MX_ATOMIC_ADD:
		return atomic_read(&locks->atomic_counter);
	case MX_ATOMIC64_ADD:
		return atomic64_read(&locks->atomic64_counter);
	case MX_CMPXCHG:
		return locks->cmpxchg_counter;
	default:
		WARN_ON_ONCE(true);
	}
	return 0;
}

static inline void mx_init(struct mx_locks *locks, enum mx_test typ)
{
	switch (typ) {
	case MX_SPIN_LOCK:
	case MX_SPIN_LOCK_IRQ:
	case MX_SPIN_LOCK_IRQSAVE:
		spin_lock_init(&locks->spinlock);
		break;
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW:
	case MX_RW_LOCK_TRW_BH:
		rwlock_init(&locks->rwlock);
		break;
	case MX_MUTEX:
		mutex_init(&locks->mutex);
		break;
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
		set_bit(MX_SOME_BIT, &locks->bits);
		break;
	/* The following relies on the vzalloc below */
	case MX_BUSTED:
	case MX_ATOMIC_ADD:
	case MX_ATOMIC64_ADD:
	case MX_CMPXCHG:
	case MX_RDS_BUSTED:
	case MX_TEST_AND_SET_BIT_LOCK:
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
	case MX_ATOMIC_XCHG:
		break;
	case MX_SINGLE_WW_MUTEX_WW:
		ww_mutex_init(&locks->ww_mutex, &ww_class);
		break;
	case MX_SINGLE_WW_MUTEX_WD:
		ww_mutex_init(&locks->ww_mutex, &wd_class);
		break;
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
	case MX_OSQ_LOCK_BUSTED:
		osq_lock_init(&locks->osq_lock);
		break;
#endif
	default:
		WARN_ON_ONCE(true);
	}
}

static inline void mx_fini(struct mx_locks *locks, enum mx_test typ)
{
	switch (typ) {
	case MX_SPIN_LOCK:
	case MX_SPIN_LOCK_IRQ:
	case MX_SPIN_LOCK_IRQSAVE:
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW:
	case MX_RW_LOCK_TRW_BH:
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
	case MX_BUSTED:
	case MX_ATOMIC_ADD:
	case MX_ATOMIC64_ADD:
	case MX_CMPXCHG:
	case MX_RDS_BUSTED:
	case MX_TEST_AND_SET_BIT_LOCK:
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
	case MX_OSQ_LOCK_BUSTED:
#endif
	case MX_ATOMIC_XCHG:
		break;
	case MX_MUTEX:
		mutex_destroy(&locks->mutex);
		break;
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
		ww_mutex_destroy(&locks->ww_mutex);
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

static void mx_inc_dec(struct work_struct *_work)
{
	struct mx_inc_dec_params *params;
	unsigned long flags;
	struct mx_locks *locks;
	unsigned int seed;
	u64 iter = 0;

	params = container_of(_work, struct mx_inc_dec_params, work);
	locks = params->locks;

	get_random_bytes(&seed, sizeof(seed));
	do {
		unsigned int inx1 = mx_rand(&seed) % params->nelems;
		unsigned int inx2 = mx_rand(&seed) % params->nelems;
		struct mx_locks *rnd_el1 = locks + inx1;
		struct mx_locks *rnd_el2 = locks + inx2;

		++iter;

		mx_acquire(rnd_el1, params->mx_type, &flags);
		mx_add(rnd_el1, params->counters + inx1, params->mx_type, 1);
		mx_release(rnd_el1, params->mx_type, &flags);

		mx_acquire(rnd_el2, params->mx_type, &flags);
		mx_add(rnd_el2, params->counters + inx2, params->mx_type, -1);
		mx_release(rnd_el2, params->mx_type, &flags);
	} while (time_before(jiffies, params->timeout));

	params->iter = iter;
	complete(&params->completion);
}

#define pr_notice_or_err(err, fmt, ...)				\
	do {							\
		if (err)					\
			pr_err(fmt, ##__VA_ARGS__);		\
		else						\
			pr_notice(fmt, ##__VA_ARGS__);		\
	} while (0)

static int run_test(const enum mx_test test, const char *mnemonic, const int threads)
{
	struct mx_inc_dec_params *params = kzalloc_objs(*params, threads);
	unsigned long start_jf = jiffies;
	u64 total_iters = 0;
	struct mx_locks *locks;
	long *counters = NULL;
	unsigned long ms;
	long sum = 0;
	int ret = 0;
	int i;

	locks = kvmalloc_array(mx_nmbr_elems, sizeof(*locks), GFP_KERNEL | __GFP_ZERO);

	if (!mx_lockless_test(test)) {
		counters = kvmalloc_array(mx_nmbr_elems, sizeof(*counters),
					  GFP_KERNEL | __GFP_ZERO);
		if (!counters) {
			ret = -ENOMEM;
			goto exit_free;
		}
	}

	if (!locks || !params) {
		ret = -ENOMEM;
		goto exit_free;
	}

	for (i = 0; i < mx_nmbr_elems; ++i)
		mx_init(locks + i, test);

	for (i = 0; i < threads; ++i) {
		struct mx_inc_dec_params *p = params + i;

		p->locks = locks;
		p->counters = counters;
		p->nelems = mx_nmbr_elems;
		p->timeout = jiffies + mx_scnds_per_test * HZ;
		p->mx_type = test;
		init_completion(&p->completion);

		INIT_WORK(&p->work, mx_inc_dec);
		queue_work(system_dfl_wq, &p->work);
	}

	for (i = 0; i < threads; ++i) {
		struct mx_inc_dec_params *p = params + i;

		wait_for_completion(&p->completion);
		total_iters += p->iter;
	}

	for (i = 0; i < mx_nmbr_elems; ++i)
		mx_fini(locks + i, test);

	if (total_iters > LONG_MAX)
		pr_warn_once("mx_test: Total number of iterations %llu exceeds LONG_MAX; consider reducing mx_scnds_per_test\n",
			     total_iters);

	for (i = 0; i < mx_nmbr_elems; ++i)
		sum += mx_read(locks + i, counters + i, test);

	ms = jiffies_to_msecs(jiffies - start_jf);

	pr_notice_or_err(sum,
			 "mx_test: %-27s result: %s sum: %ld elements: %7d elapsed: %lu.%03lu seconds\n",
			 mnemonic, !sum ? "SUCCESS" : "FAILURE", sum, mx_nmbr_elems,
			 ms / 1000, ms % 1000);
	if (sum)
		ret = -EFAULT;

exit_free:
	kvfree(counters);
	kvfree(locks);
	kfree(params);
	return ret;
}

static int __init mx_test_init(void)
{
	unsigned int threads = 4 * num_online_cpus();
	enum mx_test test = MX_ILLEGAL;
	char *mnemonic;
	unsigned int i;
	int sts;
	struct {
		enum mx_test test;
		char *mnemonic;
	} mx_test_types[] = {
		{
			.test = MX_BUSTED,
			.mnemonic = "busted",
		},
		{
			.test = MX_SPIN_LOCK,
			.mnemonic = "spin_lock",
		},
		{
			.test = MX_SPIN_LOCK_IRQ,
			.mnemonic = "spin_lock_irq",
		},
		{
			.test = MX_SPIN_LOCK_IRQSAVE,
			.mnemonic = "spin_lock_irqsave",
		},
		{
			.test = MX_RW_LOCK_W,
			.mnemonic = "write_lock",
		},
		{
			.test = MX_RW_LOCK_W_BH,
			.mnemonic = "write_lock_bh",
		},
		{
			.test = MX_RW_LOCK_TRW,
			.mnemonic = "read_trylock_write_lock",
		},
		{
			.test = MX_RW_LOCK_TRW_BH,
			.mnemonic = "read_trylock_write_lock_bh",
		},
		{
			.test = MX_MUTEX,
			.mnemonic = "mutex",
		},
		{
			.test = MX_ATOMIC_ADD,
			.mnemonic = "atomic_add",
		},
		{
			.test = MX_ATOMIC64_ADD,
			.mnemonic = "atomic64_add",
		},
		{
			.test = MX_CMPXCHG,
			.mnemonic = "cmpxchg",
		},
		{
			.test = MX_RDS_BUSTED,
			.mnemonic = "rds_busted",
		},
		{
			.test = MX_TEST_AND_SET_BIT_LOCK,
			.mnemonic = "test_and_set_bit_lock",
		},
		{
			.test = MX_TEST_AND_SET_BIT_INNOV,
			.mnemonic = "test_and_set_bit_innov",
		},
		{
			.test = MX_TEST_AND_SET_BIT_PLAIN,
			.mnemonic = "test_and_set_bit_plain",
		},
		{
			.test = MX_TEST_AND_CLEAR_BIT_INNOV,
			.mnemonic = "test_and_clear_bit_innov",
		},
		{
			.test = MX_TEST_AND_CLEAR_BIT_PLAIN,
			.mnemonic = "test_and_clear_bit_plain",
		},
		{
			.test = MX_SINGLE_WW_MUTEX_WW,
			.mnemonic = "single_ww_mutex_wound_wait",
		},
		{
			.test = MX_SINGLE_WW_MUTEX_WD,
			.mnemonic = "single_ww_mutex_wait_die",
		},
#ifdef CONFIG_LOCK_SPIN_ON_OWNER
		{
			.test = MX_OSQ_LOCK_BUSTED,
			.mnemonic = "osq_lock_busted",
		},
#endif
		{
		  .test = MX_ATOMIC_XCHG,
			.mnemonic = "atomic_xchg",
		},
	};

	/* Select the test type */
	for (i = 0; i < ARRAY_SIZE(mx_test_types); ++i)
		if (!strcmp(mx_test, mx_test_types[i].mnemonic)) {
			test = mx_test_types[i].test;
			mnemonic = mx_test_types[i].mnemonic;
			break;
		}

	if (test == MX_ILLEGAL) {
		pr_err("mx_test: unknown test type %s\n", mx_test);
		pr_notice("mx_test: legitimate test types:\n");
		for (i = 0; i < ARRAY_SIZE(mx_test_types); ++i)
			pr_notice("mx_test_types:	 %s\n", mx_test_types[i].mnemonic);
		return -ENOPROTOOPT;
	}

	if (mx_nmbr_elems < 1) {
		pr_err("Number of elements must be greater than equal to one\n");
		return -EINVAL;
	}

	if (mx_scnds_per_test > MAX_JIFFY_OFFSET / HZ) {
		pr_err("mx_scnds_per_test too large\n");
		return -EINVAL;
	}

	sts = run_test(test, mnemonic, threads);

	return sts;
}

static void __exit mx_test_exit(void)
{
}

module_init(mx_test_init);
module_exit(mx_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Håkon Bugge <Haakon.Bugge@oracle.com>");
MODULE_DESCRIPTION("mutual exclusion tests");
