// SPDX-License-Identifier: GPL-2.0
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tests.h"
#include "debug.h"
#include "dso.h"
#include "machine.h"
#include "map.h"
#include "symbol.h"
#include "util/ftrace.h"

static int test_parse_retaddr(void)
{
	char sym[128];
	u64 offset = 0;

	/* Standard format */
	TEST_ASSERT_VAL("parse standard retaddr",
			ftrace_parse_retaddr("kernel_read() { /* <-load_elf_phdrs+0x6c/0xb0 */",
					     sym, sizeof(sym), &offset));
	TEST_ASSERT_EQUAL("sym name", strcmp(sym, "load_elf_phdrs"), 0);
	TEST_ASSERT_VAL("offset", offset == 0x6c);

	/* With return value */
	TEST_ASSERT_VAL("parse retaddr with ret",
			ftrace_parse_retaddr(
				"__cond_resched(); /* <-load_elf_phdrs+0x6c/0xb0 ret=0x0 */",
				sym, sizeof(sym), &offset));
	TEST_ASSERT_EQUAL("sym name", strcmp(sym, "load_elf_phdrs"), 0);
	TEST_ASSERT_VAL("offset", offset == 0x6c);

	/* Bracketed address */
	TEST_ASSERT_VAL("parse bracketed retaddr",
			ftrace_parse_retaddr("foo() { /* <-[0xffffffff818534c8] bar+0x20/0x40 */",
					     sym, sizeof(sym), &offset));
	TEST_ASSERT_EQUAL("sym name", strcmp(sym, "bar"), 0);
	TEST_ASSERT_VAL("offset", offset == 0x20);

	/* No return address */
	TEST_ASSERT_VAL("no retaddr",
			!ftrace_parse_retaddr("do_filp_open();", sym, sizeof(sym), &offset));

	return TEST_OK;
}

static int test_filter_retaddr(void)
{
	char line1[] = "kernel_read() { /* <-load_elf_phdrs+0x6c/0xb0 */";
	char line2[] = "__cond_resched(); /* <-load_elf_phdrs+0x6c/0xb0 ret=0x0 */";
	char line3[] = "do_filp_open();";

	ftrace_filter_retaddr(line1);
	TEST_ASSERT_EQUAL("filter standard retaddr", strcmp(line1, "kernel_read() {"), 0);

	ftrace_filter_retaddr(line2);
	TEST_ASSERT_EQUAL("filter retaddr with ret", strcmp(line2, "__cond_resched();"), 0);

	ftrace_filter_retaddr(line3);
	TEST_ASSERT_EQUAL("filter line without retaddr unchanged",
			  strcmp(line3, "do_filp_open();"), 0);

	return TEST_OK;
}

#define TEST_CHECK(text, cond)					\
do {								\
	if (!(cond)) {						\
		pr_debug("FAILED %s:%d %s\n", __FILE__, __LINE__, text); \
		ret = TEST_FAIL;				\
		goto out_cleanup;				\
	}							\
} while (0)

