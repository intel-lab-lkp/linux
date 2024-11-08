/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Socket utility functions.
 *
 * Copyright IBM Corp. 2024
 */
#ifndef UTIL_SOCKET_H
#define UTIL_SOCKET_H

#include <stdbool.h>

bool setsockopt_ull_check(int fd, int level, int optname,
		unsigned long long val, char const *errmsg);
bool setsockopt_int_check(int fd, int level, int optname, int val,
		char const *errmsg);
bool setsockopt_timeval_check(int fd, int level, int optname,
		struct timeval val, char const *errmsg);

#endif /* UTIL_SOCKET_H */
