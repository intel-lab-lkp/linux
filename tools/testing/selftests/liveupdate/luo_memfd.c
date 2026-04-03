// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav (Google) <pratyush@kernel.org>
 */

/*
 * Selftests for memfd preservation via LUO.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/liveupdate.h>

#include "../kselftest.h"
#include "../kselftest_harness.h"

#include "luo_test_utils.h"

#define STATE_SESSION_NAME "luo-state"
#define STATE_MEMFD_TOKEN 1

static int luo_fd = -1;
static int stage;

int main(int argc, char *argv[])
{
	int expected_stage = 0;
	int session;

	/*
	 * The test takes an optional --stage argument. This lets callers
	 * provide the expected stage, and if that doesn't match the test errors
	 * out.
	 *
	 * Look for the stage. Since test_harness_run() doesn't recognize it,
	 * once found, remove it from argv.
	 */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--stage") == 0) {
			if (i + 1 < argc) {
				expected_stage = atoi(argv[i + 1]);
				memmove(&argv[i], &argv[i + 2], (argc - i - 1) * sizeof(char *));
				argc -= 2;
				i--;
			} else {
				ksft_exit_fail_msg("Option --stage requires an argument\n");
			}
		}
	}

	luo_fd = luo_open_device();
	if (luo_fd < 0)
		ksft_exit_skip("Failed to open %s (%s) device. Is LUO enabled?\n",
			       LUO_DEVICE, strerror(errno));

	session = luo_retrieve_session(luo_fd, STATE_SESSION_NAME);
	if (session == -ENOENT)
		stage = 1;
	else if (session >= 0)
		stage = 2;
	else
		ksft_exit_fail_perror("Failed to check for state session");

	if (expected_stage && expected_stage != stage)
		ksft_exit_fail_msg("Stage mismatch: expected %d, got %d\n",
				   expected_stage, stage);

	if (stage == 1)
		create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, 2);

	test_harness_run(argc, argv);
}
