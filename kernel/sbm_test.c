// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023-2024 Huawei Technologies Duesseldorf GmbH
 */

#include <kunit/test.h>

#include <linux/sbm.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/signal.h>

#define PASS	0x600d600d
#define FAIL	0xbad00bad

struct data {
	int value;
};

struct page_over {
	union {
		unsigned char page[PAGE_SIZE];
		struct data data;
	};
	int nextpage;
};

struct thread_data {
	const struct data *in;
	struct data *out;
};

typedef int (*sbm_threadfn)(struct kunit *test, struct thread_data *data);

struct thread_param {
	struct kunit *test;
	sbm_threadfn threadfn;
	struct thread_data *data;
	int err;
};

static void check_safe_sbm(struct kunit *test)
{
	if (!IS_ENABLED(CONFIG_HAVE_ARCH_SBM))
		kunit_skip(test, "requires arch hooks");
}

/**************************************************************
 * Helpers to handle Oops in a sandbox mode kernel thread
 *
 * The kernel does not offer a general method for recovering
 * from page faults in kernel mode. To intercept a planned
 * page fault, let a helper thread oops and die.
 */

static int call_sbm_threadfn(void *arg)
{
	struct thread_param *params = arg;

	params->err = params->threadfn(params->test, params->data);
	do_exit(0);
}

static int run_sbm_kthread(struct kunit *test, sbm_threadfn threadfn,
			   struct thread_data *data)
{
	struct thread_param params = {
		.test = test,
		.threadfn = threadfn,
		.data = data,
	};
	int pid, status;

	/* Do not let the child autoreap. */
	kernel_sigaction(SIGCHLD, SIG_DFL);

	pid = kernel_thread(call_sbm_threadfn, &params, test->name,
			    CLONE_FS | CLONE_FILES | SIGCHLD);
	KUNIT_ASSERT_GT(test, pid, 0);
	KUNIT_ASSERT_EQ(test, kernel_wait(pid, &status), pid);

	/* Ignore SIGCHLD again. */
	kernel_sigaction(SIGCHLD, SIG_IGN);

	/* Killed by a signal? */
	if (status & 0x7f)
		return -EFAULT;

	KUNIT_ASSERT_EQ(test, status, 0);
	return params.err;
}

/**************************************************************
 * Simple write to output buffer.
 *
 * Verify that the output buffer is copied back to the caller.
 */

static SBM_DEFINE_FUNC(write, struct data *, out)
{
	out->value = PASS;
	return 0;
}

static void sbm_write(struct kunit *test)
{
	struct sbm sbm;
	struct data out;
	int err;

	out.value = FAIL;
	sbm_init(&sbm);
	err = sbm_call(&sbm, write,
		       SBM_COPY_OUT(&sbm, &out, sizeof(out)));
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), 0);
	KUNIT_EXPECT_EQ(test, out.value, PASS);
}

/**************************************************************
 * Memory write past output buffer within the same page.
 *
 * Writes beyond buffer end are ignored.
 */

static SBM_DEFINE_FUNC(write_past, struct data *, out)
{
	out[0].value = PASS;
	out[1].value = FAIL;

	return 0;
}

static void sbm_write_past(struct kunit *test)
{
	struct sbm sbm;
	struct data out[2];
	int err;

	out[0].value = FAIL;
	out[1].value = PASS;
	sbm_init(&sbm);
	err = sbm_call(&sbm, write_past,
		       SBM_COPY_OUT(&sbm, &out[0], sizeof(out[0])));
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), 0);
	KUNIT_EXPECT_EQ(test, out[0].value, PASS);
	KUNIT_EXPECT_EQ(test, out[1].value, PASS);
}

/**************************************************************
 * Memory write to next page past output buffer.
 *
 * There is a guard page after the output buffer. Writes to
 * this guard page should generate a page fault.
 */

static SBM_DEFINE_FUNC(write_past_page, struct data *, out)
{
	struct page_over *over = (struct page_over *)out;

	over->nextpage = FAIL;
	barrier();
	out[0].value = FAIL;
	return 0;
}

static int sbm_write_past_page_threadfn(struct kunit *text,
					struct thread_data *data)
{
	struct sbm sbm;
	int err;

	sbm_init(&sbm);
	err = sbm_call(&sbm, write_past_page,
		       SBM_COPY_OUT(&sbm, data->out, sizeof(*data->out)));
	sbm_destroy(&sbm);
	return err;
}

