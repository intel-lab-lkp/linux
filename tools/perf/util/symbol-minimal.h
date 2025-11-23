/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_SYMBOL_MINIMAL_H
#define __PERF_SYMBOL_MINIMAL_H

struct build_id;

int sym_min__read_build_id(int _fd, const char *filename, struct build_id *bid);

#endif /* __PERF_SYMBOL_MINIMAL_H */
