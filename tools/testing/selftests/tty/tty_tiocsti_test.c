// SPDX-License-Identifier: GPL-2.0
/*
 * TTY Tests - TIOCSTI
 *
 * Copyright © 2025 Abhinav Saxena <xandfury@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <pwd.h>
#include <termios.h>
#include <grp.h>
#include <sys/capability.h>
#include <sys/prctl.h>

#include "../kselftest_harness.h"

/* Helper function to send FD via SCM_RIGHTS */
static int send_fd_via_socket(int socket_fd, int fd_to_send)
{
	struct msghdr msg = { 0 };
	struct cmsghdr *cmsg;
	char cmsg_buf[CMSG_SPACE(sizeof(int))];
	char dummy_data = 'F';
	struct iovec iov = { .iov_base = &dummy_data, .iov_len = 1 };

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf;
	msg.msg_controllen = sizeof(cmsg_buf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));

	memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

	return sendmsg(socket_fd, &msg, 0) < 0 ? -1 : 0;
}

/* Helper function to receive FD via SCM_RIGHTS */
static int recv_fd_via_socket(int socket_fd)
{
	struct msghdr msg = { 0 };
	struct cmsghdr *cmsg;
	char cmsg_buf[CMSG_SPACE(sizeof(int))];
	char dummy_data;
	struct iovec iov = { .iov_base = &dummy_data, .iov_len = 1 };
	int received_fd = -1;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf;
	msg.msg_controllen = sizeof(cmsg_buf);

	if (recvmsg(socket_fd, &msg, 0) < 0)
		return -1;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_RIGHTS) {
			memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
			break;
		}
	}

	return received_fd;
}

static inline bool has_cap_sys_admin(void)
{
	cap_t caps = cap_get_proc();

	if (!caps)
		return false;

	cap_flag_value_t cap_val;
	bool has_cap = (cap_get_flag(caps, CAP_SYS_ADMIN, CAP_EFFECTIVE,
				     &cap_val) == 0) &&
		       (cap_val == CAP_SET);

	cap_free(caps);
	return has_cap;
}

/*
 * Simple privilege drop that just changes uid/gid in current process
 * and also capabilities like CAP_SYS_ADMIN
 */
static inline bool drop_to_nobody(void)
{
	/* Drop supplementary groups */
	if (setgroups(0, NULL) != 0) {
		printf("setgroups failed: %s", strerror(errno));
		return false;
	}

	/* Change group to nobody */
	if (setgid(65534) != 0) {
		printf("setgid failed: %s", strerror(errno));
		return false;
	}

	/* Change user to nobody (this drops capabilities) */
	if (setuid(65534) != 0) {
		printf("setuid failed: %s", strerror(errno));
		return false;
	}

	/* Verify we no longer have CAP_SYS_ADMIN */
	if (has_cap_sys_admin()) {
		printf("ERROR: Still have CAP_SYS_ADMIN after changing to nobody");
		return false;
	}

	printf("Successfully changed to nobody (uid:%d gid:%d)\n", getuid(),
	       getgid());
	return true;
}

static inline int get_legacy_tiocsti_setting(void)
{
	FILE *fp;
	int value = -1;

	fp = fopen("/proc/sys/dev/tty/legacy_tiocsti", "r");
	if (!fp) {
		if (errno == ENOENT) {
			printf("legacy_tiocsti sysctl not available (kernel < 6.2)\n");
		} else {
			printf("Cannot read legacy_tiocsti: %s\n",
			       strerror(errno));
		}
		return -1;
	}

	if (fscanf(fp, "%d", &value) == 1) {
		printf("legacy_tiocsti setting=%d\n", value);

		if (value < 0 || value > 1) {
			printf("legacy_tiocsti unexpected value %d\n", value);
			value = -1;
		} else {
			printf("legacy_tiocsti=%d (%s mode)\n", value,
			       value == 0 ? "restricted" : "permissive");
		}
	} else {
		printf("Failed to parse legacy_tiocsti value");
		value = -1;
	}

	fclose(fp);
	return value;
}

static inline int test_tiocsti_injection(int fd)
{
	int ret;
	char test_char = 'X';

	ret = ioctl(fd, TIOCSTI, &test_char);
	if (ret == 0) {
		/* Clear the injected character */
		printf("TIOCSTI injection succeeded\n");
	} else {
		printf("TIOCSTI injection failed: %s (errno=%d)\n",
		       strerror(errno), errno);
	}
	return ret == 0 ? 0 : -1;
}

