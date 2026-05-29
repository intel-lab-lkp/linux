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

#include <linux/efi.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <asm/addrspace.h>
#include <asm/bootinfo.h>

const struct kexec_file_ops * const kexec_file_loaders[] = {
	&kexec_efi_ops,
	&kexec_elf_ops,
	NULL
};

int arch_kimage_file_post_load_cleanup(struct kimage *image)
{
	vfree(image->elf_headers);
	image->elf_headers = NULL;
	image->elf_headers_sz = 0;

#ifdef CONFIG_KEXEC_HANDOVER
	kvfree(image->arch.fdt);
	image->arch.fdt = NULL;
	kvfree(image->arch.efi_tables);
	image->arch.efi_tables = NULL;
#endif

	return kexec_image_post_load_cleanup_default(image);
}

/* Add the "kexec_file" command line parameter to command line. */
static void cmdline_add_loader(unsigned long *cmdline_tmplen, char *modified_cmdline)
{
	int loader_strlen;

	loader_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "kexec_file ");
	*cmdline_tmplen += loader_strlen;
}

/* Add the "initrd=start,size" command line parameter to command line. */
static void cmdline_add_initrd(struct kimage *image, unsigned long *cmdline_tmplen,
				char *modified_cmdline, unsigned long initrd)
{
	int initrd_strlen;

	initrd_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "initrd=0x%lx,0x%lx ",
		initrd, image->initrd_buf_len);
	*cmdline_tmplen += initrd_strlen;
}

#ifdef CONFIG_KEXEC_HANDOVER
/*
 * Add KHO metadata to an FDT /chosen node and load the FDT as a kexec
 * segment.  The second kernel reads linux,kho-fdt and linux,kho-scratch
 * from /chosen via early_init_dt_check_kho() and calls kho_populate().
 *
 * On FDT-based systems (initial_boot_params != NULL), the current FDT is
 * copied and the KHO properties are appended to /chosen.
 *
 * On ACPI-only systems (initial_boot_params == NULL), a minimal FDT
 * containing only /chosen is built from scratch.  machine_kexec() updates
 * the EFI config table DEVICE_TREE_GUID entry to point to this segment so
 * that the second kernel's fdt_setup() can find and parse it.
 */
