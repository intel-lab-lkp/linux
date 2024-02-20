/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This header defines some variables that change on every compilation and are
 * stored separately in the small file dynamic-vars.c, so that we can avoid
 * rebuilding some of the other C files in this directory on every incremental
 * rebuild.
 */

/* Variables containing VO__text, VO___bss_start, VO__end */
extern const unsigned long vo__text, vo___bss_start, vo__end;
extern const unsigned long kernel_total_size;
