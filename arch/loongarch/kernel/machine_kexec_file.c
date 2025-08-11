// SPDX-License-Identifier: GPL-2.0
/*
 * kexec_file for LoongArch
 *
 * Author: Youling Tang <tangyouling@kylinos.cn>
 * Copyright (C) 2025 KylinSoft Corporation.
 *
 * Most code is derived from LoongArch port of kexec-tools
 */

#define pr_fmt(fmt) "kexec_file: " fmt

#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/memblock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <asm/bootinfo.h>

const struct kexec_file_ops * const kexec_file_loaders[] = {
	&kexec_image_ops,
	NULL
};

int arch_kimage_file_post_load_cleanup(struct kimage *image)
{
	vfree(image->elf_headers);
	image->elf_headers = NULL;
	image->elf_headers_sz = 0;

	return kexec_image_post_load_cleanup_default(image);
}

int load_other_segments(struct kimage *image,
			unsigned long kernel_load_addr,
			unsigned long kernel_size,
			char *initrd, unsigned long initrd_len,
			char *cmdline, unsigned long cmdline_len)
{
	image->arch.cmdline_ptr = (unsigned long)cmdline;

	return 0;
}