static int kho_load_fdt(struct kimage *image)
{
	void *fdt;
	int ret, chosen_node;
	size_t fdt_size;
	struct kexec_buf kbuf = {
		.image		= image,
		.buf_min	= 0,
		.buf_max	= ULONG_MAX,
		.top_down	= true,
	};

	if (!image->kho.fdt || !image->kho.scratch)
		return 0;

	if (initial_boot_params) {
		/*
		 * FDT boot: copy the running kernel's FDT and append KHO
		 * properties to /chosen.
		 */

		/*
		 * Only two KHO properties are added to /chosen (linux,kho-fdt
		 * and linux,kho-scratch), so SZ_1K of extra space is
		 * sufficient.
		 */
		fdt_size = fdt_totalsize(initial_boot_params) + SZ_1K;
		fdt = kvmalloc(fdt_size, GFP_KERNEL);
		if (!fdt)
			return -ENOMEM;

		ret = fdt_open_into(initial_boot_params, fdt, fdt_size);
		if (ret < 0) {
			pr_err("Failed to open FDT: %d\n", ret);
			goto out_free;
		}

		chosen_node = fdt_path_offset(fdt, "/chosen");
		if (chosen_node == -FDT_ERR_NOTFOUND) {
			pr_debug("No /chosen node in FDT, creating one\n");
			chosen_node = fdt_add_subnode(fdt,
						      fdt_path_offset(fdt, "/"),
						      "chosen");
		}
		if (chosen_node < 0) {
			ret = chosen_node;
			goto out_free;
		}

		/* Remove stale KHO properties left by a previous kexec load */
		fdt_delprop(fdt, chosen_node, "linux,kho-fdt");
		fdt_delprop(fdt, chosen_node, "linux,kho-scratch");

		ret = fdt_appendprop_addrrange(fdt, 0, chosen_node,
					       "linux,kho-fdt",
					       image->kho.fdt, PAGE_SIZE);
		if (ret)
			goto out_free;

		ret = fdt_appendprop_addrrange(fdt, 0, chosen_node,
					       "linux,kho-scratch",
					       image->kho.scratch->mem,
					       image->kho.scratch->bufsz);
		if (ret)
			goto out_free;

		/*
		 * Shrink totalsize to the actual data size so the kexec segment
		 * allocated by kexec_add_buffer() covers only the packed FDT data.
		 * The slack added above for property insertion is part of the
		 * kvmalloc'd buffer, which is freed by kimage_file_post_load_cleanup()
		 * once the kexec image has been loaded.
		 */
		fdt_pack(fdt);
	} else {
		/*
		 * ACPI boot: build a minimal FDT containing only /chosen with
		 * the two KHO properties.  No system FDT is available to copy.
		 */

		__be64 prop[2];

		fdt_size = SZ_1K;
		fdt = kvmalloc(fdt_size, GFP_KERNEL);
		if (!fdt)
			return -ENOMEM;

		ret = fdt_create(fdt, fdt_size);
		if (ret < 0)
			goto out_free;
		ret = fdt_finish_reservemap(fdt);
		if (ret < 0)
			goto out_free;
		ret = fdt_begin_node(fdt, "");	/* root */
		if (ret < 0)
			goto out_free;
		ret = fdt_property_u32(fdt, "#address-cells", 2);
		if (ret < 0)
			goto out_free;
		ret = fdt_property_u32(fdt, "#size-cells", 2);
		if (ret < 0)
			goto out_free;
		ret = fdt_begin_node(fdt, "chosen");
		if (ret < 0)
			goto out_free;

		prop[0] = cpu_to_be64(image->kho.fdt);
		prop[1] = cpu_to_be64(PAGE_SIZE);
		ret = fdt_property(fdt, "linux,kho-fdt", prop, sizeof(prop));
		if (ret < 0)
			goto out_free;

		prop[0] = cpu_to_be64(image->kho.scratch->mem);
		prop[1] = cpu_to_be64(image->kho.scratch->bufsz);
		ret = fdt_property(fdt, "linux,kho-scratch", prop, sizeof(prop));
		if (ret < 0)
			goto out_free;

		ret = fdt_end_node(fdt);	/* chosen */
		if (ret < 0)
			goto out_free;
		ret = fdt_end_node(fdt);	/* root */
		if (ret < 0)
			goto out_free;
		ret = fdt_finish(fdt);
		if (ret < 0)
			goto out_free;
	}

	kbuf.buffer	= fdt;
	kbuf.bufsz	= fdt_totalsize(fdt);
	kbuf.memsz	= kbuf.bufsz;
	kbuf.buf_align	= PAGE_SIZE;
	kbuf.mem	= KEXEC_BUF_MEM_UNKNOWN;

	ret = kexec_add_buffer(&kbuf);
	if (ret)
		goto out_free;

	image->arch.fdt     = fdt;
	image->arch.fdt_mem = kbuf.mem;

	/*
	 * On ACPI-only systems DEVICE_TREE_GUID is not in the EFI config
	 * table, so the second kernel's efi_fdt_pointer() cannot locate the
	 * KHO FDT.  Build a new EFI config table with DEVICE_TREE_GUID added
	 * and load it as a kexec segment; machine_kexec() will update
	 * st->tables / st->nr_tables to point to it before jumping.
	 */

	/*
	 * fw_arg2 is the EFI system table physical address passed by the
	 * firmware/bootloader.  Use it directly here because
	 * image->arch.systable_ptr is set later in machine_kexec_prepare(),
	 * which runs after load_other_segments() / kho_load_fdt().
	 */
	if (!initial_boot_params && fw_arg2) {
		efi_system_table_t *st =
			(efi_system_table_t *)TO_CACHE(fw_arg2);
		efi_config_table_t *ct =
			(efi_config_table_t *)TO_CACHE((unsigned long)st->tables);
		unsigned long i;
		bool found = false;

		/*
		 * Scan the original config table;
		 * DEVICE_TREE_GUID is absent on ACPI-only systems.
		 */
		for (i = 0; i < st->nr_tables; i++) {
			if (!efi_guidcmp(ct[i].guid, DEVICE_TREE_GUID)) {
				found = true;
				break;
			}
		}

		if (!found) {
			size_t old_sz = st->nr_tables * sizeof(efi_config_table_t);
			size_t new_sz = old_sz + sizeof(efi_config_table_t);
			efi_config_table_t *new_ct;
			struct kexec_buf tbuf = {
				.image		= image,
				.buf_min	= 0,
				.buf_max	= ULONG_MAX,
				.top_down	= true,
			};

			/*
			 * Allocate a new table with n+1 entries and append
			 * the DEVICE_TREE_GUID entry.
			 */
			new_ct = kvmalloc(new_sz, GFP_KERNEL);
			if (!new_ct)
				return -ENOMEM;

			memcpy(new_ct, ct, old_sz);
			new_ct[st->nr_tables].guid  = DEVICE_TREE_GUID;
			new_ct[st->nr_tables].table = (void *)image->arch.fdt_mem;

			/* Register the new config table as a kexec segment. */
			tbuf.buffer   = new_ct;
			tbuf.bufsz    = new_sz;
			tbuf.memsz    = new_sz;
			tbuf.buf_align = sizeof(void *);
			tbuf.mem      = KEXEC_BUF_MEM_UNKNOWN;

			ret = kexec_add_buffer(&tbuf);
			if (ret) {
				kvfree(new_ct);
				return ret;
			}

			image->arch.efi_tables     = new_ct;
			image->arch.efi_tables_mem = tbuf.mem;
			image->arch.efi_tables_cnt = st->nr_tables + 1;
		}
	}

	return 0;

out_free:
	kvfree(fdt);
	return ret;
}
#endif

