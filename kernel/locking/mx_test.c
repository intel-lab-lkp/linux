// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/rwlock.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/ww_mutex.h>

static unsigned long mx_scnds_per_test = 1;
static unsigned int mx_nmbr_elems = 1000;
static unsigned int mx_min_padding = 8;
static unsigned int mx_max_padding = 8;
static char *mx_test = "spin_lock";

module_param(mx_scnds_per_test, ulong, 0444);
MODULE_PARM_DESC(mx_scnds_per_test, "Number of seconds to run for each test iteration (default is 1)");

module_param(mx_nmbr_elems, uint, 0444);
MODULE_PARM_DESC(mx_nmbr_elems, "Number of elements (default 1000)");

module_param(mx_min_padding, uint, 0444);
MODULE_PARM_DESC(mx_min_padding, "Minimum padding (default 8)");

module_param(mx_max_padding, uint, 0444);
MODULE_PARM_DESC(mx_max_padding, "Maximum padding (default 8)");

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
	MX_CMPXCHG,
	MX_RDS_BUSTED,
	MX_TEST_AND_SET_BIT_LOCK,
	MX_TEST_AND_SET_BIT_INNOV,
	MX_TEST_AND_SET_BIT_PLAIN,
	MX_TEST_AND_CLEAR_BIT_INNOV,
	MX_TEST_AND_CLEAR_BIT_PLAIN,
	MX_SINGLE_WW_MUTEX_WW,	/* Wound-Wait */
	MX_SINGLE_WW_MUTEX_WD,	/* Wait-Die */
};

union mx_elem {
	struct {
		long counter;
	} mx_busted;
	struct {
		/* Protecting counter */
		spinlock_t lock;
		long counter;
	} mx_spin;
	struct {
		rwlock_t lock;
		long counter;
	} mx_rw;
	struct {
		/* Protecting counter */
		struct mutex mutex;
		long counter;
	} mx_mutex;
	struct {
		atomic_t counter;
	} mx_atomic_add;
	struct {
		long counter;
	} mx_cmpxchg;
	struct {
		unsigned long bits;
		long counter;
	} mx_rds_busted;
	struct {
		unsigned long bits;
		long counter;
	} mx_test_and_set_bit_lock;
	struct {
		unsigned long bits;
		long counter;
	} mx_test_and_set_clr_bit;
	struct {
		struct ww_mutex ww_mutex;
		long counter;
	} mx_single_ww_mutex;
};

struct mx_inc_dec_params {
	struct work_struct work;
	union mx_elem *elements;
	unsigned int nelems;
	unsigned long timeout;
	enum mx_test mx_type;
	int padding;
	u64 iter;
	struct completion completion;
};

static inline unsigned int mx_rand(unsigned int *seed)
{
	*seed = *seed * 69069 + 111;
	return *seed;
}

static inline void mx_acquire(union mx_elem *el, enum mx_test typ, unsigned long *flags_ptr)
{
	int ret;

	switch (typ) {
	case MX_BUSTED:
		break;
	case MX_SPIN_LOCK:
		spin_lock(&el->mx_spin.lock);
		break;
	case MX_SPIN_LOCK_IRQ:
		spin_lock_irq(&el->mx_spin.lock);
		break;
	case MX_SPIN_LOCK_IRQSAVE:
		spin_lock_irqsave(&el->mx_spin.lock, *flags_ptr);
		break;
	case MX_RW_LOCK_W:
		write_lock(&el->mx_rw.lock);
		break;
	case MX_RW_LOCK_W_BH:
		write_lock_bh(&el->mx_rw.lock);
		break;
	case MX_RW_LOCK_TRW:
		while (!read_trylock(&el->mx_rw.lock))
			;
		read_unlock(&el->mx_rw.lock);
		write_lock(&el->mx_rw.lock);
		break;
	case MX_RW_LOCK_TRW_BH:
		while (!read_trylock(&el->mx_rw.lock))
			;
		read_unlock(&el->mx_rw.lock);
		write_lock_bh(&el->mx_rw.lock);
		break;
	case MX_MUTEX:
		mutex_lock(&el->mx_mutex.mutex);
		break;
	case MX_ATOMIC_ADD:
	case MX_CMPXCHG:
		break;
	case MX_RDS_BUSTED:
		while (test_and_set_bit(MX_RDS_IN_XMIT, &el->mx_rds_busted.bits))
			;
		break;
	case MX_TEST_AND_SET_BIT_LOCK:
		while (test_and_set_bit_lock(MX_RDS_IN_XMIT, &el->mx_test_and_set_bit_lock.bits))
			;
		break;
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
		while (test_and_set_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits))
			;
		break;
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
		while (!test_and_clear_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits))
			;
		break;
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
		ret = ww_mutex_lock(&el->mx_single_ww_mutex.ww_mutex, NULL);
		WARN_ONCE(ret, "ww_mutex_lock returned %d for the single w/w mutex case\n", ret);
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

