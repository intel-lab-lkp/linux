// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/types.h>

/* A hack to avoid non-static declaration for kaslr_get_random_long(). */
#define _ASM_KASLR_H_
#include <asm/sections.h>
#include <asm/bootparam.h>
#include <asm/cpuid/api.h>

extern char __relocation_end[];

static struct boot_params *boot_params_ptr __initdata;

static inline void debug_putstr(const char *str)
{
}

static inline bool has_cpuflag(int flag)
{
	u32 reg = 0;
	u32 level = native_cpuid_eax(0x0);

	if (level >= 0x00000001) {
		if (flag == X86_FEATURE_RDRAND)
			reg = native_cpuid_edx(0x1);
		else if (flag == X86_FEATURE_TSC)
			reg = native_cpuid_ecx(0x1);
	}

	return test_bit(flag & 31, (unsigned long *)&reg);
}

static unsigned long __init rotate_xor(unsigned long hash, const void *area,
				       size_t size)
{
	size_t i;
	unsigned long *ptr = (unsigned long *)area;

	for (i = 0; i < size / sizeof(hash); i++) {
		/* Rotate by odd number of bits and XOR. */
		hash = (hash << ((sizeof(hash) * 8) - 7)) | (hash >> 7);
		hash ^= ptr[i];
	}

	return hash;
}

/* Attempt to create a simple but unpredictable starting entropy. */
static unsigned long get_boot_seed(void)
{
	unsigned long hash = 0;

	hash = rotate_xor(hash, boot_params_ptr, sizeof(*boot_params_ptr));

	return hash;
}

#define KASLR_COMPRESSED_BOOT
#define KASLR_FUNC_PREFIX static __init
#include "../../lib/kaslr.c"

/* A hack to avoid non-static declaration for cmdline_find_option_bool(). */
#define _ASM_X86_CMDLINE_H
#undef CONFIG_CMDLINE_BOOL
#define builtin_cmdline NULL
#define CMDLINE_FUNC_PREFIX static __maybe_unused __init
#include "../../lib/cmdline.c"

static unsigned long __init find_random_virt_addr(unsigned long minimum,
						  unsigned long image_size)
{
	unsigned long slots, random_addr;

	/*
	 * There are how many CONFIG_PHYSICAL_ALIGN-sized slots
	 * that can hold image_size within the range of minimum to
	 * KERNEL_IMAGE_SIZE?
	 */
	slots = 1 + (KERNEL_IMAGE_SIZE - minimum - image_size) / CONFIG_PHYSICAL_ALIGN;

	random_addr = kaslr_get_random_long("Virtual") % slots;

	return random_addr * CONFIG_PHYSICAL_ALIGN + minimum;
}

void __init __relocate_kernel(unsigned long p2v_offset, struct boot_params *bp)
{
	int *reloc = (int *)rip_rel_ptr(__relocation_end);
	unsigned long image_size = rip_rel_ptr(_end) - rip_rel_ptr(_text);
	unsigned long ptr, virt_addr, delta;
	unsigned long cmd_line_ptr;

	/* If relocation has occurred during decompression, simply skip it. */
	if (bp->hdr.loadflags & KASLR_FLAG)
		return;

	cmd_line_ptr = bp->hdr.cmd_line_ptr | ((u64)bp->ext_cmd_line_ptr << 32);
	if (cmdline_find_option_bool((char *)cmd_line_ptr, "nokaslr"))
		return;

	boot_params_ptr = bp;
	virt_addr = find_random_virt_addr(LOAD_PHYSICAL_ADDR, image_size);
	delta = virt_addr - LOAD_PHYSICAL_ADDR;

	for (reloc--; *reloc; reloc--) {
		ptr = (unsigned long)(*reloc + p2v_offset);
		*(uint32_t *)ptr += delta;
	}

	for (reloc--; *reloc; reloc--) {
		ptr = (unsigned long)(*reloc + p2v_offset);
		*(uint64_t *)ptr += delta;
	}
}