#ifdef CONFIG_CRASH_DUMP

static int prepare_elf_headers(void **addr, unsigned long *sz)
{
	int ret, nr_ranges;
	uint64_t i;
	phys_addr_t start, end;
	struct crash_mem *cmem;

	nr_ranges = 2; /* for exclusion of crashkernel region */
	for_each_mem_range(i, &start, &end)
		nr_ranges++;

	cmem = kmalloc_flex(*cmem, ranges, nr_ranges);
	if (!cmem)
		return -ENOMEM;

	cmem->max_nr_ranges = nr_ranges;
	cmem->nr_ranges = 0;
	for_each_mem_range(i, &start, &end) {
		cmem->ranges[cmem->nr_ranges].start = start;
		cmem->ranges[cmem->nr_ranges].end = end - 1;
		cmem->nr_ranges++;
	}

	/* Exclude crashkernel region */
	ret = crash_exclude_mem_range(cmem, crashk_res.start, crashk_res.end);
	if (ret < 0)
		goto out;

	if (crashk_low_res.end) {
		ret = crash_exclude_mem_range(cmem, crashk_low_res.start, crashk_low_res.end);
		if (ret < 0)
			goto out;
	}

	ret = crash_prepare_elf64_headers(cmem, true, addr, sz);

out:
	kfree(cmem);
	return ret;
}

/*
 * Add the "mem=size@start" command line parameter to command line, indicating the
 * memory region the new kernel can use to boot into.
 */
static void cmdline_add_mem(unsigned long *cmdline_tmplen, char *modified_cmdline)
{
	int mem_strlen = 0;

	mem_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "mem=0x%llx@0x%llx ",
		crashk_res.end - crashk_res.start + 1, crashk_res.start);
	*cmdline_tmplen += mem_strlen;

	if (crashk_low_res.end) {
		mem_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "mem=0x%llx@0x%llx ",
			crashk_low_res.end - crashk_low_res.start + 1, crashk_low_res.start);
		*cmdline_tmplen += mem_strlen;
	}
}

/* Add the "elfcorehdr=size@start" command line parameter to command line. */
static void cmdline_add_elfcorehdr(struct kimage *image, unsigned long *cmdline_tmplen,
				   char *modified_cmdline, unsigned long elfcorehdr_sz)
{
	int elfcorehdr_strlen = 0;

	elfcorehdr_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "elfcorehdr=0x%lx@0x%lx ",
		elfcorehdr_sz, image->elf_load_addr);
	*cmdline_tmplen += elfcorehdr_strlen;
}