static int test_inline_processing(void)
{
	static const char line_entry[] = " 0)               |  wake_up_new_task() {";
	static const char line_enqueue[] =
		" 0)               |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */";
	static const char line_exit_enqueue[] = " 0) + 10.000 us   |    } /* enqueue_task */";
	static const char line_exit_task[] = " 0) + 20.000 us   |  } /* wake_up_new_task */";
	static const char line_bare_enqueue[] = " 0) + 10.000 us   |    }";
	static const char line_bare_task[] = " 0) + 20.000 us   |  }";
	static const char line_unmatched[] = " 0) +  5.000 us   |    } /* unrelated_func */";
	static const char line_verbose_entry[] =
		" 1234.567890 |   0)   bash-1234    |               |  wake_up_new_task() {";
	static const char line_verbose_enqueue[] =
		" 1234.567890 |   0)   bash-1234    |               |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */";
	static const char line_verbose_exit_enqueue[] =
		" 1234.567890 |   0)   bash-1234    |   1.234 us    |    } /* enqueue_task */";
	static const char line_verbose_exit_task[] =
		" 1234.567890 |   0)   bash-1234    |   2.567 us    |  } /* wake_up_new_task */";
	static const char line_verbose_inline_entry[] =
		" 1234.567890 |   0)   bash-1234    |               |    activate_task() { /* (inline) */";
	static const char line_verbose_inline_exit[] =
		" 1234.567890 |   0)   bash-1234    |               |    } /* activate_task (inline) */";
	struct perf_ftrace ftrace;
	char *out_buf = NULL;
	size_t out_len = 0;
	FILE *out;
	const char *unmatched_pos;
	const char *inline_exit_pos;
	const char *vmlinux;
	int ret = TEST_OK;

	memset(&ftrace, 0, sizeof(ftrace));
	ftrace.use_inline = true;
	ftrace.filter_retaddr = 1; /* Default when --graph-opts retaddr is not specified */
	ftrace.tracer = "function_graph";

	vmlinux = vmlinux_path__find();

	/* If vmlinux is present, test resolving symbols */
	if (vmlinux) {
		symbol_conf.vmlinux_name = vmlinux;
		symbol_conf.ignore_vmlinux_buildid = true;
		ret = perf_ftrace__setup_inlines(&ftrace);
		if (ret < 0 || !ftrace.machine) {
			ret = TEST_SKIP;
			goto out_cleanup;
		}

		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		/* Feed simulated function graph trace lines */
		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_exit_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_exit_task, out);

		fclose(out);
		ftrace_cpu_states__clear();

		pr_debug("Processed output:\n%s\n", out_buf);

		/* Verify activate_task entry with inline hint */
		TEST_CHECK("contains activate_task entry with inline hint",
			   strstr(out_buf, "activate_task() { /* (inline) */") != NULL);

		/* Verify activate_task exit with inline hint */
		TEST_CHECK("contains activate_task exit with inline hint",
			   strstr(out_buf, "} /* activate_task (inline) */") != NULL);

		/* Verify return address comment was filtered out by default */
		TEST_CHECK("retaddr comment filtered out",
			   strstr(out_buf, "/* <-wake_up_new_task") == NULL);

		free(out_buf);
		out_buf = NULL;

		/* Now test with filter_retaddr = 0 (user specified --graph-opts retaddr) */
		ftrace.filter_retaddr = 0;
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_exit_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_exit_task, out);

		fclose(out);
		ftrace_cpu_states__clear();

		/* Verify return address comment IS preserved when filter_retaddr = 0 */
		TEST_CHECK("retaddr comment preserved when requested",
			   strstr(out_buf, "/* <-wake_up_new_task") != NULL);

		free(out_buf);
		out_buf = NULL;

		/* Test with bare closing braces (funcgraph-tail disabled by default) */
		ftrace.filter_retaddr = 1;
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_bare_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_bare_task, out);

		fclose(out);

		TEST_CHECK("bare braces inline entry",
			   strstr(out_buf, "activate_task() { /* (inline) */") != NULL);
		TEST_CHECK("bare braces inline exit",
			   strstr(out_buf, "} /* activate_task (inline) */") != NULL);

		free(out_buf);
		out_buf = NULL;

		/* Test that unmatched exit events do not pop active inlined frames */
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_exit_enqueue, out);
		/* Unmatched exit event while activate_task is still active */
		ftrace_process_fgraph_line(&ftrace, line_unmatched, out);
		ftrace_process_fgraph_line(&ftrace, line_exit_task, out);

		fclose(out);

		unmatched_pos = strstr(out_buf, "} /* unrelated_func */");
		inline_exit_pos = strstr(out_buf, "} /* activate_task (inline) */");

		TEST_CHECK("unmatched exit present", unmatched_pos != NULL);
		TEST_CHECK("inline exit present", inline_exit_pos != NULL);
		TEST_CHECK("inline exit not prematurely popped before unmatched exit",
			   inline_exit_pos > unmatched_pos);

		free(out_buf);
		out_buf = NULL;

		/* Test recovery when an intermediate real function exit event is dropped */
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		/* line_exit_enqueue is dropped; outer wake_up_new_task exits directly */
		ftrace_process_fgraph_line(&ftrace, line_exit_task, out);

		fclose(out);

		TEST_CHECK("inline exit synthesized despite dropped callee exit",
			   strstr(out_buf, "} /* activate_task (inline) */") != NULL);

		free(out_buf);
		out_buf = NULL;

		/* Test recovery when intermediate callee exit is dropped before sibling call */
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		/* line_exit_enqueue dropped; caller makes sibling call without inlines */
		ftrace_process_fgraph_line(&ftrace,
			" 0)               |    check_preempt_curr(); /* <-wake_up_new_task+0x0/0x3e0 */",
			out);
		ftrace_process_fgraph_line(&ftrace, line_exit_task, out);

		fclose(out);

		TEST_CHECK("inline exit synthesized when callee exit dropped before sibling call",
			   strstr(out_buf, "} /* activate_task (inline) */") != NULL);

		free(out_buf);
		out_buf = NULL;

		/* Test that callers not on active stack do not synthesise inlines */
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		/* housekeeping_any_cpu called by untraced ring_buffer_unlock_commit */
		ftrace_process_fgraph_line(&ftrace,
			" 0)               |    housekeeping_any_cpu(); /* <-ring_buffer_unlock_commit+0x1a0/0x1f0 */",
			out);
		ftrace_process_fgraph_line(&ftrace, line_exit_task, out);

		fclose(out);

		TEST_CHECK("untraced caller inlines not synthesised",
			   strstr(out_buf, "rb_wakeups") == NULL &&
			   strstr(out_buf, "rb_irq_work_queue") == NULL);

		free(out_buf);
		out_buf = NULL;

		/* Test verbose mode prefix formatting for synthesised inlines */
		ftrace.graph_verbose = true;
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_verbose_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_verbose_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_verbose_exit_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_verbose_exit_task, out);

		fclose(out);
		ftrace_cpu_states__clear();

		TEST_CHECK("verbose inline entry preserves comm-pid",
			   strstr(out_buf, line_verbose_inline_entry) != NULL);
		TEST_CHECK("verbose inline exit preserves comm-pid",
			   strstr(out_buf, line_verbose_inline_exit) != NULL);

		free(out_buf);
		out_buf = NULL;
		ftrace.graph_verbose = false;
	}