static void sbm_write_past_page(struct kunit *test)
{
	struct page_over *over;
	int err;

	over = kunit_kzalloc(test, sizeof(*over), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, over);
	over->data.value = PASS;
	over->nextpage = PASS;
	if (IS_ENABLED(CONFIG_HAVE_ARCH_SBM)) {
		struct sbm sbm;

		sbm_init(&sbm);
		err = sbm_call(&sbm, write_past_page,
			       SBM_COPY_OUT(&sbm, &over->data,
					    sizeof(over->data)));
		KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
		sbm_destroy(&sbm);
	} else {
		struct thread_data data;

		data.out = &over->data;
		err = run_sbm_kthread(test, sbm_write_past_page_threadfn,
				      &data);
	}

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, over->data.value, PASS);
	KUNIT_EXPECT_EQ(test, over->nextpage, PASS);
}

/**************************************************************
 * Memory write before output buffer.
 *
 * There is a guard page before the output buffer. Writes to
 * this guard page should generate a page fault.
 */

static SBM_DEFINE_FUNC(write_before, struct data *, out)
{
	out[-1].value = FAIL;
	barrier();
	out[0].value = FAIL;
	return 0;
}

static int sbm_write_before_threadfn(struct kunit *test,
				     struct thread_data *data)
{
	struct sbm sbm;
	int err;

	sbm_init(&sbm);
	err = sbm_call(&sbm, write_before,
		       SBM_COPY_OUT(&sbm, data->out, sizeof(*data->out)));
	sbm_destroy(&sbm);
	return err;
}

static void sbm_write_before(struct kunit *test)
{
	struct data out;
	int err;

	out.value = PASS;
	if (IS_ENABLED(CONFIG_HAVE_ARCH_SBM)) {
		struct sbm sbm;

		sbm_init(&sbm);
		err = sbm_call(&sbm, write_before,
			       SBM_COPY_OUT(&sbm, &out, sizeof(out)));
		KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
		sbm_destroy(&sbm);
	} else {
		struct thread_data data;

		data.out = &out;
		err = run_sbm_kthread(test, sbm_write_before_threadfn, &data);
	}

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, out.value, PASS);
}

/**************************************************************
 * Memory write to kernel static data.
 *
 * Sandbox mode cannot write to a kernel static data.
 */

struct data static_data = { .value = FAIL };

static void sbm_write_static(struct kunit *test)
{
	struct sbm sbm;
	int err;

	check_safe_sbm(test);

	sbm_init(&sbm);
	err = sbm_call(&sbm, write, &static_data);
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
	KUNIT_EXPECT_EQ(test, static_data.value, FAIL);
}

/**************************************************************
 * Memory write to kernel BSS.
 *
 * Sandbox mode cannot write to kernel BSS.
 */

struct data static_bss;

static void sbm_write_bss(struct kunit *test)
{
	struct sbm sbm;
	int err;

	check_safe_sbm(test);

	static_bss.value = FAIL;
	sbm_init(&sbm);
	err = sbm_call(&sbm, write, &static_bss);
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
	KUNIT_EXPECT_EQ(test, static_bss.value, FAIL);
}

/**************************************************************
 * Memory write to unrelated buffer.
 *
 * Sandbox mode cannot write to the wrong buffer.
 */

static void sbm_write_wrong(struct kunit *test)
{
	struct data *out;
	struct sbm sbm;
	int err;

	check_safe_sbm(test);

	out = kunit_kzalloc(test, sizeof(*out), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, out);
	out->value = FAIL;
	sbm_init(&sbm);
	err = sbm_call(&sbm, write, out);
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
	KUNIT_EXPECT_EQ(test, out->value, FAIL);
}

/**************************************************************
 * Memory write to kernel stack.
 *
 * Sandbox mode runs on its own stack. The kernel stack cannot
 * be modified.
 */

static void sbm_write_stack(struct kunit *test)
{
	struct data out;
	struct sbm sbm;
	int err;

	check_safe_sbm(test);

	out.value = FAIL;
	sbm_init(&sbm);
	err = sbm_call(&sbm, write, &out);
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
	KUNIT_EXPECT_EQ(test, out.value, FAIL);
}

/**************************************************************
 * Simple update of an input/output buffer.
 *
 * Verify that the input buffer is copied in and also back to the caller.
 */

static SBM_DEFINE_FUNC(update, struct data *, data)
{
	data->value ^= FAIL ^ PASS;
	return 0;
}

