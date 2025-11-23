/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_SYMBOL_MINIMAL_H
#define __PERF_SYMBOL_MINIMAL_H

#include "dso.h"

struct build_id;

int sym_min__read_build_id(int _fd, const char *filename, struct build_id *bid);
int sym_min_sysfs__read_build_id(const char *filename, struct build_id *bid);
enum dso_type sym_min_dso__type_fd(int fd);

#endif /* __PERF_SYMBOL_MINIMAL_H */
