// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 KylinSoft Corporation.
 * Author: Chenghao Duan <duanchenghao@kylinos.cn>
 */

/*
 * Multi-session kexec selftest for eventfd LUO support.
 *
 * Modeled after luo_multi_session.c.
 * It creates multiple LUO sessions, each preserving different eventfd types,
 * and verifies them after kexec.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "luo_test_utils.h"

/* Session names */
#define SESSION_EMPTY     "eventfd-empty"
#define SESSION_DEFAULT   "eventfd-default"
#define SESSION_SEM       "eventfd-sem"
#define SESSION_NONBLOCK  "eventfd-nonblock"
#define SESSION_LARGE     "eventfd-large"
#define SESSION_MODIFIED  "eventfd-modified-after-preserve"

/* Tokens */
#define TOKEN_DEFAULT       0xCAFEBABE
#define TOKEN_SEM           0xDEADBEEF
#define TOKEN_NONBLOCK      0xFEEDBEEF
#define TOKEN_LARGE         0xBEEFCAFE
#define TOKEN_MODIFIED      0xABCD1234

/* Counts */
#define COUNT_INITIAL   42
#define COUNT_WRITE     10
#define COUNT_EXPECTED  (COUNT_INITIAL + COUNT_WRITE) /* 52 */
#define COUNT_SEM       5
#define COUNT_NONBLOCK  100
#define COUNT_LARGE     ((unsigned int)-1) /* UINT_MAX */
#define COUNT_MODIFIED  25
#define COUNT_MODIFY_DELTA  99

/* State tracking */
#define STATE_SESSION_NAME "eventfd_multi_state"
#define STATE_TOKEN        997

/* Eventfd verification modes */
enum verify_mode {
	VERIFY_FLAGS,        /* Only verify flags, no behavior testing */
	VERIFY_ONCE,         /* Read once and verify count */
	VERIFY_SEMAPHORE,    /* Read multiple times, verify returns 1 each time */
	VERIFY_NONBLOCK,     /* Read once in nonblock mode */
	VERIFY_LARGE         /* Read once, verify large count */
};

/* Test session configuration */
struct test_session_config {
	const char *session_name;
	unsigned long token;
	unsigned int count;
	unsigned int flags;
	enum verify_mode verify_mode;
	const char *desc;
};

/* Test session configurations */
static const struct test_session_config test_configs[] = {
	{SESSION_EMPTY, 0, 0, 0, VERIFY_FLAGS, "Empty"},
	{SESSION_DEFAULT, TOKEN_DEFAULT, COUNT_EXPECTED, 0, VERIFY_ONCE, "Default"},
	{SESSION_SEM, TOKEN_SEM, COUNT_SEM, EFD_SEMAPHORE, VERIFY_SEMAPHORE, "Semaphore"},
	{SESSION_NONBLOCK, TOKEN_NONBLOCK, COUNT_NONBLOCK, EFD_NONBLOCK, VERIFY_NONBLOCK, "Nonblock"},
	{SESSION_LARGE, TOKEN_LARGE, COUNT_LARGE, 0, VERIFY_LARGE, "Large-count"},
	{SESSION_MODIFIED, TOKEN_MODIFIED, COUNT_MODIFIED + COUNT_MODIFY_DELTA, 0, VERIFY_ONCE, "Modified after preserve (kexec handover)"},
};
#define NUM_TEST_SESSIONS ARRAY_SIZE(test_configs)

static int verify_eventfd_flags(int efd, unsigned int expected_flags, const char *desc)
{
	int actual_flags = fcntl(efd, F_GETFL);

	if (actual_flags < 0)
		return -errno;

	int expected_fd_flags = expected_flags & (EFD_NONBLOCK | EFD_CLOEXEC);
	int actual_fd_flags = actual_flags & (O_NONBLOCK | O_CLOEXEC);

	if (actual_fd_flags != expected_fd_flags) {
		ksft_print_msg("%s: flag mismatch - expected 0x%x, got 0x%x\n",
			       desc, expected_fd_flags, actual_fd_flags);
		return -EINVAL;
	}

	ksft_print_msg("  %s eventfd flags OK (0x%x)\n", desc, expected_fd_flags);
	return actual_flags; /* Return actual flags for further use */
}