static inline void mx_release(union mx_elem *el, enum mx_test typ, unsigned long *flags_ptr)
{
	switch (typ) {
	case MX_BUSTED:
		break;
	case MX_SPIN_LOCK:
		spin_unlock(&el->mx_spin.lock);
		break;
	case MX_SPIN_LOCK_IRQ:
		spin_unlock_irq(&el->mx_spin.lock);
		break;
	case MX_SPIN_LOCK_IRQSAVE:
		spin_unlock_irqrestore(&el->mx_spin.lock, *flags_ptr);
		break;
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_TRW:
		write_unlock(&el->mx_rw.lock);
		break;
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW_BH:
		write_unlock_bh(&el->mx_rw.lock);
		break;
	case MX_MUTEX:
		mutex_unlock(&el->mx_mutex.mutex);
		break;
	case MX_ATOMIC_ADD:
	case MX_CMPXCHG:
		break;
	case MX_RDS_BUSTED:
		clear_bit(MX_RDS_IN_XMIT, &el->mx_rds_busted.bits);
		/* Deliberately outside the critical region, as RDS did prior to 1422f288 */
		smp_mb__after_atomic();
		break;
	case MX_TEST_AND_SET_BIT_LOCK:
		clear_bit_unlock(MX_RDS_IN_XMIT, &el->mx_test_and_set_bit_lock.bits);
		break;
	case MX_TEST_AND_SET_BIT_INNOV:
		/*
		 * Ensuring global visibility on other processors by
		 * means of a failing test_and_set_bit()
		 */
		(void)test_and_set_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits);
		clear_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits);
		break;
	case MX_TEST_AND_SET_BIT_PLAIN:
		/* Ensure the counter gets global visiblity before releasing the lock*/
		smp_mb__before_atomic();
		clear_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits);
		break;
	case MX_TEST_AND_CLEAR_BIT_INNOV:
		/*
		 * Ensuring global visibility on other processors by
		 * means of a failing test_and_clear_bit()
		 */
		(void)test_and_clear_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits);
		set_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits);
		break;
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
		/* Ensure the counter gets global visiblity before releasing the lock*/
		smp_mb__before_atomic();
		set_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits);
		break;
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
		ww_mutex_unlock(&el->mx_single_ww_mutex.ww_mutex);
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