out_cleanup:
	free(out_buf);
	perf_ftrace__cleanup_inlines(&ftrace);
	return ret;
}
#undef TEST_CHECK

static int test_stack_saturation(void)
{
	struct perf_ftrace ftrace;
	char *out_buf = NULL;
	size_t out_len = 0;
	FILE *out;
	int i;

	memset(&ftrace, 0, sizeof(ftrace));
	ftrace.use_inline = true;
	ftrace_cpu_states__clear();

	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	/* Push root frame at depth 0 */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  root_func() {", out);

	/* Push 255 more frames to fill FTRACE_MAX_STACK (256) */
	for (i = 1; i < 256; i++)
		ftrace_process_fgraph_line(&ftrace, " 0)               |  sub_func() {", out);

	/* Push 44 overflow frames beyond FTRACE_MAX_STACK */
	for (i = 0; i < 44; i++)
		ftrace_process_fgraph_line(&ftrace, " 0)               |  overflow_func() {", out);

	/* Pop the 44 overflow frames with bare closing braces */
	for (i = 0; i < 44; i++)
		ftrace_process_fgraph_line(&ftrace, " 0)               |  }", out);

	/* Pop 255 tracked sub_func frames */
	for (i = 1; i < 256; i++)
		ftrace_process_fgraph_line(&ftrace, " 0)               |  }", out);

	/* Pop root frame - must cleanly match root_func on stack, not popped prematurely */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  } /* root_func */", out);

	/* Feed one more function entry - it must have base indentation (no indentation drift) */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  base_func() {", out);

	fclose(out);
	ftrace_cpu_states__clear();

	/* Verify base_func starts at standard 2 spaces indentation without drift */
	if (!strstr(out_buf, "|  base_func() {")) {
		pr_debug("FAILED %s:%d no indentation drift after stack saturation\n",
			 __FILE__, __LINE__);
		free(out_buf);
		return TEST_FAIL;
	}

	free(out_buf);
	out_buf = NULL;

	/* Test overflow recovery when events are dropped */
	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	ftrace_process_fgraph_line(&ftrace, " 0)               |  root_func() {", out);
	for (i = 1; i < 256; i++)
		ftrace_process_fgraph_line(&ftrace, " 0)               |  sub_func() {", out);
	for (i = 0; i < 44; i++)
		ftrace_process_fgraph_line(&ftrace, " 0)               |  overflow_func() {", out);

	/* Dropped events: root_func directly calls recovery_func */
	ftrace_process_fgraph_line(&ftrace, " 0)               |    recovery_func() { /* <-root_func+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   1.000 us    |    }", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   5.000 us    |  } /* root_func */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "recovery_func() {")) {
		pr_debug("FAILED %s:%d recovery_func missing after overflow recovery\n",
			 __FILE__, __LINE__);
		free(out_buf);
		return TEST_FAIL;
	}
	if (!strstr(out_buf, "} /* root_func */")) {
		pr_debug("FAILED %s:%d root_func exit missing after overflow recovery\n",
			 __FILE__, __LINE__);
		free(out_buf);
		return TEST_FAIL;
	}

	free(out_buf);
	return TEST_OK;
}

