// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for installing Linux-owned SMI handler
 *
 * Copyright (c) 2025 9elements GmbH
 *
 * Author: Michal Gorlas <michal.gorlas@9elements.com>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/device.h>

#include "coreboot_table.h"
#include "mm_payload.h"

struct mm_header *mm_header;
static void *shared_buffer;
static size_t blob_size;
static struct lb_pld_mm_interface_info *mm_cbtable_info;
struct mm_info *mm_info;

/*
 * This is x86_64 specific, assuming that we want this to also work on i386,
 * we either need to have "trigger_smi32" bounded by preprocessor guards(?)
 * or mm_loader32 and then mm_loader$(BITS) in Makefile(?).
 */
static int trigger_smi(u64 cmd, u64 arg, u64 retry)
{
	u64 status;
	u16 apmc_port = 0xb2;

	asm volatile("movq	%[cmd], %%rax\n\t"
		     "movq	%%rax, %%rcx\n\t"
		     "movq	%[arg], %%rbx\n\t"
		     "movq	%[retry], %%r8\n\t"
		     ".trigger:\n\t"
		     "mov	%[apmc_port], %%dx\n\t"
		     "outb	%%al, %%dx\n\t"
		     "cmpq	%%rcx, %%rax\n\t"
		     "jne	.return_changed\n\t"
		     "pushq	%%rcx\n\t"
		     "movq	$10000, %%rcx\n\t"
		     "rep	nop\n\t"
		     "popq	%%rcx\n\t"
		     "cmpq	$0, %%r8\n\t"
		     "je	.return_not_changed\n\t"
		     "decq	%%r8\n\t"
		     "jmp	.trigger\n\t"
		     ".return_changed:\n\t"
		     "movq	%%rax, %[status]\n\t"
		     "jmp	.end\n\t"
		     ".return_not_changed:\n\t"
		     "movq	%%rcx, %[status]\n\t"
		     ".end:\n\t"
		     : [status] "=r"(status)
		     : [cmd] "r"(cmd), [arg] "r"(arg), [retry] "r"(retry),
		       [apmc_port] "r"(apmc_port)
		     : "%rax", "%rbx", "%rdx", "%rcx", "%r8");

	if (status == cmd || status == PAYLOAD_MM_RET_FAILURE)
		status = PAYLOAD_MM_RET_FAILURE;
	else
		status = PAYLOAD_MM_RET_SUCCESS;

	return status;
}

static int register_entry_point(struct device *dev, struct mm_info *data, uint32_t entry_point)
{
	u64 cmd;
	u8 status;

	cmd = data->register_mm_entry_command |
	      (PAYLOAD_MM_REGISTER_ENTRY << 8);
	status = trigger_smi(cmd, entry_point, 5);
	dev_dbg(dev, ": %s: SMI returned %x\n", __func__, status);

	return status;
}

static u32 place_handler(struct device *dev)
{
	/*
	 * The handler (aka MM blob) has to be placed in low 4GB of the memory.
	 * This is because we can not assume that coreboot will be in long mode
	 * while trying to copy the blob to SMRAM. Even if so, (can be checked by
	 * reading cb_data->mm_info.requires_long_mode_call), it would make our life
	 * way too complicated (e.g. no need for shared page table).
	 */
	size_t entry32_offset;
	size_t entry64_offset;
	u16 real_mode_seg;
	const u32 *rel;
	u32 count;
	unsigned long phys_base;

	blob_size = mm_payload_size_needed();
	shared_buffer = (void *)devm_get_free_pages(dev, GFP_DMA32, get_order(blob_size));
	if (!shared_buffer)
		return -ENOMEM;

	memcpy(shared_buffer, mm_blob, blob_size);
	wbinvd();

	/*
	 * Based on arch/x86/realmode/init.c
	 * The sole purpose of doing relocations is to be able to calculate the offsets
	 * for entry points. While the absolute addresses are not valid anymore after the
	 * blob is copied to SMRAM, the distances between sections stay the same, so we
	 * can still calculate the correct entry point based on coreboot's bitness.
	 */
	phys_base = __pa(shared_buffer);
	real_mode_seg = phys_base >> 4;
	rel = (u32 *)mm_relocs;

	/* 16-bit segment relocations. */
	count = *rel++;
	while (count--) {
		u16 *seg = (u16 *)(shared_buffer + *rel++);
		*seg = real_mode_seg;
	}

	/* 32-bit linear relocations. */
	count = *rel++;
	while (count--) {
		u32 *ptr = (u32 *)(shared_buffer + *rel++);
		*ptr += phys_base;
	}

	mm_header =  (struct mm_header *)shared_buffer;

	mm_header->mm_signature = REALMODE_END_SIGNATURE;
	mm_header->mm_blob_size = mm_payload_size_needed();

	/*
	 * At this point relocations are done and we can do some cool
	 * pointer arithmetics to help coreboot determine correct entry
	 * point based on offsets.
	 */
	entry32_offset = mm_header->mm_entry_32 - (unsigned long)shared_buffer;
	entry64_offset = mm_header->mm_entry_64 - (unsigned long)shared_buffer;

	mm_header->mm_entry_32 = entry32_offset;
	mm_header->mm_entry_64 = entry64_offset;

	return (unsigned long)shared_buffer;
}

static int get_mm_info(struct coreboot_device *dev)
{
	mm_cbtable_info = &dev->mm_info;
	if (mm_cbtable_info->tag != LB_TAG_PLD_MM_INTERFACE_INFO)
		return -ENXIO;

	mm_info = devm_kzalloc(&dev->dev, sizeof(*mm_info), GFP_KERNEL);
	if (!mm_info)
		return -ENOMEM;

	mm_info->revision = mm_cbtable_info->revision;
	mm_info->requires_long_mode_call =
		mm_cbtable_info->requires_long_mode_call;
	mm_info->register_mm_entry_command =
		mm_cbtable_info->register_mm_entry_command;
	return 0;
}

static int mm_loader_probe(struct coreboot_device *dev)
{
	if (get_mm_info(dev))
		return -ENOMEM;

	u32 entry_point;

	entry_point = place_handler(&dev->dev);

	if (register_entry_point(&dev->dev, mm_info, entry_point)) {
		dev_warn(&dev->dev, ": registering entry point for MM payload failed.\n");
		return -1;
	}

	/*
	 * Gives SMI some time in case it takes longer than expected.
	 * Only useful on real hardware (tested on RaptorLake), not needed on emulation.
	 */
	mdelay(100);

	return 0;
}

static const struct coreboot_device_id mm_loader_ids[] = {
	{ .tag = LB_TAG_PLD_MM_INTERFACE_INFO },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(coreboot, mm_loader_ids);

static struct coreboot_driver mm_loader = {
	.probe = mm_loader_probe,
	.drv = {
		.name = "mm_loader",
	},
	.id_table = mm_loader_ids,
};

module_coreboot_driver(mm_loader);

MODULE_AUTHOR("Michal Gorlas <michal.gorlas@9elements.com>");
MODULE_DESCRIPTION("MM Payload loader - installs Linux-owned SMI handler");
MODULE_LICENSE("GPL v2");
