// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026, Google LLC.
 * Pratyush Yadav <pratyush@kernel.org>
 *
 * Validate preservation of a tmpfs mount and a file in it across a kexec
 * reboot. Stage 1 creates a tmpfs, puts a file in it and preserves both.
 * Stage 2 retrieves the mount, attaches it with move_mount(2), and checks that
 * the file's contents are intact and that it is reachable by path.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <libliveupdate.h>

#define TEST_SESSION_NAME	"tmpfs-session"
#define TMPFS_MNT_TOKEN		0x2A
#define TMPFS_FILE_TOKEN	0x2B

/* Constants for the state-tracking mechanism, specific to this test file. */
#define STATE_SESSION_NAME	"kexec_tmpfs_state"
#define STATE_MEMFD_TOKEN	998

#define TMPFS_DIR		"/tmpfs"
#define TMPFS_FILE_NAME		"state.bin"
#define TMPFS_FILE_PATH		TMPFS_DIR "/" TMPFS_FILE_NAME
#define TMPFS_DATA		"hello tmpfs kexec world"

#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH	0x00000004
#endif

/* nolibc has no move_mount() wrapper. */
static int move_mount_empty_from(int from_fd, const char *to_path)
{
	return syscall(__NR_move_mount, from_fd, "", AT_FDCWD, to_path,
		       MOVE_MOUNT_F_EMPTY_PATH);
}

static void write_file(int fd, const char *data, size_t len)
{
	ssize_t written = write(fd, data, len);

	if (written < 0 || (size_t)written != len)
		fail_exit("write of %zu bytes returned %zd", len, written);
}

static void verify_contents(int fd, const char *expected, size_t len)
{
	char buf[128];
	ssize_t got;

	if (len > sizeof(buf))
		fail_exit("test data too large for buffer");

	/* nolibc has no pread(). */
	if (lseek(fd, 0, SEEK_SET) < 0)
		fail_exit("lseek to the start of the file");

	got = read(fd, buf, len);
	if (got < 0 || (size_t)got != len)
		fail_exit("read of %zu bytes returned %zd", len, got);

	if (memcmp(buf, expected, len))
		fail_exit("file contents do not match");
}

/* Stage 1: Executed before the kexec reboot. */
static void run_stage_1(int luo_fd)
{
	int session_fd, mnt_fd, file_fd;

	ksft_print_msg("[STAGE 1] Starting pre-kexec setup...\n");

	ksft_print_msg("[STAGE 1] Creating state file for next stage (2)...\n");
	create_state_file(luo_fd, STATE_SESSION_NAME, STATE_MEMFD_TOKEN, 2);

	session_fd = luo_create_session(luo_fd, TEST_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_create_session for '%s'", TEST_SESSION_NAME);

	ksft_print_msg("[STAGE 1] Mounting tmpfs at %s...\n", TMPFS_DIR);
	if (mkdir(TMPFS_DIR, 0755) < 0)
		fail_exit("mkdir %s", TMPFS_DIR);
	if (mount("tmpfs", TMPFS_DIR, "tmpfs", 0, NULL) < 0)
		fail_exit("mount tmpfs at %s", TMPFS_DIR);

	/*
	 * A plain open of the mount root. LUO's preserve uses fget(), which
	 * refuses O_PATH descriptors, so an fsmount(2) fd cannot be handed to
	 * it directly either.
	 */
	mnt_fd = open(TMPFS_DIR, O_RDONLY | O_DIRECTORY);
	if (mnt_fd < 0)
		fail_exit("open %s", TMPFS_DIR);

	file_fd = open(TMPFS_FILE_PATH, O_RDWR | O_CREAT, 0644);
	if (file_fd < 0)
		fail_exit("open %s", TMPFS_FILE_PATH);

	write_file(file_fd, TMPFS_DATA, sizeof(TMPFS_DATA));

	/* The mount must be preserved before any file that lives in it. */
	ksft_print_msg("[STAGE 1] Preserving mount (token %#x)...\n",
		       TMPFS_MNT_TOKEN);
	if (luo_session_preserve_fd(session_fd, mnt_fd, TMPFS_MNT_TOKEN) < 0)
		fail_exit("luo_session_preserve_fd for the mount");

	ksft_print_msg("[STAGE 1] Preserving file (token %#x)...\n",
		       TMPFS_FILE_TOKEN);
	if (luo_session_preserve_fd(session_fd, file_fd, TMPFS_FILE_TOKEN) < 0)
		fail_exit("luo_session_preserve_fd for the file");

	close(file_fd);
	close(mnt_fd);
	close(luo_fd);
	daemonize_and_wait();
}

/* Stage 2: Executed after the kexec reboot. */
static void run_stage_2(int luo_fd, int state_session_fd)
{
	int session_fd, mnt_fd, file_fd, path_fd, stage;

	ksft_print_msg("[STAGE 2] Starting post-kexec verification...\n");

	restore_and_read_stage(state_session_fd, STATE_MEMFD_TOKEN, &stage);
	if (stage != 2)
		fail_exit("Expected stage 2, but state file contains %d", stage);

	session_fd = luo_retrieve_session(luo_fd, TEST_SESSION_NAME);
	if (session_fd < 0)
		fail_exit("luo_retrieve_session for '%s'", TEST_SESSION_NAME);

	ksft_print_msg("[STAGE 2] Retrieving mount (token %#x)...\n",
		       TMPFS_MNT_TOKEN);
	mnt_fd = luo_session_retrieve_fd(session_fd, TMPFS_MNT_TOKEN);
	if (mnt_fd < 0)
		fail_exit("luo_session_retrieve_fd for the mount");

	ksft_print_msg("[STAGE 2] Attaching the restored mount at %s...\n",
		       TMPFS_DIR);
	if (mkdir(TMPFS_DIR, 0755) < 0)
		fail_exit("mkdir %s", TMPFS_DIR);
	if (move_mount_empty_from(mnt_fd, TMPFS_DIR) < 0)
		fail_exit("move_mount of the restored mount to %s", TMPFS_DIR);
	close(mnt_fd);

	ksft_print_msg("[STAGE 2] Retrieving file (token %#x)...\n",
		       TMPFS_FILE_TOKEN);
	file_fd = luo_session_retrieve_fd(session_fd, TMPFS_FILE_TOKEN);
	if (file_fd < 0)
		fail_exit("luo_session_retrieve_fd for the file");

	/* The contents are intact through the retrieved fd. */
	verify_contents(file_fd, TMPFS_DATA, sizeof(TMPFS_DATA));

	/* And the file is reachable by path, which is the point. */
	path_fd = open(TMPFS_FILE_PATH, O_RDONLY);
	if (path_fd < 0)
		fail_exit("open restored %s by path", TMPFS_FILE_PATH);

	verify_contents(path_fd, TMPFS_DATA, sizeof(TMPFS_DATA));

	close(path_fd);
	close(file_fd);

	ksft_print_msg("[STAGE 2] Test data verified successfully.\n");
	if (luo_session_finish(session_fd) < 0)
		fail_exit("luo_session_finish for test session");
	close(session_fd);

	if (luo_session_finish(state_session_fd) < 0)
		fail_exit("luo_session_finish for state session");
	close(state_session_fd);

	ksft_print_msg("\n--- TMPFS KEXEC TEST PASSED ---\n");
}

int main(int argc, char *argv[])
{
	return luo_test(argc, argv, STATE_SESSION_NAME,
			run_stage_1, run_stage_2);
}
