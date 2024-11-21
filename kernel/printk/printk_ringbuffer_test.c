// SPDX-License-Identifier: GPL-2.0

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/random.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include <kunit/test.h>

#include "printk_ringbuffer.h"

/*
 * This KUnit tests the data integrity of the lockless printk_ringbuffer.
 * From multiple CPUs it writes messages of varying length and content while
 * a reader validates the correctness of the messages.
 *
 * IMPORTANT: The more CPUs you can use for this KUnit, the better!
 *
 * The test works by starting "num_online_cpus() - 1" writer threads, each
 * pinned to their own CPU. Each writer thread loops, writing data of varying
 * length into a printk_ringbuffer as fast as possible. The data content is
 * an embedded data struct followed by string content repeating the byte:
 *
 *      'A' + CPUID
 *
 * A reader thread is started on the remaining online CPU and ensures that
 * embedded struct content is consistent with the string and that the string
 * is terminated and is composed of the same repeating byte as its first byte.
 *
 * Because the threads are running in such tight loops, they will call
 * schedule() from time to time so the system stays functional.
 *
 * If the reader encounters an error, the test is aborted and some
 * information about the error is provided via printk. The runtime of
 * the test can be configured with the runtime_ms module parameter.
 *
 * Note that the test is performed on a separate printk_ringbuffer instance
 * and not the instance used by printk().
 */

static unsigned long runtime_ms = 10000;
module_param(runtime_ms, ulong, 0400);

/* used by writers to signal reader of new records */
static DECLARE_WAIT_QUEUE_HEAD(test_wait);

/* test data structure */
struct rbdata {
	unsigned int len;
	char text[] __counted_by(len);
};

#define MAX_RBDATA_LEN (0x7f + 1)
#define MAX_RECORD_SIZE (sizeof(struct rbdata) + MAX_RBDATA_LEN + 1)

static struct test_running {
	int runstate;
	unsigned long num;
	struct kunit *test;
} *test_running;
static int halt_test;

static void fail_record(struct kunit *test, struct rbdata *dat, u64 seq)
{
	char buf[MAX_RBDATA_LEN + 1];

	snprintf(buf, sizeof(buf), "%s", dat->text);
	buf[sizeof(buf) - 1] = 0;

	KUNIT_FAIL(test, "BAD RECORD: seq=%llu len=%u text=%s\n",
		   seq, dat->len, dat->len < sizeof(buf) ? buf : "<invalid>");
}

static bool check_data(struct rbdata *dat)
{
	unsigned int len;

	len = strnlen(dat->text, MAX_RBDATA_LEN + 1);

	/* Sane length? */
	if (len != dat->len || !len || len > MAX_RBDATA_LEN)
		return false;

	/* String repeats with the same character? */
	while (len) {
		len--;
		if (dat->text[len] != dat->text[0])
			return false;
	}

	return true;
}

/* Equivalent to CONFIG_LOG_BUF_SHIFT=13 */
DEFINE_PRINTKRB(test_rb, 8, 5);

static int prbtest_writer(void *data)
{
	struct test_running *tr = data;
	char text_id = 'A' + tr->num;
	struct prb_reserved_entry e;
	unsigned long count = 0;
	struct printk_record r;
	u64 min_ns = (u64)-1;
	struct rbdata *dat;
	u64 total_ns = 0;
	u64 max_ns = 0;
	u64 post_ns;
	u64 pre_ns;
	int len;

	set_cpus_allowed_ptr(current, cpumask_of(tr->num));

	kunit_info(tr->test, "start thread %03lu (writer)\n", tr->num);

	tr->runstate = 1;

	for (;;) {
		/* +2 to ensure at least 1 character + terminator. */
		len = sizeof(struct rbdata) + (get_random_u32() & 0x7f) + 2;

		/* specify the text sizes for reservation */
		prb_rec_init_wr(&r, len);

		pre_ns = local_clock();

		if (prb_reserve(&e, &test_rb, &r)) {
			r.info->text_len = len;

			len -= sizeof(struct rbdata) + 1;

			dat = (struct rbdata *)&r.text_buf[0];
			dat->len = len;
			memset(&dat->text[0], text_id, len);
			dat->text[len] = 0;

			prb_commit(&e);

			post_ns = local_clock();

			wake_up_interruptible(&test_wait);

			post_ns -= pre_ns;
			if (post_ns < min_ns)
				min_ns = post_ns;
			if (post_ns > max_ns)
				max_ns = post_ns;
			total_ns += post_ns;
		}

		if ((count++ & 0x3fff) == 0)
			schedule();

		if (READ_ONCE(halt_test) == 1)
			break;
	}

	kunit_info(tr->test, "end thread %03lu: wrote=%lu min_ns=%llu avg_ns=%llu max_ns=%llu\n",
		   tr->num, count, min_ns, total_ns / (u64)count, max_ns);

	tr->runstate = 2;

	return 0;
}

