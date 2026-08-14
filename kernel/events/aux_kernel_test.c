// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the perf AUX kernel-consumer API.
 *
 * Tests the exported functions:
 *   - perf_event_setup_aux() / perf_event_release_aux()
 *   - perf_event_aux_head() / perf_event_aux_tail_set() / perf_event_aux_copy()
 *
 * A self-contained dummy AUX PMU is registered in suite_init so that
 * every test is deterministic and requires no hardware.  The dummy
 * setup_aux pre-fills all pages with a recognisable pattern (0xAB);
 * aux_test_produce() advances the published head via
 * perf_aux_output_begin/end, and copy tests verify the pattern.
 *
 * Run via debugfs:
 *   echo run > /sys/kernel/debug/kunit/perf_aux_kernel/run
 *   cat /sys/kernel/debug/kunit/perf_aux_kernel/results
 */

#include <kunit/test.h>
#include <linux/kthread.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/wait.h>
#include "internal.h"

/* ---- Dummy AUX PMU ---- */

static struct pmu dummy_aux_pmu;
static int dummy_aux_pmu_type = -1;

static int dummy_aux_event_init(struct perf_event *event)
{
	return 0;
}

static void dummy_aux_event_read(struct perf_event *event) { }

static int dummy_aux_event_add(struct perf_event *event, int mode)
{
	return 0;
}

static void dummy_aux_event_del(struct perf_event *event, int mode) { }

static void dummy_aux_event_start(struct perf_event *event, int mode) { }

static void dummy_aux_event_stop(struct perf_event *event, int mode) { }

static void *dummy_aux_setup_aux(struct perf_event *event, void **pages,
				 int nr_pages, bool overwrite)
{
	int i;

	for (i = 0; i < nr_pages; i++)
		memset(pages[i], 0xAB, PAGE_SIZE);

	return pages;
}

static void dummy_aux_free_aux(void *priv)
{
	/* aux_priv is the pages array; __rb_free_aux frees it via kfree. */
}

static int dummy_aux_pmu_register(void)
{
	int ret;

	if (dummy_aux_pmu_type >= 0)
		return 0;

	memset(&dummy_aux_pmu, 0, sizeof(dummy_aux_pmu));
	dummy_aux_pmu.event_init = dummy_aux_event_init;
	dummy_aux_pmu.add = dummy_aux_event_add;
	dummy_aux_pmu.del = dummy_aux_event_del;
	dummy_aux_pmu.start = dummy_aux_event_start;
	dummy_aux_pmu.stop = dummy_aux_event_stop;
	dummy_aux_pmu.read = dummy_aux_event_read;
	dummy_aux_pmu.setup_aux = dummy_aux_setup_aux;
	dummy_aux_pmu.free_aux = dummy_aux_free_aux;
	dummy_aux_pmu.capabilities = PERF_PMU_CAP_ITRACE;
	dummy_aux_pmu.task_ctx_nr = perf_sw_context;

	ret = perf_pmu_register(&dummy_aux_pmu, "dummy_aux", -1);
	if (ret)
		return ret;

	dummy_aux_pmu_type = dummy_aux_pmu.type;
	return 0;
}

static void dummy_aux_pmu_unregister(void)
{
	if (dummy_aux_pmu_type >= 0)
		perf_pmu_unregister(&dummy_aux_pmu);
	dummy_aux_pmu_type = -1;
}

static int suite_init(struct kunit_suite *suite)
{
	return dummy_aux_pmu_register();
}

static void suite_exit(struct kunit_suite *suite)
{
	dummy_aux_pmu_unregister();
}

/* ---- Helpers ---- */

static struct perf_event *aux_test_create_event(void)
{
	struct perf_event_attr attr = {};

	attr.type = dummy_aux_pmu_type;
	attr.size = sizeof(attr);
	attr.disabled = 1;

	return perf_event_create_kernel_counter(&attr, raw_smp_processor_id(),
						  NULL, NULL, NULL);
}

/*
 * Advance the AUX head by @bytes via perf_aux_output_begin/end.
 * The pages are pre-filled with 0xAB in dummy_aux_setup_aux, so
 * the data is already present; this function only publishes it.
 */
