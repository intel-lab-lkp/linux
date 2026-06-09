/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_LIBASM_H
#define __PERF_LIBASM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/compiler.h>
#include <linux/types.h>

struct annotate_args;
struct symbol;

#ifdef HAVE_LIBASM_SUPPORT
int symbol__disassemble_libasm(const char *filename, struct symbol *sym,
				 struct annotate_args *args);
#else /* !HAVE_LIBASM_SUPPORT */
static inline int symbol__disassemble_libasm(const char *filename __maybe_unused,
					       struct symbol *sym __maybe_unused,
					       struct annotate_args *args __maybe_unused)
{
	return -1;
}
#endif /* HAVE_LIBASM_SUPPORT */

#endif /* __PERF_LIBASM_H */
