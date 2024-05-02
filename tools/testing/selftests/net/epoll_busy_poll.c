// SPDX-License-Identifier: GPL-2.0-or-later
/* Basic per-epoll context busy poll test.
 *
 * Only tests the ioctls, but should be expanded to test two connected hosts in
 * the future
 */

#define _GNU_SOURCE

#include <error.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

/* if the headers haven't been updated, we need to define some things */
#if !defined(EPOLL_IOC_TYPE)
struct epoll_params {
	uint32_t busy_poll_usecs;
	uint16_t busy_poll_budget;
	uint8_t prefer_busy_poll;

	/* pad the struct to a multiple of 64bits */
	uint8_t __pad;
};

#define EPOLL_IOC_TYPE 0x8A
#define EPIOCSPARAMS _IOW(EPOLL_IOC_TYPE, 0x01, struct epoll_params)
#define EPIOCGPARAMS _IOR(EPOLL_IOC_TYPE, 0x02, struct epoll_params)
#endif

enum epoll_test_types {
	TEST_UNDEFINED = -1,
	TEST_SIMPLE = 0,
};

static enum epoll_test_types test_type = TEST_UNDEFINED;

static void usage(const char *filepath)
{
	error(1, 0, "Usage: %s [options]", filepath);
}

static void parse_opts(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "s")) != -1) {
		switch (c) {
		case 's':
			test_type = TEST_SIMPLE;
			break;
		}
	}

	if (optind != argc)
		usage(argv[0]);
}

static void do_simple_test_get_params(int fd)
{
	/* begin by getting the epoll params from the kernel
	 *
	 * the default should be default and all fields should be zero'd by the
	 * kernel, so set params fields to garbage to test this.
	 */
	struct epoll_params *invalid_params;
	struct epoll_params params;
	int ret = 0;

	params.busy_poll_usecs = 0xff;
	params.busy_poll_budget = 0xff;
	params.prefer_busy_poll = 1;
	params.__pad = 0xf;

	if (ioctl(fd, EPIOCGPARAMS, &params) != 0)
		error(1, errno, "ioctl EPIOCGPARAMS");

	if (params.busy_poll_usecs != 0)
		error(1, 0, "EPIOCGPARAMS busy_poll_usecs should have been 0");

	if (params.busy_poll_budget != 0)
		error(1, 0, "EPIOCGPARAMS busy_poll_budget should have been 0");

	if (params.prefer_busy_poll != 0)
		error(1, 0, "EPIOCGPARAMS prefer_busy_poll should have been 0");

	if (params.__pad != 0)
		error(1, 0, "EPIOCGPARAMS __pad should have been 0");

	invalid_params = (struct epoll_params *)0xdeadbeef;
	ret = ioctl(fd, EPIOCGPARAMS, invalid_params);
	if (ret != -1)
		error(1, 0, "EPIOCGPARAMS should error with invalid params");

	if (errno != EFAULT)
		error(1, 0,
		      "EPIOCGPARAMS with invalid params should set errno to EFAULT");
}