#endif

/*
 * Try to add the initrd to the image. If it is not possible to find valid
 * locations, this function will undo changes to the image and return non zero.
 */
int load_other_segments(struct kimage *image,
			unsigned long kernel_load_addr, unsigned long kernel_size,
			char *initrd, unsigned long initrd_len, char *cmdline, unsigned long cmdline_len)
{
	int ret = 0;
	unsigned long cmdline_tmplen = 0;
	unsigned long initrd_load_addr = 0;
	unsigned long orig_segments = image->nr_segments;
	char *modified_cmdline = NULL;
	struct kexec_buf kbuf = {};

	kbuf.image = image;
	/* Don't allocate anything below the kernel */
	kbuf.buf_min = kernel_load_addr + kernel_size;

	modified_cmdline = kzalloc(COMMAND_LINE_SIZE, GFP_KERNEL);
	if (!modified_cmdline)
		return -EINVAL;

	cmdline_add_loader(&cmdline_tmplen, modified_cmdline);
	/* Ensure it's null terminated */
	modified_cmdline[COMMAND_LINE_SIZE - 1] = '\0';

#ifdef CONFIG_CRASH_DUMP
	/* Load elf core header */
	if (image->type == KEXEC_TYPE_CRASH) {
		void *headers;
		unsigned long headers_sz;

		ret = prepare_elf_headers(&headers, &headers_sz);
		if (ret < 0) {
			pr_err("Preparing elf core header failed\n");
			goto out_err;
		}

		kbuf.buffer = headers;
		kbuf.bufsz = headers_sz;
		kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
		kbuf.memsz = headers_sz;
		kbuf.buf_align = SZ_64K; /* largest supported page size */
		kbuf.buf_max = ULONG_MAX;
		kbuf.top_down = true;

		ret = kexec_add_buffer(&kbuf);
		if (ret < 0) {
			vfree(headers);
			goto out_err;
		}
		image->elf_headers = headers;
		image->elf_load_addr = kbuf.mem;
		image->elf_headers_sz = headers_sz;

		kexec_dprintk("Loaded elf core header at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
			      image->elf_load_addr, kbuf.bufsz, kbuf.memsz);

		/* Add the mem=size@start parameter to the command line */
		cmdline_add_mem(&cmdline_tmplen, modified_cmdline);

		/* Add the elfcorehdr=size@start parameter to the command line */
		cmdline_add_elfcorehdr(image, &cmdline_tmplen, modified_cmdline, headers_sz);
	}
#endif

	/* Load initrd */
	if (initrd) {
		kbuf.buffer = initrd;
		kbuf.bufsz = initrd_len;
		kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
		kbuf.memsz = initrd_len;
		kbuf.buf_align = 0;
		/* within 1GB-aligned window of up to 32GB in size */
		kbuf.buf_max = round_down(kernel_load_addr, SZ_1G) + (unsigned long)SZ_1G * 32;
		kbuf.top_down = false;

		ret = kexec_add_buffer(&kbuf);
		if (ret < 0)
			goto out_err;
		initrd_load_addr = kbuf.mem;

		kexec_dprintk("Loaded initrd at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
			      initrd_load_addr, kbuf.bufsz, kbuf.memsz);

		/* Add the initrd=start,size parameter to the command line */
		cmdline_add_initrd(image, &cmdline_tmplen, modified_cmdline, initrd_load_addr);
	}

	if (cmdline_len + cmdline_tmplen > COMMAND_LINE_SIZE) {
		pr_err("Appending command line exceeds COMMAND_LINE_SIZE\n");
		ret = -EINVAL;
		goto out_err;
	}

	memcpy(modified_cmdline + cmdline_tmplen, cmdline, cmdline_len);
	cmdline = modified_cmdline;
	image->arch.cmdline_ptr = (unsigned long)cmdline;

#ifdef CONFIG_KEXEC_HANDOVER
	ret = kho_load_fdt(image);
	if (ret)
		goto out_err;
#endif

	return 0;

out_err:
	image->nr_segments = orig_segments;
	kfree(modified_cmdline);
	return ret;
}
