// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */

#include <test_progs.h>
#include "pro_epilogue_subprog.skel.h"
#include "pro_epilogue_kfunc.skel.h"
#include "epilogue_tailcall.skel.h"
#include "pro_epilogue_goto_start.skel.h"
#include "epilogue_exit.skel.h"

struct st_ops_args {
	int a;
};

static void test_tailcall(void)
{
	LIBBPF_OPTS(bpf_test_run_opts, topts);
	struct epilogue_tailcall *skel;
	struct st_ops_args args;
	int err, prog_fd;

	skel = epilogue_tailcall__open_and_load();
	if (!ASSERT_OK_PTR(skel, "epilogue_tailcall__open_and_load"))
		return;

	topts.ctx_in = &args;
	topts.ctx_size_in = sizeof(args);

	skel->links.epilogue_tailcall =
		bpf_map__attach_struct_ops(skel->maps.epilogue_tailcall);
	if (!ASSERT_OK_PTR(skel->links.epilogue_tailcall, "attach_struct_ops"))
		goto done;

	/* tailcall prog + gen_epilogue */
	memset(&args, 0, sizeof(args));
	prog_fd = bpf_program__fd(skel->progs.syscall_epilogue_tailcall);
	err = bpf_prog_test_run_opts(prog_fd, &topts);
	ASSERT_OK(err, "bpf_prog_test_run_opts");
	ASSERT_EQ(args.a, 10001, "args.a");
	ASSERT_EQ(topts.retval, 10001 * 2, "topts.retval");

done:
	epilogue_tailcall__destroy(skel);
}

void test_pro_epilogue(void)
{
	RUN_TESTS(pro_epilogue_subprog);
	RUN_TESTS(pro_epilogue_kfunc);
	RUN_TESTS(pro_epilogue_goto_start);
	RUN_TESTS(epilogue_exit);
	if (test__start_subtest("tailcall"))
		test_tailcall();
}
