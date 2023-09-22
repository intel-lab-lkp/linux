/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_MTECOMP_H
#define __ASM_MTECOMP_H

#include <linux/types.h>

unsigned long mte_compress(u8 *tags);
bool mte_decompress(unsigned long handle, u8 *tags);
void mte_release_handle(unsigned long handle);
size_t mte_storage_size(unsigned long handle);

#endif // __ASM_MTECOMP_H
