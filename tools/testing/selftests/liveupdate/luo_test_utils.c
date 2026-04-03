// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2025-2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 * Pratyush Yadav (Google) <pratyush@kernel.org>
 *
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <ptyadav@amazon.de>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/vfs.h>
#include <linux/magic.h>

#include "luo_test_utils.h"

int cwd_is_tmpfs(void)
{
	struct statfs buf;

	if (statfs(".", &buf) < 0)
		return -errno;

	return buf.f_type == TMPFS_MAGIC;
}

/* Read exactly specified size from fd. Any less results in error. */
int read_size(int fd, char *buffer, size_t size)
{
	size_t remain = size;
	ssize_t bytes_read;

	while (remain) {
		bytes_read = read(fd, buffer, remain);
		if (bytes_read == 0)
			return -ENODATA;
		if (bytes_read < 0)
			return -errno;

		remain -= bytes_read;
		buffer += bytes_read;
	}

	return 0;
}

/* Write exactly specified size from fd. Any less results in error. */
int write_size(int fd, const char *buffer, size_t size)
{
	size_t remain = size;
	ssize_t written;

	while (remain) {
		written = write(fd, buffer, remain);
		if (written == 0)
			return -EIO;
		if (written < 0)
			return -errno;

		remain -= written;
		buffer += written;
	}

	return 0;
}

int generate_random_data(char *buffer, size_t size)
{
	int fd, ret;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -errno;

	ret = read_size(fd, buffer, size);
	close(fd);
	return ret;
}

int save_test_data(const char *filename, const char *buffer, size_t size)
{
	int fd, ret;

	fd = open(filename, O_RDWR | O_CREAT, 0666);
	if (fd < 0)
		return -errno;

	ret = write_size(fd, buffer, size);
	fsync(fd);
	close(fd);
	return ret;
}

int load_test_data(const char *filename, char *buffer, size_t size)
{
	int fd, ret;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -errno;

	ret = read_size(fd, buffer, size);
	close(fd);
	return ret;
}

/* Create and initialize a memfd with random data. */
int create_random_memfd(const char *memfd_name, char *buffer, size_t size)
{
	int fd;
	int ret;

	fd = memfd_create(memfd_name, 0);
	if (fd < 0)
		return -errno;

	ret = generate_random_data(buffer, size);
	if (ret < 0) {
		close(fd);
		return ret;
	}

	if (write_size(fd, buffer, size) < 0) {
		close(fd);
		return -errno;
	}

	/* Reset file position to beginning */
	if (lseek(fd, 0, SEEK_SET) < 0) {
		close(fd);
		return -errno;
	}

	return fd;
}

/*
 * Make sure fd contains expected data up to size. Returns 0 on success, 1 on
 * data mismatch, -errno on error.
 */
int verify_fd_content_read(int fd, const char *expected_data, size_t size)
{
	char *buffer;
	int ret;

	buffer = malloc(size);
	if (!buffer)
		return -ENOMEM;

	/* Reset file position to beginning */
	if (lseek(fd, 0, SEEK_SET) < 0) {
		ret = -errno;
		goto out;
	}

	ret = read_size(fd, buffer, size);
	if (ret < 0)
		goto out;

	if (memcmp(buffer, expected_data, size) != 0) {
		ret = 1;
		goto out;
	}

	ret = 0;

out:
	free(buffer);
	return ret;
}

/*
 * Verify fd content using mmap. Returns 0 on success, 1 on data mismatch,
 * -errno on error.
 */
int verify_fd_content_mmap(int fd, const char *expected_data, size_t size)
{
	char *mapped_mem;
	int ret;

	mapped_mem = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (mapped_mem == MAP_FAILED)
		return -errno;

	/* ret = memcmp(mapped_mem, expected_data, size) ? 1 : 0; */
	ret = 0;
	for (size_t i = 0; i < size; i++) {
		if (mapped_mem[i] != expected_data[i]) {
			ret = 1;
			break;
		}
	}

	munmap(mapped_mem, size);
	return ret;
}

int luo_open_device(void)
{
	return open(LUO_DEVICE, O_RDWR);
}

int luo_create_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_create_session arg = { .size = sizeof(arg) };

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);

	if (ioctl(luo_fd, LIVEUPDATE_IOCTL_CREATE_SESSION, &arg) < 0)
		return -errno;

	return arg.fd;
}

int luo_retrieve_session(int luo_fd, const char *name)
{
	struct liveupdate_ioctl_retrieve_session arg = { .size = sizeof(arg) };

	snprintf((char *)arg.name, LIVEUPDATE_SESSION_NAME_LENGTH, "%.*s",
		 LIVEUPDATE_SESSION_NAME_LENGTH - 1, name);

	if (ioctl(luo_fd, LIVEUPDATE_IOCTL_RETRIEVE_SESSION, &arg) < 0)
		return -errno;

	return arg.fd;
}

int create_and_preserve_memfd(int session_fd, int token, const char *data)
{
	struct liveupdate_session_preserve_fd arg = { .size = sizeof(arg) };
	long page_size = sysconf(_SC_PAGE_SIZE);
	void *map = MAP_FAILED;
	int mfd = -1, ret = -1;

	mfd = memfd_create("test_mfd", 0);
	if (mfd < 0)
		return -errno;

	if (ftruncate(mfd, page_size) != 0)
		goto out;

	map = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, mfd, 0);
	if (map == MAP_FAILED)
		goto out;

	snprintf(map, page_size, "%s", data);
	munmap(map, page_size);

	arg.fd = mfd;
	arg.token = token;
	if (ioctl(session_fd, LIVEUPDATE_SESSION_PRESERVE_FD, &arg) < 0)
		goto out;

	ret = 0;