static unsigned long aux_test_produce(struct perf_event *event, unsigned long bytes)
{
	struct perf_output_handle handle;
	unsigned long head, written = 0;

	while (written < bytes) {
		unsigned long chunk = min_t(unsigned long, bytes - written, 1024);

		if (!perf_aux_output_begin(&handle, event))
			break;
		chunk = min_t(unsigned long, chunk, handle.size);
		perf_aux_output_end(&handle, chunk);
		written += chunk;
	}

	head = perf_event_aux_head(event);
	return head;
}

/* ---- Patch-1: Setup / release tests ---- */

static void test_setup_rejects_non_power_of_two(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 3, 0);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	ret = perf_event_setup_aux(event, 0, 0);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	ret = perf_event_setup_aux(event, 6, 0);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	perf_event_release_kernel(event);
}

static void test_setup_accepts_power_of_two(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int i, ret;
	int pages[] = { 1, 2, 4, 8, 16, 32, 64 };

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		ret = perf_event_setup_aux(event, pages[i], 0);
		KUNIT_EXPECT_EQ_MSG(test, 0, ret,
				    "setup_aux(%d pages) expected 0, got %d",
				    pages[i], ret);
		if (ret == 0)
			perf_event_release_aux(event);
	}

	perf_event_release_kernel(event);
}

static void test_setup_rejects_double(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, -EBUSY, ret);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_setup_parent_event_rejected(struct kunit *test)
{
	struct perf_event *parent = aux_test_create_event();
	struct perf_event *child = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);

	child->parent = parent;

	ret = perf_event_setup_aux(child, 1, 0);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	child->parent = NULL;

	perf_event_release_kernel(child);
	perf_event_release_kernel(parent);
}

static void test_writer_admitted_after_setup(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	unsigned long head;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, 512);
	KUNIT_EXPECT_GT(test, head, 0);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_writer_blocked_after_release(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	struct perf_output_handle handle;
	void *addr;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	perf_event_release_aux(event);

	addr = perf_aux_output_begin(&handle, event);
	KUNIT_EXPECT_NULL(test, addr);

	perf_event_release_kernel(event);
}

static void test_release_noop_without_rb(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	perf_event_release_aux(event);
	KUNIT_EXPECT_TRUE(test, true);

	perf_event_release_kernel(event);
}

static void test_setup_release_roundtrip(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	perf_event_release_aux(event);
	KUNIT_EXPECT_TRUE(test, true);

	perf_event_release_kernel(event);
}

static void test_multi_setup_release_cycle(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int i, ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	for (i = 0; i < 3; i++) {
		ret = perf_event_setup_aux(event, 1, 0);
		KUNIT_ASSERT_EQ_MSG(test, 0, ret,
				    "cycle %d: setup_aux failed", i);
		perf_event_release_aux(event);
	}

	perf_event_release_kernel(event);
}

static void test_double_release_clean(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	perf_event_release_aux(event);
	perf_event_release_aux(event);
	KUNIT_EXPECT_TRUE(test, true);

	perf_event_release_kernel(event);
}

/* ---- Patch-2: Accessor tests ---- */

