/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MM_FILE_UTILS_H__
#define __MM_FILE_UTILS_H__

#include <stddef.h>

int read_file(const char *path, char *buf, size_t buflen);
void write_file(const char *path, const char *buf, size_t buflen);
unsigned long read_num(const char *path);
void write_num(const char *path, unsigned long num);

#endif
