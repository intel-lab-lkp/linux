// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <kunit/test.h>
#include <linux/compiler_attributes.h>
#include <linux/wait_bit.h>
#include <linux/instruction_pointer.h>
#include <linux/kallsyms.h>
#include <linux/jiffies.h>
#include <linux/percpu-refcount.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <trace/events/ref_trace.h>

struct data {
	unsigned long caller;
	unsigned long ip;
	const void *obj;
	atomic_t count;
};

struct data capture;

const void *chk_obj;

#define test_init()								\
	do {									\
		KUNIT_EXPECT_FALSE(						\
			test, register_trace_ref_trace_final_put(probe, NULL));	\
										\
		atomic_set_release(&capture.count, 0);				\
										\
		chk_obj = &obj;							\
	} while (0)


#define test_exit()								\
	do {									\
		/* wait for probe completion */					\
		int notimeout = wait_var_event_timeout(				\
			&capture.count,						\
			atomic_read_acquire(&capture.count),			\
			msecs_to_jiffies(10000)					\
		);								\
										\
		unregister_trace_ref_trace_final_put(probe, NULL);		\
		tracepoint_synchronize_unregister();				\
										\
		KUNIT_ASSERT_TRUE(test, notimeout);				\
										\
		KUNIT_EXPECT_EQ(test, atomic_read_acquire(&capture.count), 1);	\
										\
		KUNIT_EXPECT_TRUE(test, __kernel_text_address(capture.caller));	\
		KUNIT_EXPECT_TRUE(test, __kernel_text_address(capture.ip));	\
										\
		KUNIT_EXPECT_PTR_EQ(test, capture.obj, &obj);			\
	} while (0)

static void probe(
	  void *ignore,
	  unsigned long caller,
	  unsigned long ip,
	  const void *obj)
{
	//prevent non test func final_puts from changing captured values
	if (chk_obj != obj)
		return;

	capture.caller = caller;
	capture.ip = ip;
	capture.obj = obj;

	atomic_inc_return_release(&capture.count); //increase count
}

static void test_refcount_sub_and_test(struct kunit *test)
{
	refcount_t obj;

	test_init();
	refcount_set(&obj, 2);

	KUNIT_EXPECT_FALSE(test, refcount_dec_and_test(&obj));
	KUNIT_EXPECT_TRUE(test, refcount_dec_and_test(&obj));

	test_exit();
}

static void test_refcount_dec_if_one(struct kunit *test)
{
	refcount_t obj;

	test_init();
	refcount_set(&obj, 2);

	KUNIT_EXPECT_FALSE(test, refcount_dec_and_test(&obj));
	KUNIT_EXPECT_TRUE(test, refcount_dec_if_one(&obj));

	test_exit();
}
static void dummy_release(struct percpu_ref *ref) {}

static void test_percpu_ref_put_many(struct kunit *test)
{
	struct percpu_ref obj;

	test_init();

	KUNIT_ASSERT_FALSE(test, percpu_ref_init(&obj, dummy_release, 0, GFP_KERNEL));

	percpu_ref_get(&obj);
	percpu_ref_get(&obj);

	percpu_ref_put(&obj);
	percpu_ref_put(&obj);

	percpu_ref_switch_to_atomic_sync(&obj);

	percpu_ref_put(&obj);

	test_exit();
	percpu_ref_exit(&obj);
}

static struct kunit_case __refdata ref_trace_test_cases[] = {
	KUNIT_CASE(test_refcount_sub_and_test),
	KUNIT_CASE(test_refcount_dec_if_one),
	KUNIT_CASE(test_percpu_ref_put_many),
	{}
};

static struct kunit_suite ref_trace_test_suite = {
	.name = "ref-trace",
	.test_cases = ref_trace_test_cases
};

kunit_test_suites(&ref_trace_test_suite);

MODULE_AUTHOR("Eugene Mavick <m@mavick.dev>");
MODULE_DESCRIPTION("KUnit test for ref_trace");
MODULE_LICENSE("GPL");
