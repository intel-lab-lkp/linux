// SPDX-License-Identifier: GPL-2.0-or-later

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/syscall.h>
#include <limits.h>

#include "../../kselftest_harness.h"

// Needed for linux/fanotify.h
#ifndef __kernel_fsid_t
typedef struct {
	int val[2];
} __kernel_fsid_t;
#endif
#include <sys/fanotify.h>

static const char test_dir_templ[] = "/tmp/fanotify_perm_test.XXXXXX";

FIXTURE(fanotify)
{
	char test_dir[sizeof(test_dir_templ)];
	char test_dir2[sizeof(test_dir_templ)];
	int fan_fd;
	int fan_fd2;
	char test_file_path[PATH_MAX];
	char test_file_path2[PATH_MAX];
	char test_exec_path[PATH_MAX];
};

FIXTURE_SETUP(fanotify)
{
	int ret;

	/* Setup test directories and files */
	strcpy(self->test_dir, test_dir_templ);
	ASSERT_NE(mkdtemp(self->test_dir), NULL);
	strcpy(self->test_dir2, test_dir_templ);
	ASSERT_NE(mkdtemp(self->test_dir2), NULL);

	snprintf(self->test_file_path, PATH_MAX, "%s/test_file",
		 self->test_dir);
	snprintf(self->test_file_path2, PATH_MAX, "%s/test_file2",
		 self->test_dir2);
	snprintf(self->test_exec_path, PATH_MAX, "%s/test_exec",
		 self->test_dir);

	ret = open(self->test_file_path, O_CREAT | O_RDWR, 0644);
	ASSERT_GE(ret, 0);
	ASSERT_EQ(write(ret, "test data", 9), 9);
	close(ret);

	ret = open(self->test_file_path2, O_CREAT | O_RDWR, 0644);
	ASSERT_GE(ret, 0);
	ASSERT_EQ(write(ret, "test data2", 9), 9);
	close(ret);

	ret = open(self->test_exec_path, O_CREAT | O_RDWR, 0755);
	ASSERT_GE(ret, 0);
	ASSERT_EQ(write(ret, "#!/bin/bash\necho test\n", 22), 22);
	close(ret);

	self->fan_fd = fanotify_init(
		FAN_CLASS_PRE_CONTENT | FAN_NONBLOCK | FAN_CLOEXEC, O_RDONLY);
	ASSERT_GE(self->fan_fd, 0);

	self->fan_fd2 = fanotify_init(FAN_CLASS_PRE_CONTENT | FAN_NONBLOCK |
					      FAN_CLOEXEC | FAN_ENABLE_EVENT_ID,
				      O_RDONLY);
	ASSERT_GE(self->fan_fd2, 0);

	/* Mark the directories for permission events */
	ret = fanotify_mark(self->fan_fd, FAN_MARK_ADD,
			    FAN_OPEN_PERM | FAN_OPEN_EXEC_PERM |
				    FAN_EVENT_ON_CHILD,
			    AT_FDCWD, self->test_dir);
	ASSERT_EQ(ret, 0);

	ret = fanotify_mark(self->fan_fd2, FAN_MARK_ADD,
			    FAN_OPEN_PERM | FAN_OPEN_EXEC_PERM |
				    FAN_EVENT_ON_CHILD,
			    AT_FDCWD, self->test_dir2);
	ASSERT_EQ(ret, 0);
}

FIXTURE_TEARDOWN(fanotify)
{
	/* Clean up test directory and files */
	if (self->fan_fd > 0)
		close(self->fan_fd);
	if (self->fan_fd2 > 0)
		close(self->fan_fd2);

	EXPECT_EQ(unlink(self->test_file_path), 0);
	EXPECT_EQ(unlink(self->test_file_path2), 0);
	EXPECT_EQ(unlink(self->test_exec_path), 0);
	EXPECT_EQ(rmdir(self->test_dir), 0);
	EXPECT_EQ(rmdir(self->test_dir2), 0);
}

static struct fanotify_event_metadata *get_event(int fd)
{
	struct fanotify_event_metadata *metadata;
	ssize_t len;
	char buf[256];

	len = read(fd, buf, sizeof(buf));
	if (len <= 0)
		return NULL;

	metadata = (void *)buf;
	if (!FAN_EVENT_OK(metadata, len))
		return NULL;

	return metadata;
}

static int respond_to_event(int fd, struct fanotify_event_metadata *metadata,
			    uint32_t response, bool useEventId)
{
	struct fanotify_response resp;

	if (useEventId) {
		resp.event_id = metadata->event_id;
	} else {
		resp.fd = metadata->fd;
	}
	resp.response = response;

	return write(fd, &resp, sizeof(resp));
}

static void verify_event(struct __test_metadata *const _metadata,
			 struct fanotify_event_metadata *event,
			 uint64_t expect_mask, int expect_pid)
{
	ASSERT_NE(event, NULL);
	ASSERT_EQ(event->mask, expect_mask);

	if (expect_pid > 0)
		ASSERT_EQ(event->pid, expect_pid);
}

TEST_F(fanotify, open_perm_allow)
{
	struct fanotify_event_metadata *event;
	int fd, ret;
	pid_t child;

	child = fork();
	ASSERT_GE(child, 0);

	if (child == 0) {
		/* Try to open the file - this should trigger FAN_OPEN_PERM */
		fd = open(self->test_file_path, O_RDONLY);
		if (fd < 0)
			exit(EXIT_FAILURE);
		close(fd);
		exit(EXIT_SUCCESS);
	}

	usleep(100000);
	event = get_event(self->fan_fd);
	verify_event(_metadata, event, FAN_OPEN_PERM, child);

	/* Allow the open operation */
	close(event->fd);
	ret = respond_to_event(self->fan_fd, event, FAN_ALLOW,
			       false /* useEventId */);
	ASSERT_EQ(ret, sizeof(struct fanotify_response));

	int status;

	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
}

