// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <kunit/test.h>
#include <linux/wait_bit.h>
#include <linux/instruction_pointer.h>
#include <linux/kallsyms.h>
#include <linux/percpu-refcount.h>
#include <linux/refcount.h>
#include <trace/events/ref_trace.h>

struct data {
	unsigned long caller;
	const char *fn;
	const void *obj;
	int *flag;
	struct kunit *test;
};

struct data refc_chk;
struct data pcpu_chk;

//called when tracepoint fires
static void probe(
	  void *ignore,
	  unsigned long caller,
	  const char *fn,
	  const void *obj)
{
	struct data *chk_val;

	char func_name[KSYM_SYMBOL_LEN];

	sprint_symbol_no_offset(func_name, caller);

	if (!strcmp(func_name, "test_refcount")) {
		chk_val = &refc_chk;
		KUNIT_EXPECT_FALSE(chk_val->test, memcmp(obj, chk_val->obj, sizeof(refcount_t)));
	} else if (!strcmp(func_name, "test_percpu")) {
		chk_val = &pcpu_chk;
		KUNIT_EXPECT_FALSE(chk_val->test, memcmp(
			obj, chk_val->obj, sizeof(struct percpu_ref)));
	} else {
		//non test function origin trace events
		return;
	}

	struct kunit *test = chk_val->test;
	int *flag = chk_val->flag;

	//ensure past flag writes are done before reading
	KUNIT_EXPECT_EQ(test, 1, smp_load_acquire(flag));
	KUNIT_EXPECT_EQ(test, caller, chk_val->caller);

	smp_store_release(flag, 0); //signal probe completion
}



static void test_refcount(struct kunit *test)
{
	refcount_t refc;
	int flag_addr = 0;

	refc_chk.caller = (unsigned long)&test_refcount;
	refc_chk.fn = "__refcount_sub_and_test";
	refc_chk.obj = &refc;
	refc_chk.flag = &flag_addr;
	refc_chk.test = test;

	int *flag = refc_chk.flag;

	KUNIT_EXPECT_FALSE(test, register_trace_ref_trace_final_put(probe, NULL));

	refcount_set(&refc, 2);

	KUNIT_EXPECT_FALSE(test, refcount_dec_and_test(&refc));

	smp_store_release(flag, 1); //signal final put can happen

	KUNIT_EXPECT_TRUE(test, refcount_dec_and_test(&refc));

	wait_var_event(flag, smp_load_acquire(flag)); //wait for probe completion
	unregister_trace_ref_trace_final_put(probe, NULL);
}

static void dummy_release(struct percpu_ref *ref) {}

static void test_percpu(struct kunit *test)
{
	struct percpu_ref pcpu_ref;

	int flag_addr = 1;

	pcpu_chk.caller = (unsigned long)&test_percpu;
	pcpu_chk.fn = "percpu_ref_put_many";
	pcpu_chk.obj = &pcpu_ref;
	pcpu_chk.flag = &flag_addr;
	pcpu_chk.test = test;

	int *flag = pcpu_chk.flag;

	KUNIT_EXPECT_FALSE(test, register_trace_ref_trace_final_put(probe, NULL));

	KUNIT_EXPECT_FALSE(test, percpu_ref_init(&pcpu_ref, dummy_release, 0, GFP_KERNEL));

	percpu_ref_get(&pcpu_ref);
	percpu_ref_get(&pcpu_ref);

	percpu_ref_put(&pcpu_ref);
	percpu_ref_put(&pcpu_ref);

	percpu_ref_switch_to_atomic_sync(&pcpu_ref);

	smp_store_release(flag, 1); //signal final put can happen

	percpu_ref_put(&pcpu_ref);

	wait_var_event(flag, smp_load_acquire(flag)); //wait for probe completion
	unregister_trace_ref_trace_final_put(probe, NULL);
	percpu_ref_exit(&pcpu_ref);
}

static struct kunit_case __refdata ref_trace_test_cases[] = {
	KUNIT_CASE(test_refcount),
	KUNIT_CASE(test_percpu),
	{}
};

static struct kunit_suite ref_trace_test_suite = {
	.name = "ref-trace",
	.test_cases = ref_trace_test_cases,
};

kunit_test_suites(&ref_trace_test_suite);

MODULE_AUTHOR("Eugene Mavick <m@mavick.dev>");
MODULE_DESCRIPTION("KUnit test for ref_trace");
MODULE_LICENSE("GPL");
