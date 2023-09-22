/* SPDX-License-Identifier: GPL-2.0 */

#ifndef ARCH_ARM64_MM_MTECOMP_H_
#define ARCH_ARM64_MM_MTECOMP_H_

/* Functions exported from mtecomp.c for test_mtecomp.c. */
void mte_tags_to_ranges(u8 *tags, u8 *out_tags, unsigned short *out_sizes,
			size_t *out_len);
void mte_ranges_to_tags(u8 *r_tags, unsigned short *r_sizes, size_t r_len,
			u8 *tags);

#endif  // ARCH_ARM64_MM_TEST_MTECOMP_H_
