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
#include "evlist.h"
#include "thread_map.h"

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
	TEST_ASSERT_EQUAL("filter retaddr with ret",
			  strcmp(line2, "__cond_resched(); /* ret=0x0 */"), 0);

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
		" 1234.567890 |   0)   bash-1234    |  ....   |               |  wake_up_new_task() {";
	static const char line_verbose_enqueue[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |               |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */";
	static const char line_verbose_exit_enqueue[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |   1.234 us    |    } /* enqueue_task */";
	static const char line_verbose_exit_task[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |   2.567 us    |  } /* wake_up_new_task */";
	static const char line_verbose_inline_entry[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |               |    activate_task() { /* (inline) */";
	static const char line_verbose_inline_exit[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |               |    } /* activate_task (inline) */";
	struct perf_ftrace ftrace;
	char *out_buf = NULL;
	size_t out_len = 0;
	FILE *out;
	const char *unmatched_pos;
	const char *inline_exit_pos;
	const char *vmlinux;
	const char *saved_vmlinux = symbol_conf.vmlinux_name;
	bool saved_ignore_buildid = symbol_conf.ignore_vmlinux_buildid;
	int ret = TEST_OK;

	memset(&ftrace, 0, sizeof(ftrace));
	ftrace.use_inline = true;
	ftrace.filter_retaddr = 1; /* Default when --graph-opts retaddr is not specified */
	ftrace.tracer = "function_graph";

	vmlinux = vmlinux_path__find();

	/* If vmlinux is present, test resolving symbols */
	if (vmlinux) {
		const char *inlined_names[FTRACE_MAX_INLINES];
		int num_inlines;
		int j;

		symbol_conf.vmlinux_name = vmlinux;
		symbol_conf.ignore_vmlinux_buildid = true;
		ret = perf_ftrace__setup_inlines(&ftrace);
		if (ret < 0 || !ftrace.machine) {
			ret = TEST_SKIP;
			goto out_cleanup;
		}

		num_inlines = ftrace_resolve_inlines(ftrace.machine,
						     "wake_up_new_task", 0x1d1,
						     inlined_names,
						     FTRACE_MAX_INLINES);
		if (num_inlines <= 0 || strcmp(inlined_names[0], "activate_task") != 0) {
			pr_debug("vmlinux does not inline activate_task at wake_up_new_task+0x1d1; skipping inline test\n");
			for (j = 0; j < num_inlines; j++)
				free((void *)inlined_names[j]);
			ret = TEST_SKIP;
			goto out_cleanup;
		}
		for (j = 0; j < num_inlines; j++)
			free((void *)inlined_names[j]);

		if (ftrace_resolve_inlines(ftrace.machine, "wake_up_new_task", 0,
					   inlined_names, FTRACE_MAX_INLINES) != 0) {
			pr_debug("ftrace_resolve_inlines with offset 0 should return 0\n");
			ret = TEST_FAIL;
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

		/* Test bare closing brace matches outer frame when inner callee exit was dropped */
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		/* line_bare_enqueue is dropped! */
		ftrace_process_fgraph_line(&ftrace, line_bare_task, out);

		fclose(out);

		TEST_CHECK("dropped callee exit: inlined frame closed",
			   strstr(out_buf, "} /* activate_task (inline) */") != NULL);
		TEST_CHECK("dropped callee exit: outer bare brace emitted",
			   strstr(out_buf, "  }") != NULL);

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

		/* Test trailing inlined frame closed on untraced entry (caller_idx < 0) */
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		/* line_enqueue has inlined activate_task() open; next entry has caller_idx < 0 */
		ftrace_process_fgraph_line(&ftrace,
					   " 0)               |    irq_handler() {",
					   out);
		ftrace_process_fgraph_line(&ftrace,
					   " 0)               |    } /* irq_handler */",
					   out);
		ftrace_process_fgraph_line(&ftrace, line_exit_task, out);

		fclose(out);

		TEST_CHECK("trailing inline frame closed before untraced call",
			   strstr(out_buf, "} /* activate_task (inline) */") != NULL);
		TEST_CHECK("untraced call not indented inside trailing inline frame",
			   strstr(out_buf, "    irq_handler() {") != NULL);

		free(out_buf);
		out_buf = NULL;

		/* Test dropped exits unwound when new function starts at shallower indent */
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_enqueue, out);
		/* Exits dropped! New root entry arrives at indent 2 */
		ftrace_process_fgraph_line(&ftrace,
					   " 0)               |  schedule() {",
					   out);
		ftrace_process_fgraph_line(&ftrace,
					   " 0)               |  } /* schedule */",
					   out);

		fclose(out);

		TEST_CHECK("stale inlines closed when new root arrives",
			   strstr(out_buf, "} /* activate_task (inline) */") != NULL);
		TEST_CHECK("new root entry printed without stale indentation",
			   strstr(out_buf, "  schedule() {") != NULL);

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
	symbol_conf.vmlinux_name = saved_vmlinux;
	symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
	return ret;
}
#undef TEST_CHECK

static void feed_fgraph_entry(struct perf_ftrace *ftrace, int cpu, int depth,
			      const char *name, const char *retaddr, FILE *out)
{
	char line[1024];
	int indent = 2 + 2 * depth;

	if (retaddr) {
		snprintf(line, sizeof(line), " %d)               |%*s%s() { /* <-%s */",
			 cpu, indent, "", name, retaddr);
	} else {
		snprintf(line, sizeof(line), " %d)               |%*s%s() {",
			 cpu, indent, "", name);
	}
	ftrace_process_fgraph_line(ftrace, line, out);
}

static void feed_fgraph_exit(struct perf_ftrace *ftrace, int cpu, int depth,
			     const char *name, FILE *out)
{
	char line[1024];
	int indent = 2 + 2 * depth;

	if (name) {
		snprintf(line, sizeof(line), " %d)   1.000 us    |%*s} /* %s */",
			 cpu, indent, "", name);
	} else {
		snprintf(line, sizeof(line), " %d)   1.000 us    |%*s}",
			 cpu, indent, "");
	}
	ftrace_process_fgraph_line(ftrace, line, out);
}

static int test_stack_saturation(void)
{
	struct perf_ftrace ftrace;
	char *out_buf = NULL;
	size_t out_len = 0;
	FILE *out;
	const char *vmlinux;
	int i;

	memset(&ftrace, 0, sizeof(ftrace));
	ftrace.use_inline = true;
	ftrace_cpu_states__clear();

	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	/* Push root frame at depth 0 */
	feed_fgraph_entry(&ftrace, 0, 0, "root_func", NULL, out);

	/* Push 255 more frames to fill FTRACE_MAX_STACK (256) */
	for (i = 1; i < 256; i++)
		feed_fgraph_entry(&ftrace, 0, i, "sub_func", NULL, out);

	/* Push 44 overflow frames beyond FTRACE_MAX_STACK */
	for (i = 0; i < 44; i++)
		feed_fgraph_entry(&ftrace, 0, 256 + i, "overflow_func", NULL, out);

	/* Pop the 44 overflow frames with bare closing braces */
	for (i = 43; i >= 0; i--)
		feed_fgraph_exit(&ftrace, 0, 256 + i, NULL, out);

	/* Pop 255 tracked sub_func frames */
	for (i = 255; i >= 1; i--)
		feed_fgraph_exit(&ftrace, 0, i, NULL, out);

	/* Pop root frame - must cleanly match root_func on stack, not popped prematurely */
	feed_fgraph_exit(&ftrace, 0, 0, "root_func", out);

	/* Feed one more function entry - it must have base indentation (no indentation drift) */
	feed_fgraph_entry(&ftrace, 0, 0, "base_func", NULL, out);

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

	feed_fgraph_entry(&ftrace, 0, 0, "root_func", NULL, out);
	for (i = 1; i < 256; i++)
		feed_fgraph_entry(&ftrace, 0, i, "sub_func", NULL, out);
	for (i = 0; i < 44; i++)
		feed_fgraph_entry(&ftrace, 0, 256 + i, "overflow_func", NULL, out);

	/* Dropped events: root_func directly calls recovery_func */
	feed_fgraph_entry(&ftrace, 0, 1, "recovery_func", "root_func+0x20", out);
	feed_fgraph_exit(&ftrace, 0, 1, NULL, out);
	feed_fgraph_exit(&ftrace, 0, 0, "root_func", out);

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
	out_buf = NULL;

	/* Test that bare braces during overflow do not prematurely pop tracked frames */
	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	feed_fgraph_entry(&ftrace, 0, 0, "root_func", NULL, out);
	for (i = 1; i < 256; i++)
		feed_fgraph_entry(&ftrace, 0, i, "sub_func", NULL, out);
	for (i = 0; i < 10; i++)
		feed_fgraph_entry(&ftrace, 0, 256 + i, "overflow_func", NULL, out);

	/* Pop 10 overflow frames with bare braces */
	for (i = 9; i >= 0; i--)
		feed_fgraph_exit(&ftrace, 0, 256 + i, NULL, out);

	/* Pop 255 sub_func frames with bare braces */
	for (i = 255; i >= 1; i--)
		feed_fgraph_exit(&ftrace, 0, i, NULL, out);

	/* Root frame must still be on the stack to match caller */
	feed_fgraph_entry(&ftrace, 0, 1, "child_func", "root_func+0x10", out);
	feed_fgraph_exit(&ftrace, 0, 1, NULL, out);
	feed_fgraph_exit(&ftrace, 0, 0, "root_func", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "child_func() {")) {
		pr_debug("FAILED %s:%d child_func missing after bare braces in overflow\n",
			 __FILE__, __LINE__);
		free(out_buf);
		return TEST_FAIL;
	}
	if (!strstr(out_buf, "} /* root_func */")) {
		pr_debug("FAILED %s:%d root_func exit missing after bare braces in overflow\n",
			 __FILE__, __LINE__);
		free(out_buf);
		return TEST_FAIL;
	}

	free(out_buf);
	out_buf = NULL;

	/* Test recursive caller lookup skips stale inner invocation */
	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	/* Outer recursive invocation at indent 2 */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  rec_func() {", out);
	/* Inner recursive invocation at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
				   " 0)               |    rec_func() { /* <-rec_func+0x10 */",
				   out);
	/* Inner invocation exit is dropped by ring buffer */
	/* Outer invocation calls sibling_func at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
				   " 0)   1.000 us    |    sibling_func(); /* <-rec_func+0x20 */",
				   out);
	ftrace_process_fgraph_line(&ftrace, " 0)   2.000 us    |  } /* rec_func */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "sibling_func();")) {
		pr_debug("FAILED %s:%d sibling_func missing under recursive caller\n",
			 __FILE__, __LINE__);
		free(out_buf);
		return TEST_FAIL;
	}
	if (!strstr(out_buf, "} /* rec_func */")) {
		pr_debug("FAILED %s:%d rec_func exit missing after dropped inner exit\n",
			 __FILE__, __LINE__);
		free(out_buf);
		return TEST_FAIL;
	}
	free(out_buf);
	out_buf = NULL;

	/* Test top inlined frame when cs->overflow_depth > 0 does not misclassify exit */
	vmlinux = vmlinux_path__find();
	if (vmlinux) {
		const char *saved_vmlinux = symbol_conf.vmlinux_name;
		bool saved_ignore_buildid = symbol_conf.ignore_vmlinux_buildid;
		const char *inlined_names[FTRACE_MAX_INLINES];
		int num_inlines;
		int j;

		symbol_conf.vmlinux_name = vmlinux;
		symbol_conf.ignore_vmlinux_buildid = true;
		if (perf_ftrace__setup_inlines(&ftrace) == 0 && ftrace.machine) {
			num_inlines = ftrace_resolve_inlines(ftrace.machine,
							     "wake_up_new_task", 0x1d1,
							     inlined_names,
							     FTRACE_MAX_INLINES);
			if (num_inlines > 0 && !strcmp(inlined_names[0], "activate_task")) {
				out = open_memstream(&out_buf, &out_len);
				if (!out) {
					for (j = 0; j < num_inlines; j++)
						free((void *)inlined_names[j]);
					perf_ftrace__cleanup_inlines(&ftrace);
					symbol_conf.vmlinux_name = saved_vmlinux;
					symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
					return TEST_FAIL;
				}

				/* Fill stack up to 254 with tracked frames */
				feed_fgraph_entry(&ftrace, 0, 0, "root_func", NULL, out);
				for (i = 1; i < 254; i++)
					feed_fgraph_entry(&ftrace, 0, i, "sub_func", NULL, out);

				/* Frame 254 is wake_up_new_task */
				feed_fgraph_entry(&ftrace, 0, 254, "wake_up_new_task", NULL, out);

				/*
				 * Frame 255: enqueue_task resolves inline activate_task.
				 * Inlined frame activate_task is pushed at stack[255]
				 * (cs->depth = 256). Real frame enqueue_task overflows
				 * into cs->overflow_depth = 1.
				 */
				feed_fgraph_entry(&ftrace, 0, 255, "enqueue_task",
						  "wake_up_new_task+0x1d1/0x3e0", out);

				/*
				 * Nested child call inside overflow: does NOT pop the
				 * inlined frame!
				 */
				feed_fgraph_entry(&ftrace, 0, 256, "child_overflow",
						  "enqueue_task+0x10", out);

				/* Exit child_overflow (overflow_depth decrements to 1) */
				feed_fgraph_exit(&ftrace, 0, 256, NULL, out);

				/* Exit enqueue_task (overflow_depth decrements to 0) */
				feed_fgraph_exit(&ftrace, 0, 255, NULL, out);

				/* Exit wake_up_new_task: synthesises inline exit */
				feed_fgraph_exit(&ftrace, 0, 254, NULL, out);

				/* Pop remaining sub_func frames and root_func */
				for (i = 253; i >= 1; i--)
					feed_fgraph_exit(&ftrace, 0, i, NULL, out);
				feed_fgraph_exit(&ftrace, 0, 0, "root_func", out);

				/* Feed base frame to check for indentation drift */
				feed_fgraph_entry(&ftrace, 0, 0, "drift_check", NULL, out);

				fclose(out);
				ftrace_cpu_states__clear();

				if (!strstr(out_buf, "activate_task() { /* (inline) */") ||
				    !strstr(out_buf, "} /* activate_task (inline) */") ||
				    !strstr(out_buf, "|  drift_check() {")) {
					pr_debug("FAILED %s:%d top inlined frame saturation failed\n",
						 __FILE__, __LINE__);
					free(out_buf);
					for (j = 0; j < num_inlines; j++)
						free((void *)inlined_names[j]);
					perf_ftrace__cleanup_inlines(&ftrace);
					symbol_conf.vmlinux_name = saved_vmlinux;
					symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
					return TEST_FAIL;
				}

				free(out_buf);
				out_buf = NULL;
			}
			for (j = 0; j < num_inlines; j++)
				free((void *)inlined_names[j]);
			perf_ftrace__cleanup_inlines(&ftrace);
		}
		symbol_conf.vmlinux_name = saved_vmlinux;
		symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
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
			" 1234.567890 |   7)   bash-1234    |  ....   |               |  wake_up_new_task() { /* <-kernel_clone+0x160/0x510 */",
			" 1234.567890 |   7)   bash-1234    |  ....   |               |  wake_up_new_task() {\n",
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
	struct perf_thread_map *threads;
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

	/*
	 * Initial Task 1 (PID 100) starts running on CPU 0 before any banner
	 * has been emitted on that CPU (default kernel ftrace behaviour).
	 */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  func1() {", out);

	/* First context switch on CPU 0: Task 1 switches to <idle>-0 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task1-100   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);

	/* Idle task executes on CPU 0 - should NOT inherit func1's stack */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  default_idle() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  } /* default_idle */", out);

	/* Context switch to Task 2 (PID 200) on CPU 0 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    <idle>-0    =>    task2-200 ", out);
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

	/*
	 * Test delayed context switch banner migration where a task state
	 * allocated on CPU 3 is rekeyed from CPU 2.
	 */
	ftrace_process_fgraph_line(&ftrace, " 2)               |  func3() {", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)    <idle>-0    =>    task3-300 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    task3-300   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |    func4() {", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |    } /* func4 */", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |  } /* func3 */", out);

	/*
	 * Test delayed context switch banner migration where CPU 3 enters func6()
	 * BEFORE CPU 2 emits its delayed context switch banner. ts from CPU 2
	 * must be merged underneath existing on CPU 3.
	 */
	ftrace_process_fgraph_line(&ftrace, " 2)               |  func5() {", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)    <idle>-0    =>    task4-400 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |    func6() {", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    task4-400   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |    } /* func6 */", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |  } /* func5 */", out);

	/*
	 * Test single-task profiling across CPU migration where context switch
	 * banners are suppressed by the kernel (verif_pid prev_pid == -1).
	 */
	ftrace.evlist = evlist__new();
	if (ftrace.evlist) {
		threads = thread_map__new_by_tid(500);
		perf_evlist__set_maps(evlist__core(ftrace.evlist), NULL, threads);
		perf_thread_map__put(threads);
	}

	ftrace_cpu_states__clear();
	ftrace_process_fgraph_line(&ftrace, " 0)               |  func_a() {", out);
	ftrace_process_fgraph_line(&ftrace, " 1)               |    func_b() {", out);
	ftrace_process_fgraph_line(&ftrace, " 1)               |    } /* func_b */", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  } /* func_a */", out);

	evlist__put(ftrace.evlist);
	ftrace.evlist = NULL;

	fclose(out);
	ftrace_cpu_states__clear();

	/* Verify default_idle in Idle task started at base indentation */
	TEST_ASSERT_VAL("idle task does not inherit initial task call stack",
			strstr(out_buf, "|  default_idle() {") != NULL);

	/* Verify func2 in Task 2 started at base indentation "|  func2() {" */
	TEST_ASSERT_VAL("task2 does not inherit task1 call stack",
			strstr(out_buf, "|  func2() {") != NULL);

	/* Verify task1 exit on CPU 1 matches cleanly */
	TEST_ASSERT_VAL("task1 preserved stack across migration",
			strstr(out_buf, "1)               |  } /* func1 */") != NULL);

	/* Verify task3 preserved stack across delayed context switch banner */
	TEST_ASSERT_VAL("task3 preserved stack across delayed banner",
			strstr(out_buf, "3)               |  } /* func3 */") != NULL);

	/* Verify task4 merged outer stack when entered before delayed banner */
	TEST_ASSERT_VAL("task4 merged outer stack when entered before delayed banner",
			strstr(out_buf, "3)               |  } /* func5 */") != NULL);

	/* Verify single-task stack tracked across CPUs without banners */
	TEST_ASSERT_VAL("single-task stack tracked across CPUs without banners",
			strstr(out_buf, "0)               |  } /* func_a */") != NULL);

	/*
	 * Test migration where merged stack depth exceeds FTRACE_MAX_STACK
	 * does not cause indentation drift or corruption.
	 * CPU 2 starts without a context switch banner, accumulating under
	 * FTRACE_UNKNOWN_PID(2).
	 */
	free(out_buf);
	out_buf = NULL;
	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	for (i = 0; i < 200; i++)
		feed_fgraph_entry(&ftrace, 2, i, "deep_func", NULL, out);

	/* Migrate to CPU 3, enter more frames */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)    <idle>-0    =>    task5-500 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	for (i = 0; i < 100; i++)
		feed_fgraph_entry(&ftrace, 3, 200 + i, "more_func", NULL, out);

	/* Delayed context switch banner from CPU 2 rekeys and merges stacks (> 256) */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    task5-500   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);

	/* Unwind all frames on CPU 3 (44 in overflow_depth + 256 on stack = 300) */
	for (i = 299; i >= 0; i--)
		feed_fgraph_exit(&ftrace, 3, i, NULL, out);
	feed_fgraph_entry(&ftrace, 3, 0, "after_merge", NULL, out);

	fclose(out);
	ftrace_cpu_states__clear();

	TEST_ASSERT_VAL("no indentation drift after saturated stack rekey",
			strstr(out_buf, "|  after_merge() {") != NULL);

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