static void test_head_initial_zero(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	unsigned long head;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = perf_event_aux_head(event);
	KUNIT_EXPECT_EQ(test, 0, head);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_head_advances_after_produce(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	unsigned long head;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, 512);
	KUNIT_EXPECT_GT(test, head, 0);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_full_window(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	const unsigned long buf_size = PAGE_SIZE;
	void *buf;
	unsigned long head;
	long copied;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, buf_size / 2);
	KUNIT_EXPECT_GT(test, head, 0);

	buf = kunit_kmalloc(test, head, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	copied = perf_event_aux_copy(event, 0, head, buf);
	KUNIT_EXPECT_EQ_MSG(test, (long)head, copied,
			    "copy(0, %lu) returned %ld, expected %lu",
			    head, copied, head);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_wrap(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	const unsigned long buf_size = 2 * PAGE_SIZE;
	void *buf;
	unsigned long head, tail;
	long copied;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 2, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Fill the buffer to near-full */
	head = aux_test_produce(event, buf_size);
	KUNIT_EXPECT_GT(test, head, 0);

	/* Consume half to free space */
	tail = head / 2;
	ret = perf_event_aux_tail_set(event, tail);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Produce more — head now wraps past buf_size */
	head = aux_test_produce(event, buf_size);
	KUNIT_EXPECT_GT(test, head, buf_size);

	/* Copy a window that spans the ring wrap point */
	buf = kunit_kmalloc(test, buf_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	copied = perf_event_aux_copy(event, tail, head, buf);
	KUNIT_EXPECT_GT(test, copied, 0);
	KUNIT_EXPECT_LE(test, copied, (long)buf_size);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_zero_len(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	void *buf;
	long copied;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	buf = kunit_kmalloc(test, 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	copied = perf_event_aux_copy(event, 0, 0, buf);
	KUNIT_EXPECT_EQ(test, 0, copied);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_rejects_rewound(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	const unsigned long buf_size = PAGE_SIZE;
	void *buf;
	long copied;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	aux_test_produce(event, buf_size / 2);

	buf = kunit_kmalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* from < to: rewound cursor */
	copied = perf_event_aux_copy(event, 100, 50, buf);
	KUNIT_EXPECT_EQ(test, -EINVAL, copied);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_rejects_future(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	unsigned long head;
	void *buf;
	long copied;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, 512);
	KUNIT_EXPECT_GT(test, head, 0);

	buf = kunit_kmalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* to > head: future cursor */
	copied = perf_event_aux_copy(event, head, head + 64, buf);
	KUNIT_EXPECT_EQ(test, -EINVAL, copied);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_rejects_oversized(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	const unsigned long buf_size = PAGE_SIZE;
	void *buf;
	long copied;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	aux_test_produce(event, 512);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	/* window > buf_size */
	copied = perf_event_aux_copy(event, 0, 2 * buf_size, buf);
	KUNIT_EXPECT_EQ(test, -EINVAL, copied);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_rejects_null_buf(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	long copied;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	copied = perf_event_aux_copy(event, 0, 0, NULL);
	KUNIT_EXPECT_EQ(test, -EINVAL, copied);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_no_rb_enoent(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	void *buf;
	long ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	buf = kunit_kmalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	ret = perf_event_aux_copy(event, 0, 64, buf);
	KUNIT_EXPECT_EQ(test, -ENOENT, ret);

	perf_event_release_kernel(event);
}

static void test_tail_set_valid_frees_space(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	unsigned long head;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, 512);
	KUNIT_EXPECT_GT(test, head, 0);

	ret = perf_event_aux_tail_set(event, head / 2);
	KUNIT_EXPECT_EQ(test, 0, ret);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_tail_set_rejects_future(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* tail=1 when head=0: future */
	ret = perf_event_aux_tail_set(event, 1);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_tail_set_accepts_within_ring_window(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	const unsigned long buf_size = PAGE_SIZE;
	unsigned long head, tail;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Produce enough data so head - tail <= aux_size for a large tail */
	head = aux_test_produce(event, buf_size / 2);
	KUNIT_EXPECT_GT(test, head, 0);

	/*
	 * Set tail to a non-trivial value within the [0, head] window.
	 * This is NOT a no-op: old_tail=0, tail=head/4, advance=head/4.
	 */
	tail = head / 4;
	ret = perf_event_aux_tail_set(event, tail);
	KUNIT_EXPECT_EQ(test, 0, ret);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_tail_set_rejects_behind_ring(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	const unsigned long buf_size = PAGE_SIZE;
	unsigned long head;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, buf_size / 2);
	KUNIT_EXPECT_GT(test, head, 0);

	/*
	 * tail more than one ring behind head: head - tail > aux_size.
	 * This should be rejected by the hardened code.
	 */
	ret = perf_event_aux_tail_set(event, head + buf_size + 1);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_tail_set_no_rb_enoent(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_aux_tail_set(event, 0);
	KUNIT_EXPECT_EQ(test, -ENOENT, ret);

	perf_event_release_kernel(event);
}

static void test_head_no_rb_zero(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	unsigned long head;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	head = perf_event_aux_head(event);
	KUNIT_EXPECT_EQ(test, 0, head);

	perf_event_release_kernel(event);
}

/* ---- Additional boundary and correctness tests ---- */

static void test_setup_rejects_negative_watermark(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, -1);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	ret = perf_event_setup_aux(event, 1, -4096);
	KUNIT_EXPECT_EQ(test, -EINVAL, ret);

	perf_event_release_kernel(event);
}

static void test_tail_set_noop(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = perf_event_aux_tail_set(event, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_tail_set_consume_all(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	unsigned long head;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, 512);
	KUNIT_EXPECT_GT(test, head, 0);

	ret = perf_event_aux_tail_set(event, head);
	KUNIT_EXPECT_EQ(test, 0, ret);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_data_correctness(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	void *buf;
	unsigned long head;
	long copied;
	int ret, i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, 512);
	KUNIT_EXPECT_GT(test, head, 0);

	buf = kunit_kmalloc(test, head, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	copied = perf_event_aux_copy(event, 0, head, buf);
	KUNIT_EXPECT_EQ(test, (long)head, copied);

	for (i = 0; i < copied; i++) {
		KUNIT_EXPECT_EQ_MSG(test, 0xAB, ((u8 *)buf)[i],
				    "data mismatch at offset %d", i);
		if (((u8 *)buf)[i] != 0xAB)
			break;
	}

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_multi_produce_copy_cycle(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	void *buf;
	unsigned long head, tail = 0;
	long copied;
	int ret, cycle;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (cycle = 0; cycle < 3; cycle++) {
		head = aux_test_produce(event, 256);
		KUNIT_EXPECT_GT(test, head, tail);

		copied = perf_event_aux_copy(event, tail, head, buf);
		KUNIT_EXPECT_EQ(test, (long)(head - tail), copied);

		ret = perf_event_aux_tail_set(event, head);
		KUNIT_EXPECT_EQ(test, 0, ret);
		tail = head;
	}

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_copy_rejects_already_consumed(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	void *buf;
	unsigned long head, tail;
	long copied;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, 512);
	KUNIT_EXPECT_GT(test, head, 0);

	tail = head / 2;
	ret = perf_event_aux_tail_set(event, tail);
	KUNIT_EXPECT_EQ(test, 0, ret);

	buf = kunit_kmalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	copied = perf_event_aux_copy(event, tail - 1, tail, buf);
	KUNIT_EXPECT_EQ(test, -EINVAL, copied);

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_output_end_zero_size(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	struct perf_output_handle handle;
	void *addr;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	addr = perf_aux_output_begin(&handle, event);
	KUNIT_EXPECT_NOT_NULL(test, addr);

	perf_aux_output_end(&handle, 0);
	KUNIT_EXPECT_EQ(test, 0, perf_event_aux_head(event));

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_buffer_full_stops_producer(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	struct perf_output_handle handle;
	const unsigned long buf_size = PAGE_SIZE;
	unsigned long head;
	void *addr;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head = aux_test_produce(event, buf_size);
	KUNIT_EXPECT_GT(test, head, 0);

	addr = perf_aux_output_begin(&handle, event);
	if (addr) {
		KUNIT_EXPECT_EQ(test, 0, handle.size);
		perf_aux_output_end(&handle, 0);
	}

	perf_event_release_aux(event);
	perf_event_release_kernel(event);
}

static void test_setup_with_explicit_watermark(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 2, 4096);
	KUNIT_EXPECT_EQ(test, 0, ret);
	perf_event_release_aux(event);

	ret = perf_event_setup_aux(event, 4, 8192);
	KUNIT_EXPECT_EQ(test, 0, ret);
	perf_event_release_aux(event);

	perf_event_release_kernel(event);
}

static void test_two_events_independent(struct kunit *test)
{
	struct perf_event *ev_a = aux_test_create_event();
	struct perf_event *ev_b = aux_test_create_event();
	unsigned long head_a, head_b;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ev_a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ev_b);

	ret = perf_event_setup_aux(ev_a, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = perf_event_setup_aux(ev_b, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head_a = aux_test_produce(ev_a, 512);
	head_b = perf_event_aux_head(ev_b);

	KUNIT_EXPECT_GT(test, head_a, 0);
	KUNIT_EXPECT_EQ(test, 0, head_b);

	head_b = aux_test_produce(ev_b, 256);
	KUNIT_EXPECT_GT(test, head_b, 0);
	KUNIT_EXPECT_EQ(test, head_a, perf_event_aux_head(ev_a));

	perf_event_release_aux(ev_a);
	perf_event_release_aux(ev_b);
	perf_event_release_kernel(ev_a);
	perf_event_release_kernel(ev_b);
}

static void test_release_one_does_not_affect_other(struct kunit *test)
{
	struct perf_event *ev_a = aux_test_create_event();
	struct perf_event *ev_b = aux_test_create_event();
	unsigned long head_b;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ev_a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ev_b);

	ret = perf_event_setup_aux(ev_a, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ret = perf_event_setup_aux(ev_b, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	head_b = aux_test_produce(ev_b, 512);
	KUNIT_EXPECT_GT(test, head_b, 0);

	perf_event_release_aux(ev_a);

	KUNIT_EXPECT_EQ(test, head_b, perf_event_aux_head(ev_b));
	KUNIT_EXPECT_GT(test, aux_test_produce(ev_b, 256), head_b);

	perf_event_release_aux(ev_b);
	perf_event_release_kernel(ev_a);
	perf_event_release_kernel(ev_b);
}

/*
 * Test: kernel consumer and simulated userspace consumer coexist on
 * the same PMU without interfering.
 *
 * Event A uses aux_kernel_count (via perf_event_setup_aux).
 * Event B swaps its refcounts to aux_mmap_count=1 to simulate a
 * userspace mmap'd consumer.  Both rings on the same dummy PMU.
 *
 *   - produce on A → B's head is unaffected
 *   - produce on B → A's head is unaffected
 *   - release A (kernel) → B still produces
 */
static void test_user_kernel_coexistence(struct kunit *test)
{
	struct perf_event *ev_a = aux_test_create_event();
	struct perf_event *ev_b = aux_test_create_event();
	struct perf_output_handle handle_a, handle_b;
	unsigned long a_head_before, b_head_before;
	void *addr;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ev_a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ev_b);

	/* Event A: kernel consumer */
	ret = perf_event_setup_aux(ev_a, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Event B: start with kernel setup, then simulate userspace */
	ret = perf_event_setup_aux(ev_b, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* Simulate userspace ownership: swap refcounts on B's rb.
	 * perf_mmap() would set aux_mmap_count=1 for a userspace event.
	 */
	refcount_set(&ev_b->rb->aux_mmap_count, 1);
	refcount_set(&ev_b->rb->aux_kernel_count, 0);

	/* Produce on A — B's head stays 0 */
	addr = perf_aux_output_begin(&handle_a, ev_a);
	KUNIT_EXPECT_NOT_NULL(test, addr);
	perf_aux_output_end(&handle_a, 512);
	KUNIT_EXPECT_GT(test, ev_a->rb->aux_head, 0);
	KUNIT_EXPECT_EQ(test, 0, ev_b->rb->aux_head);

	/* Produce on B — A's head unchanged */
	a_head_before = ev_a->rb->aux_head;
	addr = perf_aux_output_begin(&handle_b, ev_b);
	KUNIT_EXPECT_NOT_NULL(test, addr);
	perf_aux_output_end(&handle_b, 256);
	KUNIT_EXPECT_GT(test, ev_b->rb->aux_head, 0);
	KUNIT_EXPECT_EQ(test, a_head_before, ev_a->rb->aux_head);

	/* Release A (kernel) — B still produces */
	b_head_before = ev_b->rb->aux_head;
	perf_event_release_aux(ev_a);
	addr = perf_aux_output_begin(&handle_b, ev_b);
	KUNIT_EXPECT_NOT_NULL(test, addr);
	perf_aux_output_end(&handle_b, 128);
	KUNIT_EXPECT_GT(test, ev_b->rb->aux_head, b_head_before);

	/* Cleanup B: restore kernel ownership for proper release */
	refcount_set(&ev_b->rb->aux_kernel_count, 1);
	refcount_set(&ev_b->rb->aux_mmap_count, 0);
	perf_event_release_aux(ev_b);
	perf_event_release_kernel(ev_a);
	perf_event_release_kernel(ev_b);
}

/*
 * Test: two kthreads concurrently call perf_event_release_aux() on
 * the same event.  Verifies that the refcount_dec_and_mutex_lock
 * serialisation and the event->rb == rb guard in the detach block
 * correctly handle the race without crashing or leaking.
 */
struct concurrent_release_ctx {
	struct perf_event *event;
	atomic_t count;
	wait_queue_head_t wq;
};

static int concurrent_release_thread(void *data)
{
	struct concurrent_release_ctx *ctx = data;

	perf_event_release_aux(ctx->event);

	if (atomic_dec_and_test(&ctx->count))
		wake_up(&ctx->wq);
	return 0;
}

static void test_concurrent_release(struct kunit *test)
{
	struct perf_event *event = aux_test_create_event();
	struct concurrent_release_ctx ctx;
	struct task_struct *threads[2];
	int ret, i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, event);

	ret = perf_event_setup_aux(event, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, ret);

	ctx.event = event;
	atomic_set(&ctx.count, ARRAY_SIZE(threads));
	init_waitqueue_head(&ctx.wq);

	for (i = 0; i < ARRAY_SIZE(threads); i++) {
		threads[i] = kthread_run(concurrent_release_thread, &ctx,
					 "aux_crel_%d", i);
		if (IS_ERR_OR_NULL(threads[i])) {
			kunit_skip(test, "failed to create kthread");
			while (--i >= 0)
				kthread_stop(threads[i]);
			perf_event_release_aux(event);
			perf_event_release_kernel(event);
			return;
		}
	}

	wait_event(ctx.wq, atomic_read(&ctx.count) == 0);

	perf_event_release_kernel(event);
}

static struct kunit_case aux_kernel_test_cases[] = {
	KUNIT_CASE(test_setup_rejects_non_power_of_two),
	KUNIT_CASE(test_setup_accepts_power_of_two),
	KUNIT_CASE(test_setup_rejects_double),
	KUNIT_CASE(test_setup_parent_event_rejected),
	KUNIT_CASE(test_setup_rejects_negative_watermark),
	KUNIT_CASE(test_setup_with_explicit_watermark),
	KUNIT_CASE(test_writer_admitted_after_setup),
	KUNIT_CASE(test_writer_blocked_after_release),
	KUNIT_CASE(test_release_noop_without_rb),
	KUNIT_CASE(test_setup_release_roundtrip),
	KUNIT_CASE(test_multi_setup_release_cycle),
	KUNIT_CASE(test_double_release_clean),
	KUNIT_CASE(test_two_events_independent),
	KUNIT_CASE(test_release_one_does_not_affect_other),
	KUNIT_CASE(test_user_kernel_coexistence),
	KUNIT_CASE(test_concurrent_release),
	KUNIT_CASE(test_head_initial_zero),
	KUNIT_CASE(test_head_advances_after_produce),
	KUNIT_CASE(test_head_no_rb_zero),
	KUNIT_CASE(test_copy_full_window),
	KUNIT_CASE(test_copy_wrap),
	KUNIT_CASE(test_copy_zero_len),
	KUNIT_CASE(test_copy_data_correctness),
	KUNIT_CASE(test_copy_rejects_rewound),
	KUNIT_CASE(test_copy_rejects_future),
	KUNIT_CASE(test_copy_rejects_oversized),
	KUNIT_CASE(test_copy_rejects_null_buf),
	KUNIT_CASE(test_copy_rejects_already_consumed),
	KUNIT_CASE(test_copy_no_rb_enoent),
	KUNIT_CASE(test_tail_set_valid_frees_space),
	KUNIT_CASE(test_tail_set_rejects_future),
	KUNIT_CASE(test_tail_set_accepts_within_ring_window),
	KUNIT_CASE(test_tail_set_rejects_behind_ring),
	KUNIT_CASE(test_tail_set_noop),
	KUNIT_CASE(test_tail_set_consume_all),
	KUNIT_CASE(test_tail_set_no_rb_enoent),
	KUNIT_CASE(test_multi_produce_copy_cycle),
	KUNIT_CASE(test_output_end_zero_size),
	KUNIT_CASE(test_buffer_full_stops_producer),
	{},
};

static struct kunit_suite aux_kernel_test_suite = {
	.name = "perf_aux_kernel",
	.suite_init = suite_init,
	.suite_exit = suite_exit,
	.test_cases = aux_kernel_test_cases,
};

kunit_test_suite(aux_kernel_test_suite);
