// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libliveupdate.h>

#define INCOMING_DIR "/sys/class/misc/liveupdate/incoming/"
#define KHO_INCOMING_DIR "/sys/kernel/kexec/kho_incoming/"

#define SESSION_CLAIMED "stats-claimed"
#define SESSION_UNCLAIMED "stats-unclaimed"

#define MFD_RETRIEVED_TOKEN 0x51
#define MFD_DROPPED_TOKEN 0x52
#define MFD_UNCLAIMED_TOKEN 0x53

#define MFD_RETRIEVED_DATA "retrieved before finish"
#define MFD_DROPPED_DATA "never retrieved"
#define MFD_UNCLAIMED_DATA "session never retrieved"

#define STATE_SESSION_NAME "handover_stats_state"
#define STATE_MEMFD_TOKEN 997

/* The first open starts deserialization. */
static char state_before_open[16];

static void read_sysfs(const char *dir, const char *name, char *buf, size_t len)
{
	char path[128];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s%s", dir, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail_exit("open %s", path);

	n = read(fd, buf, len - 1);
	if (n < 0)
		fail_exit("read %s", path);
	close(fd);

	/* A full buffer could hide the rest of a malformed value */
	if ((size_t)n == len - 1)
		ksft_exit_fail_msg("%s is longer than %zu bytes\n", path,
				   len - 2);

	buf[n] = '\0';
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';
}

static void read_incoming(const char *name, char *buf, size_t len)
{
	read_sysfs(INCOMING_DIR, name, buf, len);
}

static void expect_state(const char *state, const char *expected)
{
	if (strcmp(state, expected))
		ksft_exit_fail_msg("state is '%s', expected '%s'\n", state,
				   expected);
}

static unsigned long long read_number(const char *dir, const char *name)
{
	unsigned long long val = 0;
	char buf[32];
	char *p;

	read_sysfs(dir, name, buf, sizeof(buf));
	for (p = buf; *p >= '0' && *p <= '9'; p++) {
		if (val > (ULLONG_MAX - (*p - '0')) / 10)
			break;
		val = val * 10 + (*p - '0');
	}
	if (p == buf || *p)
		ksft_exit_fail_msg("%s%s reads '%s', not a 64-bit unsigned number\n",
				   dir, name, buf);
	return val;
}

static void expect_count(const char *name, unsigned long long expected)
{
	unsigned long long val = read_number(INCOMING_DIR, name);

	if (val != expected)
		ksft_exit_fail_msg("%s is %llu, expected %llu\n", name, val,
				   expected);
}

static void expect_kho_state(const char *expected)
{
	char state[16];

	read_sysfs(KHO_INCOMING_DIR, "state", state, sizeof(state));
	if (strcmp(state, expected))
		ksft_exit_fail_msg("kho_incoming/state is '%s', expected '%s'\n",
				   state, expected);
}

static unsigned long long kho_size(const char *name)
{
	return read_number(KHO_INCOMING_DIR, name);
}

static unsigned long long expect_more_restored(unsigned long long prev)
{
	unsigned long long restored = kho_size("restored_size");

	if (restored < prev + getpagesize() ||
	    restored > kho_size("preserved_size"))
		ksft_exit_fail_msg("restored_size went from %llu to %llu\n",
				   prev, restored);
	return restored;
}

static void expect_counts(long retrieved, long dropped)
{
	char state[16];

	read_incoming("state", state, sizeof(state));
	expect_state(state, "deserialized");

	/* Three sessions, including the one carrying the test stage. */
	expect_count("sessions", 3);
	expect_count("files", 4);
	expect_count("files_retrieved", retrieved);
	expect_count("files_failed", 0);
	expect_count("files_dropped", dropped);
}

