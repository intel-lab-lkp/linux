// SPDX-License-Identifier: GPL-2.0-only
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

/*
 * Copyright (c) 2025, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * A selftest to validate the end-to-end lifecycle of multiple LUO sessions
 * across a kexec reboot, including empty sessions and sessions with multiple
 * files.
 */

#include <libliveupdate.h>

#define SESSION_EMPTY_1 "multi-test-empty-1"
#define SESSION_EMPTY_2 "multi-test-empty-2"
#define SESSION_FILES_1 "multi-test-files-1"
#define SESSION_FILES_2 "multi-test-files-2"

#define MFD1_TOKEN 0x1001
#define MFD2_TOKEN 0x2002
#define MFD3_TOKEN 0x3003

#define MFD1_DATA "Data for session files 1"
#define MFD2_DATA "First file for session files 2"
#define MFD3_DATA "Second file for session files 2"

#define STATE_SESSION_NAME "kexec_multi_state"
#define STATE_MEMFD_TOKEN 998

/* Stage 1: Executed before the kexec reboot. */
static void run_stage_1(int luo_fd)
{
	int s_empty1_fd, s_empty2_fd, s_files1_fd, s_files2_fd;
	int tmpfs_fd;
	void *map;

	ksft_print_msg("[STAGE 1] Starting pre-kexec setup for multi-session test...\n");

	ksft_print_msg("[STAGE 1] Creating state file for next stage (2)...\n");
	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, 2);

	ksft_print_msg("[STAGE 1] Creating empty sessions '%s' and '%s'...\n",
		       SESSION_EMPTY_1, SESSION_EMPTY_2);
	s_empty1_fd = luo_create_session(luo_fd, SESSION_EMPTY_1);
	if (s_empty1_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_EMPTY_1);

	s_empty2_fd = luo_create_session(luo_fd, SESSION_EMPTY_2);
	if (s_empty2_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_EMPTY_2);

	ksft_print_msg("[STAGE 1] Creating session '%s' with one memfd...\n",
		       SESSION_FILES_1);

	s_files1_fd = luo_create_session(luo_fd, SESSION_FILES_1);
	if (s_files1_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_FILES_1);
	ksft_print_msg("  -> Preserving memfd (token %#x, content: \"%s\")\n",
		       MFD1_TOKEN, MFD1_DATA);
	if (create_and_preserve_memfd(s_files1_fd, MFD1_TOKEN, MFD1_DATA) < 0) {
		fail_exit("create_and_preserve_memfd for token %#x",
			  MFD1_TOKEN);
	}

	ksft_print_msg("[STAGE 1] Creating session '%s' with memfd and tmpfs file...\n",
		       SESSION_FILES_2);

	s_files2_fd = luo_create_session(luo_fd, SESSION_FILES_2);
	if (s_files2_fd < 0)
		fail_exit("luo_create_session for '%s'", SESSION_FILES_2);
	ksft_print_msg("  -> Preserving memfd (token %#x, content: \"%s\")\n",
		       MFD2_TOKEN, MFD2_DATA);
	if (create_and_preserve_memfd(s_files2_fd, MFD2_TOKEN, MFD2_DATA) < 0) {
		fail_exit("create_and_preserve_memfd for token %#x",
			  MFD2_TOKEN);
	}
	ksft_print_msg("  -> Populating custom tmpfs file at /mnt/mfd3_source...\n");
	mkdir("/mnt", 0777);
	mount("tmpfs", "/mnt", "tmpfs", 0, NULL);
	tmpfs_fd = open("/mnt/mfd3_source", O_CREAT | O_RDWR, 0666);
	if (tmpfs_fd < 0)
		fail_exit("failed to open tmpfs_fd");
	ksft_print_msg("  -> Preserving generic tmpfs file (token %#x, content: \"%s\")\n",
		       MFD3_TOKEN, MFD3_DATA);
	if (ftruncate(tmpfs_fd, getpagesize()) < 0)
		fail_exit("ftruncate tmpfs_fd");
	map = mmap(NULL, getpagesize(), PROT_WRITE, MAP_SHARED, tmpfs_fd, 0);
	if (map == MAP_FAILED)
		fail_exit("mmap tmpfs_fd");
	strcpy(map, MFD3_DATA);
	munmap(map, getpagesize());

	struct liveupdate_session_preserve_fd preserve_args = {
		.size = sizeof(preserve_args),
		.token = MFD3_TOKEN,
		.fd = tmpfs_fd,
	};
	if (ioctl(s_files2_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &preserve_args) < 0)
		fail_exit("preserve tmpfs fd");

	close(tmpfs_fd);

	close(luo_fd);
	daemonize_and_wait();
}

