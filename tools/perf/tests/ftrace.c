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
#include "srcline.h"
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

static void ensure_synthetic_inline(struct machine *machine,
				    const char *caller, u64 offset,
				    const char *inline_name)
{
	struct map *kmap = NULL;
	struct symbol *sym;

	if (!machine || !offset)
		return;

	sym = machine__find_kernel_symbol_by_name(machine, caller, &kmap);
	if (sym && kmap) {
		struct dso *dso = map__dso(kmap);
		u64 ip = sym->start + offset - 1;
		u64 addr = map__rip_2objdump(kmap, ip);
		struct inline_node *node;

		node = inlines__tree_find(dso__inlined_nodes(dso), addr);
		if (!node) {
			struct symbol *isym = new_inline_sym(dso, sym, inline_name);

			node = zalloc(sizeof(*node));
			if (node && isym) {
				INIT_LIST_HEAD(&node->val);
				node->addr = addr;
				if (inline_list__append(isym, NULL, node) != 0) {
					symbol__delete(isym);
					free(node);
				} else {
					inlines__tree_insert(dso__inlined_nodes(dso), node);
				}
			} else {
				if (isym)
					symbol__delete(isym);
				free(node);
			}
		} else {
			struct inline_list *pos, *outer = NULL;

			list_for_each_entry_reverse(pos, &node->val, list) {
				if (pos->symbol && symbol__inlined(pos->symbol)) {
					outer = pos;
					break;
				}
			}
			if (!outer || strcmp(outer->symbol->name, inline_name)) {
				struct symbol *isym = new_inline_sym(dso, sym, inline_name);

				if (isym) {
					inline_node__clear_frames(node);
					if (inline_list__append(isym, NULL, node) != 0)
						symbol__delete(isym);
				}
			}
		}
		map__put(kmap);
	}
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
	static const char line_verbose_nodur_entry[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |  wake_up_new_task() {";
	static const char line_verbose_nodur_enqueue[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */";
	static const char line_verbose_nodur_exit_enqueue[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |    } /* enqueue_task */";
	static const char line_verbose_nodur_exit_task[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |  } /* wake_up_new_task */";
	static const char line_verbose_nodur_inline_entry[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |    activate_task() { /* (inline) */";
	static const char line_verbose_nodur_inline_exit[] =
		" 1234.567890 |   0)   bash-1234    |  ....   |    } /* activate_task (inline) */";
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
	/* By default, graph_retaddr is 0 (return address comments are filtered) */
	ftrace.tracer = "function_graph";

	vmlinux = vmlinux_path__find();
	if (!vmlinux)
		return TEST_SKIP;

	/* If vmlinux is present, test resolving symbols */
	{
		const char *inlined_names[FTRACE_MAX_INLINES];
		int num_inlines;

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
			pr_debug("vmlinux does not inline activate_task at wake_up_new_task+0x1d1; falling back to synthetic inline\n");
			ensure_synthetic_inline(ftrace.machine, "wake_up_new_task", 0x1d1,
						"activate_task");
			num_inlines = ftrace_resolve_inlines(ftrace.machine,
							     "wake_up_new_task", 0x1d1,
							     inlined_names,
							     FTRACE_MAX_INLINES);
			if (num_inlines <= 0 || strcmp(inlined_names[0], "activate_task") != 0) {
				ret = TEST_SKIP;
				goto out_cleanup;
			}
		}

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

		/* Now test with graph_retaddr = 1 (user specified --graph-opts retaddr) */
		ftrace.graph_retaddr = 1;
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

		/* Verify return address comment IS preserved when graph_retaddr = 1 */
		TEST_CHECK("retaddr comment preserved when requested",
			   strstr(out_buf, "/* <-wake_up_new_task") != NULL);

		free(out_buf);
		out_buf = NULL;

		/* Test with bare closing braces (funcgraph-tail disabled by default) */
		ftrace.graph_retaddr = 0;
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
			   strstr(out_buf, "|  }\n") != NULL);

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
			   strstr(out_buf, "|    irq_handler() {") != NULL);

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
			   strstr(out_buf, "|  schedule() {") != NULL);

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

	/* Push 255 more frames to fill initial FTRACE_MAX_STACK (256) */
	for (i = 1; i < 256; i++)
		feed_fgraph_entry(&ftrace, 0, i, "sub_func", NULL, out);

	/* Push 44 additional frames triggering dynamic stack expansion beyond 256 */
	for (i = 0; i < 44; i++)
		feed_fgraph_entry(&ftrace, 0, 256 + i, "overflow_func", NULL, out);

	/* Pop the 44 expanded frames with bare closing braces */
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

	/* Test call depth recovery when events are dropped on deep stack */
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

	/* Test that bare braces on deep stack do not prematurely pop tracked frames */
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

	/* Test dropped inner recursive entry does not prematurely unwind outer caller */
	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	/* Outer recursive invocation at indent 2 */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  rec_func() {", out);
	/* Inner recursive entry at indent 4 is DROPPED by ring buffer */
	/* Inner recursive invocation exits at indent 4 */
	ftrace_process_fgraph_line(&ftrace, " 0)   1.000 us    |    } /* rec_func */", out);
	/* Outer invocation is still active; calls child_func at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
				   " 0)   1.000 us    |    child_func(); /* <-rec_func+0x20 */",
				   out);
	/* Outer invocation exits at indent 2 */
	ftrace_process_fgraph_line(&ftrace, " 0)   2.000 us    |  } /* rec_func */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "child_func();")) {
		pr_debug("FAILED %s:%d child_func missing after dropped inner recursive entry\n",
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

		symbol_conf.vmlinux_name = vmlinux;
		symbol_conf.ignore_vmlinux_buildid = true;
		if (perf_ftrace__setup_inlines(&ftrace) == 0 && ftrace.machine) {
			ensure_synthetic_inline(ftrace.machine, "wake_up_new_task", 0x1d1,
						"activate_task");
			num_inlines = ftrace_resolve_inlines(ftrace.machine,
							     "wake_up_new_task", 0x1d1,
							     inlined_names,
							     FTRACE_MAX_INLINES);
			if (num_inlines > 0 && !strcmp(inlined_names[0], "activate_task")) {
				out = open_memstream(&out_buf, &out_len);
				if (!out) {
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
				 * (cs->depth = 256), and real frame enqueue_task dynamically
				 * grows the stack capacity beyond 256.
				 */
				feed_fgraph_entry(&ftrace, 0, 255, "enqueue_task",
						  "wake_up_new_task+0x1d1/0x3e0", out);

				/*
				 * Nested child call on deep stack: does NOT pop the
				 * inlined frame!
				 */
				feed_fgraph_entry(&ftrace, 0, 256, "child_overflow",
						  "enqueue_task+0x10", out);

				/* Exit child_overflow */
				feed_fgraph_exit(&ftrace, 0, 256, NULL, out);

				/* Exit enqueue_task */
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
					perf_ftrace__cleanup_inlines(&ftrace);
					symbol_conf.vmlinux_name = saved_vmlinux;
					symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
					return TEST_FAIL;
				}

				free(out_buf);
				out_buf = NULL;
			}
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
			" 1234.567890 |   7)   bash-1234    |  ....   |               |  wake_up_new_task() { /* <-kernel_clone+0x160/0x510 */",
			" 1234.567890 |   7)   bash-1234    |  ....   |               |  wake_up_new_task() {\n",
		},
		{
			"verbose noduration layout", true, true,
			" 1234.567890 |   7)   bash-1234    |  ....   |  wake_up_new_task() { /* <-kernel_clone+0x160/0x510 */",
			" 1234.567890 |   7)   bash-1234    |  ....   |  wake_up_new_task() {\n",
		},
		{
			"noduration context switch marker", true, false,
			"   7)  bash-1234    =>  sh-5678",
			"   7)  bash-1234    =>  sh-5678\n",
		},
		{
			"noduration irq entry marker", true, false,
			" 7) ==========>",
			" 7) ==========>\n",
		},
		{
			"noduration irq exit marker", true, false,
			" 7) <==========",
			" 7) <==========\n",
		},
		{
			"verbose noduration irq entry marker", true, true,
			" 1234.567890 |   7)   bash-1234    |  ....   | ==========>",
			" 1234.567890 |   7)   bash-1234    |  ....   | ==========>\n",
		},
		{
			"verbose noduration irq exit marker", true, true,
			" 1234.567890 |   7)   bash-1234    |  ....   | <==========",
			" 1234.567890 |   7)   bash-1234    |  ....   | <==========\n",
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

	/* Test noduration IRQ markers do not pop active frames from tracked stack */
	memset(&ftrace, 0, sizeof(ftrace));
	ftrace.use_inline = true;
	ftrace.graph_noduration = true;
	ftrace_cpu_states__clear();

	out = open_memstream(&out_buf, &out_len);
	if (!out)
		return TEST_FAIL;

	ftrace_process_fgraph_line(&ftrace, " 0)  root_func() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ==========>", out);
	ftrace_process_fgraph_line(&ftrace, " 0) <==========", out);
	ftrace_process_fgraph_line(&ftrace, " 0)  } /* root_func */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	TEST_ASSERT_VAL("noduration irq markers preserved stack",
			strstr(out_buf, " 0)  } /* root_func */\n") != NULL);
	free(out_buf);

	return TEST_OK;
}

#define TEST_CS_ASSERT(text, cond)					\
do {									\
	if (!(cond)) {							\
		pr_debug("FAILED %s:%d %s\n", __FILE__, __LINE__, text); \
		ret = TEST_FAIL;					\
		goto out_cleanup;					\
	}								\
} while (0)

static int test_context_switch_handling(void)
{
	struct perf_ftrace ftrace;
	struct perf_thread_map *threads;
	const char *saved_vmlinux = symbol_conf.vmlinux_name;
	bool saved_ignore_buildid = symbol_conf.ignore_vmlinux_buildid;
	char *out_buf = NULL;
	size_t out_len = 0;
	const char *vmlinux;
	struct map *kmap = NULL;
	struct symbol *sym;
	FILE *out;
	int i;
	int ret = TEST_OK;

	vmlinux = vmlinux_path__find();
	if (!vmlinux)
		return TEST_SKIP;

	memset(&ftrace, 0, sizeof(ftrace));
	ftrace.use_inline = true;
	symbol_conf.vmlinux_name = vmlinux;
	symbol_conf.ignore_vmlinux_buildid = true;
	if (perf_ftrace__setup_inlines(&ftrace) < 0 || !ftrace.machine) {
		ret = TEST_SKIP;
		goto out_cleanup_conf;
	}

	sym = machine__find_kernel_symbol_by_name(ftrace.machine, "wake_up_new_task", &kmap);
	if (!sym || !kmap) {
		ret = TEST_SKIP;
		goto out_cleanup;
	}
	map__put(kmap);

	ensure_synthetic_inline(ftrace.machine, "wake_up_new_task", 0x1d1,
				"activate_task");

	ftrace_cpu_states__clear();

	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_cleanup;
	}

	/*
	 * Initial Task 1 (PID 100) starts running on CPU 0 before any banner
	 * has been emitted on that CPU (default kernel ftrace behaviour).
	 * Frame wake_up_new_task at offset 0x1d1 resolves inline activate_task.
	 */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  wake_up_new_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
				   " 0)               |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */",
				   out);

	/* First context switch on CPU 0: Task 1 switches to <idle>-0 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task1-100   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);

	/* Idle task executes on CPU 0 - must NOT inherit Task 1's inlined call stack */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  default_idle() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  } /* default_idle */", out);

	/* Context switch to Task 2 (PID 200) on CPU 0 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    <idle>-0    =>    task2-200 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);

	/* Task 2 executes func2 - must NOT inherit Task 1's inlined call stack */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  func2() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  } /* func2 */", out);

	/* Task 1 migrates and resumes on CPU 1 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    <idle>-0    =>    task1-100 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);

	/* Task 1 exits enqueue_task, then wake_up_new_task on CPU 1 */
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |    } /* enqueue_task */", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   2.000 us    |  } /* wake_up_new_task */", out);

	/*
	 * Test delayed context switch banner migration where a task state
	 * with an inlined frame on CPU 2 is rekeyed from CPU 2 to CPU 3.
	 */
	ftrace_process_fgraph_line(&ftrace, " 2)               |  wake_up_new_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
				   " 2)               |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */",
				   out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)    <idle>-0    =>    task3-300 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    task3-300   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |      func4() {", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |      } /* func4 */", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |    } /* enqueue_task */", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |  } /* wake_up_new_task */", out);

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

	/*
	 * Test migration where task with active outer frame on CPU 0 migrates
	 * to CPU 1 without an initial context switch banner, accumulating inner
	 * frames in ts under FTRACE_UNKNOWN_PID(1). Rekeying must append inner
	 * frames to existing stack rather than inverting the stack.
	 */
	ftrace_cpu_states__clear();
	ftrace_process_fgraph_line(&ftrace, " 0)               |  caller_outer() {", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task7-700   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	/* Migrates to CPU 1 without banner; inner frame called */
	ftrace_process_fgraph_line(&ftrace, " 1)               |    callee_inner() {", out);
	/* Banner on CPU 1 rekeys CPU 1 to task7 */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    task7-700   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	/* Resumes on CPU 0: callee_inner exits, followed by caller_outer */
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    <idle>-0    =>    task7-700 ", out);
	ftrace_process_fgraph_line(&ftrace, " ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |    } /* callee_inner */", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  } /* caller_outer */", out);

	/*
	 * Test task migration where older stack has scheduler frames that already
	 * exited before the task entered a callee on the new CPU.
	 * CPU 0 has caller_outer (indent 2) -> schedule (indent 4) -> __schedule (indent 6).
	 * Resumes on CPU 1 where schedule frames returned and callee_worker (indent 4) runs.
	 * Rekeying must prune schedule and __schedule from older stack rather than
	 * leaving stranded frames between caller_outer and callee_worker.
	 */
	ftrace_cpu_states__clear();
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    <idle>-0    =>    task8-800 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  caller_outer() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |    schedule() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |      __schedule() {", out);
	ftrace_process_fgraph_line(&ftrace, " 1)               |    callee_worker() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task8-800   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    task8-800   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	/* Resumes on CPU 0: bare braces pop callee_worker then caller_outer */
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    <idle>-0    =>    task8-800 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   1.000 us    |    }", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   2.000 us    |  }", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  base_verify() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   1.000 us    |  }", out);

	/*
	 * Test migration where task unwinds to depth 0 on CPU 1 before CPU 0
	 * emits its delayed context switch banner. Rekeying must prune the
	 * already-exited frames from the older stack rather than restoring them.
	 */
	ftrace_cpu_states__clear();
	ftrace_process_fgraph_line(&ftrace, " 0)               |  caller_deep() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |    callee_deep() {", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    <idle>-0    =>    task9-900 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |    } /* callee_deep */", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   2.000 us    |  } /* caller_deep */", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task9-900   =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)               |  fresh_root() {", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |  } /* fresh_root */", out);

	/*
	 * Test migration where older stack has an inlined frame at the top
	 * when rekeying. Pruning must not stall on raw_indent == -1.
	 */
	ftrace_cpu_states__clear();
	ftrace_process_fgraph_line(&ftrace, " 2)               |  wake_up_new_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
				   " 2)               |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */",
				   out);
	ftrace_process_fgraph_line(&ftrace, " 3) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)    <idle>-0    =>    task10-1000 ", out);
	ftrace_process_fgraph_line(&ftrace, " 3) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)   1.000 us    |    } /* enqueue_task */", out);
	ftrace_process_fgraph_line(&ftrace, " 3)               |    other_work() {", out);
	ftrace_process_fgraph_line(&ftrace, " 2) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    task10-1000 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 2) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 3)   1.000 us    |    } /* other_work */", out);
	ftrace_process_fgraph_line(&ftrace, " 3)   2.000 us    |  } /* wake_up_new_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	/* Verify synthetic inlined entry was emitted for Task 1 */
	TEST_CS_ASSERT("task1 emitted inlined activate_task entry",
			strstr(out_buf, "activate_task() { /* (inline) */") != NULL);

	/* Verify default_idle in Idle task started at base indentation without extra indent */
	TEST_CS_ASSERT("idle task starts at base indentation",
			strstr(out_buf, "|  default_idle() {") != NULL);
	TEST_CS_ASSERT("idle task does not inherit extra inlined indent",
			strstr(out_buf, "|    default_idle() {") == NULL);

	/* Verify func2 in Task 2 started at base indentation "|  func2() {" without extra indent */
	TEST_CS_ASSERT("task2 starts at base indentation",
			strstr(out_buf, "|  func2() {") != NULL);
	TEST_CS_ASSERT("task2 does not inherit extra inlined indent",
			strstr(out_buf, "|    func2() {") == NULL);

	/* Verify task1 synthesized inlined exit on CPU 1 */
	TEST_CS_ASSERT("task1 emitted synthesized inlined exit on CPU 1",
			strstr(out_buf,
			       "1)               |    } /* activate_task (inline) */") != NULL);

	/* Verify task3 preserved inlined stack across delayed context switch banner */
	TEST_CS_ASSERT("task3 preserved inlined frame across delayed banner",
			strstr(out_buf,
			       "3)               |    } /* activate_task (inline) */") != NULL);

	/* Verify task4 merged outer stack when entered before delayed banner */
	TEST_CS_ASSERT("task4 merged outer stack when entered before delayed banner",
			strstr(out_buf, "3)               |  } /* func5 */") != NULL);

	/* Verify single-task stack tracked across CPUs without banners */
	TEST_CS_ASSERT("single-task stack tracked across CPUs without banners",
			strstr(out_buf, "0)               |  } /* func_a */") != NULL);

	/* Verify migrated task with unbannered entry appended rather than inverted stack */
	TEST_CS_ASSERT("migrated task unwound inner callee cleanly",
			strstr(out_buf, "0)               |    } /* callee_inner */") != NULL);
	TEST_CS_ASSERT("migrated task unwound outer caller cleanly",
			strstr(out_buf, "0)               |  } /* caller_outer */") != NULL);

	/* Verify pruned scheduler frames allowed clean bare brace unwinding */
	TEST_CS_ASSERT("pruned scheduler frames allowed clean bare brace unwinding",
			strstr(out_buf, "|  base_verify() {") != NULL);

	/* Verify unwound migrated task starts at base indent */
	TEST_CS_ASSERT("unwound migrated task starts at base indent",
		       strstr(out_buf, " 1)               |  fresh_root() {") != NULL);

	/* Verify inlined frame pruned without stalling on migration */
	TEST_CS_ASSERT("inlined frame pruned without stalling on migration",
		       strstr(out_buf, " 3)   1.000 us    |    } /* other_work */") != NULL &&
		       strstr(out_buf, " 3)   2.000 us    |  } /* wake_up_new_task */") != NULL);

	/*
	 * Test migration where merged stack depth exceeds initial FTRACE_MAX_STACK (256)
	 * triggers dynamic stack growth and unwinds without indentation drift or corruption.
	 * CPU 2 starts without a context switch banner, accumulating under
	 * FTRACE_UNKNOWN_PID(2).
	 */
	free(out_buf);
	out_buf = NULL;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_cleanup;
	}

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

	/* Unwind all 300 frames on CPU 3 on dynamically resized stack */
	for (i = 299; i >= 0; i--)
		feed_fgraph_exit(&ftrace, 3, i, NULL, out);
	feed_fgraph_entry(&ftrace, 3, 0, "after_merge", NULL, out);

	fclose(out);
	ftrace_cpu_states__clear();

	TEST_CS_ASSERT("no indentation drift after saturated stack rekey",
			strstr(out_buf, "|  after_merge() {") != NULL);

	/*
	 * Test migration where task exits to depth 0 on CPU 1 (setting min_raw_indent = 2),
	 * migrates to CPU 0 entering outer frames wake_up_new_task (indent 2) -> enqueue_task (indent 4)
	 * with activate_task inlined, migrates back to CPU 1 entering inner callee_c (indent 6).
	 * Delayed rekeying of CPU 0 must not over-prune caller frames using
	 * CPU 1's previous min_raw_indent.
	 */
	free(out_buf);
	out_buf = NULL;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_cleanup;
	}

	ftrace_cpu_states__clear();
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    <idle>-0    =>    task11-1100 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)               |  root_prior() {", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |  } /* root_prior */", out);
	/* Task 11 migrates to CPU 0 without banner, entering active caller frames with inline */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  wake_up_new_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
				   " 0)               |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */",
				   out);
	/* Task 11 migrates back to CPU 1 and enters inner frame */
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    <idle>-0    =>    task11-1100 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)               |      callee_c() {", out);
	/* Delayed banner on CPU 0 triggers rekey */
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task11-1100 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	/* Resumes on CPU 1: callee_c, enqueue_task (synthesizing activate_task exit), wake_up_new_task */
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |      } /* callee_c */", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   2.000 us    |    } /* enqueue_task */", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   3.000 us    |  } /* wake_up_new_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	/* Verify active caller frames preserved despite prior exit min_raw_indent */
	TEST_CS_ASSERT("task11 emitted synthesized inlined exit on CPU 1",
		       strstr(out_buf,
			      " 1)               |    } /* activate_task (inline) */") != NULL);

	/*
	 * Test multi-stage rekeying where CPU 1 unwinds frames to depth 0 first
	 * (recording min_raw_indent = 2) and merges into an existing state,
	 * followed by CPU 0 rekeying its older UNKNOWN_PID state.
	 * The merge into existing must preserve min_raw_indent so CPU 0's older
	 * exited frames are properly pruned rather than resurrected.
	 */
	free(out_buf);
	out_buf = NULL;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_cleanup;
	}

	ftrace_cpu_states__clear();
	/* CPU 0: Task 12 enters frames under UNKNOWN_PID(0) with inline */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  wake_up_new_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
				   " 0)               |    enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */",
				   out);
	/* CPU 1: Task 12 unwinds both frames to depth 0 under UNKNOWN_PID(1) */
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |    } /* enqueue_task */", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   2.000 us    |  } /* wake_up_new_task */", out);
	/* CPU 2: pre-bind an empty state for task 12 */
	ftrace_process_fgraph_line(&ftrace, " 2) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    <idle>-0    =>    task12-1200 ", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    task12-1200 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 2) ------------------------------------------", out);
	/* CPU 1 emits banner first: merges ts1 (depth 0, min_raw_indent 2) into existing */
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    task12-1200 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	/* CPU 0 emits banner second: merges ts0 into existing, must prune exited frames */
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task12-1200 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	/* Task 12 resumes on CPU 1 and runs fresh function: bare braces must not pop stale frames */
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    <idle>-0    =>    task12-1200 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)               |  fresh_task12() {", out);
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |  }", out);

	fclose(out);
	ftrace_cpu_states__clear();

	/* Verify multi-stage rekey preserved min_raw_indent to prune stale exited frames */
	TEST_CS_ASSERT("fresh_task12 starts at base indent without resurrected frames",
		       strstr(out_buf, " 1)               |  fresh_task12() {") != NULL);
	TEST_CS_ASSERT("stale inlined frame not resurrected before fresh_task12",
		       strstr(out_buf, "} /* activate_task (inline) */\n 1)               |  fresh_task12() {") == NULL);

	/*
	 * Test migration where task exits bar() (indent 4) and enters qux() (indent 4)
	 * on unbannered CPU 1, then migrates back to CPU 0 entering child baz() (indent 6)
	 * before CPU 1 emits its context switch banner.
	 * Rekeying of CPU 1 must prune exited bar(), insert qux() between foo()
	 * and baz(), and cleanly unwind baz, qux, and foo on CPU 0.
	 */
	free(out_buf);
	out_buf = NULL;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_cleanup;
	}

	ftrace_cpu_states__clear();
	/* CPU 0: Task 13 enters root (indent 2) -> foo (indent 4) */
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    <idle>-0    =>    task13-1300 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |  root() {", out);
	ftrace_process_fgraph_line(&ftrace, " 0)               |    foo() {", out);
	/*
	 * Task 13 migrates to CPU 1 without banner: exits foo,
	 * enters wake_up_new_task -> enqueue_task.
	 */
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |    } /* foo */", out);
	ftrace_process_fgraph_line(&ftrace, " 1)               |    wake_up_new_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
				   " 1)               |      enqueue_task() { /* <-wake_up_new_task+0x1d1/0x3e0 */",
				   out);
	/* Task 13 migrates back to CPU 0 and enters child baz */
	ftrace_process_fgraph_line(&ftrace, " 0)               |        baz() {", out);
	/* Delayed banner on CPU 1 triggers rekey */
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    task13-1300 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	/*
	 * Task 13 unwinds baz, enqueue_task (synthesizing activate_task),
	 * wake_up_new_task, root on CPU 0.
	 */
	ftrace_process_fgraph_line(&ftrace, " 0)   1.000 us    |        } /* baz */", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   2.000 us    |      } /* enqueue_task */", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   3.000 us    |    } /* wake_up_new_task */", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   4.000 us    |  } /* root */", out);

	fclose(out);
	pr_debug("OUT_BUF:\n%s\n", out_buf);
	ftrace_cpu_states__clear();

	TEST_CS_ASSERT("task13 synthesized inlined exit for enqueue_task",
		       strstr(out_buf, "} /* activate_task (inline) */") != NULL);