FIXTURE(tty_tiocsti)
{
	int tty_fd;
	char *tty_name;
	bool has_tty;
	bool initial_cap_sys_admin;
	int legacy_tiocsti_setting;
};

FIXTURE_SETUP(tty_tiocsti)
{
	TH_LOG("Running as UID: %d with effective UID: %d", getuid(),
	       geteuid());

	self->tty_fd = open("/dev/tty", O_RDWR);
	self->has_tty = (self->tty_fd >= 0);

	if (self->tty_fd < 0)
		TH_LOG("Cannot open /dev/tty: %s", strerror(errno));

	self->tty_name = ttyname(STDIN_FILENO);
	TH_LOG("Current TTY: %s", self->tty_name ? self->tty_name : "none");

	self->initial_cap_sys_admin = has_cap_sys_admin();
	TH_LOG("Initial CAP_SYS_ADMIN: %s",
	       self->initial_cap_sys_admin ? "yes" : "no");

	self->legacy_tiocsti_setting = get_legacy_tiocsti_setting();
}

FIXTURE_TEARDOWN(tty_tiocsti)
{
	if (self->has_tty && self->tty_fd >= 0)
		close(self->tty_fd);
}

/* Test case 1: legacy_tiocsti != 0 (permissive mode) */
TEST_F(tty_tiocsti, permissive_mode)
{
	// clang-format off
	if (self->legacy_tiocsti_setting < 0)
		SKIP(return,
		     "legacy_tiocsti sysctl not available (kernel < 6.2)");

	if (self->legacy_tiocsti_setting == 0)
		SKIP(return,
		     "Test requires permissive mode (legacy_tiocsti=1)");
	// clang-format on

	ASSERT_TRUE(self->has_tty);

	if (self->initial_cap_sys_admin) {
		ASSERT_TRUE(drop_to_nobody());
		ASSERT_FALSE(has_cap_sys_admin());
	}

	/* In permissive mode, TIOCSTI should work without CAP_SYS_ADMIN */
	EXPECT_EQ(test_tiocsti_injection(self->tty_fd), 0)
	{
		TH_LOG("TIOCSTI should succeed in permissive mode without CAP_SYS_ADMIN");
	}
}

/* Test case 2: legacy_tiocsti == 0, without CAP_SYS_ADMIN (should fail) */
TEST_F(tty_tiocsti, restricted_mode_nopriv)
{
	// clang-format off
	if (self->legacy_tiocsti_setting < 0)
		SKIP(return,
		     "legacy_tiocsti sysctl not available (kernel < 6.2)");

	if (self->legacy_tiocsti_setting != 0)
		SKIP(return,
		     "Test requires restricted mode (legacy_tiocsti=0)");
	// clang-format on

	ASSERT_TRUE(self->has_tty);

	if (self->initial_cap_sys_admin) {
		ASSERT_TRUE(drop_to_nobody());
		ASSERT_FALSE(has_cap_sys_admin());
	}
	/* In restricted mode, TIOCSTI should fail without CAP_SYS_ADMIN */
	EXPECT_EQ(test_tiocsti_injection(self->tty_fd), -1);

	/*
	 * it might fail with either EPERM or EIO
	 * EXPECT_TRUE(errno == EPERM || errno == EIO)
	 * {
	 *      TH_LOG("Expected EPERM, got: %s", strerror(errno));
	 * }
	 */
}

/* Test case 3: legacy_tiocsti == 0, with CAP_SYS_ADMIN (should succeed) */
TEST_F(tty_tiocsti, restricted_mode_priv)
{
	// clang-format off
	if (self->legacy_tiocsti_setting < 0)
		SKIP(return,
		     "legacy_tiocsti sysctl not available (kernel < 6.2)");

	if (self->legacy_tiocsti_setting != 0)
		SKIP(return,
		     "Test requires restricted mode (legacy_tiocsti=0)");
	// clang-format on

	/* Must have CAP_SYS_ADMIN for this test */
	if (!self->initial_cap_sys_admin)
		SKIP(return, "Test requires CAP_SYS_ADMIN");

	ASSERT_TRUE(self->has_tty);
	ASSERT_TRUE(has_cap_sys_admin());

	/* In restricted mode, TIOCSTI should succeed with CAP_SYS_ADMIN */
	EXPECT_EQ(test_tiocsti_injection(self->tty_fd), 0)
	{
		TH_LOG("TIOCSTI should succeed in restricted mode with CAP_SYS_ADMIN");
	}
}

