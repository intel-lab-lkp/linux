// SPDX-License-Identifier: GPL-2.0-only
/*
 * Load ELF vmlinux file for the kexec_file_load syscall.
 *
 * Author: Youling Tang <tangyouling@kylinos.cn>
 * Copyright (C) 2025 KylinSoft Corporation.
 */

#define pr_fmt(fmt)	"kexec_file(ELF): " fmt

#include <linux/elf.h>
#include <linux/kexec.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/memblock.h>
#include <asm/image.h>
#include <asm/setup.h>

static int loongarch_kexec_elf_load(struct kimage *image, struct elfhdr *ehdr,
			 struct kexec_elf_info *elf_info,
			 struct kexec_buf *kbuf,
			 unsigned long *text_offset)
{
	int ret = -1;
	size_t i;

	/* Read in the PT_LOAD segments. */
	for (i = 0; i < ehdr->e_phnum; i++) {
		size_t size;
		const struct elf_phdr *phdr;
		struct loongarch_image_header *header;

		phdr = &elf_info->proghdrs[i];
		if (phdr->p_type != PT_LOAD)
			continue;

		size = phdr->p_filesz;
		if (size > phdr->p_memsz)
			size = phdr->p_memsz;

		kbuf->buffer = (void *)elf_info->buffer + phdr->p_offset;
		kbuf->bufsz = size;
		kbuf->buf_align = phdr->p_align;
		header = (struct loongarch_image_header *)kbuf->buffer;
		*text_offset = le64_to_cpu(header->text_offset);
		kbuf->buf_min = *text_offset;
		kbuf->memsz = le64_to_cpu(header->image_size);
		kbuf->mem = KEXEC_BUF_MEM_UNKNOWN;
		ret = kexec_add_buffer(kbuf);
		if (ret)
			break;
	}

	return ret;
}

static void *elf_kexec_load(struct kimage *image, char *kernel_buf,
			    unsigned long kernel_len, char *initrd,
			    unsigned long initrd_len, char *cmdline,
			    unsigned long cmdline_len)
{
	int ret;
	unsigned long text_offset = 0, kernel_segment_number;
	struct elfhdr ehdr;
	struct kexec_elf_info elf_info;
	struct kexec_segment *kernel_segment;
	struct kexec_buf kbuf;

	ret = kexec_build_elf_info(kernel_buf, kernel_len, &ehdr, &elf_info);
	if (ret)
		return ERR_PTR(ret);

	/* Load the kernel */
	kbuf.image = image;
	kbuf.buf_max = ULONG_MAX;
	kbuf.top_down = false;

	kernel_segment_number = image->nr_segments;

	ret = loongarch_kexec_elf_load(image, &ehdr, &elf_info, &kbuf, &text_offset);
	if (ret)
		goto out;

	/* Load additional data */
	kernel_segment = &image->segment[kernel_segment_number];
	ret = load_other_segments(image, kernel_segment->mem, kernel_segment->memsz,
				  initrd, initrd_len, cmdline, cmdline_len);
	if (ret)
		goto out;

	/* Make sure the second kernel jumps to the correct "kernel_entry". */
	image->start = kernel_segment->mem + __pa(ehdr.e_entry) - text_offset;

	kexec_dprintk("Loaded kernel at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
		      kernel_segment->mem, kbuf.bufsz, kernel_segment->memsz);

out:
	kexec_free_elf_info(&elf_info);
	return ret ? ERR_PTR(ret) : NULL;
}

const struct kexec_file_ops kexec_elf_ops = {
	.probe = kexec_elf_probe,
	.load  = elf_kexec_load,
};