TEST_F(fanotify, open_perm_deny)
{
	struct fanotify_event_metadata *event;
	int ret;
	pid_t child;

	child = fork();
	ASSERT_GE(child, 0);

	if (child == 0) {
		/* Try to open the file - this should trigger FAN_OPEN_PERM */
		int fd = open(self->test_file_path, O_RDONLY);

		/* If open succeeded, this is an error as we expect it to be denied */
		if (fd >= 0) {
			close(fd);
			exit(EXIT_FAILURE);
		}

		/* Verify the expected error */
		if (errno == EPERM)
			exit(EXIT_SUCCESS);

		exit(EXIT_FAILURE);
	}

	usleep(100000);
	event = get_event(self->fan_fd);
	verify_event(_metadata, event, FAN_OPEN_PERM, child);

	/* Deny the open operation */
	close(event->fd);
	ret = respond_to_event(self->fan_fd, event, FAN_DENY,
			       false /* useEventId */);
	ASSERT_EQ(ret, sizeof(struct fanotify_response));

	int status;

	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
}

TEST_F(fanotify, exec_perm_allow)
{
	struct fanotify_event_metadata *event;
	int ret;
	pid_t child;

	child = fork();
	ASSERT_GE(child, 0);

	if (child == 0) {
		/* Try to execute the file - this should trigger FAN_OPEN_EXEC_PERM */
		execl(self->test_exec_path, "test_exec", NULL);

		/* If we get here, execl failed */
		exit(EXIT_FAILURE);
	}

	usleep(100000);
	event = get_event(self->fan_fd);
	verify_event(_metadata, event, FAN_OPEN_EXEC_PERM, child);

	/* Allow the exec operation + ignore subsequent events */
	ASSERT_GE(fanotify_mark(self->fan_fd,
				FAN_MARK_ADD | FAN_MARK_IGNORED_MASK |
					FAN_MARK_IGNORED_SURV_MODIFY,
				FAN_OPEN_PERM | FAN_OPEN_EXEC_PERM, event->fd,
				NULL),
		  0);
	close(event->fd);
	ret = respond_to_event(self->fan_fd, event, FAN_ALLOW,
			       false /* useEventId */);
	ASSERT_EQ(ret, sizeof(struct fanotify_response));

	int status;

	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_EQ(WIFEXITED(status), 1);
}

TEST_F(fanotify, exec_perm_deny)
{
	struct fanotify_event_metadata *event;
	int ret;
	pid_t child;

	child = fork();
	ASSERT_GE(child, 0);

	if (child == 0) {
		/* Try to execute the file - this should trigger FAN_OPEN_EXEC_PERM */
		execl(self->test_exec_path, "test_exec", NULL);

		/* If execl failed with EPERM, that's what we expect */
		if (errno == EPERM)
			exit(EXIT_SUCCESS);

		exit(EXIT_FAILURE);
	}

	usleep(100000);
	event = get_event(self->fan_fd);
	verify_event(_metadata, event, FAN_OPEN_EXEC_PERM, child);

	/* Deny the exec operation */
	close(event->fd);
	ret = respond_to_event(self->fan_fd, event, FAN_DENY,
			       false /* useEventId */);
	ASSERT_EQ(ret, sizeof(struct fanotify_response));

	int status;

	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
}

TEST_F(fanotify, default_response)
{
	struct fanotify_event_metadata *event;
	int ret;
	pid_t child;
	struct fanotify_response resp;

	/* Set default response to deny */
	resp.fd = FAN_NOFD;
	resp.response = FAN_DENY | FAN_DEFAULT;
	ret = write(self->fan_fd, &resp, sizeof(resp));
	ASSERT_EQ(ret, sizeof(resp));

	child = fork();
	ASSERT_GE(child, 0);

	if (child == 0) {
		close(self->fan_fd);
		/* Try to open the file - this should trigger FAN_OPEN_PERM */
		int fd = open(self->test_file_path, O_RDONLY);

		/* If open succeeded, this is an error as we expect it to be denied */
		if (fd >= 0) {
			close(fd);
			exit(EXIT_FAILURE);
		}

		/* Verify the expected error */
		if (errno == EPERM)
			exit(EXIT_SUCCESS);

		exit(EXIT_FAILURE);
	}

	usleep(100000);
	event = get_event(self->fan_fd);
	verify_event(_metadata, event, FAN_OPEN_PERM, child);

	/* Close fanotify group to return default response (DENY) */
	close(self->fan_fd);
	self->fan_fd = -1;

	int status;

	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
}

TEST_F(fanotify, respond_via_event_id)
{
	struct fanotify_event_metadata *event;
	int fd, ret;
	pid_t child;

	child = fork();
	ASSERT_GE(child, 0);

	if (child == 0) {
		/* Try to open the file - this should trigger FAN_OPEN_PERM */
		fd = open(self->test_file_path2, O_RDONLY);
		if (fd < 0)
			exit(EXIT_FAILURE);
		close(fd);
		exit(EXIT_SUCCESS);
	}

	usleep(100000);
	event = get_event(self->fan_fd2);
	verify_event(_metadata, event, FAN_OPEN_PERM, child);
	ASSERT_EQ(event->event_id, 1);

	/* Allow the open operation */
	close(event->fd);
	ret = respond_to_event(self->fan_fd2, event, FAN_ALLOW,
			       true /* useEventId */);
	ASSERT_EQ(ret, sizeof(struct fanotify_response));

	int status;

	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
}

TEST_HARNESS_MAIN
