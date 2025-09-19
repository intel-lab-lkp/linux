// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * binfmt_script loader KFuzzTest target
 *
 * Copyright 2025 Google LLC
 */
#include <linux/binfmts.h>
#include <linux/kfuzztest.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>

struct load_script_arg {
	char buf[BINPRM_BUF_SIZE];
};

FUZZ_TEST(test_load_script, struct load_script_arg)
{
	struct linux_binprm bprm = {};
	char *arg_page;

	arg_page = (char *)get_zeroed_page(GFP_KERNEL);
	if (!arg_page)
		return;

	memcpy(bprm.buf, arg->buf, sizeof(bprm.buf));
	/*
	 * `load_script` calls remove_arg_zero, which expects argc != 0. A
	 * static value of 1 is sufficient for fuzzing.
	 */
	bprm.argc = 1;
	bprm.p = (unsigned long)arg_page + PAGE_SIZE;
	bprm.filename = kstrdup("fuzz_script", GFP_KERNEL);
	if (!bprm.filename)
		goto cleanup;
	bprm.interp = kstrdup(bprm.filename, GFP_KERNEL);
	if (!bprm.interp)
		goto cleanup;

	bprm.mm = mm_alloc();
	if (!bprm.mm)
		goto cleanup;

	/*
	 * Call the target function. We expect it to fail and return an error
	 * (e.g., at open_exec), which is fine. The goal is to survive the
	 * initial parsing logic without crashing.
	 */
	load_script(&bprm);

cleanup:
	if (bprm.mm)
		mmput(bprm.mm);
	if (bprm.interp)
		kfree(bprm.interp);
	if (bprm.filename)
		kfree(bprm.filename);
	free_page((unsigned long)arg_page);
}