static int test_column_layouts(void)
{
	static const struct {
		const char *desc;
				bool verbose;
		const char *in;
		const char *expected;
	} cases[] = {
		{
			"default layout", false,
			" 7)               |  wake_up_new_task() { /* <-kernel_clone+0x160/0x510 */",
			" 7)               |  wake_up_new_task() {\n",
		},
		{
			"verbose layout", true,
			" 1234.567890 |   7)   bash-1234    |               |  wake_up_new_task() { /* <-kernel_clone+0x160/0x510 */",
			" 1234.567890 |   7)   bash-1234    |               |  wake_up_new_task() {\n",
		},
	};
	struct perf_ftrace ftrace;
	char *out_buf = NULL;
	size_t out_len = 0;
	FILE *out;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		memset(&ftrace, 0, sizeof(ftrace));
		ftrace.use_inline = true;
		ftrace.filter_retaddr = true;
				ftrace.graph_verbose = cases[i].verbose;

		out = open_memstream(&out_buf, &out_len);
		if (!out)
			return TEST_FAIL;

		ftrace_process_fgraph_line(&ftrace, cases[i].in, out);
		fclose(out);
		ftrace_cpu_states__clear();

		if (strcmp(out_buf, cases[i].expected)) {
			pr_debug("%s: expected '%s', got '%s'\n",
				 cases[i].desc, cases[i].expected, out_buf);
			free(out_buf);
			return TEST_FAIL;
		}
		free(out_buf);
		out_buf = NULL;
	}

	return TEST_OK;
}

static int test_context_switch_handling(void)
{
	struct perf_ftrace ftrace;
	char *out_buf = NULL;
	size_t out_len = 0;
	FILE *out;

	memset(&ftrace, 0, sizeof(ftrace));
	ftrace.use_inline = true;
	ftrace_cpu_states__clear();

	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	/* Task 1 (PID 100) starts running on CPU 0 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    <idle>-0    =>    task1-100 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);

	/* Task 1 enters func1 */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  func1() {", out);

	/* Context switch to Task 2 (PID 200) on CPU 0 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task1-100   =>    task2-200 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);

	/* Task 2 executes func2 - should NOT inherit func1's indentation */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  func2() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  } /* func2 */", out);

	/* Task 1 migrates and resumes on CPU 1 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    <idle>-0    =>    task1-100 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);

	/* Task 1 exits func1 on CPU 1 - should cleanly match func1 from its preserved stack */
	ftrace_process_fgraph_line(&ftrace, " 1)               |  } /* func1 */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	/* Verify func2 in Task 2 started at base indentation "|  func2() {" */
	TEST_ASSERT_VAL("task2 does not inherit task1 call stack",
			strstr(out_buf, "|  func2() {") != NULL);

	/* Verify task1 exit on CPU 1 matches cleanly */
	TEST_ASSERT_VAL("task1 preserved stack across migration",
			strstr(out_buf, "1)               |  } /* func1 */") != NULL);

	free(out_buf);
	return TEST_OK;
}

static int test__ftrace(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	int ret;

	ret = test_parse_retaddr();
	if (ret != TEST_OK)
		return ret;

	ret = test_filter_retaddr();
	if (ret != TEST_OK)
		return ret;

	ret = test_stack_saturation();
	if (ret != TEST_OK)
		return ret;

	ret = test_context_switch_handling();
	if (ret != TEST_OK)
		return ret;

	ret = test_column_layouts();
	if (ret != TEST_OK)
		return ret;

	ret = test_inline_processing();
	if (ret != TEST_OK)
		return ret;

	return TEST_OK;
}

DEFINE_SUITE("Ftrace inline processing", ftrace);
