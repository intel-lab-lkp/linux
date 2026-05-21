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
	if (fd == -1)
		return 0;

	numread = read(fd, buf, buflen - 1);
	if (numread < 1) {
		close(fd);
		return 0;
	}

	buf[numread] = '\0';
	close(fd);

	return (unsigned int)numread;
}

void write_file(const char *path, const char *buf, size_t buflen)
{
	int fd, saved_errno;
	ssize_t numwritten;

	if (buflen < 2) {
		fprintf(stderr, "Incorrect buffer len: %zu\n", buflen);
		exit(EXIT_FAILURE);
	}

	fd = open(path, O_WRONLY);
	if (fd == -1) {
		fprintf(stderr, "%s open failed: %s\n", path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	numwritten = write(fd, buf, buflen - 1);
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	if (numwritten < 0) {
		fprintf(stderr, "%s write(%.*s) failed: %s\n",
			path, (int)(buflen - 1), buf, strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (numwritten != (ssize_t)(buflen - 1)) {
		fprintf(stderr,
			"%s write(%.*s) is truncated, expected %zu bytes, got %zd bytes\n",
			path, (int)(buflen - 1), buf, buflen - 1, numwritten);
		exit(EXIT_FAILURE);
	}
}

unsigned long read_num(const char *path)
{
	char buf[21];

	if (read_file(path, buf, sizeof(buf)) < 0) {
		perror("read_file()");
		exit(EXIT_FAILURE);
	}

	return strtoul(buf, NULL, 10);
}

void write_num(const char *path, unsigned long num)
{
	char buf[21];

	snprintf(buf, sizeof(buf), "%lu", num);
	write_file(path, buf, strlen(buf) + 1);
}
