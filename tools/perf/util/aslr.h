/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_ASLR_H
#define __PERF_ASLR_H

struct perf_tool;

struct perf_tool *aslr_tool__new(struct perf_tool *delegate);
void aslr_tool__delete(struct perf_tool *aslr);

#endif /* __PERF_ASLR_H */
