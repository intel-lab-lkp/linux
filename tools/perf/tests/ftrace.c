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
	static const char line_verbose_nodur_entry[] =
		" 1234.567890 |   0)   bash-1234    |  wake_up_new_task() {";
	static const char line_verbose_nodur_enqueue[] =
		" 1234.567890 |   0)   bash-1234    |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */";
	static const char line_verbose_nodur_exit_enqueue[] =
		" 1234.567890 |   0)   bash-1234    |    } /* enqueue_task */";
	static const char line_verbose_nodur_exit_task[] =
		" 1234.567890 |   0)   bash-1234    |  } /* wake_up_new_task */";
	static const char line_verbose_nodur_inline_entry[] =
		" 1234.567890 |   0)   bash-1234    |    activate_task() { /* (inline) */";
	static const char line_verbose_nodur_inline_exit[] =
		" 1234.567890 |   0)   bash-1234    |    } /* activate_task (inline) */";
	struct perf_ftrace ftrace;
	char *out_buf = NULL;
	size_t out_len = 0;
	FILE *out;
	const char *unmatched_pos;
	const char *inline_exit_pos;
	int ret = TEST_OK;

	memset(&ftrace, 0, sizeof(ftrace));
	ftrace.use_inline = true;
	ftrace.filter_retaddr = 1; /* Default when --graph-opts retaddr is not specified */
	ftrace.tracer = "function_graph";

	/* If vmlinux is present, test resolving symbols */
	if (access("vmlinux", R_OK) == 0) {
		symbol_conf.vmlinux_name = "vmlinux";
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

		/* Test verbose noduration prefix formatting for synthesised inlines */
		ftrace.graph_verbose = true;
		ftrace.graph_noduration = true;
		out = open_memstream(&out_buf, &out_len);
		if (!out) {
			ret = TEST_FAIL;
			goto out_cleanup;
		}

		ftrace_process_fgraph_line(&ftrace, line_verbose_nodur_entry, out);
		ftrace_process_fgraph_line(&ftrace, line_verbose_nodur_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_verbose_nodur_exit_enqueue, out);
		ftrace_process_fgraph_line(&ftrace, line_verbose_nodur_exit_task, out);

		fclose(out);
		ftrace_cpu_states__clear();

		TEST_CHECK("verbose noduration inline entry preserves comm-pid",
			   strstr(out_buf, line_verbose_nodur_inline_entry) != NULL);
		TEST_CHECK("verbose noduration inline exit preserves comm-pid",
			   strstr(out_buf, line_verbose_nodur_inline_exit) != NULL);

		free(out_buf);
		out_buf = NULL;
		ftrace.graph_verbose = false;
		ftrace.graph_noduration = false;
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
	return TEST_OK;
}

