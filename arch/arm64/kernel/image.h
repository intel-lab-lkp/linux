/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Linker script macros to generate Image header fields.
 *
 * Copyright (C) 2014 ARM Ltd.
 */
#ifndef __ARM64_KERNEL_IMAGE_H
#define __ARM64_KERNEL_IMAGE_H

#ifndef LINKER_SCRIPT
#error This file should only be included in vmlinux.lds.S
#endif

#include <asm/image.h>

#define __HEAD_FLAG(field)	(__HEAD_FLAG_##field << \
					ARM64_IMAGE_FLAG_##field##_SHIFT)

#define __HEAD_FLAG_BE		ARM64_IMAGE_FLAG_LE

#define __HEAD_FLAG_PAGE_SIZE	((PAGE_SHIFT - 10) / 2)

#define __HEAD_FLAG_PHYS_BASE	1

#define __HEAD_FLAGS		(__HEAD_FLAG(BE)	| \
				 __HEAD_FLAG(PAGE_SIZE) | \
				 __HEAD_FLAG(PHYS_BASE))

#define HEAD_SYMBOLS						\
	_kernel_size_le		=  _end - _text;		\
	_kernel_flags_le	= __HEAD_FLAGS;

#endif /* __ARM64_KERNEL_IMAGE_H */
