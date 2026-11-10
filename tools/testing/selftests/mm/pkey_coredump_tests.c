// SPDX-License-Identifier: GPL-2.0

#include <sys/wait.h>

#include "pkey-helpers.h"

#if defined(__i386__) || defined(__x86_64__)
#include "../x86/coredump.h"

static bool validate_corefile_pkru(u64 *xsave, u32 pkru)
{
	bool ret = false;

	if (!(xsave[XSTATE_SW_BYTES_OFFSET / sizeof(u64)] & XSTATE_PKEY))
		goto out;

	if (!(xsave[XSTATE_BV_OFFSET / sizeof(u64)] & XSTATE_PKEY))
		goto out;

	if (*(u32 *)(xsave + pkey_reg_xstate_offset() / sizeof(u64)) != pkru)
		goto out;

	ret = true;
out:
	return ret;
}

static void test_x86_coredump(void)
{
	u64 *corefile_xsave;
	u32 new_pkru;
	pid_t child;
	int status;

	if (!coredump_prepare())
		return;

	new_pkru = ~__read_pkey_reg();
	new_pkru &= ~3;

	child = fork();
	if (child < 0) {
		coredump_exit_fail_msg("fork() failed.");
	} else if (!child) {
		if (chdir(core_dir)) {
			printf("\tchdir() failed.\n");
			_exit(1);
		}
		__write_pkey_reg(new_pkru);
		coredump_trigger();
		_exit(1);
	}

	if (waitpid(child, &status, 0) != child ||
	    !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
		coredump_exit_fail_msg("child didn't SIGSEGV successfully.");

	corefile_map(child);
	corefile_xsave = (u64 *)retrieve_corefile_xbuf();

	if (validate_corefile_pkru(corefile_xsave, new_pkru))
		ksft_test_result_pass("PKRU was correctly core-dumped\n");
	else
		ksft_test_result_fail("PKRU was not correctly core-dumped\n");

	coredump_cleanup();
}
#endif

static void (*pkey_tests[])(void) = {
#if defined(__i386__) || defined(__x86_64__)
	test_x86_coredump,
#endif
};

int main(int argc, char *argv[])
{
	ksft_print_header();

	if (!is_pkeys_supported())
		ksft_exit_skip("pkeys not supported\n");

	ksft_set_plan(ARRAY_SIZE(pkey_tests));

	for (test_nr = 0; test_nr < ARRAY_SIZE(pkey_tests); test_nr++)
		(*pkey_tests[test_nr])();

	ksft_finished();
}