static int test_column_layouts(void)
{
	static const struct {
		const char *desc;
		bool noduration;
		bool verbose;
		const char *in;
		const char *expected;
	} cases[] = {
		{
			"default layout", false, false,
			" 7)               |  wake_up_new_task() { /* <-kernel_clone+0x160/0x510 */",
			" 7)               |  wake_up_new_task() {\n",
		},
		{
			"noduration layout", true, false,
			" 7)   housekeeping_any_cpu(); /* <-ring_buffer_unlock_commit+0x1a0/0x1f0 */",
			" 7)   housekeeping_any_cpu();\n",
		},
		{
			"verbose layout", false, true,
			" 1234.567890 |   7)   bash-1234    |               |  wake_up_new_task() { /* <-kernel_clone+0x160/0x510 */",
			" 1234.567890 |   7)   bash-1234    |               |  wake_up_new_task() {\n",
		},
		{
			"verbose noduration layout", true, true,
			" 1234.567890 |   7)   bash-1234    |  wake_up_new_task() { /* <-kernel_clone+0x160/0x510 */",
			" 1234.567890 |   7)   bash-1234    |  wake_up_new_task() {\n",
		},
		{
			"noduration context switch marker", true, false,
			"   7)  bash-1234    =>  sh-5678",
			"   7)  bash-1234    =>  sh-5678\n",
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
		ftrace.graph_noduration = cases[i].noduration;
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

static int test_graph_target_filter(void)
{
	static const char line_idle[] = " 7)   3.000 us    |  sched_idle_set_state();";
	static const char line_sw_entry[] = " 6)               |  finish_task_switch.isra.0() {";
	static const char line_sw_child[] = " 6)   1.000 us    |    _raw_spin_unlock();";
	static const char line_sw_exit[] = " 6)   2.000 us    |  }";
	static const char line_tgt_entry[] = " 2)               |  activate_task() {";
	static const char line_tgt_child[] = " 2)   1.500 us    |    enqueue_task();";
	static const char line_tgt_exit[] = " 2)   2.500 us    |  }";
	struct perf_ftrace ftrace;
	struct filter_entry *entry;
	struct filter_entry *pos, *tmp;
	char *out_buf = NULL;
	size_t out_len = 0;
	FILE *out;
	int ret = TEST_OK;
	int i;

	memset(&ftrace, 0, sizeof(ftrace));
	INIT_LIST_HEAD(&ftrace.graph_funcs);

	entry = malloc(sizeof(*entry) + strlen("activate_task") + 1);
	if (!entry)
		return TEST_FAIL;
	strcpy(entry->name, "activate_task");
	list_add_tail(&entry->list, &ftrace.graph_funcs);

	/* 1. Test without --inline */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace, line_idle, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_child, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_exit, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_child, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_exit, out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "sched_idle_set_state")) {
		pr_debug("FAILED: sched_idle_set_state not suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "finish_task_switch")) {
		pr_debug("FAILED: finish_task_switch not suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "_raw_spin_unlock")) {
		pr_debug("FAILED: _raw_spin_unlock not suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: enqueue_task child missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 2. Test with --inline */
	ftrace.use_inline = true;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace, line_idle, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_child, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_exit, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_child, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_exit, out);

	fclose(out);
	ftrace_cpu_states__clear();

	pr_debug("Target filter output with --inline:\n%s\n", out_buf);

	if (strstr(out_buf, "finish_task_switch")) {
		pr_debug("FAILED: finish_task_switch not suppressed with --inline\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing with --inline\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 3. Test suppressing detached orphan calls within target graph */
	ftrace.use_inline = true;
	ftrace.filter_retaddr = 1;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace, line_tgt_entry, out);
	/* Leaf orphan call from ring_buffer_unlock_commit */
	ftrace_process_fgraph_line(&ftrace,
		" 2)               |    housekeeping_any_cpu(); /* <-ring_buffer_unlock_commit+0x1a0/0x1f0 */",
		out);
	/* Non-leaf orphan call from irq_work_queue_on */
	ftrace_process_fgraph_line(&ftrace,
		" 2)               |    arch_irq_work_raise() { /* <-irq_work_queue_on+0x81/0xa0 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 2)               |      apic_wait_icr_idle() { /* (inline) */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 2)               |        x2apic_send_IPI_self();",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 2)               |      } /* apic_wait_icr_idle (inline) */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 2)   1.000 us    |    }",
		out);
	/* Genuine child call from activate_task */
	ftrace_process_fgraph_line(&ftrace,
		" 2)               |    enqueue_task() { /* <-activate_task+0x1d1/0x3e0 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 2)   1.500 us    |    }",
		out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_exit, out);

	fclose(out);
	ftrace_cpu_states__clear();

	pr_debug("Orphan call filter output:\n%s\n", out_buf);

	if (strstr(out_buf, "housekeeping_any_cpu")) {
		pr_debug("FAILED: housekeeping_any_cpu orphan not suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "arch_irq_work_raise")) {
		pr_debug("FAILED: arch_irq_work_raise orphan not suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "x2apic_send_IPI_self")) {
		pr_debug("FAILED: x2apic_send_IPI_self not suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task() {")) {
		pr_debug("FAILED: genuine enqueue_task missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 4. Test targeted leaf call at depth > 0 resetting filter */
	ftrace.use_inline = false;
	list_for_each_entry_safe(pos, tmp, &ftrace.graph_funcs, list) {
		list_del(&pos->list);
		free(pos);
	}
	entry = malloc(sizeof(*entry) + strlen("enqueue_task") + 1);
	if (!entry) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	strcpy(entry->name, "enqueue_task");
	list_add_tail(&entry->list, &ftrace.graph_funcs);

	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace, line_sw_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_child, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_child, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_exit, out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "finish_task_switch")) {
		pr_debug("FAILED: finish_task_switch not suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: targeted leaf enqueue_task missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "_raw_spin_unlock")) {
		pr_debug("FAILED: sibling _raw_spin_unlock not suppressed after leaf target\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 5. Test targeted leaf call at depth > 0 with inlines resetting filter */
	ftrace.use_inline = true;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace, line_sw_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_child, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_child, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_exit, out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "finish_task_switch")) {
		pr_debug("FAILED: finish_task_switch not suppressed with inlines\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: targeted leaf enqueue_task missing with inlines\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "_raw_spin_unlock")) {
		pr_debug("FAILED: sibling _raw_spin_unlock leaked with inlines\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 6. Test targeted leaf call with synthesized DWARF inlines closing and resetting filter */
	if (access("vmlinux", R_OK) == 0) {
		symbol_conf.vmlinux_name = "vmlinux";
		symbol_conf.ignore_vmlinux_buildid = true;
		if (perf_ftrace__setup_inlines(&ftrace) == 0 && ftrace.machine) {
			out = open_memstream(&out_buf, &out_len);
			if (!out) {
				perf_ftrace__cleanup_inlines(&ftrace);
				ret = TEST_FAIL;
				goto out_free_filter;
			}

			ftrace_process_fgraph_line(&ftrace,
				" 0)               |  wake_up_new_task() {", out);
			/* Targeted leaf with caller having inline activate_task */
			ftrace_process_fgraph_line(&ftrace,
				" 0)   1.500 us    |    enqueue_task(); /* <-wake_up_new_task+0x1d1/0x3e0 */",
				out);
			/* Sibling call after targeted leaf */
			ftrace_process_fgraph_line(&ftrace,
				" 0)   1.000 us    |    check_preempt_curr(); /* <-wake_up_new_task+0x0/0x3e0 */",
				out);
			ftrace_process_fgraph_line(&ftrace,
				" 0)   2.500 us    |  } /* wake_up_new_task */", out);

			fclose(out);
			ftrace_cpu_states__clear();
			perf_ftrace__cleanup_inlines(&ftrace);

			if (!strstr(out_buf, "enqueue_task();")) {
				pr_debug("FAILED: targeted leaf enqueue_task missing\n");
				ret = TEST_FAIL;
				goto out_free_filter;
			}
			if (!strstr(out_buf, "} /* activate_task (inline) */")) {
				pr_debug("FAILED: inline activate_task not closed after leaf target\n");
				ret = TEST_FAIL;
				goto out_free_filter;
			}
			if (strstr(out_buf, "check_preempt_curr")) {
				pr_debug("FAILED: sibling check_preempt_curr leaked after leaf target\n");
				ret = TEST_FAIL;
				goto out_free_filter;
			}

			free(out_buf);
			out_buf = NULL;
		}
	}

	/* 7. Test recovery when an orphan exit event is dropped by the ring buffer */
	list_for_each_entry_safe(pos, tmp, &ftrace.graph_funcs, list) {
		list_del(&pos->list);
		free(pos);
	}
	entry = malloc(sizeof(*entry) + strlen("activate_task") + 1);
	if (!entry) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	strcpy(entry->name, "activate_task");
	list_add_tail(&entry->list, &ftrace.graph_funcs);

	/* 7a. Method 1: Target function exits directly after a dropped orphan exit */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Orphan entry: housekeeping_any_cpu called from outside activate_task */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    housekeeping_any_cpu() { /* <-rb_wakeups+0x10 */",
		out);
	/* Exit of housekeeping_any_cpu is DROPPED by ring buffer (never arrives) */
	/* Target function exits with named closing brace */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.500 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: target activate_task entry missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "housekeeping_any_cpu")) {
		pr_debug("FAILED: orphan housekeeping_any_cpu leaked into output\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: target activate_task exit swallowed after orphan exit dropped\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 7b. Method 2: Target function calls legitimate child after a dropped orphan exit */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Orphan entry: housekeeping_any_cpu called from outside activate_task */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    housekeeping_any_cpu() { /* <-rb_wakeups+0x10 */",
		out);
	/* Exit of housekeeping_any_cpu is DROPPED by ring buffer */
	/* Legitimate child call from activate_task */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.200 us    |    enqueue_task(); /* <-activate_task+0x1d1/0x3e0 */",
		out);
	/* Target function exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.500 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "housekeeping_any_cpu")) {
		pr_debug("FAILED: orphan housekeeping_any_cpu leaked into output\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: legitimate child enqueue_task swallowed after orphan exit dropped\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: target activate_task exit missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/*
	 * 8. Test target_root_depth synchronisation when target root enters
	 * after dropped sibling exit.
	 */
	/* 8a. Without --inline */
	ftrace.use_inline = false;
	ftrace.filter_retaddr = 1;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Caller at depth 0 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  wake_up_new_task() {", out);
	/* Extraneous sibling at depth 1 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    stale_sibling() {", out);
	/* Exit of stale_sibling is DROPPED by ring buffer */
	/* Target function enters with caller wake_up_new_task */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-wake_up_new_task+0x10 */", out);
	/* First child of target enters and exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      enqueue_task() { /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      }", out);
	/* Second child of target enters and exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |      check_preempt_curr(); /* <-activate_task+0x30 */", out);
	/* Target function exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.500 us    |    } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "stale_sibling")) {
		pr_debug("FAILED: stale_sibling leaked into output (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task() {")) {
		pr_debug("FAILED: first child enqueue_task missing (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "check_preempt_curr();")) {
		pr_debug("FAILED: second child check_preempt_curr missing (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: target activate_task exit missing (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 8b. With --inline */
	ftrace.use_inline = true;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Caller at depth 0 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  wake_up_new_task() {", out);
	/* Extraneous sibling at depth 1 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    stale_sibling() {", out);
	/* Exit of stale_sibling is DROPPED by ring buffer */
	/* Target function enters with caller wake_up_new_task */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-wake_up_new_task+0x10 */", out);
	/* First child of target enters and exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      enqueue_task() { /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      }", out);
	/* Second child of target enters and exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |      check_preempt_curr(); /* <-activate_task+0x30 */", out);
	/* Target function exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.500 us    |    } /* activate_task */", out);
	/* Sibling call after target function */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    after_target(); /* <-wake_up_new_task+0x40 */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "stale_sibling")) {
		pr_debug("FAILED: stale_sibling leaked into output (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task() {")) {
		pr_debug("FAILED: first child enqueue_task missing (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "check_preempt_curr();")) {
		pr_debug("FAILED: second child check_preempt_curr missing (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: target activate_task exit missing (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "after_target")) {
		pr_debug("FAILED: after_target leaked into output (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/*
	 * 9. Test deep legitimate subtrees when stack depth exceeds
	 * FTRACE_MAX_STACK (overflow_depth > 0).
	 */
	/* 9a. Without --inline */
	ftrace.use_inline = false;
	ftrace.filter_retaddr = 1;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	for (i = 1; i < 256; i++) {
		ftrace_process_fgraph_line(&ftrace,
			" 0)               |    sub_func() { /* <-activate_task+0x10 */", out);
	}
	/* Push frames beyond FTRACE_MAX_STACK (256) */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    overflow_parent() { /* <-sub_func+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      overflow_child() { /* <-overflow_parent+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        overflow_leaf(); /* <-overflow_child+0x30 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.200 us    |      }", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.400 us    |    }", out);
	for (i = 1; i < 256; i++) {
		ftrace_process_fgraph_line(&ftrace,
			" 0)   1.000 us    |    }", out);
	}
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.500 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "overflow_parent() {")) {
		pr_debug("FAILED: overflow_parent entry missing (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "overflow_child() {")) {
		pr_debug("FAILED: overflow_child entry missing (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "overflow_leaf();")) {
		pr_debug("FAILED: overflow_leaf missing (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 9b. With --inline */
	ftrace.use_inline = true;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	for (i = 1; i < 256; i++) {
		ftrace_process_fgraph_line(&ftrace,
			" 0)               |    sub_func() { /* <-activate_task+0x10 */", out);
	}
	/* Push frames beyond FTRACE_MAX_STACK (256) */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    overflow_parent() { /* <-sub_func+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      overflow_child() { /* <-overflow_parent+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        overflow_leaf(); /* <-overflow_child+0x30 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.200 us    |      }", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.400 us    |    }", out);
	for (i = 1; i < 256; i++) {
		ftrace_process_fgraph_line(&ftrace,
			" 0)   1.000 us    |    }", out);
	}
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.500 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "overflow_parent() {")) {
		pr_debug("FAILED: overflow_parent entry missing (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "overflow_child() {")) {
		pr_debug("FAILED: overflow_child entry missing (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "overflow_leaf();")) {
		pr_debug("FAILED: overflow_leaf missing (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 9. Test glob wildcard filter (-G 'activate_*') */
	list_for_each_entry_safe(pos, tmp, &ftrace.graph_funcs, list) {
		list_del(&pos->list);
		free(pos);
	}
	entry = malloc(sizeof(*entry) + strlen("activate_*") + 1);
	if (!entry) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	strcpy(entry->name, "activate_*");
	list_add_tail(&entry->list, &ftrace.graph_funcs);

	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace, line_idle, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_exit, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_child, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_exit, out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "finish_task_switch")) {
		pr_debug("FAILED: finish_task_switch leaked with wildcard\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: wildcard target activate_* missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: wildcard child enqueue_task missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 10. Test wildcard filter matching compiler clone with suffix (-G '*task_switch') */
	list_for_each_entry_safe(pos, tmp, &ftrace.graph_funcs, list) {
		list_del(&pos->list);
		free(pos);
	}
	entry = malloc(sizeof(*entry) + strlen("*task_switch") + 1);
	if (!entry) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	strcpy(entry->name, "*task_switch");
	list_add_tail(&entry->list, &ftrace.graph_funcs);

	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace, line_idle, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_child, out);
	ftrace_process_fgraph_line(&ftrace, line_sw_exit, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_entry, out);
	ftrace_process_fgraph_line(&ftrace, line_tgt_exit, out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "finish_task_switch.isra.0() {")) {
		pr_debug("FAILED: wildcard target *task_switch did not match clone\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "_raw_spin_unlock();")) {
		pr_debug("FAILED: child _raw_spin_unlock missing under clone target\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "activate_task")) {
		pr_debug("FAILED: unrelated activate_task leaked under *task_switch\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

out_free_filter:
	if (out_buf)
		free(out_buf);
	list_for_each_entry_safe(pos, tmp, &ftrace.graph_funcs, list) {
		list_del(&pos->list);
		free(pos);
	}
	return ret;
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

	ret = test_column_layouts();
	if (ret != TEST_OK)
		return ret;

	ret = test_graph_target_filter();
	if (ret != TEST_OK)
		return ret;

	ret = test_inline_processing();
	if (ret != TEST_OK)
		return ret;

	return TEST_OK;
}

DEFINE_SUITE("Ftrace inline processing", ftrace);