/* Stage 2: Executed after the kexec reboot. */
static void run_stage_2(int luo_fd, int state_session_fd)
{
	int s_empty1_fd, s_empty2_fd, s_files1_fd, s_files2_fd;
	int mfd1, mfd2, mfd3, stage;

	ksft_print_msg("[STAGE 2] Starting post-kexec verification...\n");

	restore_and_read_stage(state_session_fd, STATE_MEMFD_TOKEN, &stage);
	if (stage != 2) {
		fail_exit("Expected stage 2, but state file contains %d",
			  stage);
	}

	ksft_print_msg("[STAGE 2] Retrieving all sessions...\n");
	s_empty1_fd = luo_retrieve_session(luo_fd, SESSION_EMPTY_1);
	if (s_empty1_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_EMPTY_1);

	s_empty2_fd = luo_retrieve_session(luo_fd, SESSION_EMPTY_2);
	if (s_empty2_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_EMPTY_2);

	s_files1_fd = luo_retrieve_session(luo_fd, SESSION_FILES_1);
	if (s_files1_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_FILES_1);

	s_files2_fd = luo_retrieve_session(luo_fd, SESSION_FILES_2);
	if (s_files2_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", SESSION_FILES_2);

	ksft_print_msg("[STAGE 2] Verifying contents of session '%s'...\n",
		       SESSION_FILES_1);
	ksft_print_msg("  -> Retrieving memfd (token %#x) from session...\n", MFD1_TOKEN);
	mfd1 = restore_and_verify_memfd(s_files1_fd, MFD1_TOKEN, MFD1_DATA);
	if (mfd1 < 0)
		fail_exit("restore_and_verify_memfd for token %#x", MFD1_TOKEN);
	close(mfd1);

	ksft_print_msg("[STAGE 2] Verifying contents of session '%s'...\n",
		       SESSION_FILES_2);

	ksft_print_msg("  -> Retrieving memfd (token %#x) from session...\n", MFD2_TOKEN);
	mfd2 = restore_and_verify_memfd(s_files2_fd, MFD2_TOKEN, MFD2_DATA);
	if (mfd2 < 0)
		fail_exit("restore_and_verify_memfd for token %#x", MFD2_TOKEN);
	close(mfd2);

	ksft_print_msg("  -> Creating empty tmpfs file at /mnt/mfd3_target...\n");
	mkdir("/mnt", 0777);
	mount("tmpfs", "/mnt", "tmpfs", 0, NULL);
	mfd3 = open("/mnt/mfd3_target", O_CREAT | O_RDWR, 0666);
	if (mfd3 < 0)
		fail_exit("open tmpfs target for retrieve_into");

	ksft_print_msg("  -> Validating LIVEUPDATE_SESSION_RETRIEVE_INTO_FD (token %#x)...\n",
		       MFD3_TOKEN);
	if (luo_verify_retrieve_into_memfd(s_files2_fd, MFD3_TOKEN, mfd3, MFD3_DATA) < 0)
		fail_exit("luo_verify_retrieve_into_memfd for token %#x", MFD3_TOKEN);
	close(mfd3);

	ksft_print_msg("[STAGE 2] Test data verified successfully.\n");

	ksft_print_msg("[STAGE 2] Finalizing all test sessions...\n");
	if (luo_session_finish(s_empty1_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_EMPTY_1);
	close(s_empty1_fd);

	if (luo_session_finish(s_empty2_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_EMPTY_2);
	close(s_empty2_fd);

	if (luo_session_finish(s_files1_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_FILES_1);
	close(s_files1_fd);

	if (luo_session_finish(s_files2_fd) < 0)
		fail_exit("luo_session_finish for '%s'", SESSION_FILES_2);
	close(s_files2_fd);

	ksft_print_msg("[STAGE 2] Finalizing state session...\n");
	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	ksft_print_msg("\n--- MULTI-SESSION KEXEC TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
