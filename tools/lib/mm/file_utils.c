// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "file_utils.h"

int read_file(const char *path, char *buf, size_t buflen)
{
	int fd;
	ssize_t numread;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		int err = errno;

		printf("# %s: %s (%d)\n", path, strerror(err), err);
		return -err;
	}

	numread = read(fd, buf, buflen - 1);
	if (numread < 1) {
		int err = numread ? errno : ENODATA;

		close(fd);
		printf("# %s: %s (%d)\n", path, strerror(err), err);
		return -err;
	}

	buf[numread] = '\0';
	close(fd);

	return (int)numread;
}

int write_file(const char *path, const char *buf, size_t buflen)
{
	int fd, saved_errno;
	ssize_t numwritten;

	if (buflen < 2) {
		printf("# %s: %s (%d)\n", path, strerror(EINVAL), EINVAL);
		return -EINVAL;
	}

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		int err = errno;

		printf("# %s: %s (%d)\n", path, strerror(err), err);
		return -err;
	}

	numwritten = write(fd, buf, buflen - 1);
	saved_errno = errno;
	close(fd);
	if (numwritten < 0) {
		printf("# %s: %s (%d)\n", path, strerror(saved_errno),
		       saved_errno);
		return -saved_errno;
	}
	if (numwritten != (ssize_t)(buflen - 1)) {
		printf("# %s write(%.*s) is truncated, expected %zu bytes, got %zd bytes\n",
		       path, (int)(buflen - 1), buf, buflen - 1, numwritten);
		return -EIO;
	}

	return 0;
}

int read_num(const char *path, unsigned long *num)
{
	unsigned long val;
	int err, ret;
	char buf[21];
	char *end;

	if (!num)
		return -EINVAL;

	ret = read_file(path, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	errno = 0;
	val = strtoul(buf, &end, 10);
	if (errno) {
		err = errno;
		printf("# %s: %s (%d)\n", path, strerror(err), err);
		return -err;
	}

	if (end == buf || buf[0] == '-') {
		printf("# %s: invalid numeric value\n", path);
		return -EINVAL;
	}

	if (*end == '\n')
		end++;

	if (*end != '\0') {
		printf("# %s: invalid numeric value\n", path);
		return -EINVAL;
	}

	*num = val;
	return 0;
}

int write_num(const char *path, unsigned long num)
{
	char buf[21];

	snprintf(buf, sizeof(buf), "%lu", num);
	return write_file(path, buf, strlen(buf) + 1);
}