static int prbtest_reader(void *data)
{
	struct test_running *tr = data;
	char text_buf[MAX_RECORD_SIZE];
	unsigned long total_lost = 0;
	unsigned long max_lost = 0;
	unsigned long count = 0;
	struct printk_info info;
	struct printk_record r;
	int did_sched = 1;
	u64 seq = 0;

	set_cpus_allowed_ptr(current, cpumask_of(tr->num));

	prb_rec_init_rd(&r, &info, &text_buf[0], sizeof(text_buf));

	kunit_info(tr->test, "start thread %03lu (reader)\n", tr->num);

	tr->runstate = 1;

	while (!wait_event_interruptible(test_wait,
				kthread_should_stop() ||
				prb_read_valid(&test_rb, seq, &r))) {
		bool error = false;

		if (kthread_should_stop())
			break;
		/* check/track the sequence */
		if (info.seq < seq) {
			KUNIT_FAIL(tr->test, "BAD SEQ READ: request=%llu read=%llu\n",
				   seq, info.seq);
			error = true;
		} else if (info.seq != seq && !did_sched) {
			total_lost += info.seq - seq;
			if (max_lost < info.seq - seq)
				max_lost = info.seq - seq;
		}

		if (!check_data((struct rbdata *)&r.text_buf[0])) {
			fail_record(tr->test, (struct rbdata *)&r.text_buf[0], info.seq);
			error = true;
		}

		if (error)
			WRITE_ONCE(halt_test, 1);

		did_sched = 0;
		if ((count++ & 0x3fff) == 0) {
			did_sched = 1;
			schedule();
		}

		if (READ_ONCE(halt_test) == 1)
			break;

		seq = info.seq + 1;
	}

	kunit_info(tr->test,
		   "end thread %03lu: read=%lu seq=%llu total_lost=%lu max_lost=%lu\n",
		   tr->num, count, info.seq, total_lost, max_lost);

	while (!kthread_should_stop())
		msleep(1000);
	tr->runstate = 2;

	return 0;
}

static int module_test_running;
static struct task_struct *reader_thread;

static int start_test(void *arg)
{
	struct kunit *test = arg;
	struct task_struct *thread;
	unsigned long i;
	int num_cpus;

	num_cpus = num_online_cpus();
	if (num_cpus == 1)
		kunit_skip(test, "need >1 CPUs for at least one reader and writer");

	test_running = kcalloc(num_cpus, sizeof(*test_running), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, test_running);

	module_test_running = 1;

	kunit_info(test, "starting test\n");

	for (i = 0; i < num_cpus; i++) {
		test_running[i].test = test;
		test_running[i].num = i;
		if (i < num_cpus - 1) {
			thread = kthread_run(prbtest_writer, &test_running[i],
					     "prbtest writer");
		} else {
			thread = kthread_run(prbtest_reader, &test_running[i],
					     "prbtest reader");
			reader_thread = thread;
		}
		if (IS_ERR(thread)) {
			kunit_err(test, "unable to create thread %lu\n", i);
			test_running[i].runstate = 2;
		}
	}

	/* wait until all threads finish */
	for (;;) {
		msleep(1000);

		for (i = 0; i < num_cpus; i++) {
			if (test_running[i].runstate < 2)
				break;
		}
		if (i == num_cpus)
			break;
	}

	kunit_info(test, "completed test\n");

	module_test_running = 0;

	return 0;
}

static void test_readerwriter(struct kunit *test)
{
	static bool already_run;
	int num_cpus;
	int i;

	if (already_run)
		KUNIT_FAIL_AND_ABORT(test, "test can only be run once");
	already_run = true;

	kunit_info(test, "running for %lu ms\n", runtime_ms);

	kthread_run(start_test, test, "prbtest");

	/* wait until all threads active */
	num_cpus = num_online_cpus();
	for (;;) {
		msleep(1000);

		for (i = 0; i < num_cpus; i++) {
			if (test_running[i].runstate == 0)
				break;
		}
		if (i == num_cpus)
			break;
	}

	msleep(runtime_ms);

	if (reader_thread && !IS_ERR(reader_thread))
		kthread_stop(reader_thread);

	WRITE_ONCE(halt_test, 1);

	while (module_test_running)
		msleep(1000);
	kfree(test_running);
}

static struct kunit_case prb_test_cases[] = {
	KUNIT_CASE_SLOW(test_readerwriter),
	{}
};

static struct kunit_suite prb_test_suite = {
	.name       = "printk-ringbuffer",
	.test_cases = prb_test_cases,
};
kunit_test_suite(prb_test_suite);

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);
MODULE_AUTHOR("John Ogness <john.ogness@linutronix.de>");
MODULE_DESCRIPTION("printk_ringbuffer test");
MODULE_LICENSE("GPL");
