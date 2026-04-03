// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * Pratyush Yadav (Google) <pratyush@kernel.org>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

/*
 * Selftests for memfd preservation via LUO.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/liveupdate.h>
#include <linux/sizes.h>

#include "../kselftest.h"
#include "../kselftest_harness.h"

#include "luo_test_utils.h"

#define STATE_SESSION_NAME "luo-state"
#define STATE_MEMFD_TOKEN 1

#define MEMFD_DATA_SESSION_NAME "memfd_data_session"
#define MEMFD_DATA_TOKEN 1
#define MEMFD_DATA_BUFFER_SIZE SZ_1M
#define MEMFD_DATA_FS_COPY "memfd_data_fs_copy.bin"

static int luo_fd = -1;
static int stage;

static void memfd_data_stage_1(struct __test_metadata *_metadata)
{
	int fd, session;
	char *buffer;
	struct liveupdate_session_preserve_fd preserve_arg = { .size = sizeof(preserve_arg) };

	buffer = malloc(MEMFD_DATA_BUFFER_SIZE);
	ASSERT_NE(buffer, NULL);

	session = luo_create_session(luo_fd, MEMFD_DATA_SESSION_NAME);
	ASSERT_GE(session, 0);

	fd = create_random_memfd("memfd_data", buffer, MEMFD_DATA_BUFFER_SIZE);
	ASSERT_GE(fd, 0);

	ASSERT_EQ(save_test_data(MEMFD_DATA_FS_COPY, buffer, MEMFD_DATA_BUFFER_SIZE), 0);

	preserve_arg.fd = fd;
	preserve_arg.token = MEMFD_DATA_TOKEN;
	ASSERT_GE(ioctl(session, LIVEUPDATE_SESSION_PRESERVE_FD, &preserve_arg), 0);

	daemonize_and_wait();
}

static void memfd_data_stage_2(struct __test_metadata *_metadata)
{
	int fd, session;
	char *buffer;
	struct liveupdate_session_retrieve_fd retrieve_arg = { .size = sizeof(retrieve_arg) };

	buffer = malloc(MEMFD_DATA_BUFFER_SIZE);
	ASSERT_NE(buffer, NULL);

	session = luo_retrieve_session(luo_fd, MEMFD_DATA_SESSION_NAME);
	ASSERT_GE(session, 0);

	ASSERT_EQ(load_test_data(MEMFD_DATA_FS_COPY, buffer, MEMFD_DATA_BUFFER_SIZE), 0);

	retrieve_arg.token = MEMFD_DATA_TOKEN;
	ASSERT_GE(ioctl(session, LIVEUPDATE_SESSION_RETRIEVE_FD, &retrieve_arg), 0);
	fd = retrieve_arg.fd;
	ASSERT_GE(fd, 0);

	ASSERT_EQ(verify_fd_content_read(fd, buffer, MEMFD_DATA_BUFFER_SIZE), 0);

	ASSERT_EQ(luo_session_finish(session), 0);
}

/*
 * Test that a memfd with its data is preserved across live update.
 */
TEST(memfd_data)
{
	if (cwd_is_tmpfs())
		SKIP(return, "test saves data to rootfs, cannot run on tmpfs");

	switch (stage) {
	case 1:
		memfd_data_stage_1(_metadata);
		break;
	case 2:
		memfd_data_stage_2(_metadata);
		break;
	default:
		TH_LOG("Unknown stage %d\n", stage);
		ASSERT_FALSE(true);
	}
}

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