static int ensure_nonblock(int efd, int current_flags)
{
	return fcntl(efd, F_SETFL, current_flags | O_NONBLOCK);
}

static int verify_count_once(int efd, unsigned int expected_count, const char *desc)
{
	uint64_t val;

	if (read(efd, &val, sizeof(val)) != sizeof(val))
		return -errno;

	if (val != (uint64_t)expected_count) {
		ksft_print_msg("%s: expected %u got %llu\n",
			       desc, expected_count, (unsigned long long)val);
		return -EINVAL;
	}

	ksft_print_msg("  %s eventfd OK: %u\n", desc, expected_count);
	return 0;
}

static int verify_semaphore_behavior(int efd, unsigned int expected_count, const char *desc)
{
	uint64_t val;

	/* Read expected_count times, each should return 1 */
	for (unsigned int i = 0; i < expected_count; i++) {
		if (read(efd, &val, sizeof(val)) != sizeof(val))
			return -errno;

		if (val != 1) {
			ksft_print_msg("%s: expected 1, got %llu at read %u\n",
				       desc, (unsigned long long)val, i + 1);
			return -EINVAL;
		}
		ksft_print_msg("  %s eventfd OK: %u at read %u\n", desc, (unsigned int)val, i + 1);
	}

	/* Next read should return EAGAIN (no more events) */
	if (read(efd, &val, sizeof(val)) >= 0 || errno != EAGAIN) {
		ksft_print_msg("%s: expected EAGAIN after %u reads\n",
			       desc, expected_count);
		return -EINVAL;
	}

	ksft_print_msg("  %s eventfd OK (%u reads)\n", desc, expected_count);
	return 0;
}

static int restore_and_verify_eventfd_generic(int session_fd,
					      unsigned long token,
					      unsigned int expected_count,
					      unsigned int expected_flags,
					      enum verify_mode mode,
					      const char *desc)
{
	struct liveupdate_session_retrieve_fd arg = { .size = sizeof(arg) };
	int efd, ret = 0, actual_flags;

	arg.token = token;
	if (ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg) < 0)
		return -errno;
	efd = arg.fd;

	switch (mode) {
	case VERIFY_FLAGS:
		/* Only verify flags, no behavior testing */
		ret = verify_eventfd_flags(efd, expected_flags, desc);
		if (ret < 0)
			close(efd);
		return ret < 0 ? ret : 0;

	case VERIFY_SEMAPHORE:
		/* Verify flags + semaphore behavior */
		actual_flags = verify_eventfd_flags(efd, expected_flags, desc);
		if (actual_flags < 0) {
			ret = actual_flags;
			goto out;
		}

		if (ensure_nonblock(efd, actual_flags) < 0) {
			ret = -errno;
			goto out;
		}

		ret = verify_semaphore_behavior(efd, expected_count, desc);
		break;

	case VERIFY_ONCE:
	case VERIFY_NONBLOCK:
	case VERIFY_LARGE:
		/* Verify flags + count behavior */
		actual_flags = verify_eventfd_flags(efd, expected_flags, desc);
		if (actual_flags < 0) {
			ret = actual_flags;
			goto out;
		}

		ret = verify_count_once(efd, expected_count, desc);
		break;

	default:
		ret = -EINVAL;
		break;
	}

out:
	close(efd);
	return ret;
}

static int create_and_preserve_eventfd_keep_fd(int session_fd,
					       unsigned long token,
					       unsigned int count,
					       unsigned int flags,
					       const char *desc,
					       int *efd_out)
{
	struct liveupdate_session_preserve_fd arg = { .size = sizeof(arg) };
	int efd = eventfd(count, flags);

	if (efd < 0)
		return -errno;

	arg.fd = efd;
	arg.token = token;
	if (ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &arg) < 0) {
		int ret = -errno;

		close(efd);
		return ret;
	}

	if (efd_out)
		*efd_out = efd;
	else
		close(efd);
	return 0;
}