static void sbm_update(struct kunit *test)
{
	struct sbm sbm;
	struct data data;
	int err;

	data.value = FAIL;
	sbm_init(&sbm);
	err = sbm_call(&sbm, update,
		       SBM_COPY_INOUT(&sbm, &data, sizeof(data)));
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), 0);
	KUNIT_EXPECT_EQ(test, data.value, PASS);
}

/**************************************************************
 * Copy from input buffer to output buffer.
 *
 * Verify that sandbox mode can read from the input buffer and
 * write to the output buffer.
 */

static int copy_value(const struct data *in, struct data *out)
{
	out->value = in->value;
	return 0;
}

/*
 * Define call helper and thunk explicitly to verify that this syntax also
 * works.
 */
static SBM_DEFINE_CALL(copy_value, const struct data *, in,
		       struct data *, out);
static SBM_DEFINE_THUNK(copy_value, const struct data *, in,
			struct data *, out);

static void sbm_copy_value(struct kunit *test)
{
	struct sbm sbm;
	struct data in[1];
	struct data out[1];
	int err;

	in[0].value = PASS;
	out[0].value = FAIL;
	sbm_init(&sbm);
	err = sbm_call(&sbm, copy_value,
		       SBM_COPY_IN(&sbm, in, sizeof(in)),
		       SBM_COPY_OUT(&sbm, out, sizeof(out)));
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), 0);
	KUNIT_EXPECT_EQ(test, out[0].value, PASS);
}

/**************************************************************
 * Memory read past input buffer within the same page.
 *
 * The page beyond the input buffer is initialized to zero.
 */

static SBM_DEFINE_FUNC(read_past, const struct data *, in)
{
	return in[1].value;
}

static void sbm_read_past(struct kunit *test)
{
	struct sbm sbm;
	struct data in[2];
	int err;

	in[0].value = PASS;
	in[1].value = FAIL;
	sbm_init(&sbm);
	err = sbm_call(&sbm, read_past,
		       SBM_COPY_IN(&sbm, &in[0], sizeof(in[0])));
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), 0);
}

/**************************************************************
 * Memory read from next page past input buffer.
 *
 * There is a guard page after the input buffer. Reading from
 * that page should generate a page fault.
 */

static SBM_DEFINE_FUNC(read_past_page, const struct data *, in)
{
	const struct page_over *over = (const struct page_over *)in;

	return over->nextpage;
}

static int sbm_read_past_page_threadfn(struct kunit *test,
				       struct thread_data *data)
{
	struct sbm sbm;
	int err;

	sbm_init(&sbm);
	err = sbm_call(&sbm, read_past_page,
		       SBM_COPY_IN(&sbm, data->in, sizeof(*data->in)));
	sbm_destroy(&sbm);
	return err;
}

static void sbm_read_past_page(struct kunit *test)
{
	struct page_over *over;
	int err;

	over = kunit_kzalloc(test, sizeof(*over), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, over);
	over->data.value = PASS;
	over->nextpage = FAIL;
	if (IS_ENABLED(CONFIG_HAVE_ARCH_SBM)) {
		struct sbm sbm;

		sbm_init(&sbm);
		err = sbm_call(&sbm, read_past_page,
			       SBM_COPY_IN(&sbm, &over->data,
					   sizeof(over->data)));
		KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
		sbm_destroy(&sbm);
	} else {
		struct thread_data data;

		data.in = &over->data;
		err = run_sbm_kthread(test, sbm_read_past_page_threadfn, &data);
	}

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
}

/**************************************************************
 * Memory read before input buffer.
 *
 * There is a guard page before the input buffer. Reading from
 * that page should generate a page fault.
 */

static SBM_DEFINE_FUNC(read_before, const struct data *, in)
{
	return in[-1].value;
}

static int sbm_read_before_threadfn(struct kunit *test,
				    struct thread_data *data)
{
	struct sbm sbm;
	int err;

	sbm_init(&sbm);
	err = sbm_call(&sbm, read_before,
		       SBM_COPY_IN(&sbm, data->in, sizeof(*data->in)));
	sbm_destroy(&sbm);
	return err;
}

