// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <asm/ioctls.h>

#include "kselftest.h"

#ifndef TIOCGPTPEER
int main(void)
{
	ksft_exit_skip("TIOCGPTPEER is unavailable\n");
}
#else
static int expect_no_master_hup(int master)
{
	struct pollfd pfd = { .fd = master, .events = POLLIN | POLLOUT };
	int ret;

	ret = poll(&pfd, 1, 0);
	if (ret < 0 || (pfd.revents & POLLHUP)) {
		ksft_print_msg("master poll returned %d (revents %#x)\n",
			       ret, pfd.revents);
		return -1;
	}
	return 0;
}

static int expect_master_hup(int master)
{
	struct pollfd pfd = { .fd = master, .events = POLLIN | POLLOUT };
	int ret;

	ret = poll(&pfd, 1, 0);
	if (ret != 1 || !(pfd.revents & POLLHUP)) {
		ksft_print_msg("master poll returned %d (revents %#x), expected HUP\n",
			       ret, pfd.revents);
		return -1;
	}
	return 0;
}

static int expect_locked_peer_rejected(int master)
{
	int fd;

	errno = 0;
	fd = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd >= 0) {
		ksft_print_msg("locked slave unexpectedly opened\n");
		close(fd);
		return -1;
	}
	if (errno != EIO) {
		ksft_print_msg("locked slave open returned errno %d, expected EIO\n",
			       errno);
		return -1;
	}
	return 0;
}

static int transfer_byte(int master, int slave, char value)
{
	struct pollfd pfd = { .fd = master, .events = POLLIN };
	char received;
	ssize_t ret;

	ret = write(slave, &value, 1);
	if (ret != 1) {
		ksft_print_msg("slave write returned %zd (errno %d)\n", ret, errno);
		return -1;
	}

	ret = poll(&pfd, 1, 1000);
	if (ret != 1 || !(pfd.revents & POLLIN)) {
		ksft_print_msg("master poll returned %zd (revents %#x)\n",
			       ret, pfd.revents);
		return -1;
	}

	ret = read(master, &received, 1);
	if (ret != 1 || received != value) {
		ksft_print_msg("master read returned %zd (value %#x)\n",
			       ret, ret == 1 ? (unsigned char)received : 0);
		return -1;
	}

	return 0;
}

int main(void)
{
	int master = -1, slave_a = -1, slave_b = -1;
	int locked = 1, unlocked = 0;
	int ret = KSFT_FAIL;

	ksft_print_header();
	ksft_set_plan(1);

	master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0) {
		ksft_print_msg("cannot open /dev/ptmx: %d\n", errno);
		goto out;
	}
	if (expect_no_master_hup(master) ||
	    expect_locked_peer_rejected(master) ||
	    expect_no_master_hup(master))
		goto out;

	if (ioctl(master, TIOCSPTLCK, &unlocked) < 0) {
		ksft_print_msg("cannot unlock slave: %d\n", errno);
		goto out;
	}
	slave_a = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (slave_a < 0) {
		ksft_print_msg("cannot open slave A: %d\n", errno);
		goto out;
	}
	if (transfer_byte(master, slave_a, 'A'))
		goto out;
	if (ioctl(master, TIOCSPTLCK, &locked) < 0) {
		ksft_print_msg("cannot relock slave: %d\n", errno);
		goto out;
	}
	if (expect_locked_peer_rejected(master) ||
	    transfer_byte(master, slave_a, 'B'))
		goto out;
	if (ioctl(master, TIOCSPTLCK, &unlocked) < 0) {
		ksft_print_msg("cannot unlock slave: %d\n", errno);
		goto out;
	}
	slave_b = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (slave_b < 0) {
		ksft_print_msg("cannot open slave B: %d\n", errno);
		goto out;
	}
	if (transfer_byte(master, slave_a, 'C') ||
	    transfer_byte(master, slave_b, 'D'))
		goto out;

	if (ioctl(master, TIOCSPTLCK, &locked) < 0) {
		ksft_print_msg("cannot relock slave: %d\n", errno);
		goto out;
	}
	if (transfer_byte(master, slave_a, 'E') ||
	    transfer_byte(master, slave_b, 'F'))
		goto out;

	if (expect_locked_peer_rejected(master) ||
	    transfer_byte(master, slave_a, 'G') ||
	    transfer_byte(master, slave_b, 'H'))
		goto out;
	if (close(slave_b)) {
		ksft_print_msg("cannot close slave B: %d\n", errno);
		slave_b = -1;
		goto out;
	}
	slave_b = -1;
	if (expect_no_master_hup(master) ||
	    transfer_byte(master, slave_a, 'I'))
		goto out;
	if (close(slave_a)) {
		ksft_print_msg("cannot close slave A: %d\n", errno);
		slave_a = -1;
		goto out;
	}
	slave_a = -1;
	if (expect_master_hup(master))
		goto out;
	ret = KSFT_PASS;

out:
	if (slave_b >= 0)
		close(slave_b);
	if (slave_a >= 0)
		close(slave_a);
	if (master >= 0)
		close(master);
	ksft_test_result(ret == KSFT_PASS,
			 "locked slave open preserves master and existing slave I/O\n");
	ksft_exit(ret == KSFT_PASS);
}
#endif