static int create_and_preserve_eventfd(int session_fd,
				       unsigned long token,
				       unsigned int count,
				       unsigned int flags,
				       const char *desc)
{
	return create_and_preserve_eventfd_keep_fd(session_fd, token, count,
						   flags, desc, NULL);
}

static int create_session_checked(int luo_fd, const char *session_name)
{
	int session_fd = luo_create_session(luo_fd, session_name);

	if (session_fd < 0)
		fail_exit("luo_create_session for '%s'", session_name);
	return session_fd;
}

static int retrieve_session_checked(int luo_fd, const char *session_name)
{
	int session_fd = luo_retrieve_session(luo_fd, session_name);

	if (session_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", session_name);
	return session_fd;
}

static void finish_session_checked(int session_fd, const char *session_name)
{
	if (luo_session_finish(session_fd) < 0)
		fail_exit("luo_session_finish for '%s'", session_name);
	close(session_fd);
}

static int verify_eventfd_config(int session_fd, const struct test_session_config *config)
{
	return restore_and_verify_eventfd_generic(session_fd, config->token,
						 config->count, config->flags,
						 config->verify_mode, config->desc);
}


static void run_stage_1(int luo_fd)
{
	ksft_print_msg("[STAGE 1] Starting pre-kexec setup for multi-eventfd test...\n");
	ksft_print_msg("[STAGE 1] Creating state file for next stage (2)...\n");
	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_TOKEN, 2);

	/* Create all test sessions */
	for (size_t i = 0; i < NUM_TEST_SESSIONS; i++) {
		const struct test_session_config *config = &test_configs[i];

		ksft_print_msg("[STAGE 1] Creating session '%s'...\n", config->session_name);
		int session_fd = create_session_checked(luo_fd, config->session_name);

		/* Special handling for modified session (preserve then modify) */
		if (config->token == TOKEN_MODIFIED) {
			int preserved_efd;

			if (create_and_preserve_eventfd_keep_fd(session_fd,
								config->token,
								COUNT_MODIFIED,
								config->flags,
								"modified-after-preserve",
								&preserved_efd) < 0)
				fail_exit("create_and_preserve_eventfd_keep_fd modified");

			/* Now modify the preserved eventfd's count */
			uint64_t modify_value = COUNT_MODIFY_DELTA;

			if (write(preserved_efd, &modify_value,
			    sizeof(modify_value)) != sizeof(modify_value))
				fail_exit("write to preserved eventfd after preserve");

			close(preserved_efd);
		} else {
			/* Standard session creation */
			if (create_and_preserve_eventfd(session_fd, config->token,
							config->count, config->flags,
							config->desc) < 0)
				fail_exit("create_and_preserve_eventfd %s", config->desc);
		}
	}

	close(luo_fd);
	daemonize_and_wait();
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	int session_fds[NUM_TEST_SESSIONS];
	int stage;

	ksft_print_msg("[STAGE 2] Starting post-kexec verification...\n");

	restore_and_read_stage(state_session_fd, STATE_TOKEN, &stage);
	if (stage != 2)
		fail_exit("Expected stage 2, but state file contains %d", stage);

	ksft_print_msg("[STAGE 2] Retrieving all sessions...\n");
	for (size_t i = 0; i < NUM_TEST_SESSIONS; i++)
		session_fds[i] = retrieve_session_checked(luo_fd, test_configs[i].session_name);

	ksft_print_msg("[STAGE 2] Verifying eventfds...\n");
	for (size_t i = 0; i < NUM_TEST_SESSIONS; i++) {
		if (verify_eventfd_config(session_fds[i], &test_configs[i]) < 0)
			fail_exit("verify %s eventfd", test_configs[i].desc);
	}

	ksft_print_msg("[STAGE 2] All eventfd sessions verified successfully.\n");

	ksft_print_msg("[STAGE 2] Finalizing all sessions...\n");
	for (size_t i = 0; i < NUM_TEST_SESSIONS; i++)
		finish_session_checked(session_fds[i], test_configs[i].session_name);

	ksft_print_msg("[STAGE 2] Finalizing state session...\n");
	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	ksft_print_msg("\n--- EVENTFD_LUO TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