static void sbm_read_before(struct kunit *test)
{
	struct data in;
	int err;

	in.value = PASS;
	if (IS_ENABLED(CONFIG_HAVE_ARCH_SBM)) {
		struct sbm sbm;

		sbm_init(&sbm);
		err = sbm_call(&sbm, read_before,
			       SBM_COPY_IN(&sbm, &in, sizeof(in)));
		KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
		sbm_destroy(&sbm);
	} else {
		struct thread_data data;

		data.in = &in;
		err = run_sbm_kthread(test, sbm_read_before_threadfn, &data);
	}

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
}

/**************************************************************
 * Memory read from unrelated buffer.
 *
 * Sandbox mode cannot read from the wrong buffer.
 */

static SBM_DEFINE_FUNC(read_wrong, const struct data *, in)
{
	return in->value;
}

static void sbm_read_wrong(struct kunit *test)
{
	struct data *in;
	struct sbm sbm;
	int err;

	check_safe_sbm(test);

	in = kunit_kzalloc(test, sizeof(*in), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, in);
	in->value = FAIL;
	sbm_init(&sbm);
	err = sbm_call(&sbm, read_wrong, in);
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
}

/**************************************************************
 * Stack bottom.
 *
 * Sandbox mode can read from the bottom of the kernel stack.
 * Verify that all of the kernel stack is mapped.
 */

static SBM_DEFINE_FUNC(stack_bottom)
{
	return *end_of_stack(current);
}

static void sbm_stack_bottom(struct kunit *test)
{
	struct sbm sbm;
	unsigned long *bottom;
	int err;

	bottom = end_of_stack(current);
	*bottom = PASS;
	sbm_init(&sbm);
	err = sbm_call(&sbm, stack_bottom);
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, PASS);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), 0);
	KUNIT_EXPECT_EQ(test, *bottom, PASS);
}

/**************************************************************
 * Stack overflow.
 *
 * Sandbox mode cannot overflow the stack.
 *
 * This test is not safe without SBM arch hooks, because the kernel may panic
 * when kernel stack overflow is detected.
 */

static noinline int kaboom(void)
{
	return 0;
}

static SBM_DEFINE_FUNC(stack_overflow)
{
	unsigned long old_sp = current_stack_pointer;
	int err;

	current_stack_pointer = (unsigned long)end_of_stack(current);
	barrier();
	err = kaboom();
	current_stack_pointer = old_sp;
	return err;
}

static void sbm_stack_overflow(struct kunit *test)
{
	struct sbm sbm;
	int err;

	check_safe_sbm(test);

	sbm_init(&sbm);
	err = sbm_call(&sbm, stack_overflow);
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
}

#ifdef CONFIG_X86_64

/**************************************************************
 * [X86-64] Non-canonical address.
 *
 * Sandbox mode recovers from a #GP exception. Test it by
 * memory write to a non-canonical address.
 */

static void sbm_x86_64_gp(struct kunit *test)
{
	void *non_canonical;
	struct sbm sbm;
	int err;

	check_safe_sbm(test);

	non_canonical = (void *)(1ULL << 63);
	sbm_init(&sbm);
	err = sbm_call(&sbm, write, non_canonical);
	sbm_destroy(&sbm);

	KUNIT_EXPECT_EQ(test, err, -EFAULT);
	KUNIT_EXPECT_EQ(test, sbm_error(&sbm), -EFAULT);
}

#endif

/**************************************************************
 * Test suite metadata.
 */

static struct kunit_case sbm_test_cases[] = {
	KUNIT_CASE(sbm_write),
	KUNIT_CASE(sbm_write_past),
	KUNIT_CASE(sbm_write_past_page),
	KUNIT_CASE(sbm_write_before),
	KUNIT_CASE(sbm_write_static),
	KUNIT_CASE(sbm_write_bss),
	KUNIT_CASE(sbm_write_wrong),
	KUNIT_CASE(sbm_write_stack),
	KUNIT_CASE(sbm_copy_value),
	KUNIT_CASE(sbm_read_past),
	KUNIT_CASE(sbm_read_past_page),
	KUNIT_CASE(sbm_read_before),
	KUNIT_CASE(sbm_read_wrong),
	KUNIT_CASE(sbm_update),
	KUNIT_CASE(sbm_stack_bottom),
	KUNIT_CASE(sbm_stack_overflow),
#ifdef CONFIG_X86_64
	KUNIT_CASE(sbm_x86_64_gp),
#endif
	{}
};

static struct kunit_suite sbm_test_suite = {
	.name = "sandbox_mode",
	.test_cases = sbm_test_cases,
};

kunit_test_suites(&sbm_test_suite);

MODULE_LICENSE("GPL");
