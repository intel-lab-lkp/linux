// SPDX-License-Identifier: GPL-2.0-only
/*
 * Socket utility functions.
 *
 * Copyright IBM Corp. 2024
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "util_socket.h"

/* Set "unsigned long long" socket option and check that it's indeed set */
bool setsockopt_ull_check(int fd, int level, int optname,
	unsigned long long val, char const *errmsg)
{
	unsigned long long chkval;
	socklen_t chklen;
	int err;

	err = setsockopt(fd, level, optname, &val, sizeof(val));
	if (err) {
		fprintf(stderr, "setsockopt err: %s (%d)\n",
				strerror(errno), errno);
		goto fail;
	}

	chkval = ~val; /* just make storage != val */
	chklen = sizeof(chkval);

	err = getsockopt(fd, level, optname, &chkval, &chklen);
	if (err) {
		fprintf(stderr, "getsockopt err: %s (%d)\n",
				strerror(errno), errno);
		goto fail;
	}

	if (chklen != sizeof(chkval)) {
		fprintf(stderr, "size mismatch: set %zu got %d\n", sizeof(val),
				chklen);
		goto fail;
	}

	if (chkval != val) {
		fprintf(stderr, "value mismatch: set %llu got %llu\n", val,
				chkval);
		goto fail;
	}
	return true;
fail:
	fprintf(stderr, "%s  val %llu\n", errmsg, val);
	return false;
}

/* Set "int" socket option and check that it's indeed set */
bool setsockopt_int_check(int fd, int level, int optname, int val,
		char const *errmsg)
{
	int chkval;
	socklen_t chklen;
	int err;

	err = setsockopt(fd, level, optname, &val, sizeof(val));
	if (err) {
		fprintf(stderr, "setsockopt err: %s (%d)\n",
				strerror(errno), errno);
		goto fail;
	}

	chkval = ~val; /* just make storage != val */
	chklen = sizeof(chkval);

	err = getsockopt(fd, level, optname, &chkval, &chklen);
	if (err) {
		fprintf(stderr, "getsockopt err: %s (%d)\n",
				strerror(errno), errno);
		goto fail;
	}

	if (chklen != sizeof(chkval)) {
		fprintf(stderr, "size mismatch: set %zu got %d\n", sizeof(val),
				chklen);
		goto fail;
	}

	if (chkval != val) {
		fprintf(stderr, "value mismatch: set %d got %d\n",
				val, chkval);
		goto fail;
	}
	return true;
fail:
	fprintf(stderr, "%s val %d\n", errmsg, val);
	return false;
}

static void mem_invert(unsigned char *mem, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		mem[i] = ~mem[i];
}

/* Set "timeval" socket option and check that it's indeed set */
bool setsockopt_timeval_check(int fd, int level, int optname,
		struct timeval val, char const *errmsg)
{
	struct timeval chkval;
	socklen_t chklen;
	int err;

	err = setsockopt(fd, level, optname, &val, sizeof(val));
	if (err) {
		fprintf(stderr, "setsockopt err: %s (%d)\n",
				strerror(errno), errno);
		goto fail;
	}

	 /* just make storage != val */
	chkval = val;
	mem_invert((unsigned char *) &chkval, sizeof(chkval));
	chklen = sizeof(chkval);

	err = getsockopt(fd, level, optname, &chkval, &chklen);
	if (err) {
		fprintf(stderr, "getsockopt err: %s (%d)\n",
				strerror(errno), errno);
		goto fail;
	}

	if (chklen != sizeof(chkval)) {
		fprintf(stderr, "size mismatch: set %zu got %d\n", sizeof(val),
				chklen);
		goto fail;
	}

	if (memcmp(&chkval, &val, sizeof(val)) != 0) {
		fprintf(stderr, "value mismatch: set %ld:%ld got %ld:%ld\n",
				val.tv_sec, val.tv_usec,
				chkval.tv_sec, chkval.tv_usec);
		goto fail;
	}
	return true;
fail:
	fprintf(stderr, "%s val %ld:%ld\n", errmsg, val.tv_sec, val.tv_usec);
	return false;
}
