/* SPDX-License-Identifier: GPL-2.0 */
/*
 * LoongArch binary image header.
 *
 * Author: Youling Tang <tangyouling@kylinos.cn>
 * Copyright (C) 2025 KylinSoft Corporation.
 *
 * Most code is derived from LoongArch port of kexec-tools
 */

#ifndef __ASM_IMAGE_H
#define __ASM_IMAGE_H
#ifndef __ASSEMBLY__

/**
 * struct loongarch_image_header
 *
 * @pe_sig: Optional PE format 'MZ' signature.
 * @reserved_1: Reserved.
 * @kernel_entry: Kernel image entry pointer.
 * @image_size: An estimated size of the memory image size in LSB byte order.
 * @text_offset: The image load offset in LSB byte order.
 * @reserved_2: Reserved.
 * @reserved_3: Reserved.
 * @pe_header: Optional offset to a PE format header.
 **/

struct loongarch_image_header {
	uint8_t pe_sig[2];
	uint16_t reserved_1[3];
	uint64_t kernel_entry;
	uint64_t image_size;
	uint64_t text_offset;
	uint64_t reserved_2[3];
	uint32_t reserved_3;
	uint32_t pe_header;
};

#endif /* __ASSEMBLY__ */
#endif /* __ASM_IMAGE_H */