static void run_stage_1(int luo_fd)
{
	int claimed_fd, unclaimed_fd;

	ksft_print_msg("[STAGE 1] Checking state on a cold boot...\n");
	expect_state(state_before_open, "none");
	expect_count("sessions", 0);
	expect_count("files", 0);
	expect_count("liveupdate_num", 0);
	expect_kho_state("none");
	if (kho_size("preserved_size") || kho_size("restored_size"))
		ksft_exit_fail_msg("KHO sizes aren't 0 on a cold boot\n");

	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, 2);

	ksft_print_msg("[STAGE 1] Preserving two memfds in '%s'...\n",
		       SESSION_CLAIMED);
	claimed_fd = luo_create_session(luo_fd, SESSION_CLAIMED);
	if (claimed_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_CLAIMED);
	if (create_and_preserve_memfd(claimed_fd, MFD_RETRIEVED_TOKEN,
				      MFD_RETRIEVED_DATA) < 0)
		fail_exit("create_and_preserve_memfd for token %#x",
			  MFD_RETRIEVED_TOKEN);
	if (create_and_preserve_memfd(claimed_fd, MFD_DROPPED_TOKEN,
				      MFD_DROPPED_DATA) < 0)
		fail_exit("create_and_preserve_memfd for token %#x",
			  MFD_DROPPED_TOKEN);

	ksft_print_msg("[STAGE 1] Preserving one memfd in '%s'...\n",
		       SESSION_UNCLAIMED);
	unclaimed_fd = luo_create_session(luo_fd, SESSION_UNCLAIMED);
	if (unclaimed_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_UNCLAIMED);
	if (create_and_preserve_memfd(unclaimed_fd, MFD_UNCLAIMED_TOKEN,
				      MFD_UNCLAIMED_DATA) < 0)
		fail_exit("create_and_preserve_memfd for token %#x",
			  MFD_UNCLAIMED_TOKEN);

	close(luo_fd);
	daemonize_and_wait();
}

static void run_stage_2(int luo_fd, int state_session_fd)
{
	int claimed_fd, unclaimed_fd, mfd, stage;
	unsigned long long restored;

	ksft_print_msg("[STAGE 2] Checking state before and after the first open...\n");
	expect_state(state_before_open, "pending");
	expect_counts(0, 0);
	expect_count("liveupdate_num", 1);
	expect_kho_state("revived");
	if (!kho_size("preserved_size"))
		ksft_exit_fail_msg("kho_incoming/preserved_size is 0\n");

	restore_and_read_stage(state_session_fd, STATE_MEMFD_TOKEN, &stage);
	if (stage != 2)
		fail_exit("Expected stage 2, but state file contains %d", stage);

	/* The retrieve must count before the session finishes. */
	expect_counts(1, 0);

	ksft_print_msg("[STAGE 2] Retrieving one of the two memfds in '%s'...\n",
		       SESSION_CLAIMED);
	claimed_fd = luo_retrieve_session(luo_fd, SESSION_CLAIMED);
	if (claimed_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_CLAIMED);

	restored = kho_size("restored_size");
	mfd = restore_and_verify_memfd(claimed_fd, MFD_RETRIEVED_TOKEN,
				       MFD_RETRIEVED_DATA);
	if (mfd < 0)
		fail_exit("restore_and_verify_memfd for token %#x",
			  MFD_RETRIEVED_TOKEN);
	close(mfd);
	expect_counts(2, 0);
	restored = expect_more_restored(restored);

	ksft_print_msg("[STAGE 2] Finishing '%s'...\n", SESSION_CLAIMED);
	if (luo_session_finish(claimed_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_CLAIMED);
	close(claimed_fd);

	expect_counts(2, 1);
	expect_more_restored(restored);

	ksft_print_msg("[STAGE 2] Finishing the state session...\n");
	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	/* Finishing the state session must not drop anything. */
	expect_counts(2, 1);
	restored = kho_size("restored_size");

	ksft_print_msg("[STAGE 2] Retrieving the memfd in '%s'...\n",
		       SESSION_UNCLAIMED);
	unclaimed_fd = luo_retrieve_session(luo_fd, SESSION_UNCLAIMED);
	if (unclaimed_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_UNCLAIMED);
	mfd = restore_and_verify_memfd(unclaimed_fd, MFD_UNCLAIMED_TOKEN,
				       MFD_UNCLAIMED_DATA);
	if (mfd < 0)
		fail_exit("restore_and_verify_memfd for token %#x",
			  MFD_UNCLAIMED_TOKEN);
	close(mfd);
	close(unclaimed_fd);
	expect_counts(3, 1);
	expect_more_restored(restored);

	ksft_print_msg("\n--- HANDOVER STATS TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	read_incoming("state", state_before_open, sizeof(state_before_open));

	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