static inline void mx_add(union mx_elem *el, enum mx_test typ, long addend)
{
	long old;

	switch (typ) {
	case MX_BUSTED:
		el->mx_busted.counter += addend;
		break;
	case MX_SPIN_LOCK:
	case MX_SPIN_LOCK_IRQ:
	case MX_SPIN_LOCK_IRQSAVE:
		el->mx_spin.counter += addend;
		break;
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW:
	case MX_RW_LOCK_TRW_BH:
		el->mx_rw.counter += addend;
		break;
	case MX_MUTEX:
		el->mx_mutex.counter += addend;
		break;
	case MX_ATOMIC_ADD:
		atomic_add(addend, &el->mx_atomic_add.counter);
		break;
	case MX_CMPXCHG:
		do {
			old = READ_ONCE(el->mx_cmpxchg.counter);
		} while (cmpxchg(&el->mx_cmpxchg.counter, old, old + addend) != old);
		break;
	case MX_RDS_BUSTED:
		el->mx_rds_busted.counter += addend;
		break;
	case MX_TEST_AND_SET_BIT_LOCK:
		el->mx_test_and_set_bit_lock.counter += addend;
		break;
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
		el->mx_test_and_set_clr_bit.counter += addend;
		break;
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
		el->mx_single_ww_mutex.counter += addend;
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

static inline long mx_read(union mx_elem *el, enum mx_test typ)
{
	switch (typ) {
	case MX_BUSTED:
		return el->mx_busted.counter;
	case MX_SPIN_LOCK:
	case MX_SPIN_LOCK_IRQ:
	case MX_SPIN_LOCK_IRQSAVE:
		return el->mx_spin.counter;
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW:
	case MX_RW_LOCK_TRW_BH:
		return el->mx_rw.counter;
	case MX_MUTEX:
		return el->mx_mutex.counter;
	case MX_ATOMIC_ADD:
		return atomic_read(&el->mx_atomic_add.counter);
	case MX_CMPXCHG:
		return el->mx_cmpxchg.counter;
	case MX_RDS_BUSTED:
		return el->mx_rds_busted.counter;
	case MX_TEST_AND_SET_BIT_LOCK:
		return el->mx_test_and_set_bit_lock.counter;
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
		return el->mx_test_and_set_clr_bit.counter;
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
		return el->mx_single_ww_mutex.counter;
	default:
		WARN_ON_ONCE(true);
	}
	return 0;
}

static inline void mx_init(union mx_elem *el, enum mx_test typ)
{
	switch (typ) {
	case MX_SPIN_LOCK:
	case MX_SPIN_LOCK_IRQ:
	case MX_SPIN_LOCK_IRQSAVE:
		spin_lock_init(&el->mx_spin.lock);
		break;
	case MX_RW_LOCK_W:
	case MX_RW_LOCK_W_BH:
	case MX_RW_LOCK_TRW:
	case MX_RW_LOCK_TRW_BH:
		rwlock_init(&el->mx_rw.lock);
		break;
	case MX_MUTEX:
		mutex_init(&el->mx_mutex.mutex);
		break;
	case MX_TEST_AND_CLEAR_BIT_INNOV:
	case MX_TEST_AND_CLEAR_BIT_PLAIN:
		set_bit(MX_SOME_BIT, &el->mx_test_and_set_clr_bit.bits);
		break;
	/* The following relies on the vzalloc below */
	case MX_BUSTED:
	case MX_ATOMIC_ADD:
	case MX_CMPXCHG:
	case MX_RDS_BUSTED:
	case MX_TEST_AND_SET_BIT_LOCK:
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
		break;
	case MX_SINGLE_WW_MUTEX_WW:
		ww_mutex_init(&el->mx_single_ww_mutex.ww_mutex, &ww_class);
		break;
	case MX_SINGLE_WW_MUTEX_WD:
		ww_mutex_init(&el->mx_single_ww_mutex.ww_mutex, &wd_class);
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

static inline void mx_fini(union mx_elem *el, enum mx_test typ)
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
	case MX_CMPXCHG:
	case MX_RDS_BUSTED:
	case MX_TEST_AND_SET_BIT_LOCK:
	case MX_TEST_AND_SET_BIT_INNOV:
	case MX_TEST_AND_SET_BIT_PLAIN:
		break;
	case MX_MUTEX:
		mutex_destroy(&el->mx_mutex.mutex);
		break;
	case MX_SINGLE_WW_MUTEX_WW:
	case MX_SINGLE_WW_MUTEX_WD:
		ww_mutex_destroy(&el->mx_single_ww_mutex.ww_mutex);
		break;
	default:
		WARN_ON_ONCE(true);
	}
}

/*
 * Empirically, tests have shown that alignment of the union used here
 * matters a lot when it comes to detecting incorrect locking schemes
 * or error in locking primitives. The alignment will to a variable
 * extent create false-sharing in the different cache levels, which is
 * like a catalyst provoking bugs.
 */
static inline union mx_elem *add_pad(const union mx_elem *el, size_t inx, size_t pad)
{
	return (union mx_elem *)((uintptr_t)el + inx * (sizeof(*el) + pad));
}

static void mx_inc_dec(struct work_struct *_work)
{
	struct mx_inc_dec_params *params;
	unsigned long flags;
	union mx_elem *el;
	unsigned int seed;
	u64 iter = 0;
	int padding;

	params = container_of(_work, struct mx_inc_dec_params, work);
	el = params->elements;
	padding = params->padding;

	get_random_bytes(&seed, sizeof(seed));
	do {
		unsigned int inx1 = mx_rand(&seed) % params->nelems;
		unsigned int inx2 = mx_rand(&seed) % params->nelems;
		union mx_elem *rnd_el1 = add_pad(el, inx1, padding);
		union mx_elem *rnd_el2 = add_pad(el, inx2, padding);

		++iter;

		mx_acquire(rnd_el1, params->mx_type, &flags);
		mx_add(rnd_el1, params->mx_type, 1);
		mx_release(rnd_el1, params->mx_type, &flags);

		mx_acquire(rnd_el2, params->mx_type, &flags);
		mx_add(rnd_el2, params->mx_type, -1);
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

static int run_test(const enum mx_test test, const char *mnemonic, const int threads,
		    const int padding)
{
	struct mx_inc_dec_params *params = kzalloc_objs(*params, threads);
	unsigned long start_jf = jiffies;
	u64 total_iters = 0;
	union mx_elem *el = NULL;
	unsigned long ms;
	long sum = 0;
	size_t bytes;
	int ret = 0;
	int i;

	if (check_mul_overflow((size_t)mx_nmbr_elems,
			       sizeof(*el) + (size_t)padding, &bytes)) {
		ret = -EOVERFLOW;
		goto exit_free;
	}

	el = vzalloc(bytes);

	if (!el || !params) {
		ret = -ENOMEM;
		goto exit_free;
	}

	for (i = 0; i < mx_nmbr_elems; ++i)
		mx_init(add_pad(el, i, padding), test);

	for (i = 0; i < threads; ++i) {
		struct mx_inc_dec_params *p = params + i;

		p->elements = el;
		p->nelems = mx_nmbr_elems;
		p->timeout = jiffies + mx_scnds_per_test * HZ;
		p->mx_type = test;
		p->padding = padding;
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
		mx_fini(add_pad(el, i, padding), test);

	if (total_iters > LONG_MAX)
		pr_warn_once("mx_test: Total number of iterations %llu exceeds LONG_MAX; consider reducing mx_scnds_per_test\n",
			     total_iters);

	for (i = 0; i < mx_nmbr_elems; ++i)
		sum += mx_read(add_pad(el, i, padding), test);
	ms = jiffies_to_msecs(jiffies - start_jf);

	pr_notice_or_err(sum,
			 "mx_test: %-27s padding: %2d result: %s sum: %ld elements: %d elapsed: %lu.%03lu seconds\n",
			 mnemonic, padding, !sum ? "SUCCESS" : "FAILURE", sum, mx_nmbr_elems,
			 ms / 1000, ms % 1000);
	if (sum)
		ret = -EFAULT;

exit_free:
	vfree(el);
	kfree(params);
	return ret;
}

static int __init mx_test_init(void)
{
	unsigned int threads = 4 * num_online_cpus();
	enum mx_test test = MX_ILLEGAL;
	char *mnemonic;
	unsigned int i;
	unsigned int p;
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

	if ((mx_min_padding % 8) || (mx_max_padding % 8)) {
		pr_err("mx_test: padding must be a multiple of 8 bytes\n");
		return -EINVAL;
	}

	if (mx_min_padding > mx_max_padding) {
		pr_err("mx_test: mx_min_padding must be less than equal to mx_max_padding\n");
		return -EINVAL;
	}

	if (mx_max_padding > L1_CACHE_BYTES) {
		pr_err("mx_test: mx_max_padding must be less than equal to L1_CACHE_BYTES (%d)\n",
		       L1_CACHE_BYTES);
		return -EINVAL;
	}

	if (mx_nmbr_elems < 1) {
		pr_err("Number of elements must be greater than equal to one\n");
		return -EINVAL;
	}

	if (mx_scnds_per_test > MAX_JIFFY_OFFSET / HZ) {
		pr_err("mx_scnds_per_test too large\n");
		return -EINVAL;
	}

	for (p = mx_min_padding; p <= mx_max_padding; p += 8) {
		int sts = run_test(test, mnemonic, threads,  p);

		if (sts)
			return sts;
	}

	return 0;
}

static void __exit mx_test_exit(void)
{
}

module_init(mx_test_init);
module_exit(mx_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Håkon Bugge <Haakon.Bugge@oracle.com>");
MODULE_DESCRIPTION("mutual exclusion tests");

