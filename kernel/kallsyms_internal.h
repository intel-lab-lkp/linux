/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_KALLSYMS_INTERNAL_H_
#define LINUX_KALLSYMS_INTERNAL_H_

#include <linux/types.h>

/*
 * These constants determine the size of the kallsyms_markers array
 * written by scripts/kallsyms.c during the build.  The symbol values
 * must be kept in sync.
 */
#define KALLSYMS_MARKER_SHIFT 4
#define KALLSYMS_MARKER_SIZE  (1U << KALLSYMS_MARKER_SHIFT)
#define KALLSYMS_MARKER_MASK  (KALLSYMS_MARKER_SIZE - 1U)

extern const int kallsyms_offsets[];
extern const u8 kallsyms_names[];

extern const unsigned int kallsyms_num_syms;

extern const char kallsyms_token_table[];
extern const u16 kallsyms_token_index[];

extern const unsigned int kallsyms_markers[];
extern const u8 kallsyms_seqs_of_names[];

#endif // LINUX_KALLSYMS_INTERNAL_H_