/* Test TIOCSTI security with file descriptor passing */
TEST_F(tty_tiocsti, fd_passing_security)
{
	// clang-format off
	if (self->legacy_tiocsti_setting < 0)
		SKIP(return,
		     "legacy_tiocsti sysctl not available (kernel < 6.2)");

	if (self->legacy_tiocsti_setting != 0)
		SKIP(return,
		     "Test requires restricted mode (legacy_tiocsti=0)");
	// clang-format on

	/* Must start with CAP_SYS_ADMIN */
	if (!self->initial_cap_sys_admin)
		SKIP(return, "Test requires initial CAP_SYS_ADMIN");

	int sockpair[2];
	pid_t child_pid;

	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockpair), 0);

	child_pid = fork();
	ASSERT_GE(child_pid, 0)
	TH_LOG("Fork failed: %s", strerror(errno));

	if (child_pid == 0) {
		/* Child process - become unprivileged, open TTY, send FD to parent */
		close(sockpair[0]);

		TH_LOG("Child: Dropping privileges...");

		/* Drop to nobody user (loses all capabilities) */
		drop_to_nobody();

		/* Verify we no longer have CAP_SYS_ADMIN */
		if (has_cap_sys_admin()) {
			TH_LOG("Child: Failed to drop CAP_SYS_ADMIN");
			_exit(1);
		}

		TH_LOG("Child: Opening TTY as unprivileged user...");

		int unprivileged_tty_fd = open("/dev/tty", O_RDWR);

		if (unprivileged_tty_fd < 0) {
			TH_LOG("Child: Cannot open TTY: %s", strerror(errno));
			_exit(1);
		}

		/* Test that we can't use TIOCSTI directly (should fail) */

		char test_char = 'X';

		if (ioctl(unprivileged_tty_fd, TIOCSTI, &test_char) == 0) {
			TH_LOG("Child: ERROR - Direct TIOCSTI succeeded unexpectedly!");
			close(unprivileged_tty_fd);
			_exit(1);
		}
		TH_LOG("Child: Good - Direct TIOCSTI failed as expected: %s",
		       strerror(errno));

		/* Send the TTY FD to privileged parent via SCM_RIGHTS */
		TH_LOG("Child: Sending TTY FD to privileged parent...");
		if (send_fd_via_socket(sockpair[1], unprivileged_tty_fd) != 0) {
			TH_LOG("Child: Failed to send FD");
			close(unprivileged_tty_fd);
			_exit(1);
		}

		close(unprivileged_tty_fd);
		close(sockpair[1]);
		_exit(0); /* Child success */

	} else {
		/* Parent process - keep CAP_SYS_ADMIN, receive FD, test TIOCSTI */
		close(sockpair[1]);

		TH_LOG("Parent: Waiting for TTY FD from unprivileged child...");

		/* Verify we still have CAP_SYS_ADMIN */
		ASSERT_TRUE(has_cap_sys_admin());

		/* Receive the TTY FD from unprivileged child */
		int received_fd = recv_fd_via_socket(sockpair[0]);

		ASSERT_GE(received_fd, 0)
		TH_LOG("Parent: Received FD %d (opened by unprivileged process)",
		       received_fd);

		/*
		 * VULNERABILITY TEST: Try TIOCSTI with FD opened by unprivileged process
		 * This should FAIL even though parent has CAP_SYS_ADMIN
		 * because the FD was opened by unprivileged process
		 */
		char attack_char = 'V'; /* V for Vulnerability */
		int ret = ioctl(received_fd, TIOCSTI, &attack_char);

		TH_LOG("Parent: Testing TIOCSTI on FD from unprivileged process...");
		if (ret == 0) {
			TH_LOG("*** VULNERABILITY DETECTED ***");
			TH_LOG("Privileged process can use TIOCSTI on unprivileged FD");
		} else {
			TH_LOG("TIOCSTI failed on unprivileged FD: %s",
			       strerror(errno));
			EXPECT_EQ(errno, EPERM);
		}
		close(received_fd);
		close(sockpair[0]);

		/* Wait for child */
		int status;

		ASSERT_EQ(waitpid(child_pid, &status, 0), child_pid);
		EXPECT_EQ(WEXITSTATUS(status), 0);
		ASSERT_NE(ret, 0);
	}
}

TEST_HARNESS_MAIN