static void do_simple_test_set_invalid(int fd)
{
	/* Set some unacceptable values and check for error */
	struct epoll_params *invalid_params;
	struct epoll_params params;
	int ret;

	memset(&params, 0, sizeof(struct epoll_params));

	params.__pad = 1;

	ret = ioctl(fd, EPIOCSPARAMS, &params);

	if (ret != -1)
		error(1, 0, "EPIOCSPARAMS with non-zero __pad should error");

	if (errno != EINVAL)
		error(1, 0, "EPIOCSPARAMS with non-zero __pad errno should be EINVAL");

	params.__pad = 0;
	params.busy_poll_usecs = (unsigned int)INT_MAX + 1;

	ret = ioctl(fd, EPIOCSPARAMS, &params);

	if (ret != -1)
		error(1, 0, "EPIOCSPARAMS should error busy_poll_usecs > S32_MAX");

	if (errno != EINVAL)
		error(1, 0, "EPIOCSPARAMS with busy_poll_usecs > S32_MAX, errno should be EINVAL");

	params.__pad = 0;
	params.busy_poll_usecs = 32;
	params.prefer_busy_poll = 2;

	ret = ioctl(fd, EPIOCSPARAMS, &params);

	if (ret != -1)
		error(1, 0, "EPIOCSPARAMS should error prefer_busy_poll > 1");

	if (errno != EINVAL)
		error(1, 0, "EPIOCSPARAMS with prefer_busy_poll > 1 errno should be EINVAL");

	params.__pad = 0;
	params.busy_poll_usecs = 32;
	params.prefer_busy_poll = 1;
	params.busy_poll_budget = 65535;

	ret = ioctl(fd, EPIOCSPARAMS, &params);

	if (ret != -1)
		error(1, 0, "EPIOCSPARAMS should error busy_poll_budget > NAPI_POLL_WEIGHT");

	if (errno != EPERM)
		error(1, 0,
		      "EPIOCSPARAMS with busy_poll_budget > NAPI_POLL_WEIGHT (without CAP_NET_ADMIN) errno should be EPERM");

	invalid_params = (struct epoll_params *)0xdeadbeef;
	ret = ioctl(fd, EPIOCSPARAMS, invalid_params);

	if (ret != -1)
		error(1, 0, "EPIOCSPARAMS should error when epoll_params is invalid");

	if (errno != EFAULT)
		error(1, 0, "EPIOCSPARAMS should set errno to EFAULT when epoll_params is invalid");
}

static void do_simple_test_set_and_get_valid(int fd)
{
	struct epoll_params params;
	int ret;

	memset(&params, 0, sizeof(struct epoll_params));

	params.busy_poll_usecs = 25;
	params.busy_poll_budget = 16;
	params.prefer_busy_poll = 1;

	ret = ioctl(fd, EPIOCSPARAMS, &params);

	if (ret != 0)
		error(1, errno, "EPIOCSPARAMS with valid params should not error");

	/* check that the kernel returns the same values back */

	memset(&params, 0, sizeof(struct epoll_params));

	ret = ioctl(fd, EPIOCGPARAMS, &params);

	if (ret != 0)
		error(1, errno, "EPIOCGPARAMS should not error");

	if (params.busy_poll_usecs != 25 ||
	    params.busy_poll_budget != 16 ||
	    params.prefer_busy_poll != 1 ||
	    params.__pad != 0)
		error(1, 0, "EPIOCGPARAMS returned incorrect values");
}

static void do_simple_test_invalid_fd(void)
{
	struct epoll_params params;
	int ret;
	int fd;

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);

	if (fd == -1)
		error(1, errno, "creating unix socket");

	ret = ioctl(fd, EPIOCGPARAMS, &params);

	if (ret != -1)
		error(1, 0, "EPIOCGPARAMS on invalid epoll FD should error");

	if (errno != ENOTTY)
		error(1, 0, "EPIOCGPARAMS on invalid epoll FD should set errno to ENOTTY");

	memset(&params, 0, sizeof(struct epoll_params));

	ret = ioctl(fd, EPIOCSPARAMS, &params);

	if (ret != -1)
		error(1, 0, "EPIOCSPARAMS on invalid epoll FD should error");

	if (errno != ENOTTY)
		error(1, 0, "EPIOCSPARAMS on invalid epoll FD should set errno to ENOTTY");
}

static void do_simple_test_invalid_ioctl(int fd)
{
	struct epoll_params params;
	int invalid_ioctl = EPIOCGPARAMS + 10;
	int ret;

	ret = ioctl(fd, invalid_ioctl, &params);

	if (ret != -1)
		error(1, 0, "invalid ioctl should return error");

	if (errno != EINVAL)
		error(1, 0, "invalid ioctl should set errno to EINVAL");
}

static void do_simple_test(void)
{
	int fd;

	fd = epoll_create1(0);
	if (fd == -1)
		error(1, errno, "epoll_create");

	do_simple_test_invalid_fd();
	do_simple_test_invalid_ioctl(fd);
	do_simple_test_get_params(fd);
	do_simple_test_set_invalid(fd);
	do_simple_test_set_and_get_valid(fd);

	if (close(fd))
		error(1, errno, "close");
}

int main(int argc, char **argv)
{
	parse_opts(argc, argv);

	if (test_type == TEST_SIMPLE)
		do_simple_test();
	else
		error(1, 0, "unknown test type: %d", test_type);

	return 0;
}