out_cleanup:
	if (out_buf)
		free(out_buf);
	perf_ftrace__cleanup_inlines(&ftrace);
out_cleanup_conf:
	symbol_conf.vmlinux_name = saved_vmlinux;
	symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
	return ret;
}
#undef TEST_CS_ASSERT

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
	const char *vmlinux;
	const char *inline_entry, *inline_dup, *inline_exit, *child_exit, *sibling_call;
	const char *first_act, *second_act, *first_enq, *second_enq;
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

	vmlinux = vmlinux_path__find();

	/* 6. Test targeted leaf call with synthesized DWARF inlines closing and resetting filter */
	if (vmlinux) {
		const char *inlined_names[FTRACE_MAX_INLINES];
		int num_inlines;
		const char *saved_vmlinux = symbol_conf.vmlinux_name;
		bool saved_ignore_buildid = symbol_conf.ignore_vmlinux_buildid;

		symbol_conf.vmlinux_name = vmlinux;
		symbol_conf.ignore_vmlinux_buildid = true;
		if (perf_ftrace__setup_inlines(&ftrace) == 0 && ftrace.machine) {
			ensure_synthetic_inline(ftrace.machine, "wake_up_new_task", 0x1d1,
						"activate_task");
			num_inlines = ftrace_resolve_inlines(ftrace.machine,
							     "wake_up_new_task", 0x1d1,
							     inlined_names,
							     FTRACE_MAX_INLINES);
			if (num_inlines > 0 && !strcmp(inlined_names[0], "activate_task")) {
				out = open_memstream(&out_buf, &out_len);
				if (!out) {
					perf_ftrace__cleanup_inlines(&ftrace);
					symbol_conf.vmlinux_name = saved_vmlinux;
					symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
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

				if (!strstr(out_buf, "enqueue_task();")) {
					pr_debug("FAILED: targeted leaf enqueue_task missing\n");
					ret = TEST_FAIL;
				} else if (!strstr(out_buf, "} /* activate_task (inline) */")) {
					pr_debug("FAILED: inline activate_task not closed after leaf target\n");
					ret = TEST_FAIL;
				} else if (strstr(out_buf, "check_preempt_curr")) {
					pr_debug("FAILED: sibling check_preempt_curr leaked after leaf target\n");
					ret = TEST_FAIL;
				}

				free(out_buf);
				out_buf = NULL;

				/*
				 * 6b. Test child function exit inside inlined function does not
				 * prematurely close inlined frame (Issue 1).
				 */
				list_for_each_entry_safe(pos, tmp, &ftrace.graph_funcs, list) {
					list_del(&pos->list);
					free(pos);
				}
				entry = malloc(sizeof(*entry) + strlen("wake_up_new_task") + 1);
				if (!entry) {
					perf_ftrace__cleanup_inlines(&ftrace);
					symbol_conf.vmlinux_name = saved_vmlinux;
					symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
					ret = TEST_FAIL;
					goto out_free_filter;
				}
				strcpy(entry->name, "wake_up_new_task");
				list_add_tail(&entry->list, &ftrace.graph_funcs);

				out = open_memstream(&out_buf, &out_len);
				if (!out) {
					perf_ftrace__cleanup_inlines(&ftrace);
					symbol_conf.vmlinux_name = saved_vmlinux;
					symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
					ret = TEST_FAIL;
					goto out_free_filter;
				}

				ftrace_process_fgraph_line(&ftrace,
					" 0)               |  wake_up_new_task() {", out);
				ftrace_process_fgraph_line(&ftrace,
					" 0)               |    enqueue_task() { /* <-wake_up_new_task+0x1d1 */",
					out);
				ftrace_process_fgraph_line(&ftrace,
					" 0)   1.000 us    |    } /* enqueue_task */", out);
				ftrace_process_fgraph_line(&ftrace,
					" 0)   1.500 us    |    check_preempt_curr(); /* <-wake_up_new_task+0x1d1 */",
					out);
				ftrace_process_fgraph_line(&ftrace,
					" 0)   2.500 us    |  } /* wake_up_new_task */", out);

				fclose(out);
				ftrace_cpu_states__clear();

				inline_entry = strstr(out_buf, "activate_task() { /* (inline) */");
				if (inline_entry)
					inline_dup = strstr(inline_entry + 1,
							    "activate_task() { /* (inline) */");
				else
					inline_dup = NULL;
				inline_exit = strstr(out_buf, "} /* activate_task (inline) */");
				child_exit = strstr(out_buf, "} /* enqueue_task */");
				sibling_call = strstr(out_buf, "check_preempt_curr();");

				if (!inline_entry) {
					pr_debug("FAILED: inline activate_task entry missing\n");
					ret = TEST_FAIL;
				} else if (inline_dup) {
					pr_debug("FAILED: duplicate inline activate_task synthesized\n");
					ret = TEST_FAIL;
				} else if (!inline_exit || !child_exit || !sibling_call) {
					pr_debug("FAILED: expected trace frames missing\n");
					ret = TEST_FAIL;
				} else if (inline_exit < sibling_call) {
					pr_debug("FAILED: inline activate_task closed before sibling call\n");
					ret = TEST_FAIL;
				}

				free(out_buf);
				out_buf = NULL;

				/*
				 * 6c. Test recursive target root function with --inline (Issue 2).
				 * Inner target root exit must not prematurely close inlined frames
				 * or unseat outer target root.
				 */
				out = open_memstream(&out_buf, &out_len);
				if (!out) {
					perf_ftrace__cleanup_inlines(&ftrace);
					symbol_conf.vmlinux_name = saved_vmlinux;
					symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
					ret = TEST_FAIL;
					goto out_free_filter;
				}

				/* Outer target root */
				ftrace_process_fgraph_line(&ftrace,
					" 0)               |  wake_up_new_task() {", out);
				/* Recursive target root call with inline activate_task */
				ftrace_process_fgraph_line(&ftrace,
					" 0)               |    wake_up_new_task() { /* <-wake_up_new_task+0x1d1 */",
					out);
				ftrace_process_fgraph_line(&ftrace,
					" 0)   1.000 us    |      enqueue_task(); /* <-wake_up_new_task+0x10 */",
					out);
				/* Inner target root exits */
				ftrace_process_fgraph_line(&ftrace,
					" 0)   2.000 us    |    } /* wake_up_new_task */", out);
				/* Sibling call under inlined activate_task in outer target */
				ftrace_process_fgraph_line(&ftrace,
					" 0)   1.500 us    |    check_preempt_curr(); /* <-wake_up_new_task+0x1d1 */",
					out);
				/* Outer target root exits */
				ftrace_process_fgraph_line(&ftrace,
					" 0)   4.000 us    |  } /* wake_up_new_task */", out);

				fclose(out);
				ftrace_cpu_states__clear();

				inline_entry = strstr(out_buf, "activate_task() { /* (inline) */");
				inline_exit = strstr(out_buf, "} /* activate_task (inline) */");
				sibling_call = strstr(out_buf, "check_preempt_curr();");

				if (!inline_entry) {
					pr_debug("FAILED: recursive target: inline activate_task missing\n");
					ret = TEST_FAIL;
				} else if (!inline_exit || !sibling_call) {
					pr_debug("FAILED: recursive target: expected frames missing\n");
					ret = TEST_FAIL;
				} else if (inline_exit < sibling_call) {
					pr_debug("FAILED: recursive target: inline closed before outer sibling\n");
					ret = TEST_FAIL;
				}

				free(out_buf);
				out_buf = NULL;
			}
			perf_ftrace__cleanup_inlines(&ftrace);
		}
		symbol_conf.vmlinux_name = saved_vmlinux;
		symbol_conf.ignore_vmlinux_buildid = saved_ignore_buildid;
		if (ret != TEST_OK)
			goto out_free_filter;
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
		" 0)               |      overflow_parent() { /* <-sub_func+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |        overflow_child() { /* <-overflow_parent+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |          overflow_leaf(); /* <-overflow_child+0x30 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.200 us    |        }", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.400 us    |      }", out);
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
		" 0)               |      overflow_parent() { /* <-sub_func+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |        overflow_child() { /* <-overflow_parent+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |          overflow_leaf(); /* <-overflow_child+0x30 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.200 us    |        }", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.400 us    |      }", out);
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

	/*
	 * 11. Test that orphan exits sharing name with target active frame
	 * do not collide (Issue 1).
	 */
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

	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    _raw_spin_unlock() { /* <-activate_task+0x10 */", out);
	/* Detached interrupt enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      smp_apic_timer_interrupt() { /* <-irq_work+0x10 */", out);
	/* Interrupt invokes _raw_spin_unlock and exits with named brace */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |        _raw_spin_unlock() { /* <-smp_apic_timer_interrupt+0x20 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        } /* _raw_spin_unlock */", out);
	/* Interrupt exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |      }", out);
	/* Target's _raw_spin_unlock exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |    } /* _raw_spin_unlock */", out);
	/* Target continues and exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   4.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "smp_apic_timer_interrupt")) {
		pr_debug("FAILED: orphan smp_apic_timer_interrupt leaked into output\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: activate_task exit missing or prematurely unspooled\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 12. Test target function entering at deep stack (depth >= 256)
	 * and exiting without leaking sibling (Issue 2).
	 */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Push 256 non-target outer frames */
	for (i = 0; i < 256; i++) {
		ftrace_process_fgraph_line(&ftrace,
			" 0)               |  outer_func() {", out);
	}
	/* Target function enters at depth 256 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    enqueue_task(); /* <-activate_task+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  } /* activate_task */", out);
	/* Unrelated sibling enters at depth 256 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  unrelated_sibling() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    unrelated_child();", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  }", out);
	/* Unspool outer frames */
	for (i = 0; i < 256; i++) {
		ftrace_process_fgraph_line(&ftrace,
			" 0)               |  }", out);
	}

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: deep activate_task entry missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: deep enqueue_task child missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "unrelated_sibling")) {
		pr_debug("FAILED: unrelated_sibling leaked after deep target exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/* 13. Test orphan suppression during deep stacks (Issue 3) */
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
	/* Target call at depth 256 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      deep_child() { /* <-sub_func+0x10 */", out);
	/* Detached interrupt fires at depth 257 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |        smp_apic_timer_interrupt() { /* <-irq_work+0x10 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |          irq_sub(); /* <-smp_apic_timer_interrupt+0x20 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |        }", out);
	/* Target continues */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        deep_leaf(); /* <-deep_child+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |      }", out);
	for (i = 1; i < 256; i++) {
		ftrace_process_fgraph_line(&ftrace,
			" 0)   1.000 us    |    }", out);
	}
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "smp_apic_timer_interrupt") || strstr(out_buf, "irq_sub")) {
		pr_debug("FAILED: interrupt leaked during deep stack\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "deep_leaf();")) {
		pr_debug("FAILED: legitimate deep_leaf missing during deep stack\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/* 14. Test orphan calling common helper present on stack does not leak child (Issue 2) */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    _raw_spin_unlock() { /* <-activate_task+0x10 */", out);
	/* Detached interrupt fires */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      smp_apic_timer_interrupt() { /* <-irq_work+0x10 */",
		out);
	/* Orphan interrupt calls _raw_spin_unlock */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |        _raw_spin_unlock() { /* <-smp_apic_timer_interrupt+0x20 */",
		out);
	/* Orphan _raw_spin_unlock calls child helper with retaddr <-_raw_spin_unlock+0x10 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |          native_queued_spin_unlock(); /* <-_raw_spin_unlock+0x10 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |        }", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |      }", out);
	/* Legitimate target child call */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      queued_spin_unlock(); /* <-_raw_spin_unlock+0x10 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |    }", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   4.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "native_queued_spin_unlock")) {
		pr_debug("FAILED: orphan child native_queued_spin_unlock leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "queued_spin_unlock();")) {
		pr_debug("FAILED: target child queued_spin_unlock missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 15. Test dropped target root exit followed by outer ancestor call
	 * (Issues 4A & 4B).
	 */
	/* 15a. Without --inline */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Ancestor frame at depth 0 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  schedule() {", out);
	/* Target function enters at depth 1 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task(); /* <-activate_task+0x20 */", out);
	/* Target root exit is DROPPED by ring buffer! */
	/* Outer ancestor schedule() calls another function */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    context_switch() { /* <-schedule+0x50 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      prepare_task_switch(); /* <-context_switch+0x10 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |    }", out);
	/* Ancestor schedule() exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* schedule */", out);
	/* Subsequent target function invocation on same CPU */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    enqueue_task();", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "schedule")) {
		pr_debug("FAILED: outer ancestor schedule leaked into output (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "context_switch")) {
		pr_debug("FAILED: ancestor child context_switch leaked (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: legitimate enqueue_task missing (!inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 15b. With --inline */
	ftrace.use_inline = true;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  schedule() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task(); /* <-activate_task+0x20 */", out);
	/* Dropped target root exit */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    context_switch() { /* <-schedule+0x50 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      prepare_task_switch(); /* <-context_switch+0x10 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |    }", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* schedule */", out);
	/* Subsequent target function invocation on same CPU */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    enqueue_task();", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "schedule")) {
		pr_debug("FAILED: outer ancestor schedule leaked into output (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "context_switch")) {
		pr_debug("FAILED: ancestor child context_switch leaked (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: legitimate enqueue_task missing (--inline)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 16. Test dropped-orphan exit recovery when target root exits (Issue 5) */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  schedule() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x10 */", out);
	/* Detached orphan interrupt enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      smp_apic_timer_interrupt() { /* <-irq_work+0x10 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        irq_sub(); /* <-smp_apic_timer_interrupt+0x20 */",
		out);
	/* Orphan exit is DROPPED by ring buffer! orphan_depth remains 1 */
	/* Target function exits with named closing brace */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    } /* activate_task */", out);
	/* Ancestor schedule exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* schedule */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "smp_apic_timer_interrupt") || strstr(out_buf, "irq_sub")) {
		pr_debug("FAILED: dropped-orphan interrupt leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: activate_task exit missing after dropped orphan\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "schedule")) {
		pr_debug("FAILED: schedule leaked after target exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 17. Test dropped-orphan exit recovery with bare '}' exit (Issue 3.1) */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  schedule() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x10 */", out);
	/* Detached orphan interrupt enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      smp_apic_timer_interrupt() { /* <-irq_work+0x10 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        irq_sub(); /* <-smp_apic_timer_interrupt+0x20 */",
		out);
	/* Orphan exit is DROPPED by ring buffer! orphan_depth remains 1 */
	/* Target function exits with bare closing brace '}' */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    }", out);
	/* Ancestor schedule exits with bare '}' */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  }", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "smp_apic_timer_interrupt") || strstr(out_buf, "irq_sub")) {
		pr_debug("FAILED: dropped-orphan interrupt leaked (bare exit)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing (bare exit)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "}")) {
		pr_debug("FAILED: activate_task exit missing after dropped orphan (bare exit)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "schedule")) {
		pr_debug("FAILED: schedule leaked after target bare exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 18. Test dropped top-level target root exit followed by next invocation (Issue 3.2) */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* First top-level invocation of target root */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    enqueue_task(); /* <-activate_task+0x20 */", out);
	/* Target root exit is DROPPED by ring buffer! */
	/* Second top-level invocation of target root with untraced caller */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() { /* <-untraced_caller+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    enqueue_task(); /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	first_act = strstr(out_buf, "activate_task() {");
	second_act = first_act ? strstr(first_act + 1, "activate_task() {") : NULL;
	first_enq = strstr(out_buf, "enqueue_task();");
	second_enq = first_enq ? strstr(first_enq + 1, "enqueue_task();") : NULL;

	if (!first_act || !second_act) {
		pr_debug("FAILED: subsequent target root dropped after dropped top-level exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!first_enq || !second_enq) {
		pr_debug("FAILED: child calls missing after dropped top-level exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/* 19. Test dropped target root exit (depth > 0) followed by target recall (Issue 3.3) */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Ancestor schedule() at depth 0 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  schedule() {", out);
	/* Target function at depth 1 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task(); /* <-activate_task+0x20 */", out);
	/* Target exit is DROPPED by ring buffer! */
	/* Ancestor calls target function again */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x30 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task(); /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    } /* activate_task */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* schedule */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "schedule")) {
		pr_debug("FAILED: ancestor schedule leaked into output\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	first_act = strstr(out_buf, "activate_task() {");
	second_act = first_act ? strstr(first_act + 1, "activate_task() {") : NULL;

	if (!first_act || !second_act) {
		pr_debug("FAILED: target root not recovered on recall by ancestor\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	free(out_buf);
	out_buf = NULL;

	/*
	 * 20. Test nested target-matching call with caller_idx < 0 inside
	 * an orphan subtree does not unwind outer target call stack.
	 */
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

	entry = malloc(sizeof(*entry) + strlen("enqueue_task") + 1);
	if (!entry) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	strcpy(entry->name, "enqueue_task");
	list_add_tail(&entry->list, &ftrace.graph_funcs);

	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target root enters at root indentation */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Detached orphan interrupt enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    smp_apic_timer_interrupt() { /* <-irq_work+0x10 */", out);
	/* Nested target-matching function called inside orphan with caller_idx < 0 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      enqueue_task() { /* <-smp_apic_timer_interrupt+0x20 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      } /* enqueue_task */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |    }", out);
	/* Target continues with legitimate child */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.200 us    |    enqueue_task(); /* <-activate_task+0x1d1 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "smp_apic_timer_interrupt")) {
		pr_debug("FAILED: nested orphan interrupt leaked into output\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: legitimate child enqueue_task missing after nested target-match in orphan\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: activate_task exit missing after nested target-match in orphan\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 21. Test orphan exit named differently from target root is not
	 * misidentified as target root exit when extra_indent > 0.
	 */
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

	ftrace.use_inline = true;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target root enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Synthesised inline child adds extra_indent */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    inline_child() { /* (inline) */", out);
	/* Detached orphan interrupt enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    smp_apic_timer_interrupt() { /* <-irq_work+0x10 */", out);
	/* Orphan exits with named closing brace */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    } /* _raw_spin_unlock */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    }", out);
	/* Genuine target child */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |      enqueue_task(); /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    } /* inline_child (inline) */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   4.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "smp_apic_timer_interrupt")) {
		pr_debug("FAILED: orphan smp_apic_timer_interrupt leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: genuine child missing after orphan exit with different name\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: target root exit prematurely unspooled by orphan exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 22. Test target root entry with caller_idx < 0 purges stale
	 * non-target frames so calls matching old frames do not terminate target.
	 */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Non-target function enters; its exit is dropped */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  stale_ancestor() {", out);
	/* Target root enters with untraced caller (caller_idx < 0) */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() { /* <-untraced+0x10 */", out);
	/* Target child calls helper with retaddr to stale_ancestor name */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    helper() { /* <-stale_ancestor+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    }", out);
	/* Target legitimate child */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |    enqueue_task(); /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "stale_ancestor")) {
		pr_debug("FAILED: stale_ancestor leaked into output\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: target prematurely terminated by match on stale frame below target root\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: target root exit missing after stale frame purge\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 23. Test !use_inline dropped child exit does not prevent bare '}'
	 * from exiting target root.
	 */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target root enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Child 1 enters; its exit is DROPPED by ring buffer */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    child_one() { /* <-activate_task+0x10 */", out);
	/* Caller activate_task calls Child 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    child_two(); /* <-activate_task+0x20 */", out);
	/* Target root exits with bare closing brace '}' */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.500 us    |  }", out);
	/* Subsequent non-target function enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  unrelated_after() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    unrelated_child();", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  }", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task entry missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "child_two();")) {
		pr_debug("FAILED: child_two missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "unrelated_after")) {
		pr_debug("FAILED: unrelated_after leaked; bare '}' matched dropped child instead of target root\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 24. Test graph_thresh with -G outputs return lines when !use_inline.
	 */
	ftrace.use_inline = false;
	ftrace.graph_thresh = 1000;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)   5.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();
	ftrace.graph_thresh = 0;

	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: graph_thresh return event suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 25. Test orphan subtree invoking target root function T at deeper
	 * indentation does not reset orphan_depth or target root state.
	 */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target root enters at indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Detached orphan interrupt enters at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    irq_handler() { /* <-unrelated_irq+0x10 */", out);
	/* Orphan invokes activate_task at indent 6 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      activate_task() { /* <-irq_handler+0x20 */", out);
	/* Nested activate_task calls child inside orphan */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        orphan_child(); /* <-activate_task+0x10 */", out);
	/* Nested activate_task exits inside orphan */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |      } /* activate_task */", out);
	/* Orphan interrupt exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |    } /* irq_handler */", out);
	/* Real child of outer target root executes */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    real_child(); /* <-activate_task+0x30 */", out);
	/* Outer target root exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   5.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "orphan_child")) {
		pr_debug("FAILED: orphan_child leaked from nested invocation\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "real_child")) {
		pr_debug("FAILED: real_child missing; outer target root was prematurely closed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 26. Test IRQ markers outside target function are suppressed with -G,
	 * but preserved when inside target callgraph.
	 */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* IRQ marker outside target */
	ftrace_process_fgraph_line(&ftrace, " 0)   ==========> |", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   <========== |", out);

	/* Target enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* IRQ marker inside target */
	ftrace_process_fgraph_line(&ftrace, " 0)   ==========> |", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   <========== |", out);
	/* Target exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	first_act = strstr(out_buf, "==========>");
	if (!first_act) {
		pr_debug("FAILED: IRQ marker inside target suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	second_act = strstr(first_act + 1, "==========>");
	if (second_act) {
		pr_debug("FAILED: IRQ marker outside target leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 27. Test !has_retaddr dropped target root exit does not leak non-target.
	 */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target root enters at indent 2; exit is dropped */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Non-target enters at indent 2 without retaddr */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  nontarget_leak() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    nontarget_child();", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  }", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "nontarget_leak")) {
		pr_debug("FAILED: nontarget_leak leaked after dropped target exit (!has_retaddr)\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 28. Test !in_target unwinds frames with raw_indent >= leading_spaces
	 * before pushing to avoid stale ancestor accumulation.
	 */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Non-target stale ancestor enters at indent 2; exit is dropped */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  stale_ancestor() {", out);
	/* Real non-target parent enters at indent 2 (same indent) */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  real_parent() {", out);
	/* Real parent calls target root */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-real_parent+0x10 */", out);
	/* Target calls child that has return address pointing to stale_ancestor */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      shared_child(); /* <-stale_ancestor+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    } /* activate_task */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* real_parent */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "stale_ancestor() {")) {
		pr_debug("FAILED: stale_ancestor leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "activate_task() {")) {
		pr_debug("FAILED: activate_task missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 29. Test has_retaddr dropped target root exit followed by non-target
	 * entry at root indent resets target state and allows subsequent
	 * child target invocation.
	 */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target root 1 enters at indent 2; exit is DROPPED by ring buffer */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);

	/* Non-target parent enters at indent 2 with retaddr to untraced caller */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  unrelated_caller() { /* <-untraced_kernel+0x10 */", out);

	/* Unrelated caller invokes target root as child at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-unrelated_caller+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task(); /* <-activate_task+0x30 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    } /* activate_task */", out);

	/* Unrelated caller exits with bare closing brace '}' at indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  }", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "unrelated_caller")) {
		pr_debug("FAILED: unrelated_caller leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: child target enqueue_task missing\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 30. Test noduration IRQ markers outside target function are suppressed
	 * with -G, but preserved when inside target callgraph.
	 */
	ftrace.use_inline = false;
	ftrace.graph_noduration = true;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Noduration IRQ marker outside target */
	ftrace_process_fgraph_line(&ftrace, " 0) ==========>", out);
	ftrace_process_fgraph_line(&ftrace, " 0) <==========", out);

	/* Target enters */
	ftrace_process_fgraph_line(&ftrace,
		" 0)  activate_task() {", out);
	/* Noduration IRQ marker inside target */
	ftrace_process_fgraph_line(&ftrace, " 0) ==========>", out);
	ftrace_process_fgraph_line(&ftrace, " 0) <==========", out);
	/* Target exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();
	ftrace.graph_noduration = false;

	first_act = strstr(out_buf, "==========>");
	if (!first_act) {
		pr_debug("FAILED: noduration IRQ marker inside target suppressed\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	second_act = strstr(first_act + 1, "==========>");
	if (second_act) {
		pr_debug("FAILED: noduration IRQ marker outside target leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 31. Test ancestor frame preservation when ancestor calls sibling
	 * at leading_spaces <= target_root_indent after dropped target root exit.
	 */
	ftrace.use_inline = false;
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Ancestor schedule() enters at indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  schedule() {", out);
	/* Target function enters at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task(); /* <-activate_task+0x20 */", out);
	/* Target root exit is DROPPED by ring buffer */
	/*
	 * Ancestor schedule() calls sibling non-target function at indent 4
	 * with return address pointing into schedule (caller_idx == 0 < target_root_depth).
	 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    context_switch() { /* <-schedule+0x50 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    }", out);
	/* Ancestor schedule() calls target function again at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x60 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task(); /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    } /* activate_task */", out);
	/* Ancestor schedule() exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* schedule */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "schedule")) {
		pr_debug("FAILED: ancestor schedule leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "context_switch")) {
		pr_debug("FAILED: sibling context_switch leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	first_act = strstr(out_buf, "activate_task() {");
	second_act = first_act ? strstr(first_act + 1, "activate_task() {") : NULL;
	if (!first_act || !second_act) {
		pr_debug("FAILED: target function not tracked across ancestor sibling call\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 32. Test recovery when both an orphan exit and enclosing target root exit
	 * are dropped by ring buffer.
	 */
	/* 32a. Followed by non-target entry at root indent and child target invocation */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target root enters at indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Detached orphan interrupt enters inside target at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    smp_apic_timer_interrupt() { /* <-irq_work+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      irq_sub(); /* <-smp_apic_timer_interrupt+0x20 */", out);
	/* Orphan exit and target root exit are BOTH DROPPED by ring buffer */
	/* Non-target caller enters at root indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  unrelated_caller() { /* <-untraced_kernel+0x10 */", out);
	/* Unrelated caller invokes target root as child at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-unrelated_caller+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task(); /* <-activate_task+0x30 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    } /* activate_task */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  }", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "smp_apic_timer_interrupt") || strstr(out_buf, "irq_sub")) {
		pr_debug("FAILED: orphan leaked into output\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "unrelated_caller")) {
		pr_debug("FAILED: unrelated_caller leaked into output\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: target child enqueue_task missing after double drop recovery\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/* 32b. Followed by ancestor exit at root indent */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Ancestor schedule() enters at indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  schedule() {", out);
	/* Target function enters at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x10 */", out);
	/* Orphan interrupt enters at indent 6 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      smp_apic_timer_interrupt() { /* <-irq_work+0x10 */", out);
	/* Orphan exit and target root exit are BOTH DROPPED by ring buffer */
	/* Ancestor schedule() exits at indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* schedule */", out);
	/* Subsequent target function invocation */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |    enqueue_task();", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "schedule")) {
		pr_debug("FAILED: ancestor schedule leaked after double drop exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "smp_apic_timer_interrupt")) {
		pr_debug("FAILED: orphan smp_apic_timer_interrupt leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: subsequent target invocation suppressed after double drop exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/* 32c. Dropped target root exit followed by untraced ancestor exit (match_idx < 0) */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target function enters at indent 4 with caller_idx < 0 (untraced caller) */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-untraced_parent+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task();", out);
	/* Target root exit is DROPPED by ring buffer */
	/* Untraced ancestor schedule() exits at indent 2 (match_idx < 0) */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* schedule */", out);
	/* Out-of-target IRQ marker must NOT be printed because target has exited */
	ftrace_process_fgraph_line(&ftrace,
		" 0) ==========>", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0) <==========", out);
	/* Subsequent target function invocation */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task();", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "schedule")) {
		pr_debug("FAILED: untraced ancestor schedule leaked\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (strstr(out_buf, "==========")) {
		pr_debug("FAILED: out-of-target IRQ marker leaked after dropped root exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: second activate_task invocation suppressed after untraced drop exit\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/* 33. Rekey with to_copy == 0 when target root exited on new CPU */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* CPU 0: Task 15 enters non-target ancestor and target function */
	ftrace_process_fgraph_line(&ftrace, " 0)               |  schedule() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    activate_task() { /* <-schedule+0x10 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      enqueue_task();", out);
	/* CPU 1: Task 15 unwinds target root down to schedule (indent 2) */
	ftrace_process_fgraph_line(&ftrace, " 1)   2.000 us    |    } /* activate_task */", out);
	/* Pre-bind empty task15 state on CPU 2 */
	ftrace_process_fgraph_line(&ftrace, " 2) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    <idle>-0    =>    task15-1500 ", out);
	ftrace_process_fgraph_line(&ftrace, " 2)    task15-1500 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 2) ------------------------------------------", out);
	/* CPU 1 emits banner first: merges ts1 (depth 0, min_raw_indent 2) into existing */
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    task15-1500 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	/* CPU 0 emits banner second: to_copy is 0, base_raw_indent 2 prunes activate_task */
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task15-1500 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	/* Task 15 resumes: ancestor schedule() calls non-target sibling child */
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    <idle>-0    =>    task15-1500 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    nontarget_child(); /* <-schedule+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace, " 0)   3.000 us    |  } /* schedule */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "nontarget_child")) {
		pr_debug("FAILED: nontarget_child leaked after to_copy == 0 rekey\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 34. Test dropped-orphan recovery inside a nested child of the target root (VF1).
	 */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Target root enters at indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Non-leaf child of target enters at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    enqueue_task() { /* <-activate_task+0x10 */", out);
	/* Orphan triggered inside nested child at indent 6; its exit is DROPPED */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      housekeeping_any_cpu() { /* <-rb_wakeups+0x10 */", out);
	/* Nested child calls legitimate function at indent 6 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      update_load_avg(); /* <-enqueue_task+0x20 */", out);
	/* Another orphan inside nested child whose exit is ALSO DROPPED before child exits */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      arch_irq_work_raise() { /* <-irq_work_queue+0x10 */",
		out);
	/* Nested child exits at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    } /* enqueue_task */", out);
	/* Target root exits at indent 2 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "housekeeping_any_cpu") || strstr(out_buf, "arch_irq_work_raise")) {
		pr_debug("FAILED: nested orphan leaked in test 34\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "update_load_avg();")) {
		pr_debug("FAILED: legitimate child call update_load_avg swallowed after dropped nested orphan\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* enqueue_task */")) {
		pr_debug("FAILED: nested child exit swallowed after dropped nested orphan\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "} /* activate_task */")) {
		pr_debug("FAILED: target root exit missing in test 34\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 35. Test orphan_depth preservation and pruning across task rekeying (VF2).
	 */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/*
	 * 35a: Task enters target root and orphan on CPU 0 (unknown PID), migrates
	 * to CPU 1 where PID is bound and orphan exits, then CPU 0 emits delayed banner.
	 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    housekeeping_any_cpu() { /* <-rb_wakeups+0x10 */", out);
	/* Task migrates to CPU 1: banner binds task16-1600 to CPU 1 */
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 1)    <idle>-0    =>    task16-1600 ", out);
	ftrace_process_fgraph_line(&ftrace, " 1) ------------------------------------------", out);
	/* Orphan exits on CPU 1 at indent 4 */
	ftrace_process_fgraph_line(&ftrace, " 1)   1.000 us    |    }", out);
	/* Delayed outgoing banner on CPU 0 rekeys CPU 0 state into task16-1600 */
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	ftrace_process_fgraph_line(&ftrace, " 0)    task16-1600 =>    <idle>-0 ", out);
	ftrace_process_fgraph_line(&ftrace, " 0) ------------------------------------------", out);
	/* Legitimate target child executes on CPU 1 */
	ftrace_process_fgraph_line(&ftrace,
		" 1)   1.500 us    |    enqueue_task(); /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 1)   3.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "housekeeping_any_cpu")) {
		pr_debug("FAILED: orphan leaked in test 35a\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: stale orphan_depth from pre-migration state suppressed enqueue_task\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 36. Test orphan interrupt handler suppresses surrounding IRQ markers (VF3).
	 */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	ftrace_process_fgraph_line(&ftrace,
		" 0)               |  activate_task() {", out);
	/* Kernel emits ==========> before orphan interrupt entry */
	ftrace_process_fgraph_line(&ftrace, " 0)   ==========> |", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    sysvec_apic_timer_interrupt() { /* <-asm_call_sysvec_on_stack+0x10 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |      hrtimer_interrupt(); /* <-sysvec_apic_timer_interrupt+0x20 */",
		out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |    }", out);
	/* Kernel emits <========== after orphan interrupt return */
	ftrace_process_fgraph_line(&ftrace, " 0)   <========== |", out);
	/* Legitimate child call */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.500 us    |    enqueue_task(); /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   4.000 us    |  } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "==========")) {
		pr_debug("FAILED: empty IRQ marker pair leaked around suppressed orphan interrupt\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: enqueue_task missing after suppressed IRQ in test 36\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	free(out_buf);
	out_buf = NULL;

	/*
	 * 37. Test unmatched ancestor exit unwinds non-target ancestor frames
	 * below target_root_depth with raw_indent >= leading_spaces (VF4).
	 */
	out = open_memstream(&out_buf, &out_len);
	if (!out) {
		ret = TEST_FAIL;
		goto out_free_filter;
	}

	/* Non-target ancestor enters at indent 4 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |    mid_ancestor() {", out);
	/* Target root enters at indent 6 */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      activate_task() { /* <-mid_ancestor+0x10 */", out);
	/* Exits of activate_task (indent 6) and mid_ancestor (indent 4) are DROPPED */
	/* Untraced outer ancestor exits at indent 2 (match_idx < 0) */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   3.000 us    |  } /* outer_untraced */", out);
	/* New target root enters at indent 6 with untraced caller mid_ancestor */
	ftrace_process_fgraph_line(&ftrace,
		" 0)               |      activate_task() { /* <-mid_ancestor+0x10 */", out);
	/* Orphan inside new target root has retaddr matching mid_ancestor */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        orphan_leaf(); /* <-mid_ancestor+0x20 */", out);
	/* Legitimate child call of new target root */
	ftrace_process_fgraph_line(&ftrace,
		" 0)   1.000 us    |        enqueue_task(); /* <-activate_task+0x20 */", out);
	ftrace_process_fgraph_line(&ftrace,
		" 0)   2.000 us    |      } /* activate_task */", out);

	fclose(out);
	ftrace_cpu_states__clear();

	if (strstr(out_buf, "mid_ancestor") || strstr(out_buf, "outer_untraced") ||
	    strstr(out_buf, "orphan_leaf")) {
		pr_debug("FAILED: ancestor or orphan leaked in test 37\n");
		ret = TEST_FAIL;
		goto out_free_filter;
	}
	if (!strstr(out_buf, "enqueue_task();")) {
		pr_debug("FAILED: enqueue_task missing in test 37 (stale mid_ancestor not unwound)\n");
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

static int test__parse_retaddr(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	return test_parse_retaddr();
}

static int test__filter_retaddr(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	return test_filter_retaddr();
}

static int test__column_layouts(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	return test_column_layouts();
}

static int test__graph_target_filter(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	return test_graph_target_filter();
}

static int test__stack_saturation(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	return test_stack_saturation();
}

static int test__context_switch_handling(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	return test_context_switch_handling();
}

static int test__inline_processing(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	return test_inline_processing();
}

static struct test_case ftrace_test_cases[] = {
	TEST_CASE("Parse return address comments", parse_retaddr),
	TEST_CASE("Filter return address comments", filter_retaddr),
	TEST_CASE("Function graph column layouts", column_layouts),
	TEST_CASE("Graph target filtering", graph_target_filter),
	TEST_CASE("Stack saturation and expansion", stack_saturation),
	TEST_CASE("Context switch handling", context_switch_handling),
	TEST_CASE("Inline symbol and trace processing", inline_processing),
	{ .name = NULL, }
};

struct test_suite suite__ftrace = {
	.desc = "Ftrace inline processing",
	.test_cases = ftrace_test_cases,
};
