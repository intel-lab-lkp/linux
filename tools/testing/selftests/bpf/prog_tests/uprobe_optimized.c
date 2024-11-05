// SPDX-License-Identifier: GPL-2.0

#include <test_progs.h>

#ifdef __x86_64__

#include "sdt.h"
#include "uprobe_optimized.skel.h"

#define TRAMP "[uprobes-trampoline]"

__naked noinline void uprobe_test(void)
{
	asm volatile (".byte 0x0f, 0x1f, 0x44, 0x00, 0x00\n\t"
		      "ret\n\t");
}

static int find_uprobes_trampoline(void **start, void **end)
{
	char line[128];
	int ret = -1;
	FILE *maps;

	maps = fopen("/proc/self/maps", "r");
	if (!maps) {
		fprintf(stderr, "cannot open maps\n");
		return -1;
	}

	while (fgets(line, sizeof(line), maps)) {
		int m = -1;

		/* We care only about private r-x mappings. */
		if (sscanf(line, "%p-%p r-xp %*x %*x:%*x %*u %n", start, end, &m) != 2)
			continue;
		if (m < 0)
			continue;
		if (!strncmp(&line[m], TRAMP, sizeof(TRAMP)-1)) {
			ret = 0;
			break;
		}
	}

	fclose(maps);
	return ret;
}

static void check_attach(struct uprobe_optimized *skel, void (*trigger)(void))
{
	void *tramp_start, *tramp_end;
	struct __arch_relative_insn {
		u8 op;
		s32 raddr;
	} __packed *call;

	unsigned long delta;

	/* Uprobe gets optimized after first trigger, so let's press twice. */
	trigger();
	trigger();

	if (!ASSERT_OK(find_uprobes_trampoline(&tramp_start, &tramp_end), "uprobes_trampoline"))
		return;

	/* Make sure bpf program got executed.. */
	ASSERT_EQ(skel->bss->executed, 2, "executed");

	/* .. and check the trampoline is as expected. */
	call = (struct __arch_relative_insn *) trigger;

	delta = tramp_start > (void *) trigger ?
		tramp_start - (void *) trigger :
		(void *) trigger - tramp_start;

	/* and minus call instruction size itself */
	delta -= 5;

	ASSERT_EQ(call->op, 0xe8, "call");
	ASSERT_EQ(call->raddr, delta, "delta");
	ASSERT_EQ(tramp_end - tramp_start, 4096, "size");
}

static void check_detach(struct uprobe_optimized *skel, void (*trigger)(void))
{
	unsigned char nop5[5] = { 0x0f, 0x1f, 0x44, 0x00, 0x00 };
	void *tramp_start, *tramp_end;

	/* [uprobes_trampoline] stays after detach */
	ASSERT_OK(find_uprobes_trampoline(&tramp_start, &tramp_end), "uprobes_trampoline");
	ASSERT_OK(memcmp(trigger, nop5, 5), "nop5");
}

static void check(struct uprobe_optimized *skel, struct bpf_link *link,
		  void (*trigger)(void))
{
	check_attach(skel, trigger);
	bpf_link__destroy(link);
	check_detach(skel, uprobe_test);
}

static void test_uprobe(void)
{
	struct uprobe_optimized *skel;
	unsigned long offset;

	skel = uprobe_optimized__open_and_load();
	if (!ASSERT_OK_PTR(skel, "uprobe_optimized__open_and_load"))
		return;

	offset = get_uprobe_offset(&uprobe_test);
	if (!ASSERT_GE(offset, 0, "get_uprobe_offset"))
		goto cleanup;

	skel->links.test_1 = bpf_program__attach_uprobe_opts(skel->progs.test_1,
					0, "/proc/self/exe", offset, NULL);
	if (!ASSERT_OK_PTR(skel->links.test_1, "bpf_program__attach_uprobe_opts"))
		goto cleanup;

	check(skel, skel->links.test_1, uprobe_test);
	skel->links.test_1 = NULL;

cleanup:
	uprobe_optimized__destroy(skel);
}

static void test_uprobe_multi(void)
{
	struct uprobe_optimized *skel;

	skel = uprobe_optimized__open_and_load();
	if (!ASSERT_OK_PTR(skel, "uprobe_optimized__open_and_load"))
		return;

	skel->links.test_2 = bpf_program__attach_uprobe_multi(skel->progs.test_2,
						0, "/proc/self/exe", "uprobe_test", NULL);
	if (!ASSERT_OK_PTR(skel->links.test_2, "bpf_program__attach_uprobe_multi"))
		goto cleanup;

	check(skel, skel->links.test_2, uprobe_test);
	skel->links.test_2 = NULL;

cleanup:
	uprobe_optimized__destroy(skel);
}

__naked noinline void usdt_test(void)
{
	STAP_PROBE(optimized_uprobe, usdt);
	asm volatile ("ret\n");
}

static void test_usdt(void)
{
	struct uprobe_optimized *skel;

	skel = uprobe_optimized__open_and_load();
	if (!ASSERT_OK_PTR(skel, "uprobe_optimized__open_and_load"))
		return;

	skel->links.test_3 = bpf_program__attach_usdt(skel->progs.test_3,
						-1 /* all PIDs */, "/proc/self/exe",
						"optimized_uprobe", "usdt", NULL);
	if (!ASSERT_OK_PTR(skel->links.test_3, "bpf_program__attach_usdt"))
		goto cleanup;

	check(skel, skel->links.test_3, usdt_test);
	skel->links.test_3 = NULL;

cleanup:
	uprobe_optimized__destroy(skel);
}

static void test_optimized(void)
{
	if (test__start_subtest("uprobe"))
		test_uprobe();
	if (test__start_subtest("uprobe_multi"))
		test_uprobe_multi();
	if (test__start_subtest("usdt"))
		test_usdt();
}
#else
static void test_optimized(void)
{
	test__skip();
}
#endif /* __x86_64__ */

void test_uprobe_optimized(void)
{
	test_optimized();
}
