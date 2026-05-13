// SPDX-License-Identifier: GPL-2.0
/*
 * P-SEAMLDR support for TDX module management features like runtime updates
 *
 * Copyright (C) 2025 Intel Corporation
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <asm/seamldr.h>

#include "seamcall_internal.h"

/* P-SEAMLDR SEAMCALL leaf function */
#define P_SEAMLDR_INFO			0x8000000000000000

#define SEAMLDR_MAX_NR_MODULE_PAGES	496
#define SEAMLDR_MAX_NR_SIG_PAGES	1

/*
 * The seamldr_params "scenario" field specifies the operation mode:
 * 0: Install TDX module from scratch (not used by kernel)
 * 1: Update existing TDX module to a compatible version
 */
#define SEAMLDR_SCENARIO_UPDATE		1

/*
 * This is called the "SEAMLDR_PARAMS" data structure and is defined
 * in "SEAM Loader (SEAMLDR) Interface Specification".
 *
 * It describes the TDX module that will be installed.
 */
struct seamldr_params {
	u32	version;
	u32	scenario;
	u64	sigstruct_pages_pa_list[SEAMLDR_MAX_NR_SIG_PAGES];
	u8	reserved[104];
	u64	module_nr_pages;
	u64	module_pages_pa_list[SEAMLDR_MAX_NR_MODULE_PAGES];
} __packed;

static_assert(sizeof(struct seamldr_params) == 4096);

/*
 * Serialize P-SEAMLDR calls since the hardware only allows a single CPU to
 * interact with P-SEAMLDR simultaneously. Use raw version as the calls can
 * be made with interrupts disabled, where plain spinlocks are prohibited in
 * PREEMPT_RT kernels as they become sleeping locks.
 */
static DEFINE_RAW_SPINLOCK(seamldr_lock);

static int seamldr_call(u64 fn, struct tdx_module_args *args)
{
	guard(raw_spinlock)(&seamldr_lock);
	return seamcall_prerr(fn, args);
}

int seamldr_get_info(struct seamldr_info *seamldr_info)
{
	struct tdx_module_args args = {};

	/*
	 * Use slow_virt_to_phys() since @seamldr_info may be allocated on
	 * the stack.
	 */
	args.rcx = slow_virt_to_phys(seamldr_info);
	return seamldr_call(P_SEAMLDR_INFO, &args);
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_get_info, "tdx-host");

#define TDX_IMAGE_VERSION_2		0x200

struct tdx_image_header {
	u16	version; // This ABI is always 0x200
	u16	checksum;
	u8	signature[8];
	u32	sigstruct_nr_pages;
	u32	module_nr_pages;
	u8	reserved[4076];
} __packed;

#define HEADER_SIZE sizeof(struct tdx_image_header)
static_assert(HEADER_SIZE == 4096);

/* Intel TDX module update ABI structure. aka. "TDX module blob". */
struct tdx_image {
	struct tdx_image_header header;
	u8 payload[]; // Contains sigstruct pages followed by module pages
};

static void populate_pa_list(u64 *pa_list, u32 max_entries, const u8 *start, u32 nr_pages)
{
	int i;

	nr_pages = MIN(nr_pages, max_entries);
	for (i = 0; i < nr_pages; i++) {
		pa_list[i] = vmalloc_to_pfn(start) << PAGE_SHIFT;
		start += PAGE_SIZE;
	}
}

static void populate_seamldr_params(struct seamldr_params *params,
				    const u8 *sig, u32 sig_nr_pages,
				    const u8 *mod, u32 mod_nr_pages)
{
	params->version			= 0;
	params->scenario		= SEAMLDR_SCENARIO_UPDATE;
	params->module_nr_pages		= mod_nr_pages;

	populate_pa_list(params->sigstruct_pages_pa_list, SEAMLDR_MAX_NR_SIG_PAGES,
			 sig, sig_nr_pages);
	populate_pa_list(params->module_pages_pa_list, SEAMLDR_MAX_NR_MODULE_PAGES,
			 mod, mod_nr_pages);
}

static int init_seamldr_params(struct seamldr_params *params, const u8 *data, u32 size)
{
	const struct tdx_image *image		= (const void *)data;
	const struct tdx_image_header *header	= &image->header;

	u32 sigstruct_len	= header->sigstruct_nr_pages * PAGE_SIZE;
	u32 module_len		= header->module_nr_pages * PAGE_SIZE;

	u8 *header_start	= (u8 *)header;
	u8 *header_end		= header_start + HEADER_SIZE;

	u8 *sigstruct_start	= header_end;
	u8 *sigstruct_end	= sigstruct_start + sigstruct_len;

	u8 *module_start	= sigstruct_end;

	/* Check the calculated payload size against the data size. */
	if (HEADER_SIZE + sigstruct_len + module_len != size)
		return -EINVAL;

	/*
	 * Don't care about user passing the wrong file, but protect
	 * kernel ABI by preventing accepting garbage.
	 */
	if (header->version != TDX_IMAGE_VERSION_2)
		return -EINVAL;

	if (memcmp(header->signature, "TDX-BLOB", sizeof(header->signature)))
		return -EINVAL;

	if (memchr_inv(header->reserved, 0, sizeof(header->reserved)))
		return -EINVAL;

	populate_seamldr_params(params, sigstruct_start, header->sigstruct_nr_pages,
				module_start, header->module_nr_pages);
	return 0;
}

/**
 * seamldr_install_module - Install a new TDX module.
 * @data: Pointer to the TDX module image.
 * @size: Size of the TDX module image.
 *
 * Returns 0 on success, negative error code on failure.
 */
int seamldr_install_module(const u8 *data, u32 size)
{
	struct seamldr_params *params;
	int ret;

	params = kzalloc_obj(*params);
	if (!params)
		return -ENOMEM;

	ret = init_seamldr_params(params, data, size);
	if (ret)
		goto out;

	/* TODO: Update TDX module here */
out:
	kfree(params);
	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_install_module, "tdx-host");