out:
	if (ret != 0 && errno != 0)
		ret = -errno;
	if (mfd >= 0)
		close(mfd);
	return ret;
}

int restore_and_verify_memfd(int session_fd, int token,
			     const char *expected_data)
{
	struct liveupdate_session_retrieve_fd arg = { .size = sizeof(arg) };
	long page_size = sysconf(_SC_PAGE_SIZE);
	void *map = MAP_FAILED;
	int mfd = -1, ret = -1;

	arg.token = token;
	if (ioctl(session_fd, LIVEUPDATE_SESSION_RETRIEVE_FD, &arg) < 0)
		return -errno;
	mfd = arg.fd;

	map = mmap(NULL, page_size, PROT_READ, MAP_SHARED, mfd, 0);
	if (map == MAP_FAILED)
		goto out;

	if (expected_data && strcmp(expected_data, map) != 0) {
		ksft_print_msg("Data mismatch! Expected '%s', Got '%s'\n",
			       expected_data, (char *)map);
		ret = -EINVAL;
		goto out_munmap;
	}

	ret = mfd;
out_munmap:
	munmap(map, page_size);
out:
	if (ret < 0 && errno != 0)
		ret = -errno;
	if (ret < 0 && mfd >= 0)
		close(mfd);
	return ret;
}

int luo_session_finish(int session_fd)
{
	struct liveupdate_session_finish arg = { .size = sizeof(arg) };

	if (ioctl(session_fd, LIVEUPDATE_SESSION_FINISH, &arg) < 0)
		return -errno;

	return 0;
}

void create_state_file(int luo_fd, const char *session_name, int token,
		       int next_stage)
{
	char buf[32];
	int state_session_fd;

	state_session_fd = luo_create_session(luo_fd, session_name);
	if (state_session_fd < 0)
		fail_exit("luo_create_session for state tracking");

	snprintf(buf, sizeof(buf), "%d", next_stage);
	if (create_and_preserve_memfd(state_session_fd, token, buf) < 0)
		fail_exit("create_and_preserve_memfd for state tracking");

	/*
	 * DO NOT close session FD, otherwise it is going to be unpreserved
	 */
}

void restore_and_read_stage(int state_session_fd, int token, int *stage)
{
	char buf[32] = {0};
	int mfd;

	mfd = restore_and_verify_memfd(state_session_fd, token, NULL);
	if (mfd < 0)
		fail_exit("failed to restore state memfd");

	if (read(mfd, buf, sizeof(buf) - 1) < 0)
		fail_exit("failed to read state mfd");

	*stage = atoi(buf);

	close(mfd);
}

void daemonize_and_wait(void)
{
	pid_t pid;

	ksft_print_msg("[STAGE 1] Forking persistent child to hold sessions...\n");

	pid = fork();
	if (pid < 0)
		fail_exit("fork failed");

	if (pid > 0) {
		ksft_print_msg("[STAGE 1] Child PID: %d. Resources are pinned.\n", pid);
		ksft_print_msg("[STAGE 1] You may now perform kexec reboot.\n");
		exit(EXIT_SUCCESS);
	}

	/* Detach from terminal so closing the window doesn't kill us */
	if (setsid() < 0)
		fail_exit("setsid failed");

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	/* Change dir to root to avoid locking filesystems */
	if (chdir("/") < 0)
		exit(EXIT_FAILURE);

	while (1)
		sleep(60);
}

static int parse_stage_args(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"stage", required_argument, 0, 's'},
		{0, 0, 0, 0}
	};
	int option_index = 0;
	int stage = 1;
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "s:", long_options, &option_index)) != -1) {
		switch (opt) {
		case 's':
			stage = atoi(optarg);
			if (stage != 1 && stage != 2)
				fail_exit("Invalid stage argument");
			break;
		default:
			fail_exit("Unknown argument");
		}
	}
	return stage;
}

int luo_test(int argc, char *argv[],
	     const char *state_session_name,
	     luo_test_stage1_fn stage1,
	     luo_test_stage2_fn stage2)
{
	int target_stage = parse_stage_args(argc, argv);
	int luo_fd = luo_open_device();
	int state_session_fd;
	int detected_stage;

	if (luo_fd < 0) {
		ksft_exit_skip("Failed to open %s. Is the luo module loaded?\n",
			       LUO_DEVICE);
	}

	state_session_fd = luo_retrieve_session(luo_fd, state_session_name);
	if (state_session_fd == -ENOENT)
		detected_stage = 1;
	else if (state_session_fd >= 0)
		detected_stage = 2;
	else
		fail_exit("Failed to check for state session");

	if (target_stage != detected_stage) {
		ksft_exit_fail_msg("Stage mismatch Requested --stage %d, but system is in stage %d.\n"
				   "(State session %s: %s)\n",
				   target_stage, detected_stage, state_session_name,
				   (detected_stage == 2) ? "EXISTS" : "MISSING");
	}

	if (target_stage == 1)
		stage1(luo_fd);
	else
		stage2(luo_fd, state_session_fd);

	return 0;
}
