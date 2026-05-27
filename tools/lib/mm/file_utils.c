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

	if (buflen < 2)
		return -EINVAL;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return -errno;

	numread = read(fd, buf, buflen - 1);
	if (numread < 1) {
		int err = numread ? -errno : -ENODATA;

		close(fd);
		return err;
	}

	buf[numread] = '\0';
	close(fd);

	return (int)numread;
}

int write_file(const char *path, const char *buf, size_t buflen)
{
	int fd, saved_errno;
	ssize_t numwritten;

	if (buflen < 2)
		return -EINVAL;

	fd = open(path, O_WRONLY);
	if (fd == -1)
		return -errno;

	numwritten = write(fd, buf, buflen - 1);
	saved_errno = errno;
	close(fd);
	if (numwritten < 0)
		return -saved_errno;
	if (numwritten != (ssize_t)(buflen - 1))
		return -EIO;

	return 0;
}

int read_num(const char *path, unsigned long *num)
{
	char buf[21];
	int ret;

	if (!num)
		return -EINVAL;

	ret = read_file(path, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	*num = strtoul(buf, NULL, 10);
	return 0;
}

int write_num(const char *path, unsigned long num)
{
	char buf[21];

	snprintf(buf, sizeof(buf), "%lu", num);
	return write_file(path, buf, strlen(buf) + 1);
}
